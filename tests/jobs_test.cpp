#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <syrax/jobs.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using Catch::Matchers::ContainsSubstring;
using namespace std::chrono_literals;

// Glaze no refleja tipos en namespace anonimo: por eso lleva nombre.
namespace jtest {

inline int         ejecutados = 0;
inline std::string ultimo;

struct Saluda {
    std::string a;

    static constexpr auto name = "saluda";

    drogon::Task<void> handle() const {
        ++ejecutados;
        ultimo = a;
        co_return;
    }
};

struct Revienta {
    int intento = 0;

    static constexpr auto name  = "revienta";
    static constexpr int  tries = 3;

    drogon::Task<void> handle() const {
        throw std::runtime_error("la tercera tampoco");
        co_return;
    }
};

class TempQueue {
public:
    TempQueue() : path_{fs::temp_directory_path() / name()} {
        syrax::jobs::registry().clear();

        std::error_code ec;
        fs::remove(path_, ec);

        client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);

        syrax::db::activeDialect() = syrax::Dialect::Sqlite;

        auto driver = std::make_shared<syrax::jobs::DatabaseDriver>(
            [client = client_] { return client; }, 90s);

        drogon::sync_wait(driver->install());
        syrax::jobs::use(driver);
    }

    ~TempQueue() {
        syrax::jobs::use(nullptr);

        // Igual que el de Redis: el cliente se queda vivo hasta que el proceso
        // termine, en vez de cerrarse con trabajo todavia en vuelo.
        static std::vector<drogon::orm::DbClientPtr> vivos;
        vivos.push_back(std::move(client_));

        std::error_code ec;
        fs::remove(path_, ec);
    }

    const drogon::orm::DbClientPtr& db() const { return client_; }

private:
    static std::string name() {
        static int n = 0;
        return "syrax_jobs_" + std::to_string(::getpid()) + "_" + std::to_string(n++) + ".db";
    }

    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

}  // namespace jtest

using jtest::Revienta;
using jtest::Saluda;
using jtest::TempQueue;

namespace jobs = syrax::jobs;

TEST_CASE("un job despachado sale de la cola con un intento contado", "[jobs]") {
    TempQueue cola;

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "ada"}));

    const auto job = drogon::sync_wait(jobs::driver().pop("default"));

    REQUIRE(job.has_value());
    CHECK(job->name == "saluda");
    CHECK(job->queue == "default");
    CHECK(job->attempts == 1);
    CHECK_THAT(job->payload, ContainsSubstring("ada"));
}

TEST_CASE("la cola vacia no devuelve nada", "[jobs]") {
    TempQueue cola;
    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
}

TEST_CASE("un job reservado no lo agarra otro worker", "[jobs]") {
    TempQueue cola;

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "ada"}));

    CHECK(drogon::sync_wait(jobs::driver().pop("default")).has_value());
    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
}

TEST_CASE("un job diferido no aparece antes de tiempo", "[jobs]") {
    TempQueue cola;

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "ada"}, jobs::in(60s)));

    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
}

TEST_CASE("cada cola es suya", "[jobs]") {
    TempQueue cola;

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "ada"}, jobs::on("mails")));

    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
    CHECK(drogon::sync_wait(jobs::driver().pop("mails")).has_value());
}

TEST_CASE("el job corre y desaparece de la cola", "[jobs]") {
    TempQueue cola;
    jobs::handle<Saluda>();

    jtest::ejecutados = 0;
    drogon::sync_wait(jobs::dispatch(Saluda{.a = "grace"}));

    const auto job = drogon::sync_wait(jobs::driver().pop("default"));
    REQUIRE(job.has_value());

    drogon::sync_wait(jobs::perform(*job, 0s));

    CHECK(jtest::ejecutados == 1);
    CHECK(jtest::ultimo == "grace");
    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
    CHECK(drogon::sync_wait(jobs::driver().failed()).empty());
}

TEST_CASE("un job que falla vuelve a la cola hasta agotar sus intentos", "[jobs]") {
    TempQueue cola;
    jobs::handle<Revienta>();

    drogon::sync_wait(jobs::dispatch(Revienta{.intento = 1}));

    // tries = 3: dos vueltas a la cola y a la tercera se rinde.
    for (int i = 1; i <= 3; ++i) {
        const auto job = drogon::sync_wait(jobs::driver().pop("default"));
        REQUIRE(job.has_value());
        CHECK(job->attempts == i);

        drogon::sync_wait(jobs::perform(*job, 0s));
    }

    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());

    const auto fallidos = drogon::sync_wait(jobs::driver().failed());
    REQUIRE(fallidos.size() == 1);
    CHECK(fallidos[0].job.name == "revienta");
    CHECK(fallidos[0].job.attempts == 3);
    CHECK_THAT(fallidos[0].error, ContainsSubstring("la tercera tampoco"));
}

TEST_CASE("un job sin handler no se reintenta para siempre", "[jobs]") {
    TempQueue cola;

    // Un nombre que este binario no sabe ejecutar: es lo que pasa cuando se
    // despliega un worker viejo contra una cola que ya trae jobs nuevos.
    drogon::sync_wait(jobs::driver().push(
        jobs::Envelope{.name = "fantasma", .queue = "default", .payload = "{}"}, 0s));

    const auto job = drogon::sync_wait(jobs::driver().pop("default"));
    REQUIRE(job.has_value());

    drogon::sync_wait(jobs::perform(*job, 0s));

    const auto fallidos = drogon::sync_wait(jobs::driver().failed());
    REQUIRE(fallidos.size() == 1);
    CHECK_THAT(fallidos[0].error, ContainsSubstring("jobs::handle"));
}

TEST_CASE("los fallidos se pueden reencolar", "[jobs]") {
    TempQueue cola;
    jobs::handle<Revienta>();

    drogon::sync_wait(jobs::dispatch(Revienta{}));
    for (int i = 1; i <= 3; ++i) {
        const auto job = drogon::sync_wait(jobs::driver().pop("default"));
        REQUIRE(job.has_value());
        drogon::sync_wait(jobs::perform(*job, 0s));
    }

    CHECK(drogon::sync_wait(jobs::driver().retryFailed()) == 1);
    CHECK(drogon::sync_wait(jobs::driver().failed()).empty());

    const auto vuelto = drogon::sync_wait(jobs::driver().pop("default"));
    REQUIRE(vuelto.has_value());
    CHECK(vuelto->attempts == 1);
}

TEST_CASE("un job encolado en una transaccion que se deshace no existe", "[jobs]") {
    TempQueue cola;

    CHECK_THROWS(drogon::sync_wait(syrax::db::transactionOn(
        cola.db(),
        [](const syrax::db::Tx& tx) -> drogon::Task<void> {
            co_await jobs::dispatch(Saluda{.a = "ada"}, tx);
            throw std::runtime_error("algo salio mal despues de encolar");
        })));

    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
}

TEST_CASE("un driver sin configurar lo dice", "[jobs]") {
    jobs::use(nullptr);
    CHECK_THROWS_WITH(jobs::driver(), ContainsSubstring("jobs::connect"));
}

TEST_CASE("un driver desconocido no pasa en silencio", "[jobs]") {
    CHECK_THROWS_AS(jobs::connect({.driver = "rabbitmq"}), std::invalid_argument);
}

// ------------------------------------------------------------------- redis

// Estos necesitan un Redis de verdad. Sin el se saltan con un motivo, en vez
// de pasar en silencio haciendo creer que el driver esta probado.
namespace jtest {

// execCommandCoro devuelve un awaiter que sync_wait no acepta suelto: tiene
// que ir dentro de una corrutina.
inline drogon::Task<void> ping(drogon::nosql::RedisClientPtr client) {
    co_await client->execCommandCoro("ping");
    co_return;
}

inline drogon::Task<void> limpiar(drogon::nosql::RedisClientPtr client, std::string prefix) {
    // Las cadenas se copian antes del siguiente co_await: un RedisResult deja
    // de ser valido en cuanto la corrutina se suspende otra vez.
    std::vector<std::string> claves;
    {
        const auto filas = co_await client->execCommandCoro("keys %s", (prefix + "*").c_str());
        for (const auto& fila : filas.asArray()) claves.push_back(fila.asString());
    }

    for (const auto& clave : claves) {
        co_await client->execCommandCoro("del %s", clave.c_str());
    }
    co_return;
}

// Un solo cliente para todo el binario, no uno por test: crear y destruir
// conexiones a Redis en rafaga deja comandos en el aire cuando la que los
// llevaba se cierra, y la espera no termina nunca. Una aplicacion de verdad
// tampoco abre un cliente por peticion.
// Un solo cliente para todo el binario, no uno por test: crear y destruir
// conexiones a Redis en rafaga deja comandos en el aire cuando la que los
// llevaba se cierra. Una aplicacion de verdad tampoco abre un cliente por
// peticion.
//
// Los reintentos son por un fallo de Drogon: una de cada cien veces, un
// cliente recien creado se queda sin mandar el primer comando. Con timeout,
// eso es una excepcion en vez de una espera infinita, y se vuelve a intentar.
// Si hay algo escuchando, con un socket pelado. Crear un cliente de Drogon
// contra un puerto muerto y luego soltarlo termina uniendo el hilo del loop
// consigo mismo y abortando el proceso, asi que se pregunta antes.
inline bool redisReachable(const std::string& host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }

    const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

// Un solo cliente para todo el binario, no uno por test: crear y destruir
// conexiones a Redis en rafaga deja comandos en el aire cuando la que los
// llevaba se cierra. Una aplicacion de verdad tampoco abre un cliente por
// peticion.
//
// Los reintentos son por un fallo de Drogon: una de cada cien veces, un
// cliente recien creado se queda sin mandar el primer comando. Con timeout,
// eso es una excepcion en vez de una espera infinita, y se vuelve a intentar.
inline drogon::nosql::RedisClientPtr sharedRedis() {
    // A proposito nunca se destruye: cerrar un cliente de Drogon mientras
    // todavia hay callbacks en vuelo termina uniendo el hilo del loop consigo
    // mismo, y el proceso aborta. Un binario de tests dura lo que dura.
    static auto* client = new drogon::nosql::RedisClientPtr([] {
        const auto host = syrax::env("REDIS_TEST_HOST", "127.0.0.1");
        const auto port = static_cast<std::uint16_t>(syrax::envInt("REDIS_TEST_PORT", 6379));

        if (!redisReachable(host, port)) return drogon::nosql::RedisClientPtr{};

        static std::vector<drogon::nosql::RedisClientPtr> vivos;

        for (int intento = 0; intento < 3; ++intento) {
            auto nuevo = drogon::nosql::RedisClient::newRedisClient(
                trantor::InetAddress(host, port), 1);
            nuevo->setTimeout(3.0);
            vivos.push_back(nuevo);

            try {
                drogon::sync_wait(ping(nuevo));
                return nuevo;
            } catch (...) {
                continue;
            }
        }
        return drogon::nosql::RedisClientPtr{};
    }());

    return *client;
}

class TempRedisQueue {
public:
    bool ready() const { return client_ != nullptr; }

    TempRedisQueue() {
        syrax::jobs::registry().clear();

        client_ = sharedRedis();
        if (!client_) return;

        static int n = 0;
        prefix_ = "syrax:test:" + std::to_string(::getpid()) + ":" + std::to_string(n++) + ":";

        syrax::jobs::use(std::make_shared<syrax::jobs::RedisDriver>(
            [client = client_] { return client; }, prefix_, 90s));
    }

    ~TempRedisQueue() {
        if (!client_) return;

        drogon::sync_wait(limpiar(client_, prefix_));
        syrax::jobs::use(nullptr);
    }

private:
    drogon::nosql::RedisClientPtr client_;
    std::string                   prefix_;
};

}  // namespace jtest

using jtest::TempRedisQueue;

#define REQUIERE_REDIS(cola)                                                            \
    if (!(cola).ready()) SKIP("no hay Redis en REDIS_TEST_HOST:REDIS_TEST_PORT")

TEST_CASE("redis: un job despachado sale con un intento contado", "[jobs][redis]") {
    TempRedisQueue cola;
    REQUIERE_REDIS(cola);

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "ada"}));

    const auto job = drogon::sync_wait(jobs::driver().pop("default"));

    REQUIRE(job.has_value());
    CHECK(job->name == "saluda");
    CHECK(job->attempts == 1);
    CHECK_THAT(job->payload, ContainsSubstring("ada"));
}

TEST_CASE("redis: la cola vacia no devuelve nada", "[jobs][redis]") {
    TempRedisQueue cola;
    REQUIERE_REDIS(cola);

    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
}

TEST_CASE("redis: un job reservado no lo agarra otro worker", "[jobs][redis]") {
    TempRedisQueue cola;
    REQUIERE_REDIS(cola);

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "ada"}));

    CHECK(drogon::sync_wait(jobs::driver().pop("default")).has_value());
    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
}

TEST_CASE("redis: un job diferido no aparece antes de tiempo", "[jobs][redis]") {
    TempRedisQueue cola;
    REQUIERE_REDIS(cola);

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "ada"}, jobs::in(60s)));
    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "alan"}, jobs::in(-60s)));
    CHECK(drogon::sync_wait(jobs::driver().pop("default")).has_value());
}

TEST_CASE("redis: cada cola es suya", "[jobs][redis]") {
    TempRedisQueue cola;
    REQUIERE_REDIS(cola);

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "ada"}, jobs::on("mails")));

    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());
    CHECK(drogon::sync_wait(jobs::driver().pop("mails")).has_value());
}

TEST_CASE("redis: el job corre y desaparece de la cola", "[jobs][redis]") {
    TempRedisQueue cola;
    REQUIERE_REDIS(cola);

    jobs::handle<Saluda>();
    jtest::ejecutados = 0;

    drogon::sync_wait(jobs::dispatch(Saluda{.a = "grace"}));

    const auto job = drogon::sync_wait(jobs::driver().pop("default"));
    REQUIRE(job.has_value());

    drogon::sync_wait(jobs::perform(*job, 0s));

    CHECK(jtest::ejecutados == 1);
    CHECK(drogon::sync_wait(jobs::driver().failed()).empty());
}

TEST_CASE("redis: un job que falla se rinde al agotar los intentos", "[jobs][redis]") {
    TempRedisQueue cola;
    REQUIERE_REDIS(cola);

    jobs::handle<Revienta>();
    drogon::sync_wait(jobs::dispatch(Revienta{}));

    for (int i = 1; i <= 3; ++i) {
        const auto job = drogon::sync_wait(jobs::driver().pop("default"));
        REQUIRE(job.has_value());
        CHECK(job->attempts == i);

        drogon::sync_wait(jobs::perform(*job, 0s));
    }

    CHECK_FALSE(drogon::sync_wait(jobs::driver().pop("default")).has_value());

    const auto fallidos = drogon::sync_wait(jobs::driver().failed());
    REQUIRE(fallidos.size() == 1);
    CHECK(fallidos[0].job.attempts == 3);
    CHECK_THAT(fallidos[0].error, ContainsSubstring("la tercera tampoco"));
}

TEST_CASE("redis: los fallidos se pueden reencolar", "[jobs][redis]") {
    TempRedisQueue cola;
    REQUIERE_REDIS(cola);

    jobs::handle<Revienta>();
    drogon::sync_wait(jobs::dispatch(Revienta{}));

    for (int i = 1; i <= 3; ++i) {
        const auto job = drogon::sync_wait(jobs::driver().pop("default"));
        REQUIRE(job.has_value());
        drogon::sync_wait(jobs::perform(*job, 0s));
    }

    CHECK(drogon::sync_wait(jobs::driver().retryFailed()) == 1);
    CHECK(drogon::sync_wait(jobs::driver().failed()).empty());

    const auto vuelto = drogon::sync_wait(jobs::driver().pop("default"));
    REQUIRE(vuelto.has_value());
    CHECK(vuelto->attempts == 1);
}

TEST_CASE("con redis no se puede encolar dentro de una transaccion", "[jobs]") {
    TempQueue cola;

    // El driver no llega a usarse: el intento se rechaza antes de tocar Redis.
    jobs::use(std::make_shared<jobs::RedisDriver>(
        [] { return drogon::nosql::RedisClientPtr{}; }, "syrax:test:", 90s));

    CHECK_THROWS_WITH(
        drogon::sync_wait(syrax::db::transactionOn(
            cola.db(),
            [](const syrax::db::Tx& tx) -> drogon::Task<void> {
                co_await jobs::dispatch(Saluda{.a = "ada"}, tx);
            })),
        ContainsSubstring("database"));
}
