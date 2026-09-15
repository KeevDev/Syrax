#include <catch2/catch_test_macros.hpp>

#include <syrax/auth.hpp>
#include <syrax/middleware.hpp>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>
#include <string>

using namespace syrax;

namespace {

// Un Request de mentira para probar middlewares sin levantar un servidor.
Request fakeRequest(const std::string& path,
                    const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath(path);
    for (const auto& [name, value] : headers) req->addHeader(name, value);
    return Request{req};
}

}  // namespace

TEST_CASE("requireApiKey rechaza si falta la cabecera", "[middleware]") {
    auto       mw      = requireApiKey("secreta");
    auto       request = fakeRequest("/x");
    const auto error   = mw(request);

    REQUIRE(error.has_value());
    CHECK(error->status == 401);
}

TEST_CASE("requireApiKey acepta la clave correcta y rechaza la incorrecta", "[middleware]") {
    auto mw = requireApiKey("secreta");

    auto buena = fakeRequest("/x", {{"X-Api-Key", "secreta"}});
    CHECK_FALSE(mw(buena).has_value());

    auto mala = fakeRequest("/x", {{"X-Api-Key", "otra-cosa"}});
    CHECK(mala.header("X-Api-Key") == "otra-cosa");
    CHECK(mw(mala).has_value());

    // Distinta longitud: no debe reventar ni pasar.
    auto corta = fakeRequest("/x", {{"X-Api-Key", "s"}});
    CHECK(mw(corta).has_value());
}

TEST_CASE("rateLimit corta al superar el limite", "[middleware]") {
    using namespace std::chrono_literals;

    auto mw = rateLimit(3, 60s);

    for (int i = 0; i < 3; ++i) {
        auto request = fakeRequest("/x");
        CHECK_FALSE(mw(request).has_value());
    }

    auto cuarta = fakeRequest("/x");
    const auto error = mw(cuarta);
    REQUIRE(error.has_value());
    CHECK(error->status == 429);
}

TEST_CASE("la ventana de rateLimit se reinicia", "[middleware]") {
    using namespace std::chrono_literals;

    auto mw = rateLimit(1, 1ms);

    auto primera = fakeRequest("/x");
    CHECK_FALSE(mw(primera).has_value());

    std::this_thread::sleep_for(20ms);

    auto despues = fakeRequest("/x");
    CHECK_FALSE(mw(despues).has_value());
}

TEST_CASE("el estado por request sobrevive entre lecturas", "[middleware]") {
    auto request = fakeRequest("/x");

    CHECK_FALSE(request.has("auth.sub"));
    CHECK(request.get("auth.sub", "nadie") == "nadie");

    request.set("auth.sub", "user-7");

    CHECK(request.has("auth.sub"));
    CHECK(request.get("auth.sub") == "user-7");
}

TEST_CASE("bearer deja el sujeto y el rol en el request", "[middleware][auth]") {
    const auto token = auth::sign(auth::Claims{.sub = "user-9", .role = "editor"}, "s3cr3t");

    auto mw      = auth::bearer("s3cr3t");
    auto request = fakeRequest("/x", {{"Authorization", "Bearer " + token}});

    REQUIRE_FALSE(mw(request).has_value());
    CHECK(request.get("auth.sub") == "user-9");
    CHECK(request.get("auth.role") == "editor");
}

TEST_CASE("bearer rechaza token ausente, mal formado o invalido", "[middleware][auth]") {
    auto mw = auth::bearer("s3cr3t");

    auto sinCabecera = fakeRequest("/x");
    CHECK(mw(sinCabecera)->status == 401);

    auto sinPrefijo = fakeRequest("/x", {{"Authorization", "abc.def.ghi"}});
    CHECK(mw(sinPrefijo)->status == 401);

    auto firmaMala = fakeRequest("/x", {{"Authorization", "Bearer a.b.c"}});
    CHECK(mw(firmaMala)->status == 401);

    const auto otroSecreto = auth::sign(auth::Claims{.sub = "x"}, "otro");
    auto       ajeno = fakeRequest("/x", {{"Authorization", "Bearer " + otroSecreto}});
    CHECK(mw(ajeno)->status == 401);
}

TEST_CASE("securityHeaders agrega las cabeceras esperadas", "[middleware]") {
    auto response = drogon::HttpResponse::newHttpResponse();
    securityHeaders()(response);

    CHECK(response->getHeader("X-Content-Type-Options") == "nosniff");
    CHECK(response->getHeader("X-Frame-Options") == "DENY");
    CHECK(response->getHeader("Referrer-Policy") == "no-referrer");
}
