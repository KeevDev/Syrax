#include <catch2/catch_test_macros.hpp>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <syrax/query.hpp>

#include <unistd.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// Glaze no refleja tipos en namespace anonimo: por eso lleva nombre.
namespace qtest {

struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;

    static constexpr auto table = "users";
};

// Clave primaria con otro nombre, para comprobar que no esta cableado a "id".
struct Doc {
    std::int64_t doc_id;
    std::string  title;

    static constexpr auto table      = "docs";
    static constexpr auto primaryKey = "doc_id";
};

// Un estado como enum: la columna guarda el entero, el struct los nombres.
enum class Status : std::int64_t { Draft = 1, Sent = 2, Paid = 3 };

struct Invoice {
    std::int64_t id;
    Status       status;
    int          total;

    static constexpr auto table = "invoices";
};

class TempDb {
public:
    TempDb() : path_{fs::temp_directory_path() / name()} {
        client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);

        client_->execSqlSync(
            "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, "
            "email TEXT, age INTEGER)");
        client_->execSqlSync(
            "INSERT INTO users (name, email, age) VALUES "
            "('ada', 'ada@x.com', 36), ('alan', 'alan@x.com', 41), "
            "('grace', 'grace@x.com', 45), ('linus', NULL, 17)");
        client_->execSqlSync(
            "CREATE TABLE docs (doc_id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT)");
        client_->execSqlSync(
            "CREATE TABLE invoices (id INTEGER PRIMARY KEY AUTOINCREMENT, status INTEGER, "
            "total INTEGER)");
        client_->execSqlSync(
            "INSERT INTO invoices (status, total) VALUES (1, 100), (2, 200), (3, 300)");
    }
    ~TempDb() {
        client_.reset();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    const drogon::orm::DbClientPtr& get() const { return client_; }

private:
    static std::string name() {
        static int n = 0;
        return "syrax_q_" + std::to_string(::getpid()) + "_" + std::to_string(n++) + ".db";
    }
    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

// El dialecto es estado global que fija configureFromEnv(); en los tests se
// pone a mano y se restaura.
struct SqliteDialect {
    syrax::db::Dialect previous = syrax::db::dialect();
    SqliteDialect()  { syrax::db::activeDialect() = syrax::db::Dialect::Sqlite; }
    ~SqliteDialect() { syrax::db::activeDialect() = previous; }
};

}  // namespace qtest

using namespace qtest;
using syrax::Dir;
using syrax::Query;

TEST_CASE("el nombre de la columna sale del puntero a miembro", "[query]") {
    CHECK(syrax::detail::columnOf(&User::email) == "email");
    CHECK(syrax::detail::columnOf(&User::age) == "age");
    CHECK(syrax::detail::columnOf(&Doc::doc_id) == "doc_id");
}

TEST_CASE("where genera el SQL esperado", "[query]") {
    SqliteDialect dialect;

    const auto sql = Query<User>().where(&User::age, ">", 18).orderBy(&User::name).toSql();

    CHECK(sql == R"(SELECT "id", "name", "email", "age" FROM "users" WHERE "age" > ? )"
                 R"(ORDER BY "name" ASC)");
}

TEST_CASE("los filtros se encadenan con AND y OR", "[query]") {
    SqliteDialect dialect;

    const auto sql = Query<User>()
                         .where(&User::age, ">", 18)
                         .orWhere(&User::name, "LIKE", std::string{"a%"})
                         .whereNull(&User::email)
                         .toSql();

    CHECK(sql.find(R"("age" > ? OR "name" LIKE ? AND "email" IS NULL)") != std::string::npos);
}

TEST_CASE("get devuelve los structs mapeados", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto adultos = drogon::sync_wait(
        Query<User>(db.get()).where(&User::age, ">", 18).orderBy(&User::age).get());

    REQUIRE(adultos.size() == 3);
    CHECK(adultos[0].name == "ada");
    CHECK(adultos[2].name == "grace");
}

TEST_CASE("first devuelve una sola fila o nada", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto mayor = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::age, Dir::Desc).first());

    REQUIRE(mayor.has_value());
    CHECK(mayor->name == "grace");

    const auto ninguno = drogon::sync_wait(
        Query<User>(db.get()).where(&User::age, ">", 200).first());
    CHECK_FALSE(ninguno.has_value());
}

TEST_CASE("count y exists no traen las filas", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 4);
    CHECK(drogon::sync_wait(Query<User>(db.get()).where(&User::age, "<", 18).count()) == 1);
    CHECK(drogon::sync_wait(Query<User>(db.get()).where(&User::age, ">", 100).exists()) == false);
}

TEST_CASE("whereIn acepta una lista", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto algunos = drogon::sync_wait(
        Query<User>(db.get()).whereIn(&User::name, {std::string{"ada"}, std::string{"grace"}})
            .orderBy(&User::name).get());

    REQUIRE(algunos.size() == 2);
    CHECK(algunos[0].name == "ada");
    CHECK(algunos[1].name == "grace");
}

TEST_CASE("un whereIn vacio no devuelve la tabla entera", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    // El riesgo real: generar "IN ()" o ignorar el filtro y devolverlo todo.
    const auto ninguno = drogon::sync_wait(
        Query<User>(db.get()).whereIn(&User::name, std::vector<std::string>{}).get());

    CHECK(ninguno.empty());
}

TEST_CASE("whereNull distingue NULL de vacio", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto sinEmail = drogon::sync_wait(Query<User>(db.get()).whereNull(&User::email).get());
    REQUIRE(sinEmail.size() == 1);
    CHECK(sinEmail.front().name == "linus");

    CHECK(drogon::sync_wait(Query<User>(db.get()).whereNotNull(&User::email).count()) == 3);
}

TEST_CASE("limit y offset paginan", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto pagina = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::age).limit(2).offset(1).get());

    REQUIRE(pagina.size() == 2);
    CHECK(pagina[0].name == "ada");
    CHECK(pagina[1].name == "alan");
}

TEST_CASE("del borra solo lo filtrado", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto borradas =
        drogon::sync_wait(Query<User>(db.get()).where(&User::age, "<", 18).del());

    CHECK(borradas == 1);
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 3);
}

TEST_CASE("save inserta y rellena la clave primaria", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    User nuevo{.id = 0, .name = "hedy", .email = "hedy@x.com", .age = 30};
    drogon::sync_wait(syrax::save(nuevo, db.get()));

    // El id lo asigna la base y vuelve al objeto: eso es lo que lo hace
    // utilizable como objeto y no como un DTO de ida.
    CHECK(nuevo.id > 0);
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 5);
}

TEST_CASE("save actualiza si la clave primaria ya viene puesta", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    auto ada = drogon::sync_wait(
        Query<User>(db.get()).where(&User::name, "=", std::string{"ada"}).first());
    REQUIRE(ada.has_value());

    ada->age = 99;
    drogon::sync_wait(syrax::save(*ada, db.get()));

    const auto releida = drogon::sync_wait(
        Query<User>(db.get()).where(&User::name, "=", std::string{"ada"}).first());

    REQUIRE(releida.has_value());
    CHECK(releida->age == 99);
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 4);  // no duplico
}

TEST_CASE("remove borra por clave primaria", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    auto ada = drogon::sync_wait(
        Query<User>(db.get()).where(&User::name, "=", std::string{"ada"}).first());
    REQUIRE(ada.has_value());

    CHECK(drogon::sync_wait(syrax::remove(*ada, db.get())));
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 3);
}

TEST_CASE("la clave primaria no esta cableada a 'id'", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    Doc doc{.doc_id = 0, .title = "acta"};
    drogon::sync_wait(syrax::save(doc, db.get()));
    CHECK(doc.doc_id > 0);

    doc.title = "acta revisada";
    drogon::sync_wait(syrax::save(doc, db.get()));

    const auto releido = drogon::sync_wait(Query<Doc>(db.get()).first());
    REQUIRE(releido.has_value());
    CHECK(releido->title == "acta revisada");
}

TEST_CASE("postgres numera los parametros y sqlite no", "[query]") {
    const auto previo = syrax::db::dialect();

    syrax::db::activeDialect() = syrax::db::Dialect::Postgres;
    const auto pg = Query<User>().where(&User::age, ">", 18)
                        .where(&User::name, "=", std::string{"ada"}).toSql();
    CHECK(pg.find("\"age\" > $1") != std::string::npos);
    CHECK(pg.find("\"name\" = $2") != std::string::npos);

    syrax::db::activeDialect() = syrax::db::Dialect::Sqlite;
    const auto lite = Query<User>().where(&User::age, ">", 18)
                          .where(&User::name, "=", std::string{"ada"}).toSql();
    CHECK(lite.find("\"age\" > ?") != std::string::npos);

    syrax::db::activeDialect() = previo;
}

// ------------------------------------------------------------ agrupacion

TEST_CASE("whereGroup pone los parentesis", "[query]") {
    SqliteDialect dialect;

    const auto sql = Query<User>()
                         .where(&User::age, ">", 18)
                         .whereGroup([](auto& g) {
                             g.where(&User::name, "LIKE", std::string{"a%"})
                                 .orWhere(&User::email, "IS", nullptr);
                         })
                         .toSql();

    CHECK(sql.find(R"("age" > ? AND ("name" LIKE ? OR "email" IS ?))") != std::string::npos);
}

TEST_CASE("un grupo no rompe la numeracion de los que vienen detras", "[query]") {
    const auto previo = syrax::db::dialect();
    syrax::db::activeDialect() = syrax::db::Dialect::Postgres;

    const auto sql = Query<User>()
                         .where(&User::age, ">", 18)
                         .whereGroup([](auto& g) {
                             g.where(&User::name, "=", std::string{"ada"})
                                 .orWhere(&User::name, "=", std::string{"grace"});
                         })
                         .where(&User::age, "<", 99)
                         .toSql();

    CHECK(sql.find(R"("age" > $1 AND ("name" = $2 OR "name" = $3) AND "age" < $4)") !=
          std::string::npos);

    syrax::db::activeDialect() = previo;
}

TEST_CASE("un grupo vacio no deja un parentesis suelto", "[query]") {
    SqliteDialect dialect;

    const auto sql = Query<User>().where(&User::age, ">", 18).whereGroup([](auto&) {}).toSql();

    CHECK(sql.find("()") == std::string::npos);
    CHECK(sql.find(R"(WHERE "age" > ?)") != std::string::npos);
}

TEST_CASE("agrupar cambia que filas vuelven, no solo el SQL", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    // Encadenado plano: SQL aplica AND antes que OR, asi que esto es
    // (age > 18 AND name = 'ada') OR name = 'linus' -> entran los dos.
    const auto plano = drogon::sync_wait(Query<User>(db.get())
                                             .where(&User::age, ">", 18)
                                             .where(&User::name, "=", std::string{"ada"})
                                             .orWhere(&User::name, "=", std::string{"linus"})
                                             .get());
    CHECK(plano.size() == 2);

    // Agrupado: age > 18 AND (name = 'ada' OR name = 'linus'). Linus tiene 17.
    const auto agrupado = drogon::sync_wait(Query<User>(db.get())
                                                .where(&User::age, ">", 18)
                                                .whereGroup([](auto& g) {
                                                    g.where(&User::name, "=", std::string{"ada"})
                                                        .orWhere(&User::name, "=",
                                                                 std::string{"linus"});
                                                })
                                                .get());
    REQUIRE(agrupado.size() == 1);
    CHECK(agrupado.front().name == "ada");
}

// --------------------------------------------------------------- update

TEST_CASE("update cambia las filas filtradas en una sola consulta", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto tocadas = drogon::sync_wait(Query<User>(db.get())
                                               .where(&User::age, "<", 18)
                                               .set(&User::name, std::string{"menor"})
                                               .set(&User::age, 0)
                                               .update());
    CHECK(tocadas == 1);

    // Si el orden de enlace fuera el otro, sqlite habria puesto el 18 en el
    // nombre o filtrado por "menor": el numero de filas ya no cuadraria.
    const auto menor = drogon::sync_wait(
        Query<User>(db.get()).where(&User::name, "=", std::string{"menor"}).first());
    REQUIRE(menor.has_value());
    CHECK(menor->age == 0);

    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 4);
    CHECK(drogon::sync_wait(
              Query<User>(db.get()).where(&User::name, "=", std::string{"ada"}).count()) == 1);
}

TEST_CASE("un update sin set no toca nada", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    // "UPDATE users SET  WHERE ..." no es SQL: el builder ni lo intenta.
    CHECK(drogon::sync_wait(Query<User>(db.get()).where(&User::age, "<", 18).update()) == 0);
    CHECK(drogon::sync_wait(
              Query<User>(db.get()).where(&User::name, "=", std::string{"linus"}).count()) == 1);
}

TEST_CASE("en postgres el SET se numera detras del WHERE", "[query]") {
    const auto previo = syrax::db::dialect();
    syrax::db::activeDialect() = syrax::db::Dialect::Postgres;

    const auto sql = Query<User>()
                         .where(&User::age, "<", 18)
                         .set(&User::name, std::string{"menor"})
                         .toUpdateSql();

    CHECK(sql == R"(UPDATE "users" SET "name" = $2 WHERE "age" < $1)");

    syrax::db::activeDialect() = previo;
}

// ---------------------------------------------------------------- enums

TEST_CASE("un enum se enlaza como su tipo subyacente", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    CHECK(drogon::sync_wait(
              Query<Invoice>(db.get()).where(&Invoice::status, "=", Status::Paid).count()) == 1);

    const auto pendientes = drogon::sync_wait(
        Query<Invoice>(db.get())
            .whereIn(&Invoice::status, {Status::Draft, Status::Sent})
            .orderBy(&Invoice::total)
            .get());

    REQUIRE(pendientes.size() == 2);
    CHECK(pendientes.front().status == Status::Draft);
}

TEST_CASE("un campo enum vuelve del SELECT con su nombre", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    Invoice nueva{.id = 0, .status = Status::Sent, .total = 42};
    drogon::sync_wait(syrax::save(nueva, db.get()));
    REQUIRE(nueva.id > 0);
    CHECK(nueva.status == Status::Sent);

    drogon::sync_wait(Query<Invoice>(db.get())
                          .where(&Invoice::id, "=", nueva.id)
                          .set(&Invoice::status, Status::Paid)
                          .update());

    const auto releida = drogon::sync_wait(
        Query<Invoice>(db.get()).where(&Invoice::id, "=", nueva.id).first());

    REQUIRE(releida.has_value());
    CHECK(releida->status == Status::Paid);
    CHECK(releida->total == 42);
}
