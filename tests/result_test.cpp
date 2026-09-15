#include <catch2/catch_test_macros.hpp>

#include <syrax/result.hpp>

#include <string>

using syrax::Conflict;
using syrax::NotFound;
using syrax::Result;

namespace {
struct User {
    int         id;
    std::string name;
};
}  // namespace

TEST_CASE("Result lleva el valor cuando todo sale bien", "[result]") {
    Result<User> result = User{.id = 7, .name = "kevin"};

    REQUIRE(result.ok());
    CHECK(result.value().id == 7);
    CHECK(result.value().name == "kevin");
}

TEST_CASE("Result lleva el error cuando algo falla", "[result]") {
    Result<User> result = NotFound("user not found");

    REQUIRE_FALSE(result.ok());
    CHECK(result.error().status == 404);
    CHECK(result.error().message == "user not found");
}

TEST_CASE("los constructores implicitos permiten devolver sin ceremonia", "[result]") {
    // Este es el punto de tener constructores implicitos: que un handler
    // pueda hacer `return user;` o `return Conflict(...)` sin envolver nada.
    const auto handler = [](bool fail) -> Result<User> {
        if (fail) return Conflict("email already registered");
        return User{.id = 1, .name = "ada"};
    };

    CHECK(handler(false).ok());
    CHECK(handler(true).error().status == 409);
}

TEST_CASE("cada helper mapea a su codigo HTTP", "[result]") {
    CHECK(syrax::BadRequest("x").status == 400);
    CHECK(syrax::Unauthorized("x").status == 401);
    CHECK(syrax::Forbidden("x").status == 403);
    CHECK(syrax::NotFound("x").status == 404);
    CHECK(syrax::Conflict("x").status == 409);
    CHECK(syrax::Unprocessable("x").status == 422);
    CHECK(syrax::Internal("x").status == 500);
}
