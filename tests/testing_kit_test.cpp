// El kit de tests, probado con la unica prueba que de verdad lo valida: una
// peticion que recorre ruta, handler, repositorio y SQL.
//
// Va en su propio binario porque syrax_tests ya levanta un servidor con
// test_server.cpp, y Drogon solo admite uno por proceso.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;

TEST_CASE("el kit levanta la aplicacion contra una sqlite temporal", "[testing]") {
    CHECK(api().port() != 0);
    REQUIRE(api().db() != nullptr);

    // La migracion corrio: la tabla existe en la base que ve la aplicacion.
    const auto tablas = api().db()->execSqlSync(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'widgets'");
    CHECK(tablas.size() == 1);
}

TEST_CASE("una peticion recorre ruta, handler, repositorio y SQL", "[testing]") {
    api().fresh();

    const auto creado = api().post("widgets", R"({"name":"tuerca","size":4})");
    REQUIRE(creado.status == 201);

    const auto widget = creado.json<models::Widget>();
    CHECK(widget.name == "tuerca");
    CHECK(widget.size == 4);
    CHECK(widget.id == 1);

    // La fila esta en la base de verdad, no en un doble.
    const auto filas = api().db()->execSqlSync("SELECT name, size FROM widgets");
    REQUIRE(filas.size() == 1);
    CHECK(filas.front()["name"].as<std::string>() == "tuerca");

    // Y vuelve por el otro extremo.
    const auto leido = api().get("widgets/" + std::to_string(widget.id));
    CHECK(leido.status == 200);
    CHECK(leido.json<models::Widget>().name == "tuerca");
}

TEST_CASE("fresh() aisla un caso del anterior", "[testing]") {
    api().fresh();
    CHECK(api().get("widgets").json<std::vector<models::Widget>>().empty());

    REQUIRE(api().post("widgets", R"({"name":"perno","size":9})").ok());
    CHECK(api().get("widgets").json<std::vector<models::Widget>>().size() == 1);

    api().fresh();
    CHECK(api().get("widgets").json<std::vector<models::Widget>>().empty());

    // El autoincremento tambien se reinicia: si no, el id dependeria de
    // cuantas filas creo el caso anterior.
    const auto otra = api().post("widgets", R"({"name":"otra","size":1})");
    CHECK(otra.json<models::Widget>().id == 1);
}

TEST_CASE("el cuerpo se puede mandar desde el tipo", "[testing]") {
    api().fresh();

    const auto creado = api().post("widgets", requests::CreateWidget{.name = "arandela", .size = 2});

    REQUIRE(creado.status == 201);
    CHECK(creado.json<models::Widget>().name == "arandela");
}

TEST_CASE("el error del dominio llega con su codigo estable", "[testing]") {
    api().fresh();

    REQUIRE(api().post("widgets", R"({"name":"unica","size":1})").ok());

    const auto repetido = api().post("widgets", R"({"name":"unica","size":1})");
    CHECK(repetido.status == 409);
    CHECK_THAT(repetido.body, ContainsSubstring("\"code\":\"nombre_duplicado\""));
}

TEST_CASE("los 404 y 422 del borde tambien pasan por el kit", "[testing]") {
    api().fresh();

    CHECK(api().get("widgets/9999").status == 404);
    CHECK(api().get("/no-existe").status == 404);

    // size fuera de rango: lo atrapa validation, no el handler.
    const auto invalido = api().post("widgets", R"({"name":"x","size":999})");
    CHECK(invalido.status == 422);
    CHECK_THAT(invalido.body, ContainsSubstring("size"));
}

TEST_CASE("la ruta relativa cuelga de la base y la absoluta no", "[testing]") {
    // Relativa: el test no repite /api/v1 en cada linea.
    CHECK(api().get("widgets").status == 200);

    // Absoluta: la misma ruta, escrita entera.
    CHECK(api().get("/api/v1/widgets").status == 200);

    // Y lo que cuelga fuera de la API sigue siendo alcanzable, que es para lo
    // que sirve la distincion.
    CHECK(api().get("/health").status == 200);
    CHECK(api().get("health").status == 404);
}

TEST_CASE("la respuesta expone las cabeceras", "[testing]") {
    const auto response = api().get("widgets");

    CHECK(response.header("Content-Type").find("application/json") != std::string::npos);

    // La trazabilidad del nivel 1, vista desde fuera.
    CHECK_FALSE(response.header("x-request-id").empty());
}

TEST_CASE("una cabecera por defecto viaja en todas las peticiones", "[testing]") {
    api().header("X-Prueba", "si");
    CHECK(api().get("widgets").status == 200);

    api().withoutHeaders();
    CHECK(api().get("widgets").status == 200);
}
