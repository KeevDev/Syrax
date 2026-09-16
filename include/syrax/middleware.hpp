#pragma once

#include <drogon/drogon.h>

#include <syrax/cache.hpp>
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
// Sincrono: cubre CORS, cabeceras, rate limiting en proceso y verificacion de
// tokens sin pagar nada. Es el que hay que usar mientras sirva.
using Middleware = std::function<std::optional<Error>(Request&)>;

// El mismo contrato, pero pudiendo esperar. Existe porque un limite de peticiones
// compartido entre instancias tiene que preguntarle a Redis, y bloquear el hilo
// del event loop para eso convierte el limitador en el cuello de botella que
// venia a evitar.
//
// Corre DESPUES de todos los sincronos, a proposito: lo que se puede rechazar
// sin salir del proceso se rechaza antes de gastar una ida y vuelta a la red.
using AsyncMiddleware = std::function<drogon::Task<std::optional<Error>>(Request&)>;

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
// Por quien se cuenta. Por defecto la IP, que es lo que sirve contra el abuso
// anonimo; una API autenticada casi siempre quiere contar por clave o por
// usuario, y eso no lo puede adivinar el framework.
using RateKey = std::function<std::string(const Request&)>;

namespace detail {

inline RateKey keyByIp() {
    return [](const Request& request) { return request.ip(); };
}

}  // namespace detail

// El limite compartido entre instancias.
//
// El de arriba cuenta en un mapa del proceso, asi que con tres replicas detras
// de un balanceador el limite real es el triple del configurado: cada una deja
// pasar su cuota entera. Con el contador en Redis las tres miran el mismo
// numero, que es lo unico que hace que "120 por minuto" signifique 120.
//
// La ventana es fija y va dentro de la clave. Eso vale un detalle que conviene
// saber: en el peor caso —una rafaga al final de una ventana y otra al
// principio de la siguiente— pasan hasta 2x el limite en un intervalo corto.
// La alternativa es una ventana deslizante con un sorted set, que cuesta una
// entrada por peticion y un ZREMRANGEBYSCORE en cada una. Para proteger una
// API de abuso, la ventana fija sobra; si algun dia hace falta la otra, que
// entre como una funcion aparte y no como una opcion de esta.
//
// Si Redis no responde, la peticion PASA. Es deliberado: un limitador que
// tumba la API cuando se cae su almacen convierte una degradacion en una
// caida, y el limite existe para proteger la API, no para ser otro motivo de
// que no funcione.
inline AsyncMiddleware rateLimitShared(int maxRequests, std::chrono::seconds window,
                                       RateKey key = {}, std::string on = "default") {
    if (!key) key = detail::keyByIp();

    return [maxRequests, window, key, on](Request& request)
               -> drogon::Task<std::optional<Error>> {
        // El indice de la ventana va en la clave, asi que la clave rota sola y
        // el EXPIRE de cada peticion es idempotente. Con una clave fija habria
        // que ponerlo solo en la primera, y si esa llamada se pierde el cliente
        // queda bloqueado para siempre.
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

        const auto bucket = now / window.count();
        const auto clave  = "syrax:rl:" + key(request) + ":" + std::to_string(bucket);

        try {
            auto redis = cache::client(on);

            const auto count = (co_await redis->execCommandCoro("incr %s", clave.c_str())).asInteger();
            co_await redis->execCommandCoro("expire %s %d", clave.c_str(),
                                            static_cast<int>(window.count()));

            if (count > maxRequests) {
                co_return Error{.status  = 429,
                                .message = "too many requests",
                                .code    = "rate_limited"};
            }
        } catch (const std::exception&) {
            // Ver arriba: fallar abierto es la decision, no un descuido.
            co_return std::nullopt;
        }
        co_return std::nullopt;
    };
}

inline Middleware rateLimit(int maxRequests, std::chrono::milliseconds window,
                            RateKey key = {}) {
    if (!key) key = detail::keyByIp();

    struct Bucket {
        std::chrono::steady_clock::time_point start;
        int                                   count;
    };

    auto state = std::make_shared<std::mutex>();
    auto seen  = std::make_shared<std::unordered_map<std::string, Bucket>>();

    return [state, seen, maxRequests, window, key](Request& request) -> std::optional<Error> {
        const auto now = std::chrono::steady_clock::now();

        const std::lock_guard lock{*state};

        auto& bucket = (*seen)[key(request)];
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
