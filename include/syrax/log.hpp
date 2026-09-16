#pragma once

#include <drogon/drogon.h>

#include <syrax/env.hpp>
#include <syrax/middleware.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace syrax::log {

// El log de Syrax existe por una razon concreta: poder encontrar UNA peticion.
// Sin un identificador que viaje con ella, un 500 reportado por un usuario no
// se puede buscar, y un job que falla no se puede atar a la peticion que lo
// encolo.
//
// Por eso la pieza central no es el formato ni los niveles, sino el request-id.

enum class Level { Debug, Info, Warn, Error };

struct Field {
    std::string key;
    std::string value;
};

// ------------------------------------------------------------------ formato
//
// Dos formatos, y la eleccion se hace sola: delante de una terminal se escribe
// para un humano y con color; detras de un pipe —un contenedor, el CI, un
// recolector de logs— se escribe una linea JSON por evento, que es lo que esas
// herramientas saben leer. LOG_FORMAT=json|text fuerza uno de los dos.
enum class Format { Text, Json };

namespace detail {

inline bool isTty() { return ::isatty(STDOUT_FILENO) == 1; }

inline Format resolveFormat() {
    const auto choice = syrax::env("LOG_FORMAT", "");
    if (choice == "json") return Format::Json;
    if (choice == "text") return Format::Text;
    return isTty() ? Format::Text : Format::Json;
}

inline Format& format() {
    static Format value = resolveFormat();
    return value;
}

inline Level levelFromName(const std::string& name, Level fallback) {
    if (name == "debug") return Level::Debug;
    if (name == "info")  return Level::Info;
    if (name == "warn")  return Level::Warn;
    if (name == "error") return Level::Error;
    return fallback;
}

inline Level& minimum() {
    static Level value = levelFromName(syrax::env("LOG_LEVEL", "info"), Level::Info);
    return value;
}

inline std::string_view levelName(Level level) {
    switch (level) {
        case Level::Debug: return "debug";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "info";
}

// El color se usa solo en modo texto, que solo se elige cuando hay terminal.
inline std::string_view levelColor(Level level) {
    switch (level) {
        case Level::Debug: return "\033[90m";
        case Level::Info:  return "\033[36m";
        case Level::Warn:  return "\033[33m";
        case Level::Error: return "\033[31m";
    }
    return "";
}

inline std::string_view statusColor(int status) {
    if (status >= 500) return "\033[31m";
    if (status >= 400) return "\033[33m";
    if (status >= 300) return "\033[36m";
    return "\033[32m";
}

inline std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[7];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

inline std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::milliseconds>(
                           std::chrono::time_point_cast<std::chrono::milliseconds>(now)));
}

// Una sola escritura por linea. Sin esto, dos hilos que loguean a la vez
// entrelazan sus caracteres y el resultado no lo lee ni una maquina ni nadie.
inline void write(std::string line) {
    static std::mutex boca;
    std::lock_guard<std::mutex> cerrada{boca};
    std::cout << line << '\n';
    std::cout.flush();
}

}  // namespace detail

inline void format(Format value) { detail::format() = value; }
inline void level(Level value)   { detail::minimum() = value; }

// ------------------------------------------------------------------ emitir

inline void emit(Level level, std::string_view message, std::initializer_list<Field> fields = {}) {
    if (static_cast<int>(level) < static_cast<int>(detail::minimum())) return;

    if (detail::format() == Format::Json) {
        std::string line = "{\"ts\":\"" + detail::timestamp() + "\",\"level\":\"" +
                           std::string{detail::levelName(level)} + "\",\"msg\":\"" +
                           detail::escape(message) + "\"";
        for (const auto& field : fields) {
            line += ",\"" + detail::escape(field.key) + "\":\"" + detail::escape(field.value) + "\"";
        }
        detail::write(line + "}");
        return;
    }

    std::string line = std::string{detail::levelColor(level)} +
                       std::format("{:>5}", detail::levelName(level)) + "\033[0m  " +
                       std::string{message};
    for (const auto& field : fields) {
        line += "  \033[90m" + field.key + "=\033[0m" + field.value;
    }
    detail::write(line);
}

inline void debug(std::string_view m, std::initializer_list<Field> f = {}) { emit(Level::Debug, m, f); }
inline void info(std::string_view m, std::initializer_list<Field> f = {})  { emit(Level::Info, m, f); }
inline void warn(std::string_view m, std::initializer_list<Field> f = {})  { emit(Level::Warn, m, f); }
inline void error(std::string_view m, std::initializer_list<Field> f = {}) { emit(Level::Error, m, f); }

// ------------------------------------------------------------- el request-id

// Corto a proposito: no es un secreto ni una clave primaria, es algo que un
// humano tiene que poder leer en voz alta por telefono y pegar en un buscador.
inline std::string newRequestId() {
    static constexpr std::string_view alfabeto = "0123456789abcdefghijklmnopqrstuvwxyz";
    static std::atomic<std::uint64_t> semilla{
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};

    thread_local std::mt19937_64 motor{semilla.fetch_add(0x9e3779b97f4a7c15ULL)};

    std::string id(12, '0');
    for (char& c : id) c = alfabeto[motor() % alfabeto.size()];
    return id;
}

// La clave con la que el id viaja en los atributos del request. Es publica
// porque un job encolado desde un handler la necesita para heredarlo.
inline constexpr std::string_view kRequestIdKey = "request.id";
inline constexpr std::string_view kRequestIdHeader = "X-Request-Id";

// Lee el id de una peticion en curso. Vacio si no hay.
inline std::string requestId(const Request& request) {
    return request.get(std::string{kRequestIdKey});
}

namespace detail {

inline constexpr std::string_view kStartedKey = "request.started";

inline std::string micros() {
    return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

inline bool& accessEnabled() {
    static bool value = syrax::envBool("LOG_ACCESS", true);
    return value;
}

}  // namespace detail

inline void access(bool enabled) { detail::accessEnabled() = enabled; }

// Registra el id y el log de acceso. Lo llama App::run() por su cuenta: la
// trazabilidad que hay que acordarse de encender es la que no esta cuando
// hace falta.
inline void install() {
    // Un id que venga de fuera se respeta: si el gateway o el servicio que
    // llama ya puso uno, la traza tiene que cruzar el salto entera.
    drogon::app().registerPreHandlingAdvice(
        [](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&&,
           drogon::AdviceChainCallback&& next) {
            auto heredado = req->getHeader(std::string{kRequestIdHeader});
            req->attributes()->insert(std::string{kRequestIdKey},
                                      heredado.empty() ? newRequestId() : std::move(heredado));
            req->attributes()->insert(std::string{detail::kStartedKey}, detail::micros());
            next();
        });

    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            std::string id;
            if (req->attributes()->find(std::string{kRequestIdKey})) {
                id = req->attributes()->get<std::string>(std::string{kRequestIdKey});
            }

            // El cliente se lleva el id en la respuesta. Sin esto, quien
            // reporta el fallo no tiene nada que citar.
            if (!id.empty()) resp->addHeader(std::string{kRequestIdHeader}, id);

            if (!detail::accessEnabled()) return;

            std::string ms = "?";
            if (req->attributes()->find(std::string{detail::kStartedKey})) {
                const auto desde = std::stoll(
                    req->attributes()->get<std::string>(std::string{detail::kStartedKey}));
                const auto hasta = std::stoll(detail::micros());
                ms = std::format("{:.1f}", static_cast<double>(hasta - desde) / 1000.0);
            }

            const int status = static_cast<int>(resp->statusCode());

            if (detail::format() == Format::Json) {
                emit(status >= 500 ? Level::Error : Level::Info, "request",
                     {{"method", std::string{req->methodString()}},
                      {"path", std::string{req->path()}},
                      {"status", std::to_string(status)},
                      {"ms", ms},
                      {"ip", req->peerAddr().toIp()},
                      {"request_id", id}});
                return;
            }

            // En terminal la linea se escribe para leerla de un vistazo
            // mientras llegan: el estado en color manda, el resto acompana.
            detail::write(std::format("{}{:>3}\033[0m  {:<6} {:<38} \033[90m{:>7}ms  {}\033[0m",
                                      detail::statusColor(status), status,
                                      std::string{req->methodString()}, std::string{req->path()},
                                      ms, id));
        });
}

}  // namespace syrax::log
