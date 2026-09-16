// Idempotency-Key. Se prueba con peticiones de verdad contra Redis de verdad
// porque lo que importa es el efecto observable: que el segundo POST no cree
// una segunda fila.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace {

// Una clave distinta por caso: el almacen vive en Redis y sobrevive al
// proceso, asi que reusar una haria fallar la segunda ejecucion de la suite.
std::string clave(const std::string& prefijo) {
    return prefijo + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

syrax::testing::Response crear(const std::string& key, const std::string& body) {
    api().withoutHeaders().header("Idempotency-Key", key);
    return api().post("widgets", body);
}

int filas() {
    return static_cast<int>(api().db()->execSqlSync("SELECT id FROM widgets").size());
}

}  // namespace

TEST_CASE("el reintento no crea una segunda fila", "[idempotency]") {
    if (!bootstrap::redisReachable()) {
        WARN("sin Redis, se salta");
        return;
    }
    api().fresh();

    const auto key  = clave("reintento");
    const auto body = R"({"name":"tuerca","size":4})";

    const auto primera = crear(key, body);
    REQUIRE(primera.status == 201);
    REQUIRE(filas() == 1);

    // Lo mismo que haria un movil que cambio de wifi a datos: reintentar.
    const auto segunda = crear(key, body);

    CHECK(segunda.status == 201);
    CHECK(segunda.body == primera.body);

    // Lo unico que de verdad importa: sigue habiendo UNA fila.
    CHECK(filas() == 1);

    api().withoutHeaders();
}

TEST_CASE("la respuesta repetida se marca como tal", "[idempotency]") {
    if (!bootstrap::redisReachable()) {
        WARN("sin Redis, se salta");
        return;
    }
    api().fresh();

    const auto key  = clave("marca");
    const auto body = R"({"name":"perno","size":9})";

    CHECK(crear(key, body).header("Idempotent-Replay").empty());
    CHECK(crear(key, body).header("Idempotent-Replay") == "true");

    api().withoutHeaders();
}

TEST_CASE("la misma clave con otro cuerpo es un error, no un replay", "[idempotency]") {
    if (!bootstrap::redisReachable()) {
        WARN("sin Redis, se salta");
        return;
    }
    api().fresh();

    const auto key = clave("distinto");

    REQUIRE(crear(key, R"({"name":"uno","size":1})").status == 201);

    // Devolverle la respuesta del primero seria peor que fallar: el cliente se
    // quedaria creyendo que creo el segundo.
    const auto otro = crear(key, R"({"name":"dos","size":2})");

    CHECK(otro.status == 422);
    CHECK_THAT(otro.body, ContainsSubstring("idempotency_key_reused"));
    CHECK(filas() == 1);

    api().withoutHeaders();
}

TEST_CASE("el error de la clave reusada no envenena la clave", "[idempotency]") {
    if (!bootstrap::redisReachable()) {
        WARN("sin Redis, se salta");
        return;
    }
    api().fresh();

    const auto key  = clave("veneno");
    const auto body = R"({"name":"original","size":3})";

    REQUIRE(crear(key, body).status == 201);
    REQUIRE(crear(key, R"({"name":"otro","size":7})").status == 422);

    // El 422 es una respuesta SOBRE la clave, no el resultado de la operacion.
    // Si se hubiera guardado, el reintento legitimo recibiria ese 422.
    const auto reintento = crear(key, body);
    CHECK(reintento.status == 201);
    CHECK(reintento.header("Idempotent-Replay") == "true");

    api().withoutHeaders();
}

TEST_CASE("sin la cabecera todo sigue como antes", "[idempotency]") {
    api().fresh();
    api().withoutHeaders();

    // Dos POST distintos sin clave crean dos filas: la proteccion es opcional
    // y no cambia el comportamiento de quien no la pide.
    REQUIRE(api().post("widgets", R"({"name":"alfa","size":1})").status == 201);
    REQUIRE(api().post("widgets", R"({"name":"beta","size":2})").status == 201);

    CHECK(filas() == 2);
}

TEST_CASE("un GET no pasa por el mecanismo", "[idempotency]") {
    if (!bootstrap::redisReachable()) {
        WARN("sin Redis, se salta");
        return;
    }
    api().fresh();

    // Un GET ya es idempotente por definicion; guardarle la respuesta seria un
    // cache, que es otra cosa y con otras reglas de invalidacion.
    api().withoutHeaders().header("Idempotency-Key", clave("lectura"));

    CHECK(api().get("widgets").status == 200);
    CHECK(api().get("widgets").header("Idempotent-Replay").empty());

    api().withoutHeaders();
}

TEST_CASE("un error del dominio se guarda: repetirlo da el mismo error", "[idempotency]") {
    if (!bootstrap::redisReachable()) {
        WARN("sin Redis, se salta");
        return;
    }
    api().fresh();

    REQUIRE(api().post("widgets", R"({"name":"unica","size":1})").status == 201);

    const auto key  = clave("conflicto");
    const auto body = R"({"name":"unica","size":1})";

    const auto primera = crear(key, body);
    REQUIRE(primera.status == 409);

    // Un 409 de negocio SI es el resultado de la operacion, asi que se
    // recuerda: el reintento tiene que ver lo mismo que vio el original.
    const auto segunda = crear(key, body);
    CHECK(segunda.status == 409);
    CHECK(segunda.header("Idempotent-Replay") == "true");

    api().withoutHeaders();
}
