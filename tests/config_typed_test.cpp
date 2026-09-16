// La configuracion tipada: llenar un struct desde el entorno y validarlo al
// arrancar, en vez de repartir env("DB_POOL", "4") por el bootstrap.

#include <syrax/config.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace {

// Pone una variable y la deja como estaba al salir del scope, para que un
// caso no le cambie el entorno al siguiente.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value) : name_{std::move(name)} {
        if (const char* previous = std::getenv(name_.c_str())) {
            had_      = true;
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (had_) {
            ::setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    bool        had_ = false;
    std::string previous_;
};

struct Config {
    std::string    dbEngine = "postgres";
    int            dbPool   = 4;
    unsigned short appPort  = 8080;
    bool           docs     = true;
    double         timeout  = 2.5;

    static auto rules() {
        return syrax::rules(syrax::field(&Config::dbPool).range(1, 64),
                            syrax::field(&Config::dbEngine).notEmpty());
    }
};

}  // namespace

TEST_CASE("el nombre de la variable sale del campo", "[config]") {
    CHECK(syrax::config::envName("dbPool") == "DB_POOL");
    CHECK(syrax::config::envName("apiBase") == "API_BASE");
    CHECK(syrax::config::envName("port") == "PORT");
    CHECK(syrax::config::envName("queueRetryAfter") == "QUEUE_RETRY_AFTER");

    // Un campo que ya venia separado no gana un guion de mas.
    CHECK(syrax::config::envName("db_Pool") == "DB_POOL");
}

TEST_CASE("sin variables, cada campo se queda con su valor por defecto", "[config]") {
    std::vector<syrax::config::Problem> problems;
    const auto                          cfg = syrax::config::read<Config>(problems);

    CHECK(problems.empty());
    CHECK(cfg.dbEngine == "postgres");
    CHECK(cfg.dbPool == 4);
    CHECK(cfg.docs);
}

TEST_CASE("cada tipo se lee del entorno", "[config]") {
    ScopedEnv engine{"DB_ENGINE", "sqlite"};
    ScopedEnv pool{"DB_POOL", "16"};
    ScopedEnv port{"APP_PORT", "9000"};
    ScopedEnv docs{"DOCS", "off"};
    ScopedEnv timeout{"TIMEOUT", "0.5"};

    std::vector<syrax::config::Problem> problems;
    const auto                          cfg = syrax::config::read<Config>(problems);

    REQUIRE(problems.empty());
    CHECK(cfg.dbEngine == "sqlite");
    CHECK(cfg.dbPool == 16);
    CHECK(cfg.appPort == 9000);
    CHECK_FALSE(cfg.docs);
    CHECK(cfg.timeout == 0.5);
}

TEST_CASE("un valor que no convierte falla con el nombre del campo", "[config]") {
    ScopedEnv pool{"DB_POOL", "cuatro"};

    CHECK_THROWS_AS(syrax::config::load<Config>(), syrax::config::Invalid);

    try {
        syrax::config::load<Config>();
    } catch (const syrax::config::Invalid& e) {
        REQUIRE(e.problems().size() == 1);
        CHECK(e.problems().front().field == "dbPool");
        CHECK(e.problems().front().variable == "DB_POOL");
        CHECK(e.problems().front().value == "cuatro");

        // El mensaje tiene que servir para arreglarlo sin leer el codigo.
        CHECK_THAT(e.what(), ContainsSubstring("DB_POOL"));
        CHECK_THAT(e.what(), ContainsSubstring("cuatro"));
        CHECK_THAT(e.what(), ContainsSubstring("entero"));
    }
}

TEST_CASE("un entero que no cabe en el campo es un error, no un truncamiento", "[config]") {
    // stoll acepta el 70000 y castearlo a unsigned short daria 4464: la
    // aplicacion escucharia en un puerto que nadie pidio.
    ScopedEnv port{"APP_PORT", "70000"};

    CHECK_THROWS_AS(syrax::config::load<Config>(), syrax::config::Invalid);
}

TEST_CASE("las reglas del tipo se aplican al arrancar", "[config]") {
    ScopedEnv pool{"DB_POOL", "0"};

    try {
        syrax::config::load<Config>();
        FAIL("tenia que haber lanzado");
    } catch (const syrax::config::Invalid& e) {
        REQUIRE(e.problems().size() == 1);
        CHECK(e.problems().front().field == "dbPool");
        CHECK(e.problems().front().variable == "DB_POOL");
    }
}

TEST_CASE("los problemas se juntan todos, no se para en el primero", "[config]") {
    ScopedEnv pool{"DB_POOL", "cuatro"};
    ScopedEnv port{"APP_PORT", "ochenta"};
    ScopedEnv docs{"DOCS", "quiza"};

    try {
        syrax::config::load<Config>();
        FAIL("tenia que haber lanzado");
    } catch (const syrax::config::Invalid& e) {
        // Quien tiene tres variables mal quiere verlas de una, no arrancar
        // tres veces para descubrirlas por turnos.
        CHECK(e.problems().size() == 3);
    }
}

TEST_CASE("un booleano acepta lo que la gente escribe en un .env", "[config]") {
    for (const auto* si : {"1", "true", "TRUE", "yes", "on"}) {
        ScopedEnv docs{"DOCS", si};

        std::vector<syrax::config::Problem> problems;
        CHECK(syrax::config::read<Config>(problems).docs);
        CHECK(problems.empty());
    }

    for (const auto* no : {"0", "false", "no", "off"}) {
        ScopedEnv docs{"DOCS", no};

        std::vector<syrax::config::Problem> problems;
        CHECK_FALSE(syrax::config::read<Config>(problems).docs);
        CHECK(problems.empty());
    }
}

TEST_CASE("una variable vacia cuenta como ausente", "[config]") {
    // Un `DB_ENGINE=` en el .env es lo que escribe alguien que queria quitar
    // la linea. Tomarlo como cadena vacia rompe el arranque con un mensaje
    // sobre una regla, en vez de usar el valor por defecto que ya estaba.
    ScopedEnv engine{"DB_ENGINE", ""};

    std::vector<syrax::config::Problem> problems;
    CHECK(syrax::config::read<Config>(problems).dbEngine == "postgres");
    CHECK(problems.empty());
}
