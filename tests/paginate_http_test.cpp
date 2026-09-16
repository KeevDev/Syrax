// La paginacion vista desde fuera: el sobre que recibe el cliente y lo que
// OpenAPI dice de el. El calculo en si esta en query_test.cpp.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

using Catch::Matchers::ContainsSubstring;

TEST_CASE("una ruta paginada devuelve el sobre completo", "[paginate][http]") {
    api().fresh();

    for (const auto* nombre : {"uno", "dos", "tres"}) {
        REQUIRE(api().post("widgets",
                           std::string{R"({"name":")"} + nombre + R"(","size":1})")
                    .ok());
    }

    const auto response = api().get("paginados?page=1&per_page=2");
    REQUIRE(response.status == 200);

    const auto pagina = response.json<syrax::Page<models::Widget>>();

    CHECK(pagina.data.size() == 2);
    CHECK(pagina.total == 3);
    CHECK(pagina.pages == 2);
    CHECK(pagina.hasMore);

    const auto segunda = api().get("paginados?page=2&per_page=2").json<syrax::Page<models::Widget>>();
    CHECK(segunda.data.size() == 1);
    CHECK_FALSE(segunda.hasMore);
}

TEST_CASE("OpenAPI documenta el sobre sin que nadie lo escriba", "[paginate][openapi]") {
    const auto spec = api().get("/openapi.json");
    REQUIRE(spec.status == 200);

    // El esquema sale de los mismos tipos que usa el handler, asi que la forma
    // de la pagina no puede desincronizarse del codigo.
    CHECK_THAT(spec.body, ContainsSubstring("hasMore"));
    CHECK_THAT(spec.body, ContainsSubstring("perPage"));
    CHECK_THAT(spec.body, ContainsSubstring("\"total\""));
}
