#include <catch2/catch_test_macros.hpp>

#include <drogon/orm/DbClient.h>
#include <syrax/db.hpp>

#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {

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

}  // namespace

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
