#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <syrax/migration.hpp>

#include <stdexcept>

using Catch::Matchers::ContainsSubstring;
using syrax::Blueprint;
using syrax::Dialect;
using syrax::Schema;

namespace {

// Junta todas las sentencias que produjo una migracion, para poder buscar
// fragmentos dentro sin depender del formato exacto.
std::string ddl(Dialect dialect, const std::function<void(Blueprint&)>& build) {
    Schema schema{dialect};
    schema.create("users", build);

    std::string all;
    for (const auto& statement : schema.statements()) all += statement + ";\n";
    return all;
}

}  // namespace

TEST_CASE("id() usa el tipo autoincremental de cada motor", "[schema]") {
    const auto builder = [](Blueprint& t) { t.id(); };

    CHECK_THAT(ddl(Dialect::Postgres, builder), ContainsSubstring("\"id\" BIGSERIAL PRIMARY KEY"));
    CHECK_THAT(ddl(Dialect::Sqlite, builder), ContainsSubstring("\"id\" INTEGER PRIMARY KEY"));
}

TEST_CASE("string() se mapea a VARCHAR en postgres y TEXT en sqlite", "[schema]") {
    const auto builder = [](Blueprint& t) { t.string("name", 120); };

    CHECK_THAT(ddl(Dialect::Postgres, builder), ContainsSubstring("VARCHAR(120)"));
    CHECK_THAT(ddl(Dialect::Sqlite, builder), ContainsSubstring("\"name\" TEXT"));
}

TEST_CASE("las columnas son NOT NULL salvo que se pidan nullable", "[schema]") {
    CHECK_THAT(ddl(Dialect::Postgres, [](Blueprint& t) { t.string("name"); }),
               ContainsSubstring("NOT NULL"));

    CHECK_THAT(ddl(Dialect::Postgres, [](Blueprint& t) { t.string("bio").nullable(); }),
               !ContainsSubstring("NOT NULL"));
}

TEST_CASE("los modificadores se encadenan", "[schema]") {
    const auto sql = ddl(Dialect::Postgres, [](Blueprint& t) {
        t.string("email").unique();
        t.integer("age").defaultTo("18");
    });

    CHECK_THAT(sql, ContainsSubstring("\"email\" VARCHAR(255) NOT NULL UNIQUE"));
    CHECK_THAT(sql, ContainsSubstring("DEFAULT 18"));
}

TEST_CASE("la clave primaria no lleva NOT NULL ni UNIQUE redundantes", "[schema]") {
    // Son implicitos en PRIMARY KEY; repetirlos es ruido y algunos motores
    // lo rechazan.
    const auto sql = ddl(Dialect::Postgres, [](Blueprint& t) { t.id(); });

    CHECK_THAT(sql, ContainsSubstring("PRIMARY KEY"));
    CHECK_THAT(sql, !ContainsSubstring("PRIMARY KEY NOT NULL"));
    CHECK_THAT(sql, !ContainsSubstring("PRIMARY KEY UNIQUE"));
}

TEST_CASE("foreignId genera la referencia", "[schema]") {
    const auto sql = ddl(Dialect::Postgres, [](Blueprint& t) {
        t.foreignId("user_id", "users").onDeleteCascade();
    });

    CHECK_THAT(sql, ContainsSubstring("REFERENCES \"users\"(\"id\")"));
    CHECK_THAT(sql, ContainsSubstring("ON DELETE CASCADE"));
}

TEST_CASE("index() emite un CREATE INDEX aparte", "[schema]") {
    const auto sql = ddl(Dialect::Postgres, [](Blueprint& t) { t.string("email").index(); });

    CHECK_THAT(sql, ContainsSubstring("CREATE INDEX IF NOT EXISTS \"idx_users_email\""));
}

TEST_CASE("timestamps() agrega created_at y updated_at", "[schema]") {
    const auto sql = ddl(Dialect::Postgres, [](Blueprint& t) { t.timestamps(); });

    CHECK_THAT(sql, ContainsSubstring("\"created_at\""));
    CHECK_THAT(sql, ContainsSubstring("\"updated_at\""));
    CHECK_THAT(sql, ContainsSubstring("DEFAULT CURRENT_TIMESTAMP"));
}

TEST_CASE("create usa IF NOT EXISTS para poder reaplicarse", "[schema]") {
    CHECK_THAT(ddl(Dialect::Postgres, [](Blueprint& t) { t.id(); }),
               ContainsSubstring("CREATE TABLE IF NOT EXISTS \"users\""));
}

TEST_CASE("drop y raw", "[schema]") {
    Schema schema{Dialect::Sqlite};
    schema.drop("users");
    schema.raw("PRAGMA foreign_keys = ON");

    REQUIRE(schema.statements().size() == 2);
    CHECK_THAT(schema.statements()[0], ContainsSubstring("DROP TABLE IF EXISTS \"users\""));
    CHECK(schema.statements()[1] == "PRAGMA foreign_keys = ON");
}

// ---------------------------------------------------------------- ALTER

namespace {

std::string alterDdl(Dialect dialect, const std::function<void(Blueprint&)>& build) {
    Schema schema{dialect};
    schema.table("users", build);

    std::string all;
    for (const auto& statement : schema.statements()) all += statement + ";\n";
    return all;
}

}  // namespace

TEST_CASE("table() agrega columnas con ADD COLUMN", "[schema][alter]") {
    const auto sql = alterDdl(Dialect::Postgres,
                              [](Blueprint& t) { t.string("phone").nullable(); });

    CHECK_THAT(sql, ContainsSubstring("ALTER TABLE \"users\" ADD COLUMN \"phone\" VARCHAR(255)"));
}

TEST_CASE("agregar NOT NULL sin DEFAULT es un error explicito", "[schema][alter]") {
    // Falla en ambos motores si la tabla tiene filas. Mejor un mensaje claro
    // aqui que un error de SQL cripto en produccion.
    CHECK_THROWS_AS(alterDdl(Dialect::Postgres, [](Blueprint& t) { t.string("phone"); }),
                    std::logic_error);

    CHECK_NOTHROW(alterDdl(Dialect::Postgres, [](Blueprint& t) { t.string("phone").nullable(); }));
    CHECK_NOTHROW(
        alterDdl(Dialect::Postgres, [](Blueprint& t) { t.string("phone").defaultTo("''"); }));
}

TEST_CASE("dropColumn y renameColumn", "[schema][alter]") {
    const auto sql = alterDdl(Dialect::Sqlite, [](Blueprint& t) {
        t.dropColumn("age");
        t.renameColumn("name", "full_name");
    });

    CHECK_THAT(sql, ContainsSubstring("DROP COLUMN \"age\""));
    CHECK_THAT(sql, ContainsSubstring("RENAME COLUMN \"name\" TO \"full_name\""));
}

TEST_CASE("los renames van antes que los ADD COLUMN", "[schema][alter]") {
    // Para poder renombrar una columna y agregar otra con el nombre viejo en
    // la misma migracion sin que choquen.
    Schema schema{Dialect::Postgres};
    schema.table("users", [](Blueprint& t) {
        t.string("name").nullable();
        t.renameColumn("name", "old_name");
    });

    REQUIRE(schema.statements().size() == 2);
    CHECK_THAT(schema.statements()[0], ContainsSubstring("RENAME COLUMN"));
    CHECK_THAT(schema.statements()[1], ContainsSubstring("ADD COLUMN"));
}

TEST_CASE("rename() cambia el nombre de la tabla", "[schema][alter]") {
    Schema schema{Dialect::Postgres};
    schema.rename("users", "people");

    REQUIRE(schema.statements().size() == 1);
    CHECK_THAT(schema.statements()[0],
               ContainsSubstring("ALTER TABLE \"users\" RENAME TO \"people\""));
}

TEST_CASE("dropIndex quita el indice por convencion de nombre", "[schema][alter]") {
    const auto sql = alterDdl(Dialect::Postgres, [](Blueprint& t) { t.dropIndex("email"); });

    CHECK_THAT(sql, ContainsSubstring("DROP INDEX IF EXISTS \"idx_users_email\""));
}

TEST_CASE("change() cambia el tipo y la nulabilidad de una columna", "[schema][alter]") {
    Schema schema{Dialect::Postgres};

    schema.table("users", [](Blueprint& t) {
        t.string("email", 320).nullable().change();
    });

    const auto& sql = schema.statements();
    REQUIRE(sql.size() == 2);

    // Postgres necesita un ALTER por aspecto: tipo y nulabilidad no van juntos.
    CHECK(sql[0] == R"(ALTER TABLE "users" ALTER COLUMN "email" TYPE VARCHAR(320))");
    CHECK(sql[1] == R"(ALTER TABLE "users" ALTER COLUMN "email" DROP NOT NULL)");
}

TEST_CASE("change() a NOT NULL con default emite las tres sentencias", "[schema][alter]") {
    Schema schema{Dialect::Postgres};

    schema.table("users", [](Blueprint& t) {
        t.integer("age").defaultTo("0").change();
    });

    const auto& sql = schema.statements();
    REQUIRE(sql.size() == 3);
    CHECK(sql[1] == R"(ALTER TABLE "users" ALTER COLUMN "age" SET NOT NULL)");
    CHECK(sql[2] == R"(ALTER TABLE "users" ALTER COLUMN "age" SET DEFAULT 0)");
}

TEST_CASE("quitar el default hay que pedirlo explicitamente", "[schema][alter]") {
    Schema schema{Dialect::Postgres};

    schema.table("users", [](Blueprint& t) {
        t.integer("age").nullable().dropDefault().change();
    });

    CHECK(schema.statements().back() == R"(ALTER TABLE "users" ALTER COLUMN "age" DROP DEFAULT)");
}

TEST_CASE("castUsing acompana una conversion que postgres no hace sola", "[schema][alter]") {
    Schema schema{Dialect::Postgres};

    schema.table("users", [](Blueprint& t) {
        t.integer("age").nullable().castUsing("age::integer").change();
    });

    CHECK(schema.statements()[0] ==
          R"(ALTER TABLE "users" ALTER COLUMN "age" TYPE INTEGER USING age::integer)");
}

TEST_CASE("change() con unique agrega la constraint con nombre predecible", "[schema][alter]") {
    Schema schema{Dialect::Postgres};

    schema.table("users", [](Blueprint& t) {
        t.string("email").unique().change();
    });

    CHECK(schema.statements().back() ==
          R"(ALTER TABLE "users" ADD CONSTRAINT "uq_users_email" UNIQUE ("email"))");
}

TEST_CASE("dropUnique usa el mismo nombre que genera unique()", "[schema][alter]") {
    Schema schema{Dialect::Postgres};

    schema.table("users", [](Blueprint& t) { t.dropUnique("email"); });

    CHECK(schema.statements()[0] ==
          R"(ALTER TABLE "users" DROP CONSTRAINT IF EXISTS "uq_users_email")");
}

TEST_CASE("un check se agrega y se quita por nombre", "[schema][alter]") {
    Schema schema{Dialect::Postgres};

    schema.table("users", [](Blueprint& t) {
        t.check("age_no_negativa", "age >= 0");
        t.dropConstraint("vieja");
    });

    const auto& sql = schema.statements();
    CHECK(sql[0] == R"(ALTER TABLE "users" ADD CONSTRAINT "age_no_negativa" CHECK (age >= 0))");
    CHECK(sql[1] == R"(ALTER TABLE "users" DROP CONSTRAINT IF EXISTS "vieja")");
}

TEST_CASE("change() en sqlite falla con un mensaje que dice que hacer", "[schema][alter]") {
    Schema schema{Dialect::Sqlite};

    // No es una carencia de syrax: sqlite solo soporta RENAME, ADD y DROP
    // COLUMN. Cambiar un tipo exige reconstruir la tabla entera.
    CHECK_THROWS_WITH(
        schema.table("users", [](Blueprint& t) { t.string("email").change(); }),
        ContainsSubstring("sqlite no soporta ALTER COLUMN") &&
            ContainsSubstring("Schema::raw()"));
}

TEST_CASE("agregar y modificar conviven en la misma migracion", "[schema][alter]") {
    Schema schema{Dialect::Postgres};

    schema.table("users", [](Blueprint& t) {
        t.string("phone").nullable();            // nueva
        t.string("email", 320).nullable().change();  // existente
    });

    const auto& sql = schema.statements();
    CHECK(sql[0] == R"(ALTER TABLE "users" ADD COLUMN "phone" VARCHAR(255))");
    CHECK(sql[1] == R"(ALTER TABLE "users" ALTER COLUMN "email" TYPE VARCHAR(320))");
}

// ------------------------------------------------------------------ MySQL

TEST_CASE("mysql cita los identificadores con acentos graves") {
    const auto sql = ddl(Dialect::Mysql, [](Blueprint& t) { t.string("name"); });

    CHECK_THAT(sql, ContainsSubstring("`users`"));
    CHECK_THAT(sql, ContainsSubstring("`name`"));
    CHECK_THAT(sql, !ContainsSubstring("\"users\""));
}

TEST_CASE("mysql numera la clave primaria con AUTO_INCREMENT") {
    CHECK_THAT(ddl(Dialect::Mysql, [](Blueprint& t) { t.id(); }),
               ContainsSubstring("`id` BIGINT AUTO_INCREMENT PRIMARY KEY"));
}

TEST_CASE("los tipos cambian con el motor") {
    const auto builder = [](Blueprint& t) {
        t.boolean("activo");
        t.json("meta");
        t.timestamp("visto_at");
    };

    const auto mysql = ddl(Dialect::Mysql, builder);
    CHECK_THAT(mysql, ContainsSubstring("TINYINT(1)"));
    CHECK_THAT(mysql, ContainsSubstring("`meta` JSON"));
    CHECK_THAT(mysql, ContainsSubstring("DATETIME"));

    const auto postgres = ddl(Dialect::Postgres, builder);
    CHECK_THAT(postgres, ContainsSubstring("BOOLEAN"));
    CHECK_THAT(postgres, ContainsSubstring("JSONB"));
    CHECK_THAT(postgres, ContainsSubstring("TIMESTAMPTZ"));
}

TEST_CASE("mysql no acepta IF NOT EXISTS al crear un indice") {
    const auto sql = ddl(Dialect::Mysql, [](Blueprint& t) { t.string("email").index(); });

    CHECK_THAT(sql, ContainsSubstring("CREATE INDEX `idx_users_email` ON `users`"));
    CHECK_THAT(sql, !ContainsSubstring("IF NOT EXISTS"));
}

TEST_CASE("mysql redefine la columna en una sola sentencia") {
    Schema schema{Dialect::Mysql};
    schema.table("users", [](Blueprint& t) { t.string("email", 320).nullable().change(); });

    REQUIRE(schema.statements().size() == 1);
    CHECK_THAT(schema.statements()[0],
               ContainsSubstring("MODIFY COLUMN `email` VARCHAR(320) NULL"));
}

TEST_CASE("en mysql un unique se quita como indice") {
    Schema schema{Dialect::Mysql};
    schema.table("users", [](Blueprint& t) { t.dropUnique("email"); });

    REQUIRE(schema.statements().size() == 1);
    CHECK_THAT(schema.statements()[0], ContainsSubstring("DROP INDEX `uq_users_email`"));
}

TEST_CASE("en mysql un check se quita con DROP CHECK") {
    Schema schema{Dialect::Mysql};
    schema.table("users", [](Blueprint& t) { t.dropConstraint("age_no_negativa"); });

    REQUIRE(schema.statements().size() == 1);
    CHECK_THAT(schema.statements()[0], ContainsSubstring("DROP CHECK `age_no_negativa`"));
}

TEST_CASE("el indice de mysql se borra nombrando la tabla") {
    Schema schema{Dialect::Mysql};
    schema.table("users", [](Blueprint& t) { t.dropIndex("email"); });

    REQUIRE(schema.statements().size() == 1);
    CHECK_THAT(schema.statements()[0], ContainsSubstring("DROP INDEX `idx_users_email` ON `users`"));
}

TEST_CASE("sqlite sigue sin poder alterar columnas") {
    Schema schema{Dialect::Sqlite};
    CHECK_THROWS_AS(
        schema.table("users", [](Blueprint& t) { t.string("email").change(); }),
        std::logic_error);
}
