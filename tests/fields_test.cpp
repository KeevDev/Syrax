// Serializacion parcial: ?fields=id,name.
//
// La mitad de arriba prueba el filtro sobre JSON suelto, que es donde viven
// las decisiones; la de abajo, una peticion de verdad.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;

namespace {

std::string filtrar(const std::string& body, const std::string& pedidos) {
    bool       desconocidos = false;
    const auto out = syrax::fields::apply(body, syrax::fields::parse(pedidos), desconocidos);
    return out.value_or("");
}

}  // namespace

TEST_CASE("parse aguanta los espacios de una URL escrita a mano", "[fields]") {
    CHECK(syrax::fields::parse("id,name") == std::vector<std::string>{"id", "name"});
    CHECK(syrax::fields::parse("id, name , age") == std::vector<std::string>{"id", "name", "age"});
    CHECK(syrax::fields::parse("id,,name") == std::vector<std::string>{"id", "name"});
    CHECK(syrax::fields::parse("").empty());
    CHECK(syrax::fields::parse("  ").empty());
}

TEST_CASE("un objeto se queda con las claves pedidas", "[fields]") {
    const auto out = filtrar(R"({"id":1,"name":"Ada","email":"a@x.com","age":36})", "id,name");

    CHECK_THAT(out, ContainsSubstring("\"id\":1"));
    CHECK_THAT(out, ContainsSubstring("\"name\":\"Ada\""));
    CHECK_THAT(out, !ContainsSubstring("email"));
    CHECK_THAT(out, !ContainsSubstring("age"));
}

TEST_CASE("el orden es el que pidio el cliente", "[fields]") {
    const auto out = filtrar(R"({"id":1,"name":"Ada","age":36})", "age,id");

    // Mas util que el orden del struct, y gratis al construir el objeto nuevo.
    CHECK(out.find("age") < out.find("id"));
}

TEST_CASE("en un array se filtra cada elemento", "[fields]") {
    const auto out = filtrar(R"([{"id":1,"name":"Ada","age":36},{"id":2,"name":"Alan","age":41}])",
                             "name");

    CHECK_THAT(out, ContainsSubstring("Ada"));
    CHECK_THAT(out, ContainsSubstring("Alan"));
    CHECK_THAT(out, !ContainsSubstring("age"));
    CHECK_THAT(out, !ContainsSubstring("\"id\""));
}

TEST_CASE("el sobre de una pagina no se filtra, su data si", "[fields]") {
    const auto out = filtrar(
        R"({"data":[{"id":1,"name":"Ada","age":36}],"total":1,"page":1,"perPage":15,"pages":1,"hasMore":false})",
        "name");

    // Sin esto, pedir dos campos dejaria al cliente sin el total y sin forma
    // de pedir la pagina siguiente.
    CHECK_THAT(out, ContainsSubstring("\"total\":1"));
    CHECK_THAT(out, ContainsSubstring("\"hasMore\""));
    CHECK_THAT(out, ContainsSubstring("Ada"));

    // "age" a secas aparece dentro de "page" y "perPage" del sobre, asi que
    // lo que se comprueba es el campo, no la subcadena.
    CHECK_THAT(out, !ContainsSubstring("\"age\":36"));
}

TEST_CASE("un campo que no existe se ignora si hay otro que si", "[fields]") {
    const auto out = filtrar(R"({"id":1,"name":"Ada"})", "id,inventado");

    CHECK_THAT(out, ContainsSubstring("\"id\":1"));
    CHECK_THAT(out, !ContainsSubstring("inventado"));
}

TEST_CASE("si ninguno existe se avisa, en vez de devolver objetos vacios", "[fields]") {
    bool desconocidos = false;
    const auto out =
        syrax::fields::apply(R"({"id":1,"name":"Ada"})", syrax::fields::parse("nombre"), desconocidos);

    // Un cliente que recibe {} no tiene forma de saber que escribio mal el
    // campo. Esta es la diferencia entre ignorar y esconder.
    CHECK(desconocidos);
    CHECK_FALSE(out.has_value());
}

TEST_CASE("una lista vacia no es un error del cliente", "[fields]") {
    bool desconocidos = false;
    syrax::fields::apply("[]", syrax::fields::parse("name"), desconocidos);

    // No hay claves que encontrar porque no hay elementos, no porque el
    // cliente se equivocara.
    CHECK_FALSE(desconocidos);
}

TEST_CASE("sin campos pedidos no se toca nada", "[fields]") {
    bool desconocidos = false;
    CHECK_FALSE(syrax::fields::apply(R"({"id":1})", {}, desconocidos).has_value());
    CHECK_FALSE(desconocidos);
}

// ------------------------------------------------------ sobre una peticion

TEST_CASE("?fields recorta la respuesta de verdad", "[fields][http]") {
    api().fresh();
    REQUIRE(api().post("widgets", R"({"name":"tuerca","size":4})").ok());

    const auto completo = api().get("widgets");
    CHECK_THAT(completo.body, ContainsSubstring("size"));

    const auto recortado = api().get("widgets?fields=name");

    CHECK(recortado.status == 200);
    CHECK_THAT(recortado.body, ContainsSubstring("tuerca"));
    CHECK_THAT(recortado.body, !ContainsSubstring("size"));
    CHECK_THAT(recortado.body, !ContainsSubstring("\"id\""));
}

TEST_CASE("?fields sobre una pagina conserva el sobre", "[fields][http]") {
    api().fresh();
    REQUIRE(api().post("widgets", R"({"name":"perno","size":9})").ok());

    const auto response = api().get("paginados?fields=name");

    REQUIRE(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"total\":1"));
    CHECK_THAT(response.body, ContainsSubstring("perno"));
    CHECK_THAT(response.body, !ContainsSubstring("\"size\""));
}

TEST_CASE("un ?fields con solo nombres inventados da 400", "[fields][http]") {
    api().fresh();
    REQUIRE(api().post("widgets", R"({"name":"arandela","size":2})").ok());

    const auto response = api().get("widgets?fields=nombre");

    CHECK(response.status == 400);
    CHECK_THAT(response.body, ContainsSubstring("unknown_fields"));
}

TEST_CASE("a un error no se le recortan los campos", "[fields][http]") {
    api().fresh();

    // Filtrarle los campos a un error dejaria al cliente sin el mensaje que
    // explica que paso.
    const auto response = api().get("widgets/9999?fields=id");

    CHECK(response.status == 404);
    CHECK_THAT(response.body, ContainsSubstring("message"));
}
