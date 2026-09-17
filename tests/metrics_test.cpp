// Las metricas, vistas desde fuera: lo que Prometheus se va a encontrar.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

using Catch::Matchers::ContainsSubstring;

namespace {

// Lee un contador del texto de Prometheus.
double valorDe(const std::string& cuerpo, const std::string& nombre) {
    // Se busca la linea que EMPIEZA por el nombre, no la que lo contiene: las
    // lineas "# HELP nombre ..." tambien lo llevan.
    const auto at = cuerpo.find("\n" + nombre + " ");
    if (at == std::string::npos) return -1;

    const auto desde = at + nombre.size() + 2;
    return std::stod(cuerpo.substr(desde, cuerpo.find('\n', desde) - desde));
}

}  // namespace

TEST_CASE("/metrics sale en texto plano, no en JSON", "[metrics]") {
    const auto response = api().get("/metrics");

    REQUIRE(response.status == 200);

    // Envolverlo en el sobre de la API lo haria inutil para lo unico que existe.
    CHECK_THAT(response.header("Content-Type"), ContainsSubstring("text/plain"));
    CHECK_THAT(response.body, ContainsSubstring("# TYPE syrax_requests_total counter"));
}

TEST_CASE("los cuatro contadores estan", "[metrics]") {
    const auto cuerpo = api().get("/metrics").body;

    CHECK(valorDe(cuerpo, "syrax_requests_total") >= 0);
    CHECK(valorDe(cuerpo, "syrax_requests_failed_total") >= 0);
    CHECK(valorDe(cuerpo, "syrax_request_duration_seconds_sum") >= 0);
    CHECK(valorDe(cuerpo, "syrax_requests_in_flight") >= 0);
}

TEST_CASE("una peticion sube el contador", "[metrics]") {
    const auto antes = valorDe(api().get("/metrics").body, "syrax_requests_total");

    api().get("widgets");

    const auto despues = valorDe(api().get("/metrics").body, "syrax_requests_total");
    CHECK(despues == antes + 1);
}

TEST_CASE("/metrics no se cuenta a si mismo", "[metrics]") {
    // Prometheus raspa cada 15 segundos: contarlo haria que el trafico de la
    // grafica fuera, en una API tranquila, casi todo el propio scrape.
    const auto primera = valorDe(api().get("/metrics").body, "syrax_requests_total");
    const auto segunda = valorDe(api().get("/metrics").body, "syrax_requests_total");

    CHECK(primera == segunda);
}

TEST_CASE("un 4xx no cuenta como fallo del servicio", "[metrics]") {
    const auto antes = valorDe(api().get("/metrics").body, "syrax_requests_failed_total");

    api().get("widgets/9999");      // 404
    api().get("/no-existe-nada");   // 404

    // Un 404 es el cliente pidiendo mal. Contarlo haria que la grafica de
    // errores suba cuando lo que pasa es que alguien escanea rutas.
    const auto despues = valorDe(api().get("/metrics").body, "syrax_requests_failed_total");
    CHECK(despues == antes);
}

TEST_CASE("un 5xx si cuenta como fallo", "[metrics]") {
    const auto antes = valorDe(api().get("/metrics").body, "syrax_requests_failed_total");

    api().get("/falla");

    const auto despues = valorDe(api().get("/metrics").body, "syrax_requests_failed_total");
    CHECK(despues == antes + 1);
}

TEST_CASE("el tiempo acumulado crece", "[metrics]") {
    const auto antes = valorDe(api().get("/metrics").body, "syrax_request_duration_seconds_sum");

    for (int i = 0; i < 5; ++i) api().get("widgets");

    const auto despues = valorDe(api().get("/metrics").body, "syrax_request_duration_seconds_sum");
    CHECK(despues > antes);
}

TEST_CASE("in_flight vuelve a cero cuando no hay nada en curso", "[metrics]") {
    api().get("widgets");

    // La peticion a /metrics no se cuenta, asi que en reposo esto es 0: si
    // quedara en 1, el gauge estaria perdiendo decrementos.
    CHECK(valorDe(api().get("/metrics").body, "syrax_requests_in_flight") == 0);
}
