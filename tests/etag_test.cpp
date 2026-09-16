// ETag y 304. Se prueba con peticiones de verdad porque lo que importa es lo
// que ve el cliente: la cabecera que sale y el cuerpo que deja de salir.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("un GET de exito sale con su ETag", "[etag]") {
    api().fresh();

    const auto response = api().get("widgets");

    REQUIRE(response.status == 200);

    const auto tag = response.header("ETag");
    REQUIRE_FALSE(tag.empty());

    // Entre comillas, como pide la especificacion: sin ellas hay
    // intermediarios que lo descartan sin decir nada.
    CHECK(tag.front() == '"');
    CHECK(tag.back() == '"');
}

TEST_CASE("el mismo contenido da el mismo ETag", "[etag]") {
    api().fresh();

    const auto primera = api().get("widgets");
    const auto segunda = api().get("widgets");

    CHECK(primera.header("ETag") == segunda.header("ETag"));
}

TEST_CASE("si el contenido cambia, el ETag cambia", "[etag]") {
    api().fresh();
    const auto vacia = api().get("widgets").header("ETag");

    REQUIRE(api().post("widgets", R"({"name":"tuerca","size":4})").ok());
    const auto conUna = api().get("widgets").header("ETag");

    CHECK(vacia != conUna);
}

TEST_CASE("If-None-Match con el ETag actual devuelve 304 y cuerpo vacio", "[etag]") {
    api().fresh();

    const auto primera = api().get("widgets");
    REQUIRE(primera.status == 200);
    REQUIRE_FALSE(primera.body.empty());

    api().withoutHeaders().header("If-None-Match", primera.header("ETag"));
    const auto segunda = api().get("widgets");

    CHECK(segunda.status == 304);

    // Lo unico que ahorra un 304 es el cuerpo. Si viniera, la feature no
    // estaria haciendo nada.
    CHECK(segunda.body.empty());

    api().withoutHeaders();
}

TEST_CASE("If-None-Match con un ETag viejo devuelve el recurso entero", "[etag]") {
    api().fresh();
    const auto viejo = api().get("widgets").header("ETag");

    REQUIRE(api().post("widgets", R"({"name":"perno","size":9})").ok());

    api().withoutHeaders().header("If-None-Match", viejo);
    const auto response = api().get("widgets");

    CHECK(response.status == 200);
    CHECK_FALSE(response.body.empty());
    CHECK(response.header("ETag") != viejo);

    api().withoutHeaders();
}

TEST_CASE("el comodin tambien vale", "[etag]") {
    api().fresh();
    REQUIRE(api().get("widgets").status == 200);

    api().withoutHeaders().header("If-None-Match", "*");
    CHECK(api().get("widgets").status == 304);

    api().withoutHeaders();
}

TEST_CASE("un POST no lleva ETag", "[etag]") {
    api().fresh();

    // Un 201 no es cacheable, y un 304 a una escritura seria mentira.
    const auto creado = api().post("widgets", R"({"name":"arandela","size":2})");

    REQUIRE(creado.status == 201);
    CHECK(creado.header("ETag").empty());
}

TEST_CASE("una respuesta de error no lleva ETag", "[etag]") {
    api().fresh();

    const auto noEncontrado = api().get("widgets/9999");

    REQUIRE(noEncontrado.status == 404);
    CHECK(noEncontrado.header("ETag").empty());
}
