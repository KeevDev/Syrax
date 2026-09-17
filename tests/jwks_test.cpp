// Verificar tokens de terceros contra un JWKS.
//
// La app de prueba hace de proveedor: publica su JWKS en /jwks.json y tiene
// /protegido detras del middleware. Los tokens se firman de verdad con la
// clave privada de testkeys, porque un token escrito a mano no probaria nada
// mas que el parser.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace {

struct Eco {
    int         hits = 0;
    std::string requestId;      // aqui viaja el sub del actor
    std::string authorization;  // y aqui su scope
};

std::int64_t ahora() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const std::string kCabecera =
    R"({"alg":"RS256","typ":"JWT","kid":")" + testkeys::kid() + R"("})";

std::string payload(const std::string& extra = "") {
    return R"({"sub":"usuario-7","iss":"https://emisor.de.prueba/","aud":"api-de-prueba",)"
           R"("scope":"pedidos:leer pedidos:escribir","role":"editor","exp":)" +
           std::to_string(ahora() + 3600) + (extra.empty() ? "" : "," + extra) + "}";
}

syrax::testing::Response protegido(const std::string& token) {
    // El puerto solo se conoce con el servidor ya arriba: por eso la URL del
    // JWKS se resuelve al usarla y no al cablear el middleware.
    testkeys::puerto() = api().port();

    api().withoutHeaders().header("Authorization", "Bearer " + token);
    const auto response = api().get("/protegido");
    api().withoutHeaders();
    return response;
}

}  // namespace

TEST_CASE("un token firmado por el proveedor entra", "[jwks]") {
    const auto response = protegido(testkeys::token(kCabecera, payload()));

    REQUIRE(response.status == 200);

    const auto eco = response.json<Eco>();
    CHECK(eco.requestId == "usuario-7");

    // Y el scope llega en el formato que ya entiende requireScope, sin
    // traducir nada: es el del claim de OAuth2.
    CHECK(eco.authorization == "pedidos:leer pedidos:escribir");
}

TEST_CASE("sin cabecera Authorization es 401", "[jwks]") {
    testkeys::puerto() = api().port();
    api().withoutHeaders();

    CHECK(api().get("/protegido").status == 401);
}

TEST_CASE("una firma que no cuadra no entra", "[jwks]") {
    // El mismo token, con la firma cambiada.
    const auto response = protegido(testkeys::token(kCabecera, payload(), /*firmar=*/false));

    CHECK(response.status == 401);
}

TEST_CASE("alg=none no cuela", "[jwks]") {
    // El agujero clasico: si el verificador obedeciera al algoritmo que declara
    // el propio token, bastaria con decir que no hay firma.
    const auto response = protegido(
        testkeys::token(R"({"alg":"none","typ":"JWT"})", payload(), /*firmar=*/false));

    CHECK(response.status == 401);
    CHECK(response.body.find("algorithm") != std::string::npos);
}

TEST_CASE("alg=HS256 tampoco, aunque venga firmado", "[jwks]") {
    // La otra mitad del mismo agujero: firmar con HS256 usando la clave
    // PUBLICA como secreto compartido. Se rechaza por el algoritmo, antes de
    // mirar la firma siquiera.
    const auto response =
        protegido(testkeys::token(R"({"alg":"HS256","typ":"JWT"})", payload()));

    CHECK(response.status == 401);
    CHECK(response.body.find("algorithm") != std::string::npos);
}

TEST_CASE("un kid que no existe no entra", "[jwks]") {
    const auto response = protegido(
        testkeys::token(R"({"alg":"RS256","typ":"JWT","kid":"inventado"})", payload()));

    CHECK(response.status == 401);
}

TEST_CASE("un token caducado no entra", "[jwks]") {
    const auto vencido =
        R"({"sub":"usuario-7","iss":"https://emisor.de.prueba/","aud":"api-de-prueba","exp":)" +
        std::to_string(ahora() - 10) + "}";

    CHECK(protegido(testkeys::token(kCabecera, vencido)).status == 401);
}

TEST_CASE("un token de otra audiencia no entra, aunque este bien firmado", "[jwks]") {
    // Es el caso que la firma NO detecta: un token perfectamente emitido por
    // el mismo proveedor, pero para otra aplicacion.
    const auto ajeno =
        R"({"sub":"usuario-7","iss":"https://emisor.de.prueba/","aud":"otra-api","exp":)" +
        std::to_string(ahora() + 3600) + "}";

    const auto response = protegido(testkeys::token(kCabecera, ajeno));

    CHECK(response.status == 401);
    CHECK(response.body.find("audience") != std::string::npos);
}

TEST_CASE("un emisor distinto no entra", "[jwks]") {
    const auto otro =
        R"({"sub":"usuario-7","iss":"https://otro.emisor/","aud":"api-de-prueba","exp":)" +
        std::to_string(ahora() + 3600) + "}";

    const auto response = protegido(testkeys::token(kCabecera, otro));

    CHECK(response.status == 401);
    CHECK(response.body.find("issuer") != std::string::npos);
}

TEST_CASE("un aud en array tambien vale", "[jwks]") {
    // La especificacion permite las dos formas y los proveedores usan las dos:
    // aceptar solo una deja fuera a la mitad sin decir por que.
    const auto enArray =
        R"({"sub":"usuario-7","iss":"https://emisor.de.prueba/",)"
        R"("aud":["otra-api","api-de-prueba"],"exp":)" +
        std::to_string(ahora() + 3600) + "}";

    CHECK(protegido(testkeys::token(kCabecera, enArray)).status == 200);
}

TEST_CASE("un token con basura en vez de JSON no revienta", "[jwks]") {
    CHECK(protegido("no.es.un.token").status == 401);
    CHECK(protegido("soloUnaParte").status == 401);
}

TEST_CASE("las claves se cachean: no se baja el JWKS en cada peticion", "[jwks]") {
    testkeys::puerto() = api().port();

    // La primera peticion puede tener que bajarlas; a partir de ahi, no.
    protegido(testkeys::token(kCabecera, payload()));

    const auto antes = api().get("/cuenta").json<Eco>().hits;
    for (int i = 0; i < 5; ++i) protegido(testkeys::token(kCabecera, payload()));
    const auto despues = api().get("/cuenta").json<Eco>().hits;

    // /jwks.json incrementa el contador cada vez que se sirve. Bajarlo en cada
    // peticion seria una llamada de red por peticion.
    CHECK(despues == antes);
}

