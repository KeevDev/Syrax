#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <syrax/syrax.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>

using Catch::Matchers::ContainsSubstring;
using syrax::Dialect;

namespace {

// Pone una variable de entorno y la deja como estaba al salir del bloque.
struct ScopedEnv {
    std::string key;
    bool        existed;
    std::string previous;

    ScopedEnv(std::string name, const char* value) : key{std::move(name)} {
        const char* current = std::getenv(key.c_str());
        existed             = current != nullptr;
        if (existed) previous = current;

        ::setenv(key.c_str(), value, /*overwrite=*/1);
    }

    ~ScopedEnv() {
        if (existed) ::setenv(key.c_str(), previous.c_str(), 1);
        else         ::unsetenv(key.c_str());
    }
};

}  // namespace

TEST_CASE("env devuelve el respaldo cuando la variable no existe") {
    ::unsetenv("SYRAX_TEST_AUSENTE");
    CHECK(syrax::env("SYRAX_TEST_AUSENTE", "respaldo") == "respaldo");
}

TEST_CASE("una variable vacia cuenta como ausente") {
    ScopedEnv vacia{"SYRAX_TEST_VACIA", ""};
    CHECK(syrax::env("SYRAX_TEST_VACIA", "respaldo") == "respaldo");
}

TEST_CASE("envInt lee enteros") {
    ScopedEnv puerto{"SYRAX_TEST_ENTERO", "5532"};
    CHECK(syrax::envInt("SYRAX_TEST_ENTERO", 5432) == 5532);
}

TEST_CASE("envInt cae al respaldo si el valor no es un numero") {
    ScopedEnv roto{"SYRAX_TEST_ENTERO", "cinco mil"};
    CHECK(syrax::envInt("SYRAX_TEST_ENTERO", 5432) == 5432);
}

TEST_CASE("envBool acepta las formas que la gente escribe") {
    {
        ScopedEnv on{"SYRAX_TEST_BOOL", "yes"};
        CHECK(syrax::envBool("SYRAX_TEST_BOOL", false));
    }
    {
        ScopedEnv off{"SYRAX_TEST_BOOL", "OFF"};
        CHECK_FALSE(syrax::envBool("SYRAX_TEST_BOOL", true));
    }
}

TEST_CASE("el motor se reconoce por cualquiera de sus nombres") {
    CHECK(syrax::db::engineFromName("postgres") == Dialect::Postgres);
    CHECK(syrax::db::engineFromName("PostgreSQL") == Dialect::Postgres);
    CHECK(syrax::db::engineFromName("pgsql") == Dialect::Postgres);
    CHECK(syrax::db::engineFromName("mysql") == Dialect::Mysql);
    CHECK(syrax::db::engineFromName("mariadb") == Dialect::Mysql);
    CHECK(syrax::db::engineFromName("sqlite3") == Dialect::Sqlite);
}

TEST_CASE("un motor desconocido no pasa en silencio") {
    CHECK_THROWS_AS(syrax::db::engineFromName("oracle"), std::invalid_argument);
}

TEST_CASE("cada motor tiene su puerto habitual") {
    CHECK(syrax::db::defaultPort(Dialect::Postgres) == 5432);
    CHECK(syrax::db::defaultPort(Dialect::Mysql) == 3306);
    CHECK(syrax::db::defaultPort(Dialect::Sqlite) == 0);
}

TEST_CASE("la conexion se arma desde el entorno") {
    ScopedEnv engine{"DB_ENGINE", "mysql"};
    ScopedEnv host{"DB_HOST", "db.interno"};
    ScopedEnv port{"DB_PORT", "3307"};
    ScopedEnv name{"DB_NAME", "facturas"};

    const auto conn = syrax::db::envConnection();

    CHECK(conn.engine == "mysql");
    CHECK(conn.host == "db.interno");
    CHECK(conn.port == 3307);
    CHECK(conn.database == "facturas");
    CHECK(conn.name == "default");
}

TEST_CASE("sin DB_PORT la conexion deja que el motor ponga el suyo") {
    ::unsetenv("DB_PORT");
    CHECK(syrax::db::envConnection().port == 0);
}

TEST_CASE("una conexion con nombre propio conserva el nombre") {
    CHECK(syrax::db::envConnection("informes").name == "informes");
}

TEST_CASE("el cache tambien se configura desde el entorno") {
    ScopedEnv host{"REDIS_HOST", "cache.interno"};
    ScopedEnv db{"REDIS_DB", "3"};

    const auto conn = syrax::cache::envConnection();

    CHECK(conn.host == "cache.interno");
    CHECK(conn.port == 6379);
    CHECK(conn.database == 3);
}

TEST_CASE("pedir el cache antes de arrancar avisa en vez de reventar") {
    CHECK_THROWS_WITH(syrax::cache::client("sesiones"), ContainsSubstring("sesiones"));
}
