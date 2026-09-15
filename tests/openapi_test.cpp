#include <catch2/catch_test_macros.hpp>

#include <syrax/openapi.hpp>

#include <json/json.h>

#include <string>

using syrax::buildOpenApi;
using syrax::RouteInfo;

namespace {

Json::Value parse(const std::string& text) {
    Json::Value  doc;
    Json::Reader reader;
    REQUIRE(reader.parse(text, doc));
    return doc;
}

}  // namespace

TEST_CASE("el documento declara openapi 3.1 y el titulo", "[openapi]") {
    const auto doc = parse(buildOpenApi({}, "Mi API", "2.0.0"));

    CHECK(doc["openapi"].asString() == "3.1.0");
    CHECK(doc["info"]["title"].asString() == "Mi API");
    CHECK(doc["info"]["version"].asString() == "2.0.0");
}

TEST_CASE("cada ruta aparece bajo su path y metodo", "[openapi]") {
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/users", .responseSchema = R"({"type":"array"})", .okStatus = 200},
        {.method = "post", .path = "/users", .requestSchema = R"({"type":"object"})",
         .responseSchema = R"({"type":"object"})", .okStatus = 201},
    };

    const auto doc = parse(buildOpenApi(routes, "API", "1.0.0"));

    REQUIRE(doc["paths"].isMember("/users"));
    CHECK(doc["paths"]["/users"].isMember("get"));
    CHECK(doc["paths"]["/users"].isMember("post"));
    CHECK(doc["paths"]["/users"]["post"]["responses"].isMember("201"));
    CHECK(doc["paths"]["/users"]["get"]["responses"].isMember("200"));
}

TEST_CASE("solo se declara requestBody si el handler recibe uno", "[openapi]") {
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/users", .responseSchema = "{}", .okStatus = 200},
        {.method = "post", .path = "/users", .requestSchema = R"({"type":"object"})",
         .responseSchema = "{}", .okStatus = 201},
    };

    const auto doc = parse(buildOpenApi(routes, "API", "1.0.0"));

    CHECK_FALSE(doc["paths"]["/users"]["get"].isMember("requestBody"));
    REQUIRE(doc["paths"]["/users"]["post"].isMember("requestBody"));
    CHECK(doc["paths"]["/users"]["post"]["requestBody"]["required"].asBool());
}

TEST_CASE("los {params} del path se declaran como parametros", "[openapi]") {
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/orders/{orderId}/items/{itemId}",
         .responseSchema = "{}", .okStatus = 200},
    };

    // El documento tiene que vivir en una variable con nombre: atar una
    // referencia a un subobjeto de un temporal NO extiende su vida cuando se
    // pasa por operator[], y queda colgando.
    const auto  doc    = parse(buildOpenApi(routes, "API", "1.0.0"));
    const auto& params = doc["paths"]["/orders/{orderId}/items/{itemId}"]["get"]["parameters"];

    REQUIRE(params.size() == 2);
    CHECK(params[0]["name"].asString() == "orderId");
    CHECK(params[1]["name"].asString() == "itemId");
    CHECK(params[0]["in"].asString() == "path");
    CHECK(params[0]["required"].asBool());
}

TEST_CASE("una ruta sin params no declara parameters", "[openapi]") {
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/health", .responseSchema = "{}", .okStatus = 200}};

    CHECK_FALSE(parse(buildOpenApi(routes, "API", "1.0.0"))["paths"]["/health"]["get"]
                    .isMember("parameters"));
}

TEST_CASE("toda ruta documenta el contrato de error uniforme", "[openapi]") {
    // Cualquier handler puede devolver un Error, asi que los codigos de fallo
    // se documentan solos y con la misma forma.
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/users", .responseSchema = "{}", .okStatus = 200}};

    const auto  doc       = parse(buildOpenApi(routes, "API", "1.0.0"));
    const auto& responses = doc["paths"]["/users"]["get"]["responses"];

    for (const char* code : {"400", "404", "409", "422", "500"}) {
        REQUIRE(responses.isMember(code));
    }

    const auto& schema = responses["404"]["content"]["application/json"]["schema"];
    CHECK(schema["properties"]["error"]["properties"].isMember("status"));
    CHECK(schema["properties"]["error"]["properties"].isMember("message"));
}

TEST_CASE("un esquema invalido no rompe el documento", "[openapi]") {
    // Si Glaze fallara al generar un esquema, la ruta debe seguir apareciendo
    // aunque sin detalle, en vez de tumbar toda la documentacion.
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/users", .responseSchema = "no es json", .okStatus = 200}};

    const auto doc = parse(buildOpenApi(routes, "API", "1.0.0"));
    CHECK(doc["paths"]["/users"]["get"]["responses"].isMember("200"));
}
