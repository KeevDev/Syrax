#pragma once

// Tareas periodicas.
//
// Era no-objetivo mientras no habia colas —"para eso esta cron"—, y sigue
// siendo verdad que cron funciona. Lo que cambia con la cola es que ya existe
// el sitio donde poner el trabajo: el scheduler no ejecuta nada, **encola**. Un
// `dailyAt("03:00")` es una llamada a jobs::dispatch a las tres, y a partir de
// ahi el job es un job como los demas, con sus reintentos y su registro de
// fallos. Eso es lo que hace que el delta sea pequeño.
//
//   syrax::schedule::every(std::chrono::minutes{5}).dispatch(PurgeSessions{});
//   syrax::schedule::dailyAt("03:00").dispatch(NightlyReport{});
//   syrax::schedule::hourlyAt(30).onQueue("informes").dispatch(Rollup{});
//
// Y en el proyecto, un proceso aparte del worker:
//
//   syrax schedule:work
//
// No hay expresiones cron a proposito. Un `*/15 9-17 * * 1-5` es un lenguaje
// entero —rangos, pasos, listas, dias de semana con dos numeraciones distintas
// para el domingo— y escribirlo mal no da un error, da una tarea que corre
// cuando no toca. Las tres de arriba cubren lo que pide una API; para el resto,
// cron existe y sabe hacerlo mejor.
//
// Tres cosas que conviene saber antes de montarlo:
//
//   - **Corre UNA sola instancia.** Dos schedulers encolan cada tarea dos
//     veces. No hay cerrojo distribuido aqui, y fingir que lo hay seria peor
//     que decirlo: en Kubernetes, replicas: 1 en ese deployment.
//   - **Una tarea perdida no se recupera.** Si el proceso estaba caido a las
//     03:00, la tarea de las 03:00 no corre; la siguiente es mañana. Es
//     deliberado: encolar a las 03:40 el informe de las 03:00 casi nunca es lo
//     que alguien queria, y cuando lo es, se pide a mano.
//   - **La hora es la local del proceso.** "03:00" es las tres donde corre el
//     contenedor, asi que conviene fijarle el TZ y no dejarlo al azar del host.

#include <syrax/jobs.hpp>

#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace syrax::schedule {

// Dado un instante, cuando toca la proxima vez. Devolver siempre un valor
// ESTRICTAMENTE mayor es lo que garantiza que el bucle avance: una que
// devolviera el mismo instante se dispararia en cada vuelta.
using NextAt = std::function<std::int64_t(std::int64_t)>;

namespace detail {

struct Entry {
    std::string                         label;
    NextAt                              next;
    std::function<drogon::Task<void>()> fire;
    std::int64_t                        at = 0;
};

inline std::vector<Entry>& entries() {
    static std::vector<Entry> value;
    return value;
}

inline std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// La medianoche local del dia al que pertenece `stamp`. Va por localtime y no
// por aritmetica sobre el epoch a proposito: un pais con horario de verano
// tiene dias de 23 y de 25 horas, y "mañana a la misma hora" no es "dentro de
// 86400 segundos" dos veces al año.
inline std::int64_t midnightOf(std::int64_t stamp) {
    std::time_t t = static_cast<std::time_t>(stamp);
    std::tm     parts{};
    ::localtime_r(&t, &parts);

    parts.tm_hour = 0;
    parts.tm_min  = 0;
    parts.tm_sec  = 0;
    parts.tm_isdst = -1;  // que mktime decida si ese dia habia horario de verano

    return static_cast<std::int64_t>(::mktime(&parts));
}

inline std::int64_t atLocalTime(std::int64_t day, int hour, int minute) {
    std::time_t t = static_cast<std::time_t>(day);
    std::tm     parts{};
    ::localtime_r(&t, &parts);

    parts.tm_hour  = hour;
    parts.tm_min   = minute;
    parts.tm_sec   = 0;
    parts.tm_isdst = -1;

    return static_cast<std::int64_t>(::mktime(&parts));
}

}  // namespace detail

// Lo que se esta construyendo hasta que se llama a dispatch(). Nada se
// registra antes: una tarea sin trabajo no tiene sentido, y dejarla a medias
// en la lista solo serviria para que el bucle la recorra sin hacer nada.
class Builder {
public:
    Builder(std::string label, NextAt next) : label_{std::move(label)}, next_{std::move(next)} {}

    // La cola donde va el job. Vacia es la de por defecto.
    Builder& onQueue(std::string name) {
        queue_ = std::move(name);
        return *this;
    }

    // Registra la tarea. El job viaja por copia porque se encola muchas veces,
    // una por vencimiento, y cada una necesita su propio payload.
    template <typename T>
    void dispatch(T job) {
        const auto queue = queue_;

        detail::entries().push_back(detail::Entry{
            .label = label_ + " -> " + jobs::detail::nameOf<T>(),
            .next  = next_,
            .fire =
                [job, queue]() -> drogon::Task<void> {
                    jobs::Options opts;
                    if (!queue.empty()) opts.onQueue(queue);

                    co_await jobs::dispatch(job, opts);
                },
        });
    }

private:
    std::string label_;
    NextAt      next_;
    std::string queue_;
};

// Cada tanto, contado desde que arranca el scheduler. La primera vez es
// DESPUES del primer intervalo, no al arrancar: un every(1h) que dispara al
// levantar el proceso convierte cada despliegue en una ejecucion extra.
inline Builder every(std::chrono::seconds interval) {
    if (interval <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("syrax::schedule::every: el intervalo tiene que ser positivo");
    }

    const auto segundos = interval.count();

    return Builder{"cada " + std::to_string(segundos) + "s",
                   [segundos](std::int64_t from) { return from + segundos; }};
}

// Todos los dias a esa hora local. Formato "HH:MM".
inline Builder dailyAt(const std::string& hhmm) {
    if (hhmm.size() != 5 || hhmm[2] != ':') {
        throw std::invalid_argument("syrax::schedule::dailyAt: la hora se escribe \"HH:MM\", no '" +
                                    hhmm + "'");
    }

    const int hour   = std::stoi(hhmm.substr(0, 2));
    const int minute = std::stoi(hhmm.substr(3, 2));

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        throw std::invalid_argument("syrax::schedule::dailyAt: '" + hhmm + "' no es una hora");
    }

    return Builder{"a las " + hhmm, [hour, minute](std::int64_t from) {
                       const auto hoy = detail::atLocalTime(detail::midnightOf(from), hour, minute);
                       if (hoy > from) return hoy;

                       // El dia siguiente se calcula desde el mediodia y no
                       // sumando 86400: con cambio de hora, medianoche + 24h
                       // puede caer en el mismo dia o saltarse uno.
                       const auto mañana = detail::midnightOf(from + 86400 + 43200);
                       return detail::atLocalTime(mañana, hour, minute);
                   }};
}

// Cada hora, en ese minuto.
inline Builder hourlyAt(int minute) {
    if (minute < 0 || minute > 59) {
        throw std::invalid_argument("syrax::schedule::hourlyAt: el minuto va de 0 a 59");
    }

    return Builder{"al minuto " + std::to_string(minute), [minute](std::int64_t from) {
                       std::time_t t = static_cast<std::time_t>(from);
                       std::tm     parts{};
                       ::localtime_r(&t, &parts);

                       parts.tm_min   = minute;
                       parts.tm_sec   = 0;
                       parts.tm_isdst = -1;

                       const auto esta = static_cast<std::int64_t>(::mktime(&parts));
                       if (esta > from) return esta;

                       parts.tm_hour += 1;
                       parts.tm_isdst = -1;
                       return static_cast<std::int64_t>(::mktime(&parts));
                   }};
}

// Olvida lo registrado. Existe para los tests.
inline void reset() { detail::entries().clear(); }

// Cuantas tareas hay. Util para comprobar el bootstrap sin levantar nada.
inline std::size_t size() { return detail::entries().size(); }

// Las etiquetas de lo registrado, para `syrax schedule:list`.
inline std::vector<std::string> labels() {
    std::vector<std::string> out;
    for (const auto& entry : detail::entries()) out.push_back(entry.label);
    return out;
}

namespace detail {

// Una vuelta del bucle: encola lo que haya vencido y devuelve cuantas.
//
// Esta separada de loop() para poder probarla sin esperar a un reloj de
// verdad, que es la unica forma de que un test de esto no tarde minutos.
inline drogon::Task<int> tick(std::int64_t at) {
    int encoladas = 0;

    for (auto& entry : entries()) {
        if (at < entry.at) continue;

        // El proximo vencimiento se calcula ANTES de encolar: si dispatch
        // lanza, la tarea sigue avanzando en vez de reintentar en cada vuelta
        // del bucle contra una base que no responde.
        entry.at = entry.next(at);

        std::cout << "  encolando " << entry.label << std::flush;
        co_await entry.fire();
        std::cout << "  ok\n";

        ++encoladas;
    }
    co_return encoladas;
}

inline std::atomic<bool>& running() {
    static std::atomic<bool> flag{true};
    return flag;
}

inline void stopGracefully() {
    if (!running()) std::_Exit(130);

    running() = false;
    std::cout << "\n  scheduler detenido\n" << std::flush;
}

inline drogon::Task<void> loop() {
    const auto arranque = now();
    for (auto& entry : entries()) entry.at = entry.next(arranque);

    std::cout << "\n  " << entries().size() << " tarea(s) programada(s)\n";
    for (const auto& entry : entries()) std::cout << "    " << entry.label << "\n";
    std::cout << "\n";

    while (running()) {
        co_await tick(now());

        // Un segundo de resolucion: las tres formas de programar tienen el
        // minuto como unidad mas fina, asi que sondear mas rapido solo gasta
        // vueltas y mas lento se saltaria un minuto exacto.
        co_await drogon::sleepCoro(drogon::app().getLoop(), 1.0);
    }
    co_return;
}

}  // namespace detail

// El scheduler. Mismo patron que jobs::work(): levanta el loop de Drogon sin
// escuchar en ningun puerto, para que encolar pueda usar la base y el cache.
inline int work() {
    if (detail::entries().empty()) {
        std::cerr << "\nerror: no hay ninguna tarea programada.\n\n"
                  << "  Declaralas en bootstrap con syrax::schedule::every(...),\n"
                  << "  dailyAt(\"HH:MM\") o hourlyAt(N), y llama a .dispatch(TuJob{}).\n\n";
        return 1;
    }

    detail::running() = true;

    drogon::app().setTermSignalHandler(detail::stopGracefully);
    drogon::app().setIntSignalHandler(detail::stopGracefully);

    drogon::app().registerBeginningAdvice([] {
        drogon::async_run([]() -> drogon::Task<void> {
            co_await detail::loop();
            drogon::app().quit();
            co_return;
        });
    });

    drogon::app().run();
    return 0;
}

// Lista lo programado sin levantar nada.
inline int list() {
    if (detail::entries().empty()) {
        std::cout << "\n  no hay tareas programadas\n\n";
        return 0;
    }

    std::cout << "\n";
    for (const auto& label : labels()) std::cout << "  " << label << "\n";
    std::cout << "\n  " << detail::entries().size() << " tarea(s)\n\n";
    return 0;
}

}  // namespace syrax::schedule
