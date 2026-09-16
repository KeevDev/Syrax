#pragma once

#include <drogon/drogon.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/nosql/RedisResult.h>
#include <glaze/glaze.hpp>

#include <syrax/env.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <string>
#include <utility>

namespace syrax::cache {

// Los datos del Redis, con la misma forma que db::Connection: la aplicacion
// los arma en su configuracion y llama a connect().
struct Connection {
    std::string    host        = "127.0.0.1";
    unsigned short port        = 6379;
    std::string    username    = "";
    std::string    password    = "";
    unsigned int   database    = 0;
    std::size_t    connections = 1;
    double         timeout     = -1.0;
    bool           fast        = false;
    std::string    name        = "default";
};

namespace detail {

inline std::vector<std::string>& names() {
    static std::vector<std::string> value;
    return value;
}

}  // namespace detail

// Los nombres de los Redis registrados, por el mismo motivo que en db: Drogon
// no los expone y /health los necesita para no dar por sana una aplicacion
// cuyo cache esta caido.
inline const std::vector<std::string>& registered() {
    return detail::names();
}

inline void connect(const Connection& conn) {
    drogon::app().createRedisClient(conn.host, conn.port, conn.name, conn.password,
                                    conn.connections, conn.fast, conn.timeout, conn.database,
                                    conn.username);

    auto& names = detail::names();
    if (std::find(names.begin(), names.end(), conn.name) == names.end()) {
        names.push_back(conn.name);
    }
}

// Lo que describe el entorno:
//
//   REDIS_HOST REDIS_PORT REDIS_USER REDIS_PASSWORD REDIS_DB REDIS_POOL
inline Connection envConnection(const std::string& name = "default") {
    Connection conn;
    conn.host        = env("REDIS_HOST", conn.host);
    conn.port        = static_cast<unsigned short>(envInt("REDIS_PORT", conn.port));
    conn.username    = env("REDIS_USER", conn.username);
    conn.password    = env("REDIS_PASSWORD", conn.password);
    conn.database    = static_cast<unsigned int>(envInt("REDIS_DB", conn.database));
    conn.connections = static_cast<std::size_t>(envInt("REDIS_POOL", 1));
    conn.name        = name;
    return conn;
}

inline void configureFromEnv() {
    loadDotEnv();
    connect(envConnection());
}

// El cliente crudo, para los comandos que no cubren los ayudantes de abajo.
//
// Drogon lo entrega recien despues de arrancar el framework, asi que esto se
// llama desde un handler, no desde la configuracion.
inline drogon::nosql::RedisClientPtr client(const std::string& name = "default") {
    // Drogon arma sus clientes al arrancar y pedirlos antes es un segfault,
    // no un nullptr. Esto convierte ese error en una frase.
    if (!drogon::app().isRunning()) {
        throw std::runtime_error(
            "syrax: el cache '" + name +
            "' todavia no existe: Drogon crea sus clientes al arrancar. "
            "Usalo dentro de un handler, no en la configuracion.");
    }

    auto redis = drogon::app().getRedisClient(name);
    if (!redis) {
        throw std::runtime_error("syrax: no hay un Redis llamado '" + name +
                                 "'. Configuralo con cache::connect() antes de app.run().");
    }
    return redis;
}

using Ttl = std::chrono::seconds;

// GET. Una clave que no existe devuelve nullopt: distinto de existir vacia.
inline drogon::Task<std::optional<std::string>> get(std::string key,
                                                    std::string on = "default") {
    const auto result = co_await client(on)->execCommandCoro("get %s", key.c_str());
    if (result.isNil()) co_return std::nullopt;

    co_return result.asString();
}

// SET, con expiracion opcional. Sin ttl la clave se queda hasta que alguien
// la borre, que casi nunca es lo que quieres en un cache.
inline drogon::Task<void> put(std::string key, std::string value, Ttl ttl = Ttl::zero(),
                              std::string on = "default") {
    auto redis = client(on);

    if (ttl > Ttl::zero()) {
        co_await redis->execCommandCoro("set %s %s ex %d", key.c_str(), value.c_str(),
                                        static_cast<int>(ttl.count()));
    } else {
        co_await redis->execCommandCoro("set %s %s", key.c_str(), value.c_str());
    }
    co_return;
}

inline drogon::Task<bool> forget(std::string key, std::string on = "default") {
    const auto result = co_await client(on)->execCommandCoro("del %s", key.c_str());
    co_return result.asInteger() > 0;
}

inline drogon::Task<bool> has(std::string key, std::string on = "default") {
    const auto result = co_await client(on)->execCommandCoro("exists %s", key.c_str());
    co_return result.asInteger() > 0;
}

// El patron de siempre: devolver lo cacheado y, si no esta, calcularlo y
// guardarlo. El valor viaja como JSON, asi que sirve para cualquier struct
// que el resto del framework ya sabe serializar.
//
//   co_return co_await cache::remember<std::vector<models::User>>(
//       "users.activos", std::chrono::minutes{10},
//       [] { return repo::activos(); });
template <typename T, typename F>
drogon::Task<T> remember(std::string key, Ttl ttl, F produce, std::string on = "default") {
    if (const auto cached = co_await get(key, on)) {
        T value{};
        if (!glz::read_json(value, *cached)) co_return value;
        // Un JSON que ya no encaja con el tipo (cambio el struct) se trata
        // como un fallo de cache, no como un error: se recalcula y se pisa.
    }

    T value = co_await produce();
    co_await put(key, glz::write_json(value).value_or("null"), ttl, on);
    co_return value;
}

}  // namespace syrax::cache
