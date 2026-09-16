#include <catch2/catch_test_macros.hpp>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <syrax/db.hpp>
#include <syrax/migration.hpp>

#include <stdexcept>

#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

// Los tipos que se reflejan NO pueden vivir en un namespace anonimo: Glaze
// toma su nombre por una variable `extern` y un tipo sin enlace no puede
// nombrarse desde otra unidad de traduccion. GCC lo deja pasar, clang lo
// rechaza. Por eso el namespace lleva nombre.
namespace dbtest {

struct Person {
    std::int64_t id;
    std::string  name;
    int          age;
};

struct WithOptional {
    std::int64_t                id;
    std::optional<std::string>  nickname;
};

// Crea una base sqlite temporal y la borra al salir del scope.
class TempDb {
public:
    TempDb() : path_{fs::temp_directory_path() / uniqueName()} {
        // Un proceso anterior con este mismo pid pudo morir sin limpiar. Abrir
        // encima de su archivo dejaria las filas de los dos, y el test falla
        // una vez cada muchas sin explicacion.
        std::error_code ec;
        fs::remove(path_, ec);

        client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);
    }
    ~TempDb() {
        client_.reset();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    const drogon::orm::DbClientPtr& operator->() const { return client_; }
    const drogon::orm::DbClientPtr& get() const { return client_; }

private:
    static std::string uniqueName() {
        static int counter = 0;
        return "syrax_test_" + std::to_string(::getpid()) + "_" +
               std::to_string(counter++) + ".db";
    }

    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

}  // namespace dbtest

using namespace dbtest;

TEST_CASE("fromRow mapea columnas a campos por nombre", "[db]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");
    db->execSqlSync("INSERT INTO people VALUES (7, 'ada', 36)");

    const auto result = db->execSqlSync("SELECT id, name, age FROM people");
    REQUIRE(result.size() == 1);

    const auto person = syrax::db::fromRow<Person>(result.front());
    CHECK(person.id == 7);
    CHECK(person.name == "ada");
    CHECK(person.age == 36);
}

TEST_CASE("el orden de las columnas en el SELECT no importa", "[db]") {
    // El vinculo es por NOMBRE, no por posicion: es lo que hace que no haya
    // que escribir el mapeo a mano.
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");
    db->execSqlSync("INSERT INTO people VALUES (1, 'grace', 85)");

    const auto result = db->execSqlSync("SELECT age, name, id FROM people");
    const auto person = syrax::db::fromRow<Person>(result.front());

    CHECK(person.id == 1);
    CHECK(person.name == "grace");
    CHECK(person.age == 85);
}

TEST_CASE("una columna NULL deja el campo en su valor por defecto", "[db]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");
    db->execSqlSync("INSERT INTO people VALUES (2, NULL, NULL)");

    const auto person = syrax::db::fromRow<Person>(
        db->execSqlSync("SELECT id, name, age FROM people").front());

    CHECK(person.id == 2);
    CHECK(person.name.empty());
    CHECK(person.age == 0);
}

TEST_CASE("std::optional distingue NULL de vacio", "[db]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE users (id INTEGER, nickname TEXT)");
    db->execSqlSync("INSERT INTO users VALUES (1, 'kev'), (2, NULL)");

    const auto rows = db->execSqlSync("SELECT id, nickname FROM users ORDER BY id");

    const auto conValor = syrax::db::fromRow<WithOptional>(rows[0]);
    REQUIRE(conValor.nickname.has_value());
    CHECK(*conValor.nickname == "kev");

    const auto conNull = syrax::db::fromRow<WithOptional>(rows[1]);
    CHECK_FALSE(conNull.nickname.has_value());
}

TEST_CASE("loadDotEnv no pisa variables que ya existen", "[db]") {
    // El entorno real siempre debe ganarle al archivo: es lo que permite
    // sobreescribir credenciales en produccion sin tocar el .env.
    ::setenv("SYRAX_TEST_VAR", "del-entorno", /*overwrite=*/1);

    const auto path = fs::temp_directory_path() / "syrax_test.env";
    {
        std::ofstream file(path);
        file << "SYRAX_TEST_VAR=del-archivo\n";
        file << "SYRAX_TEST_NUEVA=\"entre comillas\"\n";
    }

    syrax::db::loadDotEnv(path.string());

    CHECK(syrax::db::env("SYRAX_TEST_VAR", "") == "del-entorno");
    CHECK(syrax::db::env("SYRAX_TEST_NUEVA", "") == "entre comillas");

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("env devuelve el fallback si la variable no existe", "[db]") {
    CHECK(syrax::db::env("SYRAX_NO_EXISTE_JAMAS", "fallback") == "fallback");
}

TEST_CASE("el SQL de ALTER se ejecuta de verdad en sqlite", "[db][alter]") {
    // Generar SQL plausible no basta: hay que comprobar que el motor lo acepta.
    TempDb db;

    syrax::Schema create{syrax::Dialect::Sqlite};
    create.create("things", [](syrax::Blueprint& t) {
        t.id();
        t.string("name");
    });
    for (const auto& sql : create.statements()) db->execSqlSync(sql);

    db->execSqlSync("INSERT INTO things (name) VALUES ('uno')");

    syrax::Schema alter{syrax::Dialect::Sqlite};
    alter.table("things", [](syrax::Blueprint& t) {
        t.string("color").defaultTo("'rojo'");
        t.renameColumn("name", "label");
    });
    for (const auto& sql : alter.statements()) db->execSqlSync(sql);

    const auto row = db->execSqlSync("SELECT id, label, color FROM things").front();
    CHECK(row["label"].as<std::string>() == "uno");
    CHECK(row["color"].as<std::string>() == "rojo");

    // Y que el DROP tambien se aplica.
    syrax::Schema drop{syrax::Dialect::Sqlite};
    drop.table("things", [](syrax::Blueprint& t) { t.dropColumn("color"); });
    for (const auto& sql : drop.statements()) db->execSqlSync(sql);

    CHECK_THROWS(db->execSqlSync("SELECT color FROM things"));
}

TEST_CASE("una transaccion confirma lo que hizo el cuerpo", "[db][tx]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");

    drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<void> {
            co_await tx.execute("INSERT INTO people VALUES (1, 'ada', 36)");
            co_await tx.execute("INSERT INTO people VALUES (2, 'alan', 41)");
        }));

    CHECK(db->execSqlSync("SELECT id FROM people").size() == 2);
}

TEST_CASE("si el cuerpo lanza, la transaccion se deshace entera", "[db][tx]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");
    db->execSqlSync("INSERT INTO people VALUES (9, 'previo', 1)");

    CHECK_THROWS_AS(
        drogon::sync_wait(syrax::db::transactionOn(
            db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<void> {
                co_await tx.execute("INSERT INTO people VALUES (1, 'ada', 36)");
                throw std::runtime_error("algo salio mal a mitad");
            })),
        std::runtime_error);

    // El INSERT de dentro no queda, y lo de antes sigue intacto: eso es lo que
    // significa "entera".
    const auto rows = db->execSqlSync("SELECT id FROM people");
    REQUIRE(rows.size() == 1);
    CHECK(rows.front()["id"].as<int>() == 9);
}

TEST_CASE("rollback explicito deshace sin lanzar", "[db][tx]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");

    // Abortar puede ser una decision de negocio, no un error.
    const auto aborted = drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<bool> {
            co_await tx.execute("INSERT INTO people VALUES (1, 'ada', 36)");
            tx.rollback();
            co_return true;
        }));

    CHECK(aborted);
    CHECK(db->execSqlSync("SELECT id FROM people").empty());
}

TEST_CASE("dentro de la transaccion se lee lo que ella misma escribio", "[db][tx]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");

    const auto found = drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<std::optional<Person>> {
            co_await tx.execute("INSERT INTO people VALUES (5, 'grace', 45)");
            co_return co_await tx.findOne<Person>("SELECT id, name, age FROM people WHERE id = 5");
        }));

    REQUIRE(found.has_value());
    CHECK(found->name == "grace");
}

TEST_CASE("scalar devuelve un valor suelto sin declarar un struct", "[db]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");
    db->execSqlSync("INSERT INTO people VALUES (1, 'ada', 36), (2, 'alan', 41)");

    const auto total = drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<std::int64_t> {
            co_return co_await tx.scalar<std::int64_t>("SELECT count(*) FROM people");
        }));

    CHECK(total == 2);
}

TEST_CASE("scalar sobre una tabla vacia da el valor por defecto", "[db]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT, age INTEGER)");

    // max() de cero filas es NULL, no un error: devuelve 0 en vez de reventar.
    const auto maximo = drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<std::int64_t> {
            co_return co_await tx.scalar<std::int64_t>("SELECT max(age) FROM people");
        }));

    CHECK(maximo == 0);
}

TEST_CASE("un COMMIT que falla no se traga en silencio", "[db]") {
    TempDb db;
    db->execSqlSync("PRAGMA foreign_keys = ON");
    db->execSqlSync("CREATE TABLE padre (id INTEGER PRIMARY KEY)");
    db->execSqlSync(
        "CREATE TABLE hijo (id INTEGER PRIMARY KEY, padre_id INTEGER "
        "REFERENCES padre(id) DEFERRABLE INITIALLY DEFERRED)");

    // Una clave ajena diferida no se comprueba al INSERT: revienta en el
    // COMMIT. Es exactamente el caso que antes respondia 201 y perdia la
    // fila, porque el COMMIT ocurria despues de que el handler terminara.
    // Que deja el motor despues de un COMMIT roto es asunto suyo; lo que
    // aqui se comprueba es que el fallo llega a quien pidio la transaccion.
    CHECK_THROWS_AS(drogon::sync_wait(syrax::db::transactionOn(
                        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<void> {
                            co_await tx.execute(
                                "INSERT INTO hijo (id, padre_id) VALUES (1, 999)");
                        })),
                    syrax::db::CommitFailed);
}

TEST_CASE("un rollback del cuerpo no se queda esperando un COMMIT que no llega", "[db]") {
    TempDb db;
    db->execSqlSync("CREATE TABLE people (id INTEGER, name TEXT)");

    // Drogon no invoca el callback de commit si la transaccion ya se deshizo,
    // asi que esperarlo aqui colgaria la corrutina para siempre. Que este test
    // termine es el test.
    drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<void> {
            co_await tx.execute("INSERT INTO people (id, name) VALUES (1, 'ada')");
            tx.rollback();
            co_return;
        }));

    const auto total = drogon::sync_wait(
        syrax::db::transactionOn(db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<std::int64_t> {
            co_return co_await tx.scalar<std::int64_t>("SELECT count(*) FROM people");
        }));

    CHECK(total == 0);
}
