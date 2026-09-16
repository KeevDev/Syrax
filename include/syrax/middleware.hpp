#pragma once

#include <drogon/drogon.h>

#include <syrax/result.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace syrax {

// Lo que ve un middleware. Envuelve el request de Drogon para dar una
// superficie chica y estable, sin cerrar la puerta: drogon() devuelve el
// objeto crudo.
class Request {
public:
    explicit Request(const drogon::HttpRequestPtr& request) : request_{request} {}

    std::string method() const { return request_->methodString(); }
    std::string path() const { return std::string{request_->path()}; }

    std::string header(const std::string& name) const { return request_->getHeader(name); }
    std::string query(const std::string& name) const { return request_->getParameter(name); }
    std::string ip() const { return request_->peerAddr().toIp(); }

    // Estado por request. Vive en el objeto request, no en el hilo, asi que
    // sobrevive a los co_await: las corrutinas migran de hilo y un
    // thread_local aqui seria un bug silencioso.
    void set(const std::string& key, const std::string& value) {
        request_->attributes()->insert(key, value);
    }

    bool has(const std::string& key) const { return request_->attributes()->find(key); }

    std::string get(const std::string& key, std::string fallback = {}) const {
        if (!has(key)) return fallback;
        return request_->attributes()->get<std::string>(key);
    }

    const drogon::HttpRequestPtr& drogon() const { return request_; }

private:
    drogon::HttpRequestPtr request_;
};

// Devuelve nullopt para dejar pasar, o un Error para cortar la cadena.
// Sincrono a proposito: cubre CORS, cabeceras, rate limiting y verificacion
// de tokens sin arrastrar la complejidad de una cadena asincrona. Un
// middleware que necesite consultar la base de datos todavia no cabe aqui.
using Middleware = std::function<std::optional<Error>(Request&)>;

// Para modificar la respuesta ya construida (cabeceras de seguridad, CORS).
using ResponseMiddleware = std::function<void(const drogon::HttpResponsePtr&)>;

namespace detail {

// Drogon ya tiene renderizada la respuesta para cuando corren sus advices de
// pre-sending, asi que addHeader() ahi no llega al cliente (se comprobo: el
// advice dispara, la cabecera no sale). Por eso la cadena se aplica cuando
// Syrax construye la respuesta, y no despues.
//
// Es estado global, que normalmente evitariamos; aqui es aceptable porque
// drogon::app() ya es un singleton y solo hay una aplicacion por proceso.
inline std::vector<ResponseMiddleware>& responseChain() {
    static std::vector<ResponseMiddleware> chain;
    return chain;
}

}  // namespace detail

// Se aplica a TODA respuesta que sale del framework, incluidos los errores.
// Que las cabeceras de seguridad y CORS no dependan de si alguien sobrescribio
// el formato del error es justo el punto.
inline void applyResponseChain(const drogon::HttpResponsePtr& response) {
    for (const auto& fn : detail::responseChain()) fn(response);
}

struct CorsOptions {
    std::vector<std::string> origins{"*"};
    std::vector<std::string> methods{"GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
    std::vector<std::string> headers{"Content-Type", "Authorization"};
    bool                     credentials = false;
    int                      maxAge      = 86400;
};

namespace detail {

inline std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out += parts[i];
        if (i + 1 < parts.size()) out += sep;
    }
    return out;
}

}  // namespace detail

// ---------------------------------------------------------- middlewares

// Cabeceras que mitigan ataques comunes. Son gratis y casi siempre se
// olvidan, asi que conviene tenerlas a un metodo de distancia.
inline ResponseMiddleware securityHeaders() {
    return [](const drogon::HttpResponsePtr& response) {
        response->addHeader("X-Content-Type-Options", "nosniff");
        response->addHeader("X-Frame-Options", "DENY");
        response->addHeader("Referrer-Policy", "no-referrer");
        response->addHeader("Cross-Origin-Opener-Policy", "same-origin");
        response->addHeader("Cross-Origin-Resource-Policy", "same-origin");
    };
}

// Limita peticiones por IP en una ventana deslizante simple.
//
// El contador vive en memoria del proceso: con varias instancias cada una
// lleva su propia cuenta. Para un limite compartido hace falta Redis, que
// esta fuera de alcance.
inline Middleware rateLimit(int maxRequests, std::chrono::milliseconds window) {
    struct Bucket {
        std::chrono::steady_clock::time_point start;
        int                                   count;
    };

    auto state = std::make_shared<std::mutex>();
    auto seen  = std::make_shared<std::unordered_map<std::string, Bucket>>();

    return [state, seen, maxRequests, window](Request& request) -> std::optional<Error> {
        const auto now = std::chrono::steady_clock::now();
        const auto ip  = request.ip();

        const std::lock_guard lock{*state};

        auto& bucket = (*seen)[ip];
        if (bucket.count == 0 || now - bucket.start > window) {
            bucket = Bucket{.start = now, .count = 0};
        }

        if (++bucket.count > maxRequests) {
            return Error{429, "too many requests"};
        }
        return std::nullopt;
    };
}

// Exige una clave en una cabecera. Util para APIs internas o webhooks.
inline Middleware requireApiKey(std::string expected, std::string headerName = "X-Api-Key") {
    return [expected = std::move(expected),
            headerName = std::move(headerName)](Request& request) -> std::optional<Error> {
        const auto given = request.header(headerName);
        if (given.empty()) return Unauthorized("missing " + headerName);

        // Comparacion de tiempo constante: comparar con != filtra el secreto
        // por el tiempo de respuesta.
        if (given.size() != expected.size()) return Unauthorized("invalid api key");

        unsigned char diff = 0;
        for (std::size_t i = 0; i < given.size(); ++i) {
            diff |= static_cast<unsigned char>(given[i] ^ expected[i]);
        }
        if (diff != 0) return Unauthorized("invalid api key");

        return std::nullopt;
    };
}

}  // namespace syrax
