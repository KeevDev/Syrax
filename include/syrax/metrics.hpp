#pragma once

// Cuatro numeros en /metrics, en el formato de texto que lee Prometheus.
//
//   app.metrics();
//
//   # HELP syrax_requests_total Peticiones atendidas
//   syrax_requests_total 1428
//   syrax_requests_failed_total 3
//   syrax_request_duration_seconds_sum 9.412
//   syrax_requests_in_flight 2
//
// Con eso salen las tres graficas que de verdad se miran: cuanto trafico hay,
// que porcentaje falla y cuanto se tarda de media. Un panel con esas tres
// contesta "¿va bien esto?" mejor que veinte con todo lo demas.
//
// **Y se planta ahi, a proposito.** Lo siguiente que se pide siempre es
// partirlo por ruta -syrax_requests_total{path="/users"}-, y eso no es un
// contador mas: es cardinalidad sin techo. Una ruta con un id dentro -/users/42-
// genera una serie por usuario, y el primero que pasa un bot por ahi tumba el
// Prometheus, no la API. Contenerlo obliga a normalizar la ruta a su plantilla,
// que obliga a conocer el router, que obliga a una lista de excepciones... y a
// esa altura esto ya es una libreria de metricas a medias.
//
// Cuando hagan falta histogramas y etiquetas, lo que hace falta es
// prometheus-cpp, que hace eso bien. Esto es para no tener que instalar nada
// para saber si la API respira.
//
// Los contadores son de proceso: con tres replicas hay tres /metrics, que es
// justo como Prometheus espera recogerlos.

#include <atomic>
#include <cstdint>
#include <string>

namespace syrax::metrics {

namespace detail {

// Relaxed en todos: son contadores para mirar en una grafica, no para decidir
// nada en el codigo. Pedir ordenacion aqui costaria una barrera por peticion
// para que el numero salga igual de bien.
inline std::atomic<std::uint64_t>& total() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& failed() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

// En microsegundos, para no acumular error de redondeo sumando dobles. Al
// exponerlo se convierte a segundos, que es lo que pide Prometheus.
inline std::atomic<std::uint64_t>& micros() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::int64_t>& inFlight() {
    static std::atomic<std::int64_t> value{0};
    return value;
}

}  // namespace detail

inline void started() { detail::inFlight().fetch_add(1, std::memory_order_relaxed); }

// `failed` es 5xx y no 4xx: un 404 o un 422 es el cliente pidiendo mal, y
// contarlo como fallo del servicio hace que la grafica de errores suba cuando
// lo que pasa es que alguien esta escaneando rutas.
inline void finished(int status, std::uint64_t elapsedMicros) {
    detail::total().fetch_add(1, std::memory_order_relaxed);
    detail::micros().fetch_add(elapsedMicros, std::memory_order_relaxed);
    detail::inFlight().fetch_sub(1, std::memory_order_relaxed);

    if (status >= 500) detail::failed().fetch_add(1, std::memory_order_relaxed);
}

struct Snapshot {
    std::uint64_t total    = 0;
    std::uint64_t failed   = 0;
    std::uint64_t micros   = 0;
    std::int64_t  inFlight = 0;
};

inline Snapshot snapshot() {
    return Snapshot{
        .total    = detail::total().load(std::memory_order_relaxed),
        .failed   = detail::failed().load(std::memory_order_relaxed),
        .micros   = detail::micros().load(std::memory_order_relaxed),
        .inFlight = detail::inFlight().load(std::memory_order_relaxed),
    };
}

// Pone los cuatro a cero. Existe para los tests.
inline void reset() {
    detail::total()    = 0;
    detail::failed()   = 0;
    detail::micros()   = 0;
    detail::inFlight() = 0;
}

// El formato de texto de Prometheus: una linea HELP, una TYPE y el valor.
inline std::string render() {
    const auto now = snapshot();

    const auto segundos = static_cast<double>(now.micros) / 1'000'000.0;

    std::string out;
    out += "# HELP syrax_requests_total Peticiones atendidas\n";
    out += "# TYPE syrax_requests_total counter\n";
    out += "syrax_requests_total " + std::to_string(now.total) + "\n";

    out += "# HELP syrax_requests_failed_total Peticiones que acabaron en 5xx\n";
    out += "# TYPE syrax_requests_failed_total counter\n";
    out += "syrax_requests_failed_total " + std::to_string(now.failed) + "\n";

    out += "# HELP syrax_request_duration_seconds_sum Tiempo acumulado atendiendo peticiones\n";
    out += "# TYPE syrax_request_duration_seconds_sum counter\n";
    out += "syrax_request_duration_seconds_sum " + std::to_string(segundos) + "\n";

    out += "# HELP syrax_requests_in_flight Peticiones ahora mismo en curso\n";
    out += "# TYPE syrax_requests_in_flight gauge\n";
    out += "syrax_requests_in_flight " + std::to_string(now.inFlight) + "\n";

    return out;
}

}  // namespace syrax::metrics
