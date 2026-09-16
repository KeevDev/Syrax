#pragma once

// El cliente manda POST /pedidos, la red se corta antes de que vuelva la
// respuesta, el cliente reintenta. Ahora hay dos pedidos y se ha cobrado dos
// veces. No es un caso raro: es lo que pasa cada vez que un movil cambia de
// wifi a datos a mitad de una peticion.
//
// La solucion que usa la industria es que el cliente mande una clave propia y
// el servidor recuerde la respuesta:
//
//   POST /pedidos
//   Idempotency-Key: 7f3c...
//
// La primera vez se ejecuta y se guarda el resultado; el reintento con la
// misma clave devuelve esa misma respuesta guardada, sin volver a ejecutar
// nada. Finito: es una clave, una respuesta y un TTL.
//
// Tres detalles que son la diferencia entre que esto funcione y que haga daño:
//
//   - **La misma clave con OTRO cuerpo es un error, no un replay.** Si no, un
//     cliente que reusa la clave por descuido recibe la respuesta de un pedido
//     distinto y se queda tan tranquilo. Por eso se guarda tambien una huella
//     del cuerpo y se compara.
//   - **Dos peticiones simultaneas con la misma clave.** La segunda no puede
//     ejecutarse ni puede esperar indefinidamente: recibe un 409 y reintenta.
//     Sin esto, la condicion de carrera que se venia a cerrar sigue abierta.
//   - **Un 5xx suelta la clave.** Si el servidor fallo por su cuenta, el
//     cliente TIENE que poder reintentar; dejar la clave tomada convertiria un
//     error transitorio en un bloqueo hasta que venza el TTL.
//
// Necesita Redis: la expiracion por clave ya la trae y montarla sobre una
// tabla significaria barrerla a mano.

#include <syrax/cache.hpp>
#include <syrax/middleware.hpp>
#include <syrax/result.hpp>

#include <glaze/glaze.hpp>
#include <openssl/sha.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace syrax::idempotency {

struct Options {
    // Cuanto se recuerda una respuesta. 24h es lo que usa casi todo el mundo:
    // de sobra para los reintentos de un cliente y poco para que la memoria
    // crezca sin control.
    std::chrono::seconds ttl{60 * 60 * 24};

    // La cabecera. Se deja configurable porque hay APIs que ya traen la suya.
    std::string header = "Idempotency-Key";

    // Solo los metodos que cambian estado. Un GET ya es idempotente por
    // definicion y guardarle la respuesta seria un cache, que es otra cosa.
    std::vector<std::string> methods{"POST", "PATCH"};

    // El Redis donde se guarda.
    std::string on = "default";
};

// Lo que se guarda bajo la clave. `status == 0` marca "en curso": la peticion
// que la tomo todavia no ha terminado.
struct Stored {
    std::string fingerprint;
    int         status = 0;
    std::string body;
};

enum class Claim {
    Owned,        // la clave es nuestra, adelante
    InProgress,   // otra peticion la tiene y no ha terminado
    Replay,       // ya hay respuesta guardada: devolverla
    Mismatch,     // misma clave, otro cuerpo
    Unavailable,  // Redis no responde
};

struct ClaimResult {
    Claim  state = Claim::Unavailable;
    Stored stored;
};

namespace detail {

inline std::string sha256Hex(std::string_view data, std::size_t bytes = 32) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);

    static constexpr char kHex[] = "0123456789abcdef";

    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes && i < SHA256_DIGEST_LENGTH; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 0x0F];
    }
    return out;
}

// La clave del cliente no se usa cruda: viene de fuera y puede traer cualquier
// cosa, incluidos espacios y saltos de linea que romperian el comando de
// Redis. El hash la normaliza y de paso la acota.
inline std::string keyFor(std::string_view given) {
    return "syrax:idem:" + sha256Hex(given, 16);
}

}  // namespace detail

// Toma la clave si esta libre, y si no dice por que no.
//
// El SET NX es lo que hace esto seguro: o lo pone esta peticion o no lo pone
// nadie mas. Comprobar con GET y luego escribir dejaria la ventana entre las
// dos, que es exactamente la carrera que se viene a cerrar.
inline drogon::Task<ClaimResult> claim(const std::string& given, const std::string& fingerprint,
                                       std::chrono::seconds ttl, const std::string& on) {
    const auto key = detail::keyFor(given);

    Stored enCurso{.fingerprint = fingerprint, .status = 0, .body = ""};
    std::string marca;
    if (glz::write_json(enCurso, marca)) co_return ClaimResult{.state = Claim::Unavailable};

    try {
        auto redis = cache::client(on);

        const auto puesto = co_await redis->execCommandCoro(
            "set %s %s nx ex %d", key.c_str(), marca.c_str(), static_cast<int>(ttl.count()));

        if (!puesto.isNil()) co_return ClaimResult{.state = Claim::Owned};

        // Ya estaba: o hay respuesta guardada, o alguien la tiene en curso.
        const auto existente = co_await redis->execCommandCoro("get %s", key.c_str());
        if (existente.isNil()) {
            // Vencio entre el SET y el GET. Tratarlo como en curso hace que el
            // cliente reintente, que es lo seguro.
            co_return ClaimResult{.state = Claim::InProgress};
        }

        Stored previa;
        if (glz::read_json(previa, existente.asString())) {
            co_return ClaimResult{.state = Claim::Unavailable};
        }

        if (previa.fingerprint != fingerprint) {
            co_return ClaimResult{.state = Claim::Mismatch, .stored = previa};
        }
        if (previa.status == 0) co_return ClaimResult{.state = Claim::InProgress, .stored = previa};

        co_return ClaimResult{.state = Claim::Replay, .stored = previa};
    } catch (const std::exception&) {
        co_return ClaimResult{.state = Claim::Unavailable};
    }
}

// Guarda la respuesta bajo la clave, conservando lo que quede de TTL.
inline drogon::Task<void> save(const std::string& given, const Stored& value,
                               std::chrono::seconds ttl, const std::string& on) {
    std::string payload;
    if (glz::write_json(value, payload)) co_return;

    try {
        // keepttl conserva la expiracion que puso el claim: sin eso, cada
        // guardado reiniciaria las 24h y una clave muy reintentada no
        // caducaria nunca.
        co_await cache::client(on)->execCommandCoro("set %s %s keepttl", detail::keyFor(given).c_str(),
                                                    payload.c_str());
    } catch (const std::exception&) {
        // Que no se pueda guardar no puede tumbar una peticion que ya se
        // ejecuto bien. El precio es que un reintento la repita, que es
        // exactamente donde estabamos antes de tener esto.
    }
    co_return;
}

// Suelta la clave. Se llama cuando la peticion acabo en 5xx.
inline drogon::Task<void> release(const std::string& given, const std::string& on) {
    try {
        co_await cache::client(on)->execCommandCoro("del %s", detail::keyFor(given).c_str());
    } catch (const std::exception&) {
    }
    co_return;
}

}  // namespace syrax::idempotency
