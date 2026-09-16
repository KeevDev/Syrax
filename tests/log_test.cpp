#include <catch2/catch_test_macros.hpp>

#include <syrax/log.hpp>

#include <iostream>
#include <set>
#include <sstream>
#include <string>

using namespace syrax;

namespace {

// El log escribe en std::cout a proposito —es lo que un contenedor recoge—,
// asi que para mirarlo hay que desviar el buffer.
class Capture {
public:
    Capture() : previo_{std::cout.rdbuf(buffer_.rdbuf())} {}
    ~Capture() { std::cout.rdbuf(previo_); }

    std::string text() const { return buffer_.str(); }

private:
    std::ostringstream buffer_;
    std::streambuf*    previo_;
};

}  // namespace

TEST_CASE("el request id es corto, legible y no se repite") {
    std::set<std::string> vistos;

    for (int i = 0; i < 2000; ++i) {
        const auto id = log::newRequestId();

        REQUIRE(id.size() == 12);
        for (const char c : id) {
            CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')));
        }
        vistos.insert(id);
    }

    CHECK(vistos.size() == 2000);
}

TEST_CASE("en json cada evento es una linea con sus campos") {
    log::format(log::Format::Json);
    log::level(log::Level::Debug);

    std::string salida;
    {
        Capture capture;
        log::info("usuario creado", {{"id", "7"}, {"request_id", "abc"}});
        salida = capture.text();
    }

    CHECK(salida.find("\"level\":\"info\"") != std::string::npos);
    CHECK(salida.find("\"msg\":\"usuario creado\"") != std::string::npos);
    CHECK(salida.find("\"id\":\"7\"") != std::string::npos);
    CHECK(salida.find("\"request_id\":\"abc\"") != std::string::npos);

    // Una linea por evento: si no, el recolector de logs no lo puede partir.
    CHECK(std::count(salida.begin(), salida.end(), '\n') == 1);
}

TEST_CASE("una comilla en el mensaje no rompe el json") {
    log::format(log::Format::Json);

    std::string salida;
    {
        Capture capture;
        log::error("fallo en \"users\"\n\tsegunda linea");
        salida = capture.text();
    }

    CHECK(salida.find("\\\"users\\\"") != std::string::npos);
    CHECK(salida.find("\\n\\tsegunda") != std::string::npos);
    CHECK(std::count(salida.begin(), salida.end(), '\n') == 1);
}

TEST_CASE("por debajo del nivel minimo no se escribe nada") {
    log::format(log::Format::Json);
    log::level(log::Level::Warn);

    Capture capture;
    log::debug("no deberia salir");
    log::info("tampoco");
    log::warn("esta si");

    const auto salida = capture.text();
    CHECK(salida.find("no deberia salir") == std::string::npos);
    CHECK(salida.find("tampoco") == std::string::npos);
    CHECK(salida.find("esta si") != std::string::npos);
}

TEST_CASE("en texto el mensaje sale sin comillas de json") {
    log::format(log::Format::Text);
    log::level(log::Level::Info);

    Capture capture;
    log::info("arrancando", {{"puerto", "8080"}});

    const auto salida = capture.text();
    CHECK(salida.find("arrancando") != std::string::npos);
    CHECK(salida.find("puerto=") != std::string::npos);
    CHECK(salida.find("\"msg\"") == std::string::npos);
}

TEST_CASE("requestId lee lo que dejo el middleware en la peticion") {
    auto crudo = drogon::HttpRequest::newHttpRequest();
    Request request{crudo};

    CHECK(log::requestId(request).empty());

    request.set(std::string{log::kRequestIdKey}, "abc123");
    CHECK(log::requestId(request) == "abc123");
}
