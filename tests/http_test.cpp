#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <syrax/syrax.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace syrax;

namespace {

constexpr std::uint16_t kPort = 18099;

struct CreateThing {
    std::string name;
    int         size;
};

struct Thing {
    std::int64_t id;
    std::string  name;
};

struct Echo {
    std::string value;
};

struct Signup {
    std::string name;
    std::string email;
    int         age;

    static auto rules() {
        return syrax::rules(field(&Signup::name).notEmpty().minLen(3),
                            field(&Signup::email).email(),
                            field(&Signup::age).range(18, 120));
    }
};

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

// Drogon es un singleton, asi que el servidor se levanta una sola vez para
// todo el binario de tests.
std::thread& serverThread() {
    static std::thread thread;
    return thread;
}

void ensureServer() {
    static std::once_flag once;

    std::call_once(once, [] {
        static std::atomic<bool> ready{false};

        serverThread() = std::thread([] {
            App app;
            app.withoutDocs();

            app.get("/things", []() -> Result<std::vector<Thing>> {
                return std::vector<Thing>{{.id = 1, .name = "uno"}};
            });

            app.get("/things/{id}", [](std::int64_t id) -> Result<Thing> {
                if (id == 404) return NotFound("thing not found");
                return Thing{.id = id, .name = "encontrado"};
            });

            app.post("/things", [](CreateThing body) -> Result<Thing> {
                if (body.name == "duplicado") return Conflict("ya existe");
                return Thing{.id = 99, .name = body.name};
            });

            app.put("/things/{id}", [](std::int64_t id, CreateThing body) -> Result<Thing> {
                return Thing{.id = id, .name = body.name};
            });

            app.del("/things/{id}", [](std::int64_t id) -> Result<Thing> {
                return Thing{.id = id, .name = "borrado"};
            });

            app.post("/signup", [](Signup body) -> Result<Echo> {
                return Echo{.value = body.name};
            });

            // El mismo body con reglas, pero por el camino de corrutina.
            app.post("/signup-async", [](Signup body) -> Task<Result<Echo>> {
                co_return Echo{.value = body.name};
            });

            // Handler corrutina: camino de registro distinto al sincrono.
            app.get("/async/{value}", [](std::string value) -> Task<Result<Echo>> {
                co_return Echo{.value = value};
            });

            drogon::app().registerBeginningAdvice([] { ready = true; });
            app.run(kPort);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
}

struct ServerFixture {
    ServerFixture() { ensureServer(); }
};

// Sin esto, el proceso termina con el servidor todavia vivo y Drogon aborta
// con "forbidden to run loop on threads other than event-loop thread". El
// binario salia con codigo 1 aunque todos los tests pasaran, que en CI es
// indistinguible de un fallo real.
struct DrogonShutdown : Catch::EventListenerBase {
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats&) override {
        if (!serverThread().joinable()) return;

        drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
        serverThread().join();
    }
};

}  // namespace

CATCH_REGISTER_LISTENER(DrogonShutdown)

TEST_CASE_METHOD(ServerFixture, "GET sin argumentos devuelve una lista", "[http]") {
    // Este caso rompia la deduccion del body: arity 0 desbordaba el indice.
    const auto response = request("GET", "/things");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"name\":\"uno\""));
}

TEST_CASE_METHOD(ServerFixture, "el path param llega tipado al handler", "[http]") {
    const auto response = request("GET", "/things/42");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"id\":42"));
}

TEST_CASE_METHOD(ServerFixture, "un path param no convertible da 400", "[http]") {
    const auto response = request("GET", "/things/abc");

    CHECK(response.status == 400);
    CHECK_THAT(response.body, ContainsSubstring("invalid path parameter"));
}

TEST_CASE_METHOD(ServerFixture, "POST parsea y valida el body", "[http]") {
    const auto response = request("POST", "/things", R"({"name":"cosa","size":3})");

    CHECK(response.status == 201);
    CHECK_THAT(response.body, ContainsSubstring("\"name\":\"cosa\""));
}

TEST_CASE_METHOD(ServerFixture, "un campo faltante en el body da 422", "[http]") {
    // Glaze acepta objetos incompletos por defecto; Syrax lo configura
    // estricto porque para una API eso es un request invalido.
    const auto response = request("POST", "/things", R"({"name":"cosa"})");

    CHECK(response.status == 422);
    CHECK_THAT(response.body, ContainsSubstring("missing_key"));
}

TEST_CASE_METHOD(ServerFixture, "un tipo incorrecto en el body da 422", "[http]") {
    const auto response = request("POST", "/things", R"({"name":"x","size":"grande"})");
    CHECK(response.status == 422);
}

TEST_CASE_METHOD(ServerFixture, "un JSON malformado da 422", "[http]") {
    const auto response = request("POST", "/things", "{no es json");
    CHECK(response.status == 422);
}

TEST_CASE_METHOD(ServerFixture, "el handler puede devolver un error de negocio", "[http]") {
    const auto response = request("POST", "/things", R"({"name":"duplicado","size":1})");

    CHECK(response.status == 409);
    CHECK_THAT(response.body, ContainsSubstring("ya existe"));
}

TEST_CASE_METHOD(ServerFixture, "PUT recibe path param y body a la vez", "[http]") {
    // Sin esto no hay CRUD: el body es siempre el ultimo argumento.
    const auto response = request("PUT", "/things/7", R"({"name":"actualizado","size":1})");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"id\":7"));
    CHECK_THAT(response.body, ContainsSubstring("actualizado"));
}

TEST_CASE_METHOD(ServerFixture, "DELETE funciona", "[http]") {
    const auto response = request("DELETE", "/things/5");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("borrado"));
}

TEST_CASE_METHOD(ServerFixture, "los handlers corrutina responden", "[http]") {
    // Drogon exige una firma distinta para corrutinas (request y callback por
    // valor). Equivocarse ahi compila mal o cuelga punteros tras el co_await.
    const auto response = request("GET", "/async/hola");

    CHECK(response.status == 200);
    CHECK_THAT(response.body, ContainsSubstring("\"value\":\"hola\""));
}

TEST_CASE_METHOD(ServerFixture, "una ruta inexistente devuelve JSON, no HTML", "[http]") {
    // Drogon sirve una pagina HTML por defecto; una API debe responder JSON
    // siempre, incluso cuando el error lo genera el transporte.
    const auto response = request("GET", "/no-existe");

    CHECK(response.status == 404);
    CHECK_THAT(response.body, ContainsSubstring("\"status\":404"));
    CHECK_THAT(response.body, !ContainsSubstring("<html"));
}

TEST_CASE_METHOD(ServerFixture, "el error de negocio conserva su codigo", "[http]") {
    const auto response = request("GET", "/things/404");

    CHECK(response.status == 404);
    CHECK_THAT(response.body, ContainsSubstring("thing not found"));
}

TEST_CASE_METHOD(ServerFixture, "un body que cumple las reglas pasa", "[http][validation]") {
    const auto response =
        request("POST", "/signup", R"({"name":"Kevin","email":"kev@example.com","age":30})");

    CHECK(response.status == 201);
    CHECK_THAT(response.body, ContainsSubstring("Kevin"));
}

TEST_CASE_METHOD(ServerFixture, "un body invalido devuelve 422 con el detalle por campo",
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

TEST_CASE_METHOD(ServerFixture, "la validacion tambien corre en handlers corrutina",
                 "[http][validation]") {
    const auto response =
        request("POST", "/signup-async", R"({"name":"ab","email":"kev@example.com","age":30})");

    CHECK(response.status == 422);
    CHECK_THAT(response.body, ContainsSubstring("\"field\":\"name\""));
}

TEST_CASE_METHOD(ServerFixture, "un campo ausente sigue siendo 422 sin lista de campos",
                 "[http][validation]") {
    // Falta 'age': eso lo ataja Glaze antes de que corran las reglas.
    const auto response = request("POST", "/signup", R"({"name":"Kevin","email":"k@e.com"})");

    CHECK(response.status == 422);
    CHECK_THAT(response.body, !ContainsSubstring("validation failed"));
}
