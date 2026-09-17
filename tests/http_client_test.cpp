// El cliente HTTP. Lo que hay que probar no es que sepa hacer un GET -eso lo
// hace Drogon- sino las tres decisiones que lo separan de escribirlo a mano:
// que reintente solo lo idempotente, que no reintente lo que no tiene arreglo,
// y que la traza cruce el salto.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace {

struct Eco {
    int         hits = 0;
    std::string requestId;
    std::string authorization;
};

syrax::http::Client cliente(syrax::http::Options options = {}) {
    // Sin espera entre reintentos: lo que se comprueba es cuantos hay, no
    // cuanto tardan, y 200ms por intento haria la suite lenta sin medir nada.
    if (options.backoff == std::chrono::milliseconds{200}) {
        options.backoff = std::chrono::milliseconds{1};
    }
    return syrax::http::Client{"http://127.0.0.1:" + std::to_string(api().port()), options};
}

void reinicia() { api().get("/reinicia"); }

int llamadas() { return api().get("/cuenta").json<Eco>().hits; }

}  // namespace

TEST_CASE("un GET que falla se reintenta", "[httpclient]") {
    reinicia();

    const auto respuesta = drogon::sync_wait(cliente().get("/falla"));

    CHECK(respuesta.status == 500);

    // Un intento mas dos reintentos, que es el valor por defecto.
    CHECK(llamadas() == 3);
}

TEST_CASE("un POST que falla NO se reintenta", "[httpclient]") {
    reinicia();

    // Es la decision central. Un reintento parece gratis hasta que la primera
    // llamada SI llego y lo que se perdio fue la respuesta: entonces el
    // reintento cobra dos veces.
    const auto respuesta = drogon::sync_wait(cliente().post("/falla", std::string{}));

    CHECK(respuesta.status == 500);
    CHECK(llamadas() == 1);
}

TEST_CASE("un 404 no se reintenta aunque el metodo sea idempotente", "[httpclient]") {
    reinicia();

    // El servidor entendio y dijo que no. Repetirlo da exactamente lo mismo.
    const auto respuesta = drogon::sync_wait(cliente().get("/no-esta"));

    CHECK(respuesta.status == 404);
    CHECK(llamadas() == 1);
}

TEST_CASE("una respuesta buena no se reintenta", "[httpclient]") {
    reinicia();

    const auto respuesta = drogon::sync_wait(cliente().get("/eco"));

    CHECK(respuesta.ok());
    CHECK(llamadas() == 1);
}

TEST_CASE("los reintentos se pueden apagar", "[httpclient]") {
    reinicia();

    const auto respuesta = drogon::sync_wait(cliente({.retries = 0}).get("/falla"));

    CHECK(respuesta.status == 500);
    CHECK(llamadas() == 1);
}

TEST_CASE("el numero de reintentos se respeta", "[httpclient]") {
    reinicia();

    drogon::sync_wait(cliente({.retries = 4}).get("/falla"));

    CHECK(llamadas() == 5);
}

TEST_CASE("un servidor que no existe da status 0 y un motivo", "[httpclient]") {
    // Levanta la aplicacion aunque este caso no le pida nada: el cliente
    // necesita el loop de Drogon, y bajo ctest cada caso corre en su propio
    // proceso, donde nadie mas lo ha arrancado.
    (void)api().port();

    // Un puerto donde no hay nadie. status 0 marca "no hubo respuesta", que es
    // distinto de un 500: en el 500 el servidor contesto.
    syrax::http::Client muerto{"http://127.0.0.1:9", {.timeout = std::chrono::milliseconds{500},
                                                      .retries = 0,
                                                      .backoff = std::chrono::milliseconds{1}}};

    const auto respuesta = drogon::sync_wait(muerto.get("/lo-que-sea"));

    CHECK(respuesta.status == 0);
    CHECK(respuesta.failed());
    CHECK_FALSE(respuesta.ok());
    CHECK_FALSE(respuesta.error.empty());
}

TEST_CASE("una cabecera propia viaja en la peticion", "[httpclient]") {
    reinicia();

    auto       c         = cliente();
    const auto respuesta = drogon::sync_wait(c.bearer("un-token").get("/eco"));

    REQUIRE(respuesta.ok());
    CHECK(respuesta.json<Eco>().authorization == "Bearer un-token");
}

TEST_CASE("la respuesta expone cabeceras y cuerpo tipado", "[httpclient]") {
    reinicia();

    const auto respuesta = drogon::sync_wait(cliente().get("/eco"));

    REQUIRE(respuesta.ok());
    CHECK(respuesta.header("content-type").find("application/json") != std::string::npos);
    CHECK(respuesta.json<Eco>().hits == 1);
}

TEST_CASE("trace propaga el X-Request-Id al otro servicio", "[httpclient]") {
    reinicia();

    // Sin esto, una peticion que cruza tres servicios y falla en el tercero no
    // se puede reconstruir: cada salto estrenaria su propio id.
    //
    // Se construye un Request con la cabecera ya puesta, que es lo que el
    // middleware de trazabilidad deja en una peticion de verdad.
    auto crudo = drogon::HttpRequest::newHttpRequest();
    crudo->attributes()->insert(std::string{syrax::log::kRequestIdKey}, std::string{"traza-123"});

    syrax::Request entrante{crudo};

    auto       c         = cliente();
    const auto respuesta = drogon::sync_wait(c.trace(entrante).get("/eco"));

    REQUIRE(respuesta.ok());
    CHECK(respuesta.json<Eco>().requestId == "traza-123");
}

TEST_CASE("sin request-id no se manda una cabecera vacia", "[httpclient]") {
    reinicia();

    auto           crudo = drogon::HttpRequest::newHttpRequest();
    syrax::Request sinId{crudo};

    auto       c         = cliente();
    const auto respuesta = drogon::sync_wait(c.trace(sinId).get("/eco"));

    REQUIRE(respuesta.ok());

    // No llego ninguna cabecera, que es lo correcto: una vacia es peor que
    // ninguna, porque al otro lado el middleware de trazabilidad la respetaria
    // como si fuera un id y la traza se quedaria sin valor con el que cruzarse.
    CHECK(respuesta.json<Eco>().requestId.empty());

    // Y el servidor de enfrente genera el suyo, que es lo que tiene que pasar
    // cuando nadie le da uno.
    CHECK_FALSE(respuesta.header("X-Request-Id").empty());
}

TEST_CASE("sin el loop de Drogon el cliente avisa, no se cuelga", "[httpclient]") {
    // Este caso NO levanta la aplicacion, a proposito. Sin el guardia, la
    // corrutina esperaria a un loop que no existe y el proceso no terminaria
    // nunca: un cuelgue sin mensaje, que es el peor fallo posible.
    //
    // Bajo ctest cada caso corre en su propio proceso y esto se cumple. Al
    // correr el binario entero, otro caso ya levanto la aplicacion y la
    // condicion que se prueba no existe: entonces no hay nada que comprobar.
    if (drogon::app().isRunning()) {
        WARN("la aplicacion ya esta levantada en este proceso, se salta");
        return;
    }

    syrax::http::Client suelto{"http://127.0.0.1:9", {.retries = 0}};

    const auto respuesta = drogon::sync_wait(suelto.get("/lo-que-sea"));

    CHECK(respuesta.status == 0);
    CHECK(respuesta.failed());
    CHECK(respuesta.error.find("loop de Drogon") != std::string::npos);
}
