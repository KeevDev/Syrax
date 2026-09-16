#include <catch2/catch_test_macros.hpp>

#include <syrax/errors.hpp>

#include <json/json.h>

#include <stdexcept>
#include <string>

using namespace syrax;

namespace {

Json::Value bodyOf(const drogon::HttpResponsePtr& response) {
    Json::Value  parsed;
    Json::Reader reader;
    REQUIRE(reader.parse(std::string{response->body()}, parsed));
    return parsed;
}

struct SaldoCorto : std::runtime_error {
    SaldoCorto() : std::runtime_error("faltan 3 euros") {}
};

}  // namespace

TEST_CASE("un error sin extras sale como siempre") {
    const auto body = bodyOf(render(NotFound("no existe")));

    CHECK(body["error"]["status"].asInt() == 404);
    CHECK(body["error"]["message"].asString() == "no existe");

    // Los campos nuevos no aparecen si nadie los llena: un cliente que hoy
    // lee error.message no ve nada distinto manana.
    CHECK_FALSE(body["error"].isMember("code"));
    CHECK_FALSE(body["error"].isMember("detail"));
    CHECK_FALSE(body["error"].isMember("fields"));
}

TEST_CASE("as() y explain() no pisan el resto del error") {
    const auto error = Conflict("el email ya existe").as("email_duplicado").explain("prueba con otro");

    CHECK(error.status == 409);
    CHECK(error.message == "el email ya existe");
    CHECK(error.code == "email_duplicado");
    CHECK(error.detail == "prueba con otro");

    const auto body = bodyOf(render(error));
    CHECK(body["error"]["code"].asString() == "email_duplicado");
    CHECK(body["error"]["detail"].asString() == "prueba con otro");
}

TEST_CASE("el detalle por campo viaja dentro del error") {
    const Error invalido{.status  = 422,
                         .message = "validation failed",
                         .fields  = {{"email", "no es un email"}, {"edad", "minimo 18"}}};

    const auto body = bodyOf(render(invalido));

    REQUIRE(body["error"]["fields"].size() == 2);
    CHECK(body["error"]["fields"][0]["field"].asString() == "email");
    CHECK(body["error"]["fields"][1]["message"].asString() == "minimo 18");
}

TEST_CASE("onError sustituye el formato de todos los errores") {
    onError([](const Error& error) {
        Json::Value body;
        body["ok"]      = false;
        body["code"]    = error.code.empty() ? std::to_string(error.status) : error.code;
        body["message"] = error.message;
        return jsonResponse(body, error.status);
    });

    const auto simple = bodyOf(render(Forbidden("no puedes")));
    CHECK(simple["ok"].asBool() == false);
    CHECK(simple["code"].asString() == "403");
    CHECK_FALSE(simple.isMember("error"));

    // El 422 de validacion pasa por el mismo gancho. Si no, un proyecto que
    // cambia el formato acaba con dos formas de error en la misma API.
    const Error invalido{.status = 422, .message = "validation failed",
                         .fields = {{"email", "no es un email"}}};
    const auto validacion = bodyOf(render(invalido));
    CHECK(validacion["ok"].asBool() == false);
    CHECK_FALSE(validacion.isMember("error"));
}

TEST_CASE("un gancho que devuelve nullptr no deja al cliente sin respuesta") {
    onError([](const Error&) -> drogon::HttpResponsePtr { return nullptr; });

    const auto response = render(BadRequest("mal"));
    REQUIRE(response != nullptr);
    CHECK(bodyOf(response)["error"]["status"].asInt() == 400);
}

TEST_CASE("el codigo de estado del gancho llega a la respuesta") {
    CHECK(render(Unauthorized("token")).get()->statusCode() == drogon::k401Unauthorized);
}

TEST_CASE("sin traductores, una excepcion sigue siendo un 500 generico") {
    const auto error = errorFrom(std::runtime_error("no such table: users"));

    CHECK(error.status == 500);
    // El what() no viaja al cliente: describe el esquema a quien provoque el fallo.
    CHECK(error.message == "internal server error");
    CHECK(error.message.find("users") == std::string::npos);
}

TEST_CASE("onException traduce lo que reconoce") {
    onException([](const std::exception& thrown) -> std::optional<Error> {
        if (dynamic_cast<const SaldoCorto*>(&thrown))
            return Conflict("saldo insuficiente").as("saldo_insuficiente");
        return std::nullopt;
    });

    const auto traducido = errorFrom(SaldoCorto{});
    CHECK(traducido.status == 409);
    CHECK(traducido.code == "saldo_insuficiente");

    // Lo que no reconoce se queda como estaba, sin que el proyecto tenga que
    // reimplementar el caso por defecto.
    CHECK(errorFrom(std::runtime_error("otra cosa")).status == 500);
}

TEST_CASE("gana el primer traductor que entiende la excepcion") {
    onException([](const std::exception&) -> std::optional<Error> {
        return NotFound("el primero");
    });
    onException([](const std::exception&) -> std::optional<Error> {
        return Conflict("el segundo");
    });

    CHECK(errorFrom(std::runtime_error("x")).message == "el primero");
}

TEST_CASE("la cadena de respuesta se aplica tambien a los errores") {
    detail::responseChain().push_back([](const drogon::HttpResponsePtr& response) {
        response->addHeader("X-Probe", "si");
    });

    CHECK(render(NotFound("x"))->getHeader("X-Probe") == "si");
}
