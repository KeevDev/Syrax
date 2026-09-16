#pragma once

// Un /health que devuelve 200 fijo no comprueba nada: dice que el proceso esta
// vivo, que es justo lo que el orquestador ya sabe porque tiene el pid. Lo que
// necesita saber es otra cosa —si esta instancia PUEDE trabajar— y eso depende
// de sus dependencias.
//
// La diferencia no es academica: con un 200 fijo, un contenedor cuya base esta
// caida se reporta sano, el balanceador le sigue mandando trafico y cada
// peticion se convierte en un 500. El healthcheck existe precisamente para
// sacar esa instancia de la rotacion.
//
// Esto pregunta a cada cliente registrado: un SELECT 1 por base y un PING por
// Redis. Barato, y suficiente para distinguir los dos casos.
//
//   app.health();                    // GET /health
//   app.health("/healthz");          // con otro nombre
//
// Y para lo que el framework no puede saber:
//
//   syrax::health::probe("s3", []() -> syrax::Task<std::string> {
//       co_return co_await alcanzable() ? "" : "no responde";
//   });

#include <syrax/cache.hpp>
#include <syrax/db.hpp>
#include <syrax/result.hpp>

#include <drogon/drogon.h>

#include <chrono>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace syrax::health {

struct Check {
    std::string   name;
    std::string   status;  // "ok" | "down"
    std::string   detail;  // el error, vacio cuando esta ok
    std::int64_t  ms = 0;  // lo que tardo en contestar
};

struct Report {
    std::string        status;  // "ok" | "down"
    std::vector<Check> checks;

    bool ok() const { return status == "ok"; }
};

// Una comprobacion propia del proyecto. Devuelve el motivo del fallo, o la
// cadena vacia si todo bien: asi el caso bueno no obliga a construir nada.
using Probe = std::function<drogon::Task<std::string>()>;

namespace detail {

struct Named {
    std::string name;
    Probe       probe;
};

inline std::vector<Named>& probes() {
    static std::vector<Named> value;
    return value;
}

inline std::int64_t millisSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

// Envuelve una comprobacion para que una excepcion sea un "down" con motivo y
// no tumbe el endpoint entero. Un /health que revienta cuando una dependencia
// falla es peor que no tenerlo: el orquestador ve un 500 sin decirle cual.
inline drogon::Task<Check> run(std::string name, Probe probe) {
    const auto start = std::chrono::steady_clock::now();

    try {
        const auto motivo = co_await probe();
        co_return Check{.name   = std::move(name),
                        .status = motivo.empty() ? "ok" : "down",
                        .detail = motivo,
                        .ms     = millisSince(start)};
    } catch (const std::exception& e) {
        co_return Check{.name   = std::move(name),
                        .status = "down",
                        .detail = e.what(),
                        .ms     = millisSince(start)};
    }
}

}  // namespace detail

// Agrega una comprobacion propia. Se llama en bootstrap, antes de run().
inline void probe(std::string name, Probe fn) {
    detail::probes().push_back({std::move(name), std::move(fn)});
}

// Olvida las comprobaciones propias. Existe para los tests.
inline void reset() { detail::probes().clear(); }

// Pregunta a todo lo registrado. En serie a proposito: son dos o tres
// consultas triviales, y un /health que abre una rafaga de conexiones en
// paralelo cada pocos segundos es una carga que nadie pidio.
inline drogon::Task<Report> check() {
    Report report{.status = "ok", .checks = {}};

    for (const auto& name : db::registered()) {
        report.checks.push_back(co_await detail::run(
            name == "default" ? "database" : "database:" + name,
            [name]() -> drogon::Task<std::string> {
                auto client = db::client(name);
                if (!client) co_return "no hay cliente registrado";
                co_await client->execSqlCoro("SELECT 1");
                co_return "";
            }));
    }

    for (const auto& name : cache::registered()) {
        report.checks.push_back(co_await detail::run(
            name == "default" ? "cache" : "cache:" + name,
            [name]() -> drogon::Task<std::string> {
                co_await cache::client(name)->execCommandCoro("ping");
                co_return "";
            }));
    }

    for (const auto& extra : detail::probes()) {
        report.checks.push_back(co_await detail::run(extra.name, extra.probe));
    }

    for (const auto& check : report.checks) {
        if (check.status != "ok") report.status = "down";
    }
    co_return report;
}

}  // namespace syrax::health
