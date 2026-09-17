#pragma once

// Toda API llama a otra API, y el cliente HTTP es donde se escriben siempre los
// mismos tres errores.
//
// El primero es **no poner timeout**. Drogon lo deja en cero por defecto, que
// significa esperar para siempre: el servicio de enfrente se cuelga y el tuyo
// se lleva por delante un hilo por peticion hasta que no queda ninguno. Aqui
// hay timeout desde el principio y hay que quitarlo a mano para no tenerlo.
//
// El segundo es **reintentar un POST**. Un reintento parece gratis hasta que
// la primera llamada SI llego y lo que se perdio fue la respuesta: entonces el
// reintento cobra dos veces. Por eso esto solo reintenta metodos idempotentes
// -GET, HEAD, PUT, DELETE, OPTIONS- y nunca un POST ni un PATCH. Si quieres
// reintentar un POST, el que tiene que hacerlo seguro es el de enfrente, con
// una Idempotency-Key.
//
// El tercero es **perder la traza**. Una peticion que cruza tres servicios y
// falla en el tercero no se puede reconstruir si cada salto estrena su propio
// id. `trace(request)` propaga el X-Request-Id del nivel 1, y con eso los logs
// de los tres servicios se cruzan por un solo valor.
//
//   auto pagos = syrax::http::Client{"http://pagos:8080"};
//
//   const auto respuesta = co_await pagos.trace(request)
//                                        .bearer(token)
//                                        .post("/cobros", cuerpo);
//   if (!respuesta.ok()) co_return syrax::BadGateway("el cobro no salio");
//
//   const auto cobro = respuesta.json<Cobro>();
//
// Y se planta ahi. Sin circuit breaker y sin descubrimiento de servicios: el
// primero mal ajustado convierte una degradacion parcial en una caida total, y
// el segundo lo resuelve el despliegue, que ve todas las instancias mientras
// que un proceso solo se ve a si mismo. Los dos estan en el nivel 3 del
// roadmap, con el motivo escrito.

#include <syrax/log.hpp>
#include <syrax/middleware.hpp>
#include <syrax/result.hpp>

#include <drogon/drogon.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace syrax::http {

struct Header {
    std::string name;
    std::string value;
};

struct Response {
    int                 status = 0;
    std::string         body;
    std::vector<Header> headers;

    // Vacio salvo que la peticion no llegara a completarse. Un fallo de
    // transporte no es una excepcion a proposito: llamar a otro servicio y que
    // no conteste es un resultado posible, no algo excepcional, y con `Result`
    // en todo el framework seria raro que esto fuera lo unico que lanza.
    std::string error;

    bool ok() const { return status >= 200 && status < 300; }
    bool failed() const { return !error.empty(); }

    std::string header(std::string_view name) const {
        const auto equal = [](std::string_view a, std::string_view b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) ==
                              std::tolower(static_cast<unsigned char>(y));
                   });
        };

        for (const auto& h : headers) {
            if (equal(h.name, name)) return h.value;
        }
        return {};
    }

    template <typename T>
    T json() const {
        T value{};
        (void)glz::read_json(value, body);
        return value;
    }
};

struct Options {
    // Cero seria esperar para siempre, que es el bug que esto viene a evitar.
    std::chrono::milliseconds timeout{10000};

    // Cuantos reintentos DESPUES del primer intento, y solo en metodos
    // idempotentes.
    int retries = 2;

    // La primera espera. Cada reintento dobla: 200ms, 400ms, 800ms. Crece a
    // proposito, porque reintentar al mismo ritmo contra un servicio que se
    // esta ahogando es empujarlo mas.
    std::chrono::milliseconds backoff{200};
};

namespace detail {

// Los metodos en los que repetir la llamada no cambia el resultado. Es la
// definicion de la especificacion de HTTP, no una opinion.
inline bool idempotent(drogon::HttpMethod method) {
    return method == drogon::Get || method == drogon::Head || method == drogon::Put ||
           method == drogon::Delete || method == drogon::Options;
}

// Que merece otro intento. Un 4xx no: el servidor entendio y dijo que no, y
// repetirlo da exactamente lo mismo. El 429 es la excepcion, porque dice
// "ahora no" y no "nunca".
inline bool worthRetrying(int status) {
    return status == 0 || status == 429 || status >= 500;
}

}  // namespace detail

class Client {
public:
    explicit Client(std::string baseUrl, Options options = {})
        : base_{std::move(baseUrl)}, options_{options} {}

    // Una cabecera en todas las peticiones de este cliente.
    Client& header(std::string name, std::string value) {
        defaults_.push_back({std::move(name), std::move(value)});
        return *this;
    }

    Client& bearer(const std::string& token) { return header("Authorization", "Bearer " + token); }

    // Propaga el X-Request-Id de la peticion en curso, para que la traza cruce
    // el salto. Es lo unico que hace que un fallo en el tercer servicio se
    // pueda seguir hasta la peticion que lo origino.
    Client& trace(const Request& request) {
        const auto id = log::requestId(request);
        if (!id.empty()) header(std::string{log::kRequestIdHeader}, id);
        return *this;
    }

    drogon::Task<Response> get(std::string path) {
        co_return co_await send(drogon::Get, std::move(path), {});
    }
    drogon::Task<Response> del(std::string path) {
        co_return co_await send(drogon::Delete, std::move(path), {});
    }
    drogon::Task<Response> post(std::string path, std::string body) {
        co_return co_await send(drogon::Post, std::move(path), std::move(body));
    }
    drogon::Task<Response> put(std::string path, std::string body) {
        co_return co_await send(drogon::Put, std::move(path), std::move(body));
    }
    drogon::Task<Response> patch(std::string path, std::string body) {
        co_return co_await send(drogon::Patch, std::move(path), std::move(body));
    }

    template <typename T>
    drogon::Task<Response> post(std::string path, const T& body) {
        co_return co_await send(drogon::Post, std::move(path), toJson(body));
    }
    template <typename T>
    drogon::Task<Response> put(std::string path, const T& body) {
        co_return co_await send(drogon::Put, std::move(path), toJson(body));
    }

    drogon::Task<Response> send(drogon::HttpMethod method, std::string path, std::string body) {
        // Sin el loop de Drogon levantado, sendRequestCoro nunca se reanuda: la
        // corrutina espera a un event loop que no existe y el proceso se cuelga
        // PARA SIEMPRE. Ni el timeout salva, porque su temporizador vive en ese
        // mismo loop.
        //
        // Un cuelgue sin mensaje es el peor fallo posible: no hay traza, no hay
        // error, no hay nada que buscar. Esto lo convierte en una frase.
        if (!drogon::app().isRunning()) {
            co_return Response{
                .status = 0,
                .error  = "syrax::http: el loop de Drogon no esta corriendo, asi que la "
                          "peticion no se puede completar. Llamalo desde un handler o un "
                          "job, no desde la configuracion; en un test, levanta la "
                          "aplicacion antes."};
        }

        // Un cliente POR PETICION, y no uno guardado en el objeto. Parece un
        // despilfarro -un handshake por llamada- y por eso conviene decir que
        // la alternativa es peor.
        //
        // Un HttpClient de Drogon mantiene UNA sola conexion persistente, y el
        // pipelining viene apagado de fabrica. Guardarlo como miembro no seria
        // "el mismo cliente con keep-alive": seria un unico socket para toda la
        // aplicacion, con las peticiones concurrentes apiladas en su cola
        // interna y saliendo de una en una.
        //
        // Y el temporizador del timeout arranca AL ENCOLAR, no al enviar. Con
        // cincuenta llamadas a la vez, la ultima espera detras de cuarenta y
        // nueve con sus diez segundos ya corriendo: bajo carga empiezan a
        // caducar peticiones que nunca llegaron a salir. Peor todavia, ese
        // timeout no distingue "no salio" de "salio y no contesto", que es
        // justo la ambigüedad que el resto de este archivo existe para evitar.
        //
        // El handshake es un coste visible, constante y medible. Aquello es un
        // acantilado de latencia que en desarrollo -una peticion cada vez- no
        // se ve. Cuando el handshake sea de verdad el cuello de botella, lo que
        // toca es un pool de N clientes, no compartir uno.
        auto client = drogon::HttpClient::newHttpClient(base_);

        const int intentos =
            detail::idempotent(method) ? std::max(0, options_.retries) + 1 : 1;

        Response last;
        auto     espera = options_.backoff;

        for (int intento = 1; intento <= intentos; ++intento) {
            last = co_await once(client, method, path, body);

            if (!detail::worthRetrying(last.status)) co_return last;
            if (intento == intentos) co_return last;

            co_await drogon::sleepCoro(drogon::app().getLoop(),
                                       static_cast<double>(espera.count()) / 1000.0);
            espera *= 2;
        }
        co_return last;
    }

private:
    template <typename T>
    static std::string toJson(const T& value) {
        std::string out;
        (void)glz::write_json(value, out);
        return out;
    }

    drogon::Task<Response> once(drogon::HttpClientPtr client, drogon::HttpMethod method,
                                const std::string& path, const std::string& body) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(path);

        for (const auto& h : defaults_) req->addHeader(h.name, h.value);

        if (!body.empty()) {
            req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            req->setBody(body);
        }

        Response out;
        try {
            const auto resp = co_await client->sendRequestCoro(
                req, static_cast<double>(options_.timeout.count()) / 1000.0);

            out.status = static_cast<int>(resp->statusCode());
            out.body   = std::string{resp->body()};

            for (const auto& [name, value] : resp->headers()) out.headers.push_back({name, value});
        } catch (const std::exception& e) {
            // status 0 marca "no hubo respuesta": distinto de un 500, que si
            // la hubo. Quien lee el resultado necesita poder distinguirlos.
            out.status = 0;
            out.error  = e.what();
        }
        co_return out;
    }

    std::string         base_;
    Options             options_;
    std::vector<Header> defaults_;
};

}  // namespace syrax::http
