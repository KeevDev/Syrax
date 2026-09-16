#include <catch2/catch_test_macros.hpp>

#include <syrax/app.hpp>
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

// Glaze no refleja tipos sin enlace: el recurso de los tests va con nombre.
namespace oapi {
struct Recurso {
    std::int64_t id;
};
}  // namespace oapi
using oapi::Recurso;

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

TEST_CASE("los $defs de Glaze suben a components/schemas", "[openapi]") {
    // Tal cual lo emite Glaze para un struct con un int64_t: la referencia
    // apunta a la raiz del documento, no a la del esquema incrustado.
    const std::string schema =
        R"({"type":"array","items":{"type":"object","properties":{)"
        R"("id":{"$ref":"#/$defs/int64_t"},"name":{"type":"string"}}},)"
        R"("$defs":{"int64_t":{"type":"integer"}}})";

    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/users", .responseSchema = schema, .okStatus = 200}};

    const auto doc = parse(buildOpenApi(routes, "API", "1.0.0"));
    const auto& body =
        doc["paths"]["/users"]["get"]["responses"]["200"]["content"]["application/json"]["schema"];

    // Si esto se queda como estaba, Swagger UI corta con
    // 'Invalid object key "$defs"' al resolver la referencia.
    CHECK(body["items"]["properties"]["id"]["$ref"].asString() ==
          "#/components/schemas/int64_t");
    CHECK_FALSE(body.isMember("$defs"));

    REQUIRE(doc["components"]["schemas"].isMember("int64_t"));
    CHECK(doc["components"]["schemas"]["int64_t"]["type"].asString() == "integer");
}

TEST_CASE("dos rutas que usan el mismo tipo comparten una sola definicion", "[openapi]") {
    const std::string schema =
        R"({"type":"object","properties":{"id":{"$ref":"#/$defs/int64_t"}},)"
        R"("$defs":{"int64_t":{"type":"integer"}}})";

    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/users", .responseSchema = schema, .okStatus = 200},
        {.method = "get", .path = "/posts", .responseSchema = schema, .okStatus = 200}};

    const auto doc = parse(buildOpenApi(routes, "API", "1.0.0"));

    CHECK(doc["components"]["schemas"].getMemberNames().size() == 1);
    CHECK(doc["paths"]["/posts"]["get"]["responses"]["200"]["content"]["application/json"]
             ["schema"]["properties"]["id"]["$ref"]
                 .asString() == "#/components/schemas/int64_t");
}

TEST_CASE("sin $defs no aparece un components vacio", "[openapi]") {
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/health", .responseSchema = R"({"type":"object"})",
         .okStatus = 200}};

    const auto doc = parse(buildOpenApi(routes, "API", "1.0.0"));

    CHECK_FALSE(doc.isMember("components"));
}

TEST_CASE("el tipo del path param sale de la firma, no es string siempre", "[openapi]") {
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/users/{id}", .responseSchema = R"({"type":"object"})",
         .okStatus = 200, .paramTypes = {"integer"}},
        {.method = "get", .path = "/users/{slug}", .responseSchema = R"({"type":"object"})",
         .okStatus = 200, .paramTypes = {"string"}},
    };

    const auto doc = parse(buildOpenApi(routes, "API", "1.0.0"));

    CHECK(doc["paths"]["/users/{id}"]["get"]["parameters"][0]["schema"]["type"].asString() ==
          "integer");
    CHECK(doc["paths"]["/users/{slug}"]["get"]["parameters"][0]["schema"]["type"].asString() ==
          "string");
}

TEST_CASE("si el handler declara menos params que la ruta, no se inventa el tipo", "[openapi]") {
    const std::vector<RouteInfo> routes{
        {.method = "get", .path = "/a/{uno}/b/{dos}", .responseSchema = R"({"type":"object"})",
         .okStatus = 200, .paramTypes = {"integer"}}};

    const auto doc = parse(buildOpenApi(routes, "API", "1.0.0"));
    const auto& params = doc["paths"]["/a/{uno}/b/{dos}"]["get"]["parameters"];

    REQUIRE(params.size() == 2);
    CHECK(params[0]["schema"]["type"].asString() == "integer");
    CHECK(params[1]["schema"]["type"].asString() == "string");
}

// Lo de arriba comprueba el documento a partir de RouteInfo. Esto comprueba
// lo otro: que el tipo del handler llegue hasta ahi.
TEST_CASE("App documenta el path param con el tipo que pide el handler", "[openapi]") {
    syrax::App app;
    app.quiet();

    app.get("/users/{id}", [](std::int64_t id) -> syrax::Task<syrax::Result<Recurso>> {
        co_return Recurso{.id = id};
    });
    app.get("/posts/{slug}", [](std::string slug) -> syrax::Task<syrax::Result<Recurso>> {
        co_return Recurso{.id = static_cast<std::int64_t>(slug.size())};
    });

    const auto doc = parse(app.openApi());

    CHECK(doc["paths"]["/users/{id}"]["get"]["parameters"][0]["schema"]["type"].asString() ==
          "integer");
    CHECK(doc["paths"]["/posts/{slug}"]["get"]["parameters"][0]["schema"]["type"].asString() ==
          "string");
}

TEST_CASE("un grupo prefija las rutas que registra", "[app]") {
    syrax::App app;
    app.quiet();

    auto api = app.group("/api/v1");
    api.get("/users", []() -> syrax::Task<syrax::Result<Recurso>> {
        co_return Recurso{.id = 1};
    });
    api.get("/users/{id}", [](std::int64_t id) -> syrax::Task<syrax::Result<Recurso>> {
        co_return Recurso{.id = id};
    });

    const auto doc = parse(app.openApi());

    CHECK(doc["paths"].isMember("/api/v1/users"));
    CHECK(doc["paths"].isMember("/api/v1/users/{id}"));
    CHECK_FALSE(doc["paths"].isMember("/users"));
}

TEST_CASE("los grupos se anidan", "[app]") {
    syrax::App app;
    app.quiet();

    auto admin = app.group("/api/v1").group("/admin");
    admin.get("/stats", []() -> syrax::Task<syrax::Result<Recurso>> {
        co_return Recurso{.id = 1};
    });

    CHECK(parse(app.openApi())["paths"].isMember("/api/v1/admin/stats"));
}

TEST_CASE("un alias construye la URL sin repetir la ruta", "[app]") {
    syrax::App app;
    app.quiet();

    auto api = app.group("/api/v1");
    api.get("/posts", []() -> syrax::Task<syrax::Result<Recurso>> {
        co_return Recurso{.id = 1};
    }).as("posts.index");
    api.get("/posts/{id}/comentarios/{cid}",
            [](std::int64_t id, std::int64_t cid) -> syrax::Task<syrax::Result<Recurso>> {
                co_return Recurso{.id = id + cid};
            })
        .as("posts.comentarios");

    CHECK(syrax::urlFor("posts.index") == "/api/v1/posts");
    CHECK(syrax::urlFor("posts.comentarios", 7, 42) == "/api/v1/posts/7/comentarios/42");

    // Un alias que nadie registro no inventa una URL que no existe.
    CHECK(syrax::urlFor("no.existe", 1).empty());
}
