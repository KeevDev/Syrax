#include <catch2/catch_test_macros.hpp>

#include <syrax/validation.hpp>

#include <json/json.h>

#include <optional>
#include <string>

using namespace syrax;

// Los tipos que se reflejan NO pueden vivir en un namespace anonimo: Glaze
// toma su nombre por una variable `extern` y un tipo sin enlace no puede
// nombrarse desde otra unidad de traduccion. GCC lo deja pasar, clang lo
// rechaza. Por eso el namespace lleva nombre.
namespace valtest {

struct CreateUser {
    std::string name;
    std::string email;
    int         age;

    static auto rules() {
        return syrax::rules(
            field(&CreateUser::name).notEmpty().minLen(3).maxLen(50),
            field(&CreateUser::email).email(),
            field(&CreateUser::age).range(18, 120));
    }
};

struct Plain {
    std::string whatever;
};

struct WithOptional {
    std::optional<std::string> nickname;

    static auto rules() {
        return syrax::rules(field(&WithOptional::nickname).minLen(3));
    }
};

struct Roles {
    std::string role;

    static auto rules() {
        return syrax::rules(field(&Roles::role).oneOf({"admin", "editor", "viewer"}));
    }
};

std::optional<FieldError> find(const std::vector<FieldError>& errors, std::string_view field) {
    for (const auto& error : errors) {
        if (error.field == field) return error;
    }
    return std::nullopt;
}

}  // namespace valtest

using namespace valtest;

TEST_CASE("un cuerpo valido no produce errores", "[validation]") {
    CreateUser user{"Kevin", "kev@example.com", 30};
    CHECK(validate(user).empty());
}

TEST_CASE("los errores traen el nombre del campo", "[validation]") {
    CreateUser user{"ab", "no-arroba", 5};

    const auto errors = validate(user);

    // Los tres campos fallan, y se reportan todos, no solo el primero.
    CHECK(errors.size() == 3);
    CHECK(find(errors, "name").has_value());
    CHECK(find(errors, "email").has_value());
    CHECK(find(errors, "age").has_value());
}

TEST_CASE("un campo puede acumular varias reglas rotas", "[validation]") {
    CreateUser user{"", "kev@example.com", 30};

    const auto errors = validate(user);

    // Vacio rompe notEmpty Y minLen(3): las dos se reportan.
    CHECK(errors.size() == 2);
    CHECK(errors[0].field == "name");
    CHECK(errors[1].field == "name");
}

TEST_CASE("un tipo sin rules() se considera valido", "[validation]") {
    Plain plain{"cualquier cosa"};

    STATIC_CHECK(!Validatable<Plain>);
    CHECK(validate(plain).empty());
}

TEST_CASE("un opcional ausente no se valida", "[validation]") {
    WithOptional absent{std::nullopt};
    CHECK(validate(absent).empty());

    WithOptional present{"ab"};
    CHECK(validate(present).size() == 1);
}

TEST_CASE("email acepta y rechaza los casos de siempre", "[validation]") {
    const rule::Email check;

    CHECK_FALSE(check("kev@example.com").has_value());
    CHECK_FALSE(check("a.b+tag@sub.dominio.co").has_value());

    CHECK(check("sin-arroba.com").has_value());
    CHECK(check("@example.com").has_value());
    CHECK(check("dos@@example.com").has_value());
    CHECK(check("con espacio@example.com").has_value());
    CHECK(check("sin-punto@dominio").has_value());
    CHECK(check("termina@punto.").has_value());
}

TEST_CASE("oneOf limita a la lista", "[validation]") {
    Roles good{"admin"};
    CHECK(validate(good).empty());

    Roles bad{"root"};
    const auto errors = validate(bad);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message.find("admin") != std::string::npos);
}

TEST_CASE("notEmpty no se deja enganar por espacios", "[validation]") {
    const rule::NotEmpty check;

    CHECK(check("   ").has_value());
    CHECK(check("\t\n").has_value());
    CHECK_FALSE(check(" x ").has_value());
}

TEST_CASE("las reglas se escriben en el JSON Schema", "[validation]") {
    Json::Value schema;
    schema["properties"]["name"]["type"]  = "string";
    schema["properties"]["email"]["type"] = "string";
    schema["properties"]["age"]["type"]   = "integer";

    annotateSchema<CreateUser>(schema);

    CHECK(schema["properties"]["name"]["minLength"].asUInt64() == 3);
    CHECK(schema["properties"]["name"]["maxLength"].asUInt64() == 50);
    CHECK(schema["properties"]["email"]["format"].asString() == "email");
    CHECK(schema["properties"]["age"]["minimum"].asInt() == 18);
    CHECK(schema["properties"]["age"]["maximum"].asInt() == 120);
}

TEST_CASE("satisfies envuelve cualquier predicado", "[validation]") {
    const rule::Satisfies par{[](int v) { return v % 2 == 0; }, "debe ser par"};

    CHECK_FALSE(par(4).has_value());
    CHECK(par(5).value() == "debe ser par");
}
