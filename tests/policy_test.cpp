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
