#include <catch2/catch_test_macros.hpp>

#include <syrax/auth.hpp>
#include <syrax/policy.hpp>

using namespace syrax;

TEST_CASE("un Actor sin id no esta autenticado", "[policy]") {
    CHECK_FALSE(Actor{}.authenticated());
    CHECK(Actor{.id = "1"}.authenticated());
}

TEST_CASE("allowIf deja pasar o corta con 403", "[policy]") {
    CHECK_FALSE(allowIf(true).has_value());

    const auto denied = allowIf(false, "no puedes");
    REQUIRE(denied.has_value());
    CHECK(denied->status == 403);
    CHECK(denied->message == "no puedes");
}

TEST_CASE("denyIf es el inverso", "[policy]") {
    CHECK(denyIf(true).has_value());
    CHECK_FALSE(denyIf(false).has_value());
}

TEST_CASE("requireRole distingue no autenticado de no autorizado", "[policy]") {
    // 401 y 403 no son lo mismo: uno dice 'identificate', el otro 'no puedes'.
    const auto anonimo = requireRole(Actor{}, "admin");
    REQUIRE(anonimo.has_value());
    CHECK(anonimo->status == 401);

    const auto sinPermiso = requireRole(Actor{.id = "1", .role = "user"}, "admin");
    REQUIRE(sinPermiso.has_value());
    CHECK(sinPermiso->status == 403);

    CHECK_FALSE(requireRole(Actor{.id = "1", .role = "admin"}, "admin").has_value());
}

TEST_CASE("requireRole acepta varios roles", "[policy]") {
    const Actor editor{.id = "1", .role = "editor"};

    CHECK_FALSE(requireRole(editor, "admin", "editor").has_value());
    CHECK(requireRole(editor, "admin", "owner").has_value());
}

TEST_CASE("actorFrom lee lo que dejo el middleware de bearer", "[policy]") {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->attributes()->insert("auth.sub", std::string{"42"});
    request->attributes()->insert("auth.role", std::string{"admin"});

    syrax::Request wrapped{request};
    const auto     actor = syrax::actorFrom(wrapped);

    CHECK(actor.id == "42");
    CHECK(actor.role == "admin");
    CHECK(actor.authenticated());
    CHECK(actor.is("admin"));
}

TEST_CASE("sin token, actorFrom da un actor no autenticado", "[policy]") {
    auto           request = drogon::HttpRequest::newHttpRequest();
    syrax::Request wrapped{request};

    // No lanza ni devuelve 401: que la ausencia sea un error lo decide la
    // politica, no el helper. Una ruta publica puede querer saber quien mira.
    const auto actor = syrax::actorFrom(wrapped);

    CHECK_FALSE(actor.authenticated());
    CHECK(syrax::requireRole(actor, "admin").has_value());
    CHECK(syrax::requireRole(actor, "admin")->status == 401);
}

// ---------------------------------------------------------------- scopes

TEST_CASE("can() compara permisos exactos dentro de la cadena", "[policy][scope]") {
    const syrax::Actor actor{.id = "7", .role = "user", .scope = "pedidos:leer pedidos:escribir"};

    CHECK(actor.can("pedidos:leer"));
    CHECK(actor.can("pedidos:escribir"));

    // Sin comodines, y sin coincidencias parciales: "pedidos" no es un permiso
    // que este ahi, aunque sea prefijo de dos que si.
    CHECK_FALSE(actor.can("pedidos"));
    CHECK_FALSE(actor.can("pedidos:borrar"));
    CHECK_FALSE(actor.can("pedidos:*"));
    CHECK_FALSE(actor.can(""));
}

TEST_CASE("can() acierta en el primero, el ultimo y el unico", "[policy][scope]") {
    // Los tres casos donde un recorrido por separadores se equivoca.
    CHECK(syrax::Actor{.id = "1", .scope = "a b c"}.can("a"));
    CHECK(syrax::Actor{.id = "1", .scope = "a b c"}.can("c"));
    CHECK(syrax::Actor{.id = "1", .scope = "solo"}.can("solo"));

    CHECK_FALSE(syrax::Actor{.id = "1", .scope = ""}.can("a"));
}

TEST_CASE("requireScope exige TODOS los permisos", "[policy][scope]") {
    const syrax::Actor actor{.id = "7", .scope = "pedidos:leer"};

    CHECK_FALSE(syrax::requireScope(actor, "pedidos:leer").has_value());

    // Tiene uno de los dos, y eso no basta: son requisitos, no alternativas.
    const auto denied = syrax::requireScope(actor, "pedidos:leer", "pedidos:escribir");
    REQUIRE(denied.has_value());
    CHECK(denied->status == 403);
}

TEST_CASE("requireAnyScope se conforma con uno", "[policy][scope]") {
    const syrax::Actor actor{.id = "7", .scope = "pedidos:leer"};

    CHECK_FALSE(syrax::requireAnyScope(actor, "pedidos:leer", "pedidos:escribir").has_value());
    CHECK(syrax::requireAnyScope(actor, "otro:leer", "otro:escribir").has_value());
}

TEST_CASE("sin token es 401, no 403", "[policy][scope]") {
    // La diferencia importa: un 403 le dice al cliente que no insista, y un
    // 401 que se autentique y vuelva.
    const syrax::Actor anonimo{.id = "", .scope = "pedidos:leer"};

    const auto denied = syrax::requireScope(anonimo, "pedidos:leer");
    REQUIRE(denied.has_value());
    CHECK(denied->status == 401);
}

TEST_CASE("el scope del token llega al actor", "[policy][scope]") {
    // Es el formato de OAuth2, asi que un token de un tercero ya viene asi.
    const syrax::auth::Claims claims{.sub = "7", .role = "user", .scope = "pedidos:leer"};
    const auto          token = syrax::auth::sign(claims, "secreto");

    const auto leido = syrax::auth::verify(token, "secreto");
    REQUIRE(leido.has_value());
    CHECK(leido->scope == "pedidos:leer");
}
