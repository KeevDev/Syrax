#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <syrax/migration.hpp>

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
