#include "test_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace syrax;

namespace testsrv {

namespace {

std::thread& serverThread() {
    static std::thread thread;
    return thread;
}

std::atomic<int>& closes() {
    static std::atomic<int> n{0};
    return n;
}

std::atomic<std::uint16_t>& boundPort() {
    static std::atomic<std::uint16_t> value{0};
    return value;
}

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

Room& room() {
    static Room r;
    return r;
}

int closeCount() { return closes(); }

std::uint16_t port() { return boundPort(); }

void ensureServer() {
    static std::once_flag once;

    std::call_once(once, [] {
        static std::atomic<bool> ready{false};

        serverThread() = std::thread([] {
            App app;
            app.withoutDocs().quiet();

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

            // Handler que lanza: comprueba que el borde traduce la excepcion.
            app.get("/explota", []() -> Result<Echo> {
                throw std::runtime_error("tabla secreto no existe");
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

            app.ws("/echo", {
                .onOpen    = [](const Socket& s) { s.set("saludo", "hola"); },
                .onMessage = [](const Socket& s, std::string_view text) {
                    s.send(s.get("saludo") + ":" + std::string{text});
                },
                .onClose   = [](const Socket&) { ++closes(); },
            });

            // Segundo endpoint: prueba que el despacho por path funciona, que
            // es lo unico que distingue a un puente compartido de uno roto.
            app.ws("/room", {
                .onOpen    = [](const Socket& s) { room().join(s); },
                .onMessage = [](const Socket&, std::string_view text) { room().broadcast(text); },
                .onClose   = [](const Socket& s) { room().leave(s); },
            });

            // El aviso corre con el listener ya arriba, que es cuando Drogon
            // sabe que puerto le dio el kernel. Nada de REQUIRE aqui: esto no
            // es el hilo del test y Catch2 no lo soporta.
            drogon::app().registerBeginningAdvice([] {
                const auto listeners = drogon::app().getListeners();
                if (!listeners.empty()) boundPort() = listeners.front().toPort();
                ready = true;
            });
            app.run(0);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(ready);
        REQUIRE(boundPort() != 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
}

}  // namespace testsrv

CATCH_REGISTER_LISTENER(testsrv::DrogonShutdown)
