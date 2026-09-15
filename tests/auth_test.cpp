#include <catch2/catch_test_macros.hpp>

#include <syrax/auth.hpp>

#include <chrono>
#include <string>

using namespace syrax;
using namespace syrax::auth;

TEST_CASE("un token firmado se verifica con el mismo secreto", "[auth][jwt]") {
    const auto token = sign(Claims{.sub = "user-42", .role = "admin"}, "secreto");

    const auto claims = verify(token, "secreto");
    REQUIRE(claims.has_value());
    CHECK(claims->sub == "user-42");
    CHECK(claims->role == "admin");
    CHECK(claims->exp > claims->iat);
}

TEST_CASE("un secreto distinto no verifica", "[auth][jwt]") {
    const auto token = sign(Claims{.sub = "user-42"}, "secreto");
    CHECK_FALSE(verify(token, "otro-secreto").has_value());
}

TEST_CASE("un token manipulado no verifica", "[auth][jwt]") {
    // Cambiar el payload invalida la firma: es el punto de firmar.
    auto token = sign(Claims{.sub = "user-42", .role = "user"}, "secreto");

    const auto firstDot  = token.find('.');
    const auto secondDot = token.find('.', firstDot + 1);

    const auto forjado = token.substr(0, firstDot + 1) +
                         auth::detail::base64UrlEncode(R"({"sub":"user-42","role":"admin"})") +
                         token.substr(secondDot);

    CHECK_FALSE(verify(forjado, "secreto").has_value());
}

TEST_CASE("un token expirado no verifica", "[auth][jwt]") {
    using namespace std::chrono_literals;

    const auto token = sign(Claims{.sub = "user-42"}, "secreto", -1h);
    CHECK_FALSE(verify(token, "secreto").has_value());
}

TEST_CASE("tokens malformados no revientan ni pasan", "[auth][jwt]") {
    for (const auto* bad : {"", "sin-puntos", "solo.dos", "a.b.c", "....", "a.b.c.d"}) {
        CHECK_FALSE(verify(bad, "secreto").has_value());
    }
}

TEST_CASE("el algoritmo declarado no decide la verificacion", "[auth][jwt]") {
    // Ataque clasico: cambiar alg a "none" y quitar la firma. Como siempre
    // recalculamos HMAC y comparamos, no cuela.
    const auto header  = auth::detail::base64UrlEncode(R"({"alg":"none","typ":"JWT"})");
    const auto payload = auth::detail::base64UrlEncode(R"({"sub":"admin","exp":99999999999})");

    CHECK_FALSE(verify(header + "." + payload + ".", "secreto").has_value());
}

TEST_CASE("una contrasena se verifica contra su hash", "[auth][password]") {
    // Pocas iteraciones para que el test sea rapido; el default es 600000.
    const auto stored = hashPassword("correcta", 1000);

    CHECK(verifyPassword("correcta", stored));
    CHECK_FALSE(verifyPassword("incorrecta", stored));
}

TEST_CASE("la misma contrasena produce hashes distintos", "[auth][password]") {
    // Sal aleatoria: sin ella, dos usuarios con la misma clave son
    // identificables y las rainbow tables funcionan.
    const auto a = hashPassword("misma", 1000);
    const auto b = hashPassword("misma", 1000);

    CHECK(a != b);
    CHECK(verifyPassword("misma", a));
    CHECK(verifyPassword("misma", b));
}

TEST_CASE("un hash corrupto no verifica ni revienta", "[auth][password]") {
    for (const auto* bad : {"", "no-es-un-hash", "pbkdf2$", "pbkdf2$1000$sal", "otro$1$2$3"}) {
        CHECK_FALSE(verifyPassword("x", bad));
    }
}

TEST_CASE("el formato del hash guarda las iteraciones", "[auth][password]") {
    // Asi se puede subir el costo sin invalidar los hashes viejos.
    const auto stored = hashPassword("x", 1234);
    CHECK(stored.starts_with("pbkdf2$1234$"));
}
