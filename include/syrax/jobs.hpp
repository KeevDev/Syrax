#pragma once

#include <drogon/drogon.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/utils/coroutine.h>
#include <glaze/glaze.hpp>

#include <syrax/cache.hpp>
#include <syrax/db.hpp>
#include <syrax/env.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Colas de trabajos.
//
// Un job es un struct plano con handle(): lo que entra a la cola es su JSON,
// que Glaze ya sabe escribir y leer sin que haya que declarar nada.
//
// El worker es el mismo binario con otro argumento, no un proceso aparte que
// haya que mantener al dia: asi un job puede usar los mismos servicios,
// repositorios y conexiones que un controlador.
namespace syrax::jobs {

// El sobre en el que viaja un job. El payload es el struct del usuario; lo
// demas es lo que la cola necesita para llevarle la cuenta.
struct Envelope {
    std::string  id;
    std::string  name;
    std::string  queue;
    std::string  payload;
    std::int64_t attempts = 0;
};

inline std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Un job que agoto sus intentos, con el motivo por el que se rindio.
struct Failure {
    Envelope     job;
    std::string  error;
    std::int64_t at = 0;
};

// Donde y cuando se encola. `queue` vacia significa la de por defecto.
struct Options {
    std::string          queue;
    std::chrono::seconds delay{0};

    Options& onQueue(std::string name) { queue = std::move(name); return *this; }
    Options& after(std::chrono::seconds wait) { delay = wait; return *this; }
};

inline Options on(std::string queue)          { return Options{}.onQueue(std::move(queue)); }
inline Options in(std::chrono::seconds delay) { return Options{}.after(delay); }

// ------------------------------------------------------------------ registro

// Que hacer con un nombre de job. El worker recibe texto: sin esta tabla no
// hay forma de volver al tipo, porque C++ no tiene reflection en ejecucion.
struct Handler {
    std::function<drogon::Task<void>(std::string)> run;
    int                                            tries = 1;
};

inline std::unordered_map<std::string, Handler>& registry() {
    static std::unordered_map<std::string, Handler> handlers;
    return handlers;
}

namespace detail {

template <typename T>
constexpr int triesOf() {
    if constexpr (requires { T::tries; }) return T::tries;
    else                                  return 1;
}

template <typename T>
std::string nameOf() {
    static_assert(requires { T::name; },
                  "syrax: un job necesita 'static constexpr auto name' para viajar por la "
                  "cola: es lo que el worker lee para saber que tipo reconstruir.");
    return std::string{T::name};
}

}  // namespace detail

// Declara que este binario sabe ejecutar ese job. Va en la configuracion,
// junto a la base y el cache.
template <typename T>
void handle() {
    registry()[detail::nameOf<T>()] = Handler{
        .run =
            [](std::string payload) -> drogon::Task<void> {
                T job{};
                if (const auto error = glz::read_json(job, payload)) {
                    throw std::runtime_error("syrax: el payload de '" + detail::nameOf<T>() +
                                             "' ya no encaja con el struct: " +
                                             glz::format_error(error, payload));
                }
                co_await job.handle();
            },
        .tries = detail::triesOf<T>(),
    };
}

// ------------------------------------------------------------------- drivers

// Lo que una cola tiene que saber hacer. Dos implementaciones: una tabla y
// un Redis.
struct Driver {
    virtual ~Driver() = default;

    virtual drogon::Task<void> push(Envelope job, std::chrono::seconds delay) = 0;

    // Saca uno y lo reserva. nullopt significa "no hay nada listo ahora".
    virtual drogon::Task<std::optional<Envelope>> pop(std::string queue) = 0;

    // Salio bien: fuera de la cola.
    virtual drogon::Task<void> forget(Envelope job) = 0;

    // Salio mal pero le quedan intentos: vuelve, disponible mas tarde.
    virtual drogon::Task<void> release(Envelope job, std::chrono::seconds delay) = 0;

    // Se acabaron los intentos: al archivo de fallidos, con el motivo.
    virtual drogon::Task<void> fail(Envelope job, std::string error) = 0;

    virtual drogon::Task<std::vector<Failure>> failed()      = 0;
    virtual drogon::Task<std::int64_t>         retryFailed() = 0;
    virtual drogon::Task<void>                 install()     = 0;
};

using DriverPtr = std::shared_ptr<Driver>;

// --------------------------------------------------------- driver: base de datos

// Encolar en la misma base que el resto de la aplicacion tiene una ventaja
// que ningun otro driver puede dar: el INSERT del job entra en la misma
// transaccion que la escritura que lo provoca. Si el COMMIT falla, el job
// tampoco existe, y no queda un trabajo corriendo sobre una fila que nunca
// se guardo.
class DatabaseDriver : public Driver {
public:
    using Provider = std::function<drogon::orm::DbClientPtr()>;

    DatabaseDriver(Provider client, std::chrono::seconds retryAfter)
        : client_{std::move(client)}, retryAfter_{retryAfter} {}

    drogon::Task<void> install() override {
        auto db = client_();

        // Se pregunta antes de crear, en vez de un CREATE IF NOT EXISTS a
        // ciegas: Postgres avisa por consola cada vez que uno de esos no hace
        // nada, y eso seria en cada arranque del servidor.
        const auto ya = co_await db->execSqlCoro(exists(dialect()));
        if (!ya.empty()) co_return;

        co_await db->execSqlCoro(create(dialect()));
        co_await db->execSqlCoro(createFailed(dialect()));
        co_return;
    }

    drogon::Task<void> push(Envelope job, std::chrono::seconds delay) override {
        co_await pushOn(client_(), std::move(job), delay);
        co_return;
    }

    // La version transaccional: el mismo INSERT, pero sobre el cliente que
    // se le pase, que puede ser el de una transaccion en curso.
    drogon::Task<void> pushOn(drogon::orm::DbClientPtr db, Envelope job,
                              std::chrono::seconds delay) {
        co_await db->execSqlCoro(
            "INSERT INTO syrax_jobs (queue, name, payload, attempts, available_at) "
            "VALUES (" + args(5) + ")",
            job.queue, job.name, job.payload, job.attempts, now() + delay.count());
        co_return;
    }

    drogon::Task<std::optional<Envelope>> pop(std::string queue) override {
        auto db = client_();

        const auto ahora   = now();
        const auto caducas = ahora - retryAfter_.count();

        // Reservar es leer y marcar sin que nadie se cuele en medio. Postgres y
        // sqlite lo hacen en una sola sentencia con RETURNING, que ya es
        // atomica por si sola; SKIP LOCKED es lo unico que los separa, y es de
        // Postgres.
        if (dialect() != Dialect::Mysql) {
            const std::string salta =
                dialect() == Dialect::Postgres ? " FOR UPDATE SKIP LOCKED" : "";

            const auto rows = co_await db->execSqlCoro(
                "UPDATE syrax_jobs SET reserved_at = " + arg(1) + ", attempts = attempts + 1 "
                "WHERE id = (SELECT id FROM syrax_jobs "
                "            WHERE queue = " + arg(2) + " AND available_at <= " + arg(3) +
                    "          AND (reserved_at IS NULL OR reserved_at < " + arg(4) + ") "
                    "        ORDER BY id LIMIT 1" + salta + ") "
                "RETURNING id, queue, name, payload, attempts",
                ahora, queue, ahora, caducas);

            if (rows.empty()) co_return std::nullopt;
            co_return read(rows.front());
        }

        // MySQL no tiene UPDATE ... RETURNING, asi que las dos sentencias van
        // dentro de una transaccion para que sigan siendo una sola cosa.
        co_return co_await db::transactionOn(
            db,
            [ahora, caducas, queue](const db::Tx& tx)
                -> drogon::Task<std::optional<Envelope>> {
                const auto rows = co_await tx.client()->execSqlCoro(
                    "SELECT id, queue, name, payload, attempts FROM syrax_jobs "
                    "WHERE queue = ? AND available_at <= ? "
                    "  AND (reserved_at IS NULL OR reserved_at < ?) "
                    "ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED",
                    queue, ahora, caducas);

                if (rows.empty()) co_return std::nullopt;

                auto job = read(rows.front());
                job.attempts += 1;

                co_await tx.client()->execSqlCoro(
                    "UPDATE syrax_jobs SET reserved_at = ?, attempts = ? WHERE id = ?",
                    ahora, job.attempts, std::stoll(job.id));

                co_return job;
            });
    }

    drogon::Task<void> forget(Envelope job) override {
        co_await client_()->execSqlCoro("DELETE FROM syrax_jobs WHERE id = " + args(1),
                                        std::stoll(job.id));
        co_return;
    }

    drogon::Task<void> release(Envelope job, std::chrono::seconds delay) override {
        co_await client_()->execSqlCoro(
            "UPDATE syrax_jobs SET reserved_at = NULL, available_at = " + arg(1) +
                " WHERE id = " + arg(2),
            now() + delay.count(), std::stoll(job.id));
        co_return;
    }

    drogon::Task<void> fail(Envelope job, std::string error) override {
        auto db = client_();

        co_await db->execSqlCoro(
            "INSERT INTO syrax_failed_jobs (queue, name, payload, attempts, error, failed_at) "
            "VALUES (" + args(6) + ")",
            job.queue, job.name, job.payload, job.attempts, error, now());

        co_await db->execSqlCoro("DELETE FROM syrax_jobs WHERE id = " + arg(1),
                                 std::stoll(job.id));
        co_return;
    }

    drogon::Task<std::vector<Failure>> failed() override {
        const auto rows = co_await client_()->execSqlCoro(
            "SELECT id, queue, name, payload, attempts, error, failed_at "
            "FROM syrax_failed_jobs ORDER BY id");

        std::vector<Failure> out;
        out.reserve(rows.size());
        for (const auto& row : rows) {
            out.push_back(Failure{
                .job   = read(row),
                .error = row["error"].as<std::string>(),
                .at    = row["failed_at"].as<std::int64_t>(),
            });
        }
        co_return out;
    }

    drogon::Task<std::int64_t> retryFailed() override {
        auto db = client_();

        const auto pendientes = co_await failed();
        for (const auto& fallo : pendientes) {
            Envelope nuevo = fallo.job;
            nuevo.attempts = 0;
            co_await pushOn(db, std::move(nuevo), std::chrono::seconds{0});
        }

        co_await db->execSqlCoro("DELETE FROM syrax_failed_jobs");
        co_return static_cast<std::int64_t>(pendientes.size());
    }

private:
    static Dialect dialect() { return db::dialect(); }

    static std::string arg(std::size_t n) {
        return db::dialect() == Dialect::Postgres ? "$" + std::to_string(n) : "?";
    }

    static std::string args(std::size_t count) {
        std::string out;
        for (std::size_t i = 1; i <= count; ++i) {
            if (i > 1) out += ", ";
            out += arg(i);
        }
        return out;
    }

    static Envelope read(const drogon::orm::Row& row) {
        return Envelope{
            .id       = std::to_string(row["id"].as<std::int64_t>()),
            .name     = row["name"].as<std::string>(),
            .queue    = row["queue"].as<std::string>(),
            .payload  = row["payload"].as<std::string>(),
            .attempts = row["attempts"].as<std::int64_t>(),
        };
    }

    // Si la tabla de jobs ya esta. Cada motor guarda su catalogo en otro sitio.
    static std::string exists(Dialect dialect) {
        switch (dialect) {
            case Dialect::Postgres:
                return "SELECT 1 FROM information_schema.tables "
                       "WHERE table_schema = current_schema() AND table_name = 'syrax_jobs'";
            case Dialect::Mysql:
                return "SELECT 1 FROM information_schema.tables "
                       "WHERE table_schema = DATABASE() AND table_name = 'syrax_jobs'";
            case Dialect::Sqlite:
                return "SELECT 1 FROM sqlite_master "
                       "WHERE type = 'table' AND name = 'syrax_jobs'";
        }
        return "";
    }

    static std::string create(Dialect dialect) {
        const std::string id = dialect == Dialect::Postgres ? "BIGSERIAL PRIMARY KEY"
                               : dialect == Dialect::Mysql  ? "BIGINT AUTO_INCREMENT PRIMARY KEY"
                                            : "INTEGER PRIMARY KEY AUTOINCREMENT";
        const std::string text = dialect == Dialect::Mysql ? "VARCHAR(191)" : "TEXT";

        return "CREATE TABLE IF NOT EXISTS syrax_jobs ("
               "id " + id + ", "
               "queue " + text + " NOT NULL, "
               "name " + text + " NOT NULL, "
               "payload TEXT NOT NULL, "
               "attempts BIGINT NOT NULL DEFAULT 0, "
               "available_at BIGINT NOT NULL, "
               "reserved_at BIGINT)";
    }

    static std::string createFailed(Dialect dialect) {
        const std::string id = dialect == Dialect::Postgres ? "BIGSERIAL PRIMARY KEY"
                               : dialect == Dialect::Mysql  ? "BIGINT AUTO_INCREMENT PRIMARY KEY"
                                            : "INTEGER PRIMARY KEY AUTOINCREMENT";
        const std::string text = dialect == Dialect::Mysql ? "VARCHAR(191)" : "TEXT";

        return "CREATE TABLE IF NOT EXISTS syrax_failed_jobs ("
               "id " + id + ", "
               "queue " + text + " NOT NULL, "
               "name " + text + " NOT NULL, "
               "payload TEXT NOT NULL, "
               "attempts BIGINT NOT NULL DEFAULT 0, "
               "error TEXT NOT NULL, "
               "failed_at BIGINT NOT NULL)";
    }

    Provider             client_;
    std::chrono::seconds retryAfter_;
};

// ------------------------------------------------------------- driver: redis

// Sin tabla y sin esquema, y con el mismo Redis que ya usa el cache. A cambio
// pierde lo transaccional: encolar aqui y escribir en la base son dos sistemas
// distintos, y nada garantiza que pase lo uno si pasa lo otro.
class RedisDriver : public Driver {
public:
    using Provider = std::function<drogon::nosql::RedisClientPtr()>;

    RedisDriver(Provider client, std::string prefix, std::chrono::seconds retryAfter)
        : client_{std::move(client)}, prefix_{std::move(prefix)}, retryAfter_{retryAfter} {}

    drogon::Task<void> install() override { co_return; }

    drogon::Task<void> push(Envelope job, std::chrono::seconds delay) override {
        const auto cola = job.queue;
        const auto body = glz::write_json(job).value_or("{}");

        if (delay > std::chrono::seconds{0}) {
            co_await redis()->execCommandCoro("zadd %s %s %s", delayed(cola).c_str(),
                                              std::to_string(now() + delay.count()).c_str(),
                                              body.c_str());
            co_return;
        }
        co_await redis()->execCommandCoro("rpush %s %s", pending(cola).c_str(), body.c_str());
        co_return;
    }

    drogon::Task<std::optional<Envelope>> pop(std::string queue) override {
        const auto salida = co_await redis()->execCommandCoro(
            "eval %s 3 %s %s %s %s %s %s", kPop.data(), pending(queue).c_str(),
            delayed(queue).c_str(), reserved(queue).c_str(),
            std::to_string(now()).c_str(),
            std::to_string(now() - retryAfter_.count()).c_str(),
            std::to_string(now() + retryAfter_.count()).c_str());

        if (salida.isNil()) co_return std::nullopt;

        const auto texto = salida.asString();

        Envelope job{};
        if (glz::read_json(job, texto)) co_return std::nullopt;

        // El id de un job en Redis es su propio texto: es lo que hay que
        // pasarle a ZREM para sacarlo de los reservados.
        job.id = texto;
        co_return job;
    }

    drogon::Task<void> forget(Envelope job) override {
        co_await redis()->execCommandCoro("zrem %s %s", reserved(job.queue).c_str(),
                                          job.id.c_str());
        co_return;
    }

    drogon::Task<void> release(Envelope job, std::chrono::seconds delay) override {
        const bool diferido = delay > std::chrono::seconds{0};

        co_await redis()->execCommandCoro(
            "eval %s 3 %s %s %s %s %s %s", kRelease.data(), reserved(job.queue).c_str(),
            pending(job.queue).c_str(), delayed(job.queue).c_str(), job.id.c_str(),
            std::to_string(now() + delay.count()).c_str(), diferido ? "1" : "0");
        co_return;
    }

    drogon::Task<void> fail(Envelope job, std::string error) override {
        Envelope archivado = job;
        archivado.id.clear();

        const Failure fallo{.job = archivado, .error = std::move(error), .at = now()};

        co_await redis()->execCommandCoro("eval %s 2 %s %s %s %s", kFail.data(),
                                          reserved(job.queue).c_str(), failedKey().c_str(),
                                          job.id.c_str(),
                                          glz::write_json(fallo).value_or("{}").c_str());
        co_return;
    }

    drogon::Task<std::vector<Failure>> failed() override {
        const auto filas =
            co_await redis()->execCommandCoro("lrange %s 0 -1", failedKey().c_str());

        std::vector<Failure> out;
        for (const auto& fila : filas.asArray()) {
            Failure fallo{};
            if (!glz::read_json(fallo, fila.asString())) out.push_back(std::move(fallo));
        }
        co_return out;
    }

    drogon::Task<std::int64_t> retryFailed() override {
        const auto salida = co_await redis()->execCommandCoro(
            "eval %s 1 %s %s", kRetry.data(), failedKey().c_str(), prefix_.c_str());
        co_return salida.asInteger();
    }

private:
    drogon::nosql::RedisClientPtr redis() const { return client_(); }

    std::string pending(const std::string& queue) const { return prefix_ + queue; }
    std::string delayed(const std::string& queue) const { return prefix_ + queue + ":delayed"; }
    std::string reserved(const std::string& queue) const { return prefix_ + queue + ":reserved"; }
    std::string failedKey() const { return prefix_ + "failed"; }

    // Cada operacion es UN comando, y no por elegancia: encadenar comandos de
    // Redis desde una corrutina los emite desde dentro del callback del
    // anterior, y ahi Drogon puede dejar de drenar su propia cola de tareas y
    // quedarse esperando para siempre. Ademas, sacar un job son cuatro pasos
    // (migrar diferidos, migrar caducados, sacar, reservar) que tienen que ser
    // atomicos: si dos workers se cuelan entre medias, el mismo job corre dos
    // veces.
    static constexpr std::string_view kPop = R"LUA(
local due = redis.call('zrangebyscore', KEYS[2], '-inf', ARGV[1], 'limit', 0, 64)
for i = 1, #due do
  if redis.call('zrem', KEYS[2], due[i]) == 1 then redis.call('rpush', KEYS[1], due[i]) end
end
local stale = redis.call('zrangebyscore', KEYS[3], '-inf', ARGV[2], 'limit', 0, 64)
for i = 1, #stale do
  if redis.call('zrem', KEYS[3], stale[i]) == 1 then redis.call('rpush', KEYS[1], stale[i]) end
end
local raw = redis.call('lpop', KEYS[1])
if not raw then return false end
local job = cjson.decode(raw)
job.attempts = (job.attempts or 0) + 1
local updated = cjson.encode(job)
redis.call('zadd', KEYS[3], ARGV[3], updated)
return updated
)LUA";

    static constexpr std::string_view kRelease = R"LUA(
redis.call('zrem', KEYS[1], ARGV[1])
if ARGV[3] == '1' then
  redis.call('zadd', KEYS[3], ARGV[2], ARGV[1])
else
  redis.call('rpush', KEYS[2], ARGV[1])
end
return 1
)LUA";

    static constexpr std::string_view kFail = R"LUA(
redis.call('zrem', KEYS[1], ARGV[1])
redis.call('rpush', KEYS[2], ARGV[2])
return 1
)LUA";

    static constexpr std::string_view kRetry = R"LUA(
local items = redis.call('lrange', KEYS[1], 0, -1)
for i = 1, #items do
  local fallo = cjson.decode(items[i])
  fallo.job.attempts = 0
  redis.call('rpush', ARGV[1] .. fallo.job.queue, cjson.encode(fallo.job))
end
redis.call('del', KEYS[1])
return #items
)LUA";

    Provider             client_;
    std::string          prefix_;
    std::chrono::seconds retryAfter_;
};

// ------------------------------------------------------------- configuracion

struct Connection {
    std::string          driver      = "database";
    std::string          queue       = "default";
    std::string          redisClient = "queue";
    std::string          prefix      = "syrax:jobs:";
    std::chrono::seconds retryAfter{90};
};

inline Connection& config() {
    static Connection conn;
    return conn;
}

inline DriverPtr& active() {
    static DriverPtr driver;
    return driver;
}

inline Driver& driver() {
    if (!active()) {
        throw std::runtime_error(
            "syrax: no hay ninguna cola configurada. Llama a jobs::connect() en "
            "bootstrap antes de despachar o de arrancar el worker.");
    }
    return *active();
}

inline void connect(Connection conn) {
    if (conn.driver == "database" || conn.driver == "db") {
        active() = std::make_shared<DatabaseDriver>([] { return db::client(); },
                                                    conn.retryAfter);
    } else if (conn.driver == "redis") {
        active() = std::make_shared<RedisDriver>(
            [name = conn.redisClient] { return cache::client(name); }, conn.prefix,
            conn.retryAfter);
    } else {
        throw std::invalid_argument("syrax: driver de cola '" + conn.driver +
                                    "' desconocido. Hay 'database' y 'redis'.");
    }
    config() = std::move(conn);

    // Las tablas se crean al arrancar, como la de migraciones: un despacho no
    // puede ser el primero en descubrir que el esquema de la cola no existe.
    drogon::app().registerBeginningAdvice([] {
        drogon::async_run([]() -> drogon::Task<void> {
            try {
                co_await driver().install();
            } catch (const std::exception& e) {
                std::cerr << "syrax: no pude preparar la cola: " << e.what() << "\n";
            }
            co_return;
        });
    });
}

// Para los tests y para casos raros: pon el driver que quieras.
inline void use(DriverPtr driver) { active() = std::move(driver); }

//   QUEUE_DRIVER        database (default) | redis
//   QUEUE_NAME          la cola por defecto
//   QUEUE_RETRY_AFTER   segundos antes de dar por muerto a un worker
inline Connection envConnection() {
    Connection conn;
    conn.driver     = env("QUEUE_DRIVER", conn.driver);
    conn.queue      = env("QUEUE_NAME", conn.queue);
    conn.retryAfter = std::chrono::seconds{envInt("QUEUE_RETRY_AFTER", 90)};
    return conn;
}

inline void configureFromEnv() {
    loadDotEnv();
    connect(envConnection());
}

// ----------------------------------------------------------------- despachar

namespace detail {

template <typename T>
Envelope envelopeFor(const T& job, const Options& opts) {
    return Envelope{
        .name     = nameOf<T>(),
        .queue    = opts.queue.empty() ? config().queue : opts.queue,
        .payload  = glz::write_json(job).value_or("{}"),
        .attempts = 0,
    };
}

}  // namespace detail

template <typename T>
drogon::Task<void> dispatch(T job, Options opts = {}) {
    co_await driver().push(detail::envelopeFor(job, opts), opts.delay);
    co_return;
}

// Encolar dentro de la transaccion que provoco el trabajo. Si el COMMIT no
// llega, el job no existe: es la razon de ser del driver de base de datos.
template <typename T>
drogon::Task<void> dispatch(T job, const db::Tx& tx, Options opts = {}) {
    auto* base = dynamic_cast<DatabaseDriver*>(&driver());
    if (!base) {
        throw std::runtime_error(
            "syrax: solo el driver 'database' puede encolar dentro de tu transaccion. "
            "Con redis son dos sistemas distintos y no hay COMMIT que los una.");
    }
    co_await base->pushOn(tx.client(), detail::envelopeFor(job, opts), opts.delay);
    co_return;
}

// -------------------------------------------------------------------- worker

struct Worker {
    std::vector<std::string> queues;
    std::chrono::seconds     idle{1};
    std::chrono::seconds     backoff{10};
    std::int64_t             maxJobs = 0;
};

inline std::atomic<bool>& running() {
    static std::atomic<bool> flag{true};
    return flag;
}

// Ejecuta un job ya reservado y decide que hacer con el resultado.
inline drogon::Task<void> perform(Envelope job, std::chrono::seconds backoff) {
    const auto found = registry().find(job.name);
    if (found == registry().end()) {
        co_await driver().fail(job, "no hay handler registrado para '" + job.name +
                                        "'. Declaralo con jobs::handle<T>() en bootstrap.");
        co_return;
    }

    // No se puede co_await dentro de un catch, asi que el resultado se
    // guarda y se decide despues.
    std::string error;
    bool        ok = false;

    try {
        co_await found->second.run(job.payload);
        ok = true;
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "excepcion desconocida";
    }

    if (ok) {
        co_await driver().forget(job);
        co_return;
    }

    if (job.attempts >= found->second.tries) {
        co_await driver().fail(job, error);
        co_return;
    }

    // Espera creciente: reintentar de inmediato contra algo que esta caido
    // solo gasta los intentos que quedan.
    co_await driver().release(job, backoff * job.attempts);
    co_return;
}

inline drogon::Task<void> loop(Worker opts) {
    auto colas = opts.queues.empty() ? std::vector<std::string>{config().queue} : opts.queues;

    std::int64_t hechos = 0;

    while (running()) {
        bool hubo = false;

        for (const auto& cola : colas) {
            if (!running()) break;

            auto job = co_await driver().pop(cola);
            if (!job) continue;

            hubo = true;
            std::cout << "  " << job->name << " (intento " << job->attempts << ")" << std::flush;

            co_await perform(*job, opts.backoff);
            std::cout << "  ok\n";

            if (opts.maxJobs > 0 && ++hechos >= opts.maxJobs) running() = false;
        }

        if (!hubo && running()) {
            co_await drogon::sleepCoro(drogon::app().getLoop(),
                                       static_cast<double>(opts.idle.count()));
        }
    }

    // Si salimos por una senal y no por maxJobs, conviene decirlo: un worker
    // que desaparece en silencio parece un worker que se murio.
    if (opts.maxJobs == 0 || hechos < opts.maxJobs) {
        std::cout << "\n  worker detenido; no quedan jobs a medias\n";
    }
    co_return;
}

namespace detail {

// Un worker que muere a mitad de un job deja ese job reservado hasta que vence
// su visibilidad, y entonces se reentrega: el usuario recibe el correo dos
// veces. Como `docker compose down` manda SIGTERM a todo el mundo, eso no es
// un caso raro, es el caso normal de cada despliegue.
//
// La solucion es bajar la bandera y dejar que el loop termine lo que tiene
// entre manos. Va por setTermSignalHandler y no por std::signal porque Drogon
// instala el suyo dentro de run(): un std::signal puesto antes se pierde, y
// el sintoma es justo el que se queria evitar (se comprobo: el worker salia
// por el manejador de Drogon, sin pasar por aqui).
inline void stopGracefully() {
    if (!running()) {
        // La segunda vez va en serio: si el job en curso esta colgado, quien
        // pulso Ctrl-C dos veces quiere irse ya.
        std::_Exit(130);
    }

    running() = false;
    std::cout << "\n  terminando el job en curso antes de salir...\n" << std::flush;
}

}  // namespace detail

// El worker. Levanta el loop de Drogon sin escuchar en ningun puerto: lo unico
// que hace es sondear la cola, pero dentro del mismo mundo de corrutinas que
// un controlador, para que un job pueda usar la base y el cache igual.
inline int work(Worker opts = {}) {
    running() = true;

    drogon::app().setTermSignalHandler(detail::stopGracefully);
    drogon::app().setIntSignalHandler(detail::stopGracefully);

    drogon::app().registerBeginningAdvice([opts] {
        drogon::async_run([opts]() -> drogon::Task<void> {
            co_await loop(opts);
            drogon::app().quit();
            co_return;
        });
    });

    drogon::app().run();
    return 0;
}

namespace detail {

// Corre una corrutina con el framework levantado y sale. Es lo que hace falta
// para los comandos de la cola: sin loop no hay clientes de base ni de Redis.
inline int runOnce(std::function<drogon::Task<void>()> body) {
    auto failed = std::make_shared<bool>(false);

    drogon::app().registerBeginningAdvice([body, failed] {
        drogon::async_run([body, failed]() -> drogon::Task<void> {
            try {
                co_await body();
            } catch (const std::exception& e) {
                std::cerr << "error: " << e.what() << "\n";
                *failed = true;
            }
            drogon::app().quit();
            co_return;
        });
    });

    drogon::app().run();
    return *failed ? 1 : 0;
}

}  // namespace detail

// Los que se rindieron, con el motivo.
inline int listFailed() {
    return detail::runOnce([]() -> drogon::Task<void> {
        const auto fallidos = co_await driver().failed();

        if (fallidos.empty()) {
            std::cout << "  no hay jobs fallidos\n";
            co_return;
        }

        for (const auto& fallo : fallidos) {
            std::cout << "  " << fallo.job.name << "  (" << fallo.job.attempts
                      << " intentos)  " << fallo.error << "\n";
        }
        co_return;
    });
}

// Todos los fallidos vuelven a la cola, con la cuenta de intentos a cero.
inline int retryAll() {
    return detail::runOnce([]() -> drogon::Task<void> {
        const auto cuantos = co_await driver().retryFailed();
        std::cout << "  " << cuantos << " job(s) de vuelta en la cola\n";
        co_return;
    });
}

}  // namespace syrax::jobs
