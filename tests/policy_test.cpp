#include <catch2/catch_test_macros.hpp>

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
