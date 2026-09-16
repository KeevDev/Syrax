// La salud de la aplicacion, con las dependencias de verdad.
//
// Va en el binario del kit de tests y no en syrax_tests porque necesita una
// aplicacion levantada con una base detras, que es exactamente lo que el kit
// sabe montar. Comprobar /health contra un proceso sin base solo probaria que
// el endpoint existe, que es el bug que este punto viene a arreglar.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace {

const syrax::health::Check* find(const syrax::health::Report& report, const std::string& name) {
    const auto it = std::ranges::find(report.checks, name, &syrax::health::Check::name);
    return it == report.checks.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("con la base arriba, /health contesta 200 y dice quien respondio", "[health]") {
    const auto response = api().get("/health");

    REQUIRE(response.status == 200);

    const auto report = response.json<syrax::health::Report>();
    CHECK(report.status == "ok");

    const auto* database = find(report, "database");
    REQUIRE(database != nullptr);
    CHECK(database->status == "ok");
    CHECK(database->detail.empty());
}

TEST_CASE("una comprobacion propia entra en el informe", "[health]") {
    syrax::health::probe("propia", []() -> syrax::Task<std::string> { co_return ""; });

    const auto report = api().get("/health").json<syrax::health::Report>();
    const auto* mia   = find(report, "propia");

    REQUIRE(mia != nullptr);
    CHECK(mia->status == "ok");

    syrax::health::reset();
}

TEST_CASE("una dependencia caida da 503, y el cuerpo dice cual", "[health]") {
    syrax::health::probe("s3", []() -> syrax::Task<std::string> { co_return "no responde"; });

    const auto response = api().get("/health");

    // Lo que importa: el status, porque es lo unico que mira un orquestador.
    CHECK(response.status == 503);

    const auto report = response.json<syrax::health::Report>();
    CHECK(report.status == "down");

    const auto* s3 = find(report, "s3");
    REQUIRE(s3 != nullptr);
    CHECK(s3->status == "down");
    CHECK(s3->detail == "no responde");

    // Y la base sigue sana: el informe distingue que se cayo de que no.
    const auto* database = find(report, "database");
    REQUIRE(database != nullptr);
    CHECK(database->status == "ok");

    syrax::health::reset();
}

TEST_CASE("una comprobacion que lanza es un down con motivo, no un 500", "[health]") {
    syrax::health::probe("explota", []() -> syrax::Task<std::string> {
        throw std::runtime_error("la conexion se corto");
        co_return "";
    });

    const auto response = api().get("/health");

    // Que una dependencia falle no puede tumbar el endpoint: un 500 sin cuerpo
    // le dice al orquestador que algo va mal pero no que.
    CHECK(response.status == 503);
    CHECK_THAT(response.body, ContainsSubstring("la conexion se corto"));

    syrax::health::reset();
}
