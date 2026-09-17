// La parte de criptografia del JWKS, sin red de por medio: armar una clave
// publica desde el modulo y el exponente del JWK, y verificar una firma RS256.

#include "testkeys.hpp"

#include <syrax/jwks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("el JWK trae modulo y exponente utilizables", "[jwkscrypto]") {
    CHECK_FALSE(testkeys::modulusB64().empty());
    CHECK(testkeys::exponentB64() == "AQAB");  // 65537, el de siempre
}

TEST_CASE("la clave publica se arma desde el JWK", "[jwkscrypto]") {
    auto key = syrax::auth::detail::rsaFrom(testkeys::modulusB64(), testkeys::exponentB64());
    REQUIRE(key != nullptr);
}

TEST_CASE("una firma RS256 se verifica con la clave del JWK", "[jwkscrypto]") {
    const std::string datos = "cabecera.payload";

    auto key = syrax::auth::detail::rsaFrom(testkeys::modulusB64(), testkeys::exponentB64());
    REQUIRE(key != nullptr);

    CHECK(syrax::auth::detail::verifyRs256(key.get(), datos, testkeys::signRs256(datos)));
}

TEST_CASE("una firma de otros datos no verifica", "[jwkscrypto]") {
    auto key = syrax::auth::detail::rsaFrom(testkeys::modulusB64(), testkeys::exponentB64());
    REQUIRE(key != nullptr);

    CHECK_FALSE(syrax::auth::detail::verifyRs256(key.get(), "otra.cosa",
                                                 testkeys::signRs256("cabecera.payload")));
}
