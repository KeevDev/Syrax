#include "test_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

using Catch::Matchers::ContainsSubstring;
using testsrv::kPort;
using Fixture = testsrv::Server;

namespace {

// Cliente HTTP minimo sobre sockets. Deliberadamente tonto: los tests no
// deben depender del cliente de Drogon, que necesitaria su propio event loop.
struct Response {
    int         status = 0;
    std::string body;
};

Response request(const std::string& method, const std::string& path,
                 const std::string& body = {}) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(kPort);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + body;
    ::send(fd, req.data(), req.size(), 0);

    std::string raw;
    char        buffer[4096];
    for (ssize_t n; (n = ::recv(fd, buffer, sizeof(buffer), 0)) > 0;) {
        raw.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);

    Response response;
    if (const auto space = raw.find(' '); space != std::string::npos) {
        response.status = std::stoi(raw.substr(space + 1, 3));
    }
    if (const auto sep = raw.find("\r\n\r\n"); sep != std::string::npos) {
        response.body = raw.substr(sep + 4);
    }
    return response;
}

}  // namespace

TEST_CASE_METHOD(Fixture, "GET sin argumentos devuelve una lista", "[http]") {
    // Este caso rompia la deduccion del body: arity 0 desbordaba el indice.
    const auto response = request("GET", "/things");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"name\":\"uno\""));
}

TEST_CASE_METHOD(Fixture, "el path param llega tipado al handler", "[http]") {
    const auto response = request("GET", "/things/42");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"id\":42"));
}

TEST_CASE_METHOD(Fixture, "un path param no convertible da 400", "[http]") {
    const auto response = request("GET", "/things/abc");

    CHECK(response.status == 400);
    CHECK_THAT(response.body, ContainsSubstring("invalid path parameter"));
}

TEST_CASE_METHOD(Fixture, "POST parsea y valida el body", "[http]") {
    const auto response = request("POST", "/things", R"({"name":"cosa","size":3})");

    CHECK(response.status == 201);
    CHECK_THAT(response.body, ContainsSubstring("\"name\":\"cosa\""));
}

TEST_CASE_METHOD(Fixture, "un campo faltante en el body da 422", "[http]") {
    // Glaze acepta objetos incompletos por defecto; Syrax lo configura
    // estricto porque para una API eso es un request invalido.
    const auto response = request("POST", "/things", R"({"name":"cosa"})");

    CHECK(response.status == 422);
    CHECK_THAT(response.body, ContainsSubstring("missing_key"));
}

TEST_CASE_METHOD(Fixture, "un tipo incorrecto en el body da 422", "[http]") {
    const auto response = request("POST", "/things", R"({"name":"x","size":"grande"})");
    CHECK(response.status == 422);
}

TEST_CASE_METHOD(Fixture, "un JSON malformado da 422", "[http]") {
    const auto response = request("POST", "/things", "{no es json");
    CHECK(response.status == 422);
}

TEST_CASE_METHOD(Fixture, "el handler puede devolver un error de negocio", "[http]") {
    const auto response = request("POST", "/things", R"({"name":"duplicado","size":1})");

    CHECK(response.status == 409);
    CHECK_THAT(response.body, ContainsSubstring("ya existe"));
}

TEST_CASE_METHOD(Fixture, "PUT recibe path param y body a la vez", "[http]") {
    // Sin esto no hay CRUD: el body es siempre el ultimo argumento.
    const auto response = request("PUT", "/things/7", R"({"name":"actualizado","size":1})");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"id\":7"));
    CHECK_THAT(response.body, ContainsSubstring("actualizado"));
}

TEST_CASE_METHOD(Fixture, "DELETE funciona", "[http]") {
    const auto response = request("DELETE", "/things/5");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("borrado"));
}

TEST_CASE_METHOD(Fixture, "los handlers corrutina responden", "[http]") {
    // Drogon exige una firma distinta para corrutinas (request y callback por
    // valor). Equivocarse ahi compila mal o cuelga punteros tras el co_await.
    const auto response = request("GET", "/async/hola");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"value\":\"hola\""));
}

TEST_CASE_METHOD(Fixture, "una ruta inexistente devuelve JSON, no HTML", "[http]") {
    // Drogon sirve una pagina HTML por defecto; una API debe responder JSON
    // siempre, incluso cuando el error lo genera el transporte.
    const auto response = request("GET", "/no-existe");

    CHECK(response.status == 404);
    CHECK_THAT(response.body, ContainsSubstring("\"status\":404"));
    CHECK_THAT(response.body, !ContainsSubstring("<html"));
}

TEST_CASE_METHOD(Fixture, "el error de negocio conserva su codigo", "[http]") {
    const auto response = request("GET", "/things/404");

    CHECK(response.status == 404);
    CHECK_THAT(response.body, ContainsSubstring("thing not found"));
}

TEST_CASE_METHOD(Fixture, "un body que cumple las reglas pasa", "[http][validation]") {
    const auto response =
        request("POST", "/signup", R"({"name":"Kevin","email":"kev@example.com","age":30})");

    CHECK(response.status == 201);
    CHECK_THAT(response.body, ContainsSubstring("Kevin"));
}

TEST_CASE_METHOD(Fixture, "un body invalido devuelve 422 con el detalle por campo",
                 "[http][validation]") {
    const auto response =
        request("POST", "/signup", R"({"name":"ab","email":"roto","age":5})");

    CHECK(response.status == 422);
    CHECK_THAT(response.body, ContainsSubstring("validation failed"));

    // Los tres campos se reportan juntos: no se corta en el primero.
    CHECK_THAT(response.body, ContainsSubstring("\"field\":\"name\""));
    CHECK_THAT(response.body, ContainsSubstring("\"field\":\"email\""));
    CHECK_THAT(response.body, ContainsSubstring("\"field\":\"age\""));
}

TEST_CASE_METHOD(Fixture, "la validacion tambien corre en handlers corrutina",
                 "[http][validation]") {
    const auto response =
        request("POST", "/signup-async", R"({"name":"ab","email":"kev@example.com","age":30})");

    CHECK(response.status == 422);
    CHECK_THAT(response.body, ContainsSubstring("\"field\":\"name\""));
}

TEST_CASE_METHOD(Fixture, "un campo ausente sigue siendo 422 sin lista de campos",
                 "[http][validation]") {
    // Falta 'age': eso lo ataja Glaze antes de que corran las reglas.
    const auto response = request("POST", "/signup", R"({"name":"Kevin","email":"k@e.com"})");

    CHECK(response.status == 422);
    CHECK_THAT(response.body, !ContainsSubstring("validation failed"));
}
