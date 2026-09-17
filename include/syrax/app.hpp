#pragma once

#include <drogon/drogon.h>
#include <glaze/glaze.hpp>
#include <openssl/sha.h>

#include <syrax/db.hpp>
#include <syrax/errors.hpp>
#include <syrax/fields.hpp>
#include <syrax/health.hpp>
#include <syrax/idempotency.hpp>
#include <syrax/log.hpp>
#include <syrax/metrics.hpp>
#include <syrax/middleware.hpp>
#include <syrax/validation.hpp>
#include <syrax/ws.hpp>
#include <syrax/openapi.hpp>
#include <syrax/result.hpp>
#include <syrax/traits.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <type_traits>

namespace syrax {

// Un handler asincrono devuelve Task<Result<T>>. Es un alias de drogon::Task
// para que el codigo de aplicacion no tenga que nombrar a Drogon.
template <typename T>
using Task = drogon::Task<T>;

// El puerto donde escuchar, resuelto igual que las credenciales: del entorno,
// con .env de respaldo. Asi el puerto no queda escrito en el binario y cambia
// por despliegue sin recompilar.
//
//   APP_PORT=3000 ./mi-api
//
// Carga el .env por su cuenta para que no importe si se llama antes o despues
// de configureFromEnv(): loadDotEnv nunca pisa una variable ya existente.
//
// Un valor invalido no se ignora en silencio: quien escribio APP_PORT=ocho
// quiso decir algo, y arrancar en el 8080 como si nada le esconde el error.
inline std::uint16_t envPort(std::uint16_t fallback = 8080) {
    db::loadDotEnv();

    const auto raw = db::env("APP_PORT", "");
    if (raw.empty()) return fallback;

    unsigned   value = 0;
    const auto end   = raw.data() + raw.size();
    const auto result = std::from_chars(raw.data(), end, value);

    if (result.ec != std::errc{} || result.ptr != end || value == 0 || value > 65535) {
        std::cerr << "syrax: APP_PORT='" << raw << "' no es un puerto valido, usando "
                  << fallback << "\n";
        return fallback;
    }
    return static_cast<std::uint16_t>(value);
}

namespace detail {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

// Un tipo cuenta como "cuerpo de request" si es un struct propio del usuario.
// std::string se excluye: un handler que pide un string quiere el path param,
// no el body.
template <typename T>
concept BodyLike = std::is_class_v<T> && !std::is_same_v<T, std::string>;

// Glaze por defecto acepta objetos incompletos y deja los campos ausentes
// default-construidos. Para una API eso es inaceptable: un body sin "email"
// no es un body valido.
inline constexpr glz::opts kStrict{.error_on_missing_keys = true};

// Un handler puede devolver Result<T> (sincrono) o Task<Result<T>>
// (corrutina). Lo segundo es lo que permite hacer I/O de base de datos sin
// bloquear el event loop.
template <typename T>
struct IsTask : std::false_type {};

template <typename T>
struct IsTask<drogon::Task<T>> : std::true_type {};

template <typename T>
inline constexpr bool kIsTask = IsTask<std::remove_cvref_t<T>>::value;

// Desenvuelve el tipo que el handler realmente devuelve:
//   Result<User>        -> User
//   Task<Result<User>>  -> User
// Es lo que se necesita para sacarle el esquema a la respuesta.
template <typename T>
struct ResultValue;

template <typename T>
struct ResultValue<Result<T>> {
    using type = T;
};

template <typename T>
struct ResultValue<drogon::Task<Result<T>>> {
    using type = T;
};

template <typename T>
using ResultValueT = typename ResultValue<std::remove_cvref_t<T>>::type;

template <typename T>
std::string schemaOf() {
    std::string out;
    if (glz::write_json_schema<T>(out)) return {};

    // Si el tipo declara reglas, sus limites entran al esquema aqui. Es el
    // unico punto por el que pasan todos los esquemas, asi que documentar
    // desde aca vale tanto para el OpenAPI como para el Swagger.
    if constexpr (Validatable<T>) {
        Json::Value  parsed;
        Json::Reader reader;
        if (!reader.parse(out, parsed)) return out;

        annotateSchema<T>(parsed);
        return Json::writeString(Json::StreamWriterBuilder{}, parsed);
    }
    return out;
}

// El tipo JSON con el que se documenta un path param. Drogon lo recibe como
// texto siempre, pero el handler declara que espera, y eso es lo que el
// cliente necesita saber: /users/{id} con un int64 es integer, no string.
template <typename T>
std::string jsonTypeName() {
    using Param = std::remove_cvref_t<T>;

    if constexpr (std::is_same_v<Param, bool>)          return "boolean";
    else if constexpr (std::is_floating_point_v<Param>) return "number";
    else if constexpr (std::is_integral_v<Param>)       return "integer";
    else                                                return "string";
}

template <typename... Params>
std::vector<std::string> paramTypeNames(std::tuple<Params...>*) {
    return {jsonTypeName<Params>()...};
}

template <typename... Params>
bool allPresent(const std::tuple<std::optional<Params>...>& params) {
    // Con cero params el fold sobre pack vacio da true.
    return std::apply([](const auto&... o) { return (o.has_value() && ...); }, params);
}

// Un `&&` en una condicion de `if constexpr` NO protege a sus operandos de
// instanciarse: `tuple_element_t<N - 1, T>` con N == 0 desborda a SIZE_MAX y
// el programa no compila. Hay que cortar con un `if constexpr` de verdad.
template <typename Tuple>
constexpr bool hasTrailingBody() {
    if constexpr (std::tuple_size_v<Tuple> == 0) {
        return false;
    } else {
        return BodyLike<std::tuple_element_t<std::tuple_size_v<Tuple> - 1, Tuple>>;
    }
}

// Un handler puede pedir el Request como PRIMER argumento para leer
// cabeceras o lo que haya dejado un middleware (el usuario autenticado, por
// ejemplo). Es opcional: la mayoria de los handlers no lo necesita.
template <typename Tuple>
constexpr bool hasLeadingRequest() {
    if constexpr (std::tuple_size_v<Tuple> == 0) {
        return false;
    } else {
        return std::is_same_v<std::tuple_element_t<0, Tuple>, Request>;
    }
}

template <typename Tuple, std::size_t... I>
std::tuple<std::tuple_element_t<I + 1, Tuple>...> dropFirstHelper(std::index_sequence<I...>);

// std::conditional_t instancia SUS DOS ramas, aunque solo use una. Con una
// tupla vacia, `size - 1` desborda a SIZE_MAX y el programa no compila. Por
// eso el tamano se acota aqui en vez de confiar en el cortocircuito.
template <typename Tuple>
inline constexpr std::size_t kDropFirstSize =
    std::tuple_size_v<Tuple> > 0 ? std::tuple_size_v<Tuple> - 1 : 0;

template <typename Tuple>
using DropFirst = decltype(dropFirstHelper<Tuple>(
    std::make_index_sequence<kDropFirstSize<Tuple>>{}));

// Quita el ultimo elemento de una tupla de tipos. Sirve para separar
// "los primeros N argumentos son path params, el ultimo es el body".
template <typename Tuple, std::size_t... I>
std::tuple<std::tuple_element_t<I, Tuple>...> dropLastHelper(std::index_sequence<I...>);

template <typename Tuple>
using DropLast = decltype(dropLastHelper<Tuple>(
    std::make_index_sequence<std::tuple_size_v<Tuple> - 1>{}));

// Los path params llegan siempre como texto; Syrax hace la conversion para
// controlar el contrato de error en vez de dejar que Drogon lance.
template <typename>
using AsString = std::string;

template <typename T>
std::optional<T> convertParam(const std::string& s) {
    if constexpr (std::is_same_v<T, std::string>) {
        return s;
    } else {
        T          out{};
        const auto begin = s.data();
        const auto end   = s.data() + s.size();
        const auto res   = std::from_chars(begin, end, out);
        if (res.ec != std::errc{} || res.ptr != end) return std::nullopt;
        return out;
    }
}

// Todo error sale por syrax::render(), que es donde la aplicacion puede
// haber puesto su propio formato con onError().
// La huella de un cuerpo. SHA-256 truncado a 16 bytes: de sobra para que dos
// cuerpos distintos no colisionen, y la mitad de cabecera que el hash entero.
//
// Va entre comillas porque la especificacion lo pide asi, y un ETag sin ellas
// lo rechazan algunos intermediarios en silencio.
inline std::string etagOf(std::string_view body) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest);

    static constexpr char kHex[] = "0123456789abcdef";

    std::string out;
    out.reserve(34);
    out += '"';
    for (int i = 0; i < 16; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 0x0F];
    }
    out += '"';
    return out;
}

// Donde viaja la clave de idempotencia entre los dos advices.
inline constexpr std::string_view kIdempotencyKey = "syrax.idempotency.key";

inline drogon::HttpResponsePtr makeError(int status, std::string message) {
    return render(Error{status, std::move(message)});
}

// Un 422 de validacion lleva el detalle por campo. El `message` se mantiene
// para que un cliente que solo lee `error.message` siga funcionando, y pasa
// por el mismo render que el resto: si hay un formato propio, este tambien.
inline drogon::HttpResponsePtr makeValidationError(std::vector<FieldError> fields) {
    return render(Error{.status  = 422,
                        .message = "validation failed",
                        .fields  = std::move(fields)});
}

template <typename T>
drogon::HttpResponsePtr makeOk(const T& value, int status) {
    std::string out;
    if (glz::write_json(value, out)) {
        return makeError(500, "response serialization failed");
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(std::move(out));
    applyResponseChain(resp);
    return resp;
}

template <typename T>
void respond(const Callback& cb, const Result<T>& r, int okStatus) {
    if (!r.ok()) {
        cb(render(r.error()));
        return;
    }
    cb(makeOk(r.value(), okStatus));
}

}  // namespace detail

class App;

namespace detail {

// Los alias viven aparte de las rutas porque su vida util es otra: se
// consultan desde cualquier sitio, mucho despues de registrarlas.
inline std::unordered_map<std::string, std::string>& routeAliases() {
    static std::unordered_map<std::string, std::string> aliases;
    return aliases;
}

// "GET /api/v1/users/{id}" -> "users.show". Solo lo usa printRoutes().
inline std::unordered_map<std::string, std::string>& aliasByRoute() {
    static std::unordered_map<std::string, std::string> byRoute;
    return byRoute;
}

}  // namespace detail

// Lo que devuelve registrar una ruta. Su unico trabajo es dejarle un nombre
// con el que construir la URL mas tarde, sin repetirla a mano.
class Route {
public:
    Route(std::string method, std::string path)
        : method_{std::move(method)}, path_{std::move(path)} {}

    const Route& as(const std::string& alias) const {
        detail::routeAliases()[alias] = path_;

        // El mismo path lo comparten hasta cinco metodos, asi que para saber
        // de quien es un alias hace falta la pareja. El mapa de arriba se
        // queda como esta: route("users.show", 7) construye una URL y para
        // eso el metodo no pinta.
        detail::aliasByRoute()[method_ + " " + path_] = alias;
        return *this;
    }

    const std::string& path() const { return path_; }
    const std::string& method() const { return method_; }

private:
    std::string method_;
    std::string path_;
};

// La URL de una ruta con alias, con sus {param} rellenos en orden:
//
//   urlFor("users.show", 42)  ->  "/api/v1/users/42"
//
// Sirve para la cabecera Location de un 201, o para enlazar un recurso desde
// otro sin escribir la ruta dos veces.
template <typename... Args>
std::string urlFor(const std::string& alias, const Args&... args) {
    const auto found = detail::routeAliases().find(alias);
    if (found == detail::routeAliases().end()) return {};

    std::string url = found->second;

    const std::array<std::string, sizeof...(Args)> valores{std::format("{}", args)...};
    for (const auto& valor : valores) {
        const auto abre = url.find('{');
        if (abre == std::string::npos) break;

        const auto cierra = url.find('}', abre);
        if (cierra == std::string::npos) break;

        url.replace(abre, cierra - abre + 1, valor);
    }
    return url;
}

// Un prefijo compartido. La base de la API se escribe una vez y los endpoints
// se registran relativos a ella.
class Group {
public:
    Group(App& app, std::string prefix) : app_{&app}, prefix_{std::move(prefix)} {}

    template <typename F> Route get(const std::string& path, F&& f);
    template <typename F> Route post(const std::string& path, F&& f);
    template <typename F> Route put(const std::string& path, F&& f);
    template <typename F> Route patch(const std::string& path, F&& f);
    template <typename F> Route del(const std::string& path, F&& f);

    Group group(const std::string& prefix) const { return Group{*app_, prefix_ + prefix}; }

    const std::string& prefix() const { return prefix_; }

private:
    App*        app_;
    std::string prefix_;
};

// La aplicacion. Envuelve drogon::app() y traduce entre handlers tipados y
// el mundo de HttpRequestPtr / callbacks.
class App {
public:
    template <typename F> Route get(const std::string& path, F&& f) {
        route<false>(path, std::forward<F>(f), drogon::Get, 200);
        return Route{"GET", path};
    }
    template <typename F> Route post(const std::string& path, F&& f) {
        route<true>(path, std::forward<F>(f), drogon::Post, 201);
        return Route{"POST", path};
    }
    template <typename F> Route put(const std::string& path, F&& f) {
        route<true>(path, std::forward<F>(f), drogon::Put, 200);
        return Route{"PUT", path};
    }
    template <typename F> Route patch(const std::string& path, F&& f) {
        route<true>(path, std::forward<F>(f), drogon::Patch, 200);
        return Route{"PATCH", path};
    }
    template <typename F> Route del(const std::string& path, F&& f) {
        route<false>(path, std::forward<F>(f), drogon::Delete, 200);
        return Route{"DELETE", path};
    }

    // Registra bajo un prefijo comun: app.group("/api/v1").
    Group group(const std::string& prefix) { return Group{*this, prefix}; }

    // La base de la API: se declara una vez, en la configuracion, y de ahi
    // cuelgan todas las rutas de negocio.
    //
    //   app.base("/api/v1");                 // en bootstrap
    //   routes::v1::register_(app.api());    // en routes
    //
    // Las rutas que no son de la API (un /health, los estaticos) se siguen
    // registrando con su ruta completa: la base no es un prefijo global.
    App& base(std::string prefix) {
        base_ = std::move(prefix);
        return *this;
    }

    const std::string& base() const { return base_; }

    Group api() { return Group{*this, base_}; }

    // Middleware global: corre antes de cada handler, en orden de registro.
    App& use(Middleware middleware) {
        middlewares_.push_back({.prefix = {}, .fn = std::move(middleware)});
        return *this;
    }

    // Middleware para las rutas que empiezan con un prefijo.
    App& use(std::string prefix, Middleware middleware) {
        middlewares_.push_back({.prefix = std::move(prefix), .fn = std::move(middleware)});
        return *this;
    }

    // Middleware que puede esperar: una consulta a Redis o a la base antes de
    // decidir. Corre despues de TODOS los sincronos, para que lo que se puede
    // rechazar sin salir del proceso no pague una ida y vuelta a la red.
    App& useAsync(AsyncMiddleware middleware) {
        asyncMiddlewares_.push_back({.prefix = {}, .fn = std::move(middleware)});
        return *this;
    }

    App& useAsync(std::string prefix, AsyncMiddleware middleware) {
        asyncMiddlewares_.push_back({.prefix = std::move(prefix), .fn = std::move(middleware)});
        return *this;
    }

    // Modifica cada respuesta ya construida (cabeceras de seguridad, CORS).
    App& useOnResponse(ResponseMiddleware middleware) {
        responseMiddlewares_.push_back(std::move(middleware));
        return *this;
    }

    // Un endpoint WebSocket. El prefijo de use(prefix, ...) no aplica: los
    // middlewares de Syrax corren sobre respuestas HTTP, y un socket no las
    // tiene despues del handshake. La autenticacion va dentro de onOpen.
    //
    //   app.ws("/chat", {
    //       .onOpen    = [&](const Socket& s) { room.join(s); },
    //       .onMessage = [&](const Socket&, std::string_view text) {
    //           room.broadcast(text);
    //       },
    //       .onClose   = [&](const Socket& s) { room.leave(s); },
    //   });
    App& ws(const std::string& path, SocketHandlers handlers) {
        detail::wsRegistry()[path] = std::move(handlers);
        detail::WsBridge::addPath(path);
        sockets_.push_back(path);
        return *this;
    }

    // El documento OpenAPI de lo registrado hasta ahora, sin levantar el
    // servidor. Es lo mismo que sirve /openapi.json: util para volcarlo en
    // CI, generar clientes, o comprobar en un test que el contrato es el que
    // se cree que es.
    std::string openApi() const { return buildOpenApi(routes_, title_, version_); }

    // Lo que hay registrado, sin levantar el servidor. Es lo primero que se
    // busca al volver a un proyecto despues de un mes, y hasta ahora la unica
    // forma de saberlo era leer routes.cpp.
    //
    // Se imprime en orden de ruta y no de registro: lo que se busca es "que
    // hay bajo /users", no "que se registro primero".
    void printRoutes() const {
        if (routes_.empty() && sockets_.empty()) {
            std::cout << "\n  no hay rutas registradas\n\n";
            return;
        }

        std::vector<const RouteInfo*> ordenadas;
        ordenadas.reserve(routes_.size());
        for (const auto& route : routes_) ordenadas.push_back(&route);

        std::sort(ordenadas.begin(), ordenadas.end(), [](const auto* a, const auto* b) {
            if (a->path != b->path) return a->path < b->path;
            return a->method < b->method;
        });

        std::cout << "\n";
        for (const auto* route : ordenadas) {
            auto metodo = route->method;
            for (auto& c : metodo) c = static_cast<char>(std::toupper(c));

            std::cout << std::format("  \033[32m{:<7}\033[0m {:<44}", metodo, route->path);

            // El alias es lo que hace util a la tabla: es el nombre con el
            // que se construye la URL desde el codigo, y no se ve en ningun
            // otro sitio.
            const auto alias = detail::aliasByRoute().find(metodo + " " + route->path);
            if (alias != detail::aliasByRoute().end()) {
                std::cout << " \033[90m" << alias->second << "\033[0m";
            }
            std::cout << "\n";
        }

        for (const auto& socket : sockets_) {
            std::cout << std::format("  \033[36m{:<7}\033[0m {}\n", "WS", socket);
        }

        std::cout << "\n  " << routes_.size() << " rutas"
                  << (sockets_.empty() ? std::string{}
                                       : ", " + std::to_string(sockets_.size()) + " sockets")
                  << "\n\n";
    }

    App& cors(CorsOptions options = {}) {
        cors_ = std::move(options);
        return *this;
    }

    // Silencia el banner de arranque.
    App& quiet() {
        banner_ = false;
        return *this;
    }

    // Titulo y version que aparecen en /docs.
    App& docs(std::string title, std::string version = "1.0.0") {
        title_   = std::move(title);
        version_ = std::move(version);
        return *this;
    }

    // El endpoint de salud, que pregunta a cada base y cada Redis registrados
    // en vez de devolver un 200 fijo. 200 si todo responde, 503 si algo no.
    //
    // Va por registerHandler y no por get() porque el status depende del
    // resultado, y el de una ruta normal se fija al registrarla: es
    // precisamente lo que un healthcheck no puede hacer. El cuerpo lleva el
    // desglose, para que el 503 diga QUE se cayo y no solo que algo se cayo.
    App& health(const std::string& path = "/health") {
        note(drogon::Get, path, {}, detail::schemaOf<syrax::health::Report>(), 200);

        drogon::app().registerHandler(
            path,
            [](drogon::HttpRequestPtr, detail::Callback cb) -> drogon::Task<> {
                const auto report = co_await syrax::health::check();
                cb(detail::makeOk(report, report.ok() ? 200 : 503));
            },
            {drogon::Get});
        return *this;
    }

    // ETag y 304 para los GET.
    //
    // Cada respuesta de exito sale con un ETag calculado sobre su cuerpo, y una
    // peticion que traiga ese mismo ETag en `If-None-Match` recibe un 304 sin
    // cuerpo. Para un recurso que cambia poco y se pide mucho —un catalogo, un
    // perfil, una lista de opciones— eso es la diferencia entre mandar el JSON
    // entero cada vez y mandar una linea de cabeceras.
    //
    // Es un ETag *debil* en el sentido que importa: se calcula sobre los bytes
    // que se iban a mandar de todas formas, asi que ahorra RED pero no ahorra
    // el trabajo de generarlos. Ahorrarse tambien el trabajo obliga a saber
    // cuando cambio el recurso, y eso solo lo sabe la aplicacion.
    //
    // Va por PostHandling y no por la cadena de respuesta porque hace falta ver
    // el REQUEST —la cabecera If-None-Match— y la cadena de respuesta solo ve
    // la respuesta.
    App& etag() {
        drogon::app().registerPostHandlingAdvice(
            [](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
                // Solo GET y HEAD: un POST que devuelve 201 no es cacheable, y
                // un 304 a un DELETE seria mentira.
                const auto method = req->method();
                if (method != drogon::Get && method != drogon::Head) return;

                if (resp->statusCode() != drogon::k200OK) return;

                const auto body = resp->body();
                if (body.empty()) return;

                const auto tag = detail::etagOf(body);
                resp->addHeader("ETag", tag);

                // If-None-Match admite una lista y el comodin. Comparar por
                // substring es suficiente aqui y evita un parser por una coma.
                const auto sent = req->getHeader("If-None-Match");
                if (sent.empty()) return;

                if (sent == "*" || sent.find(tag) != std::string::npos) {
                    resp->setStatusCode(drogon::k304NotModified);
                    resp->setBody("");
                }
            });
        return *this;
    }

    // Reintentos seguros con `Idempotency-Key`.
    //
    // Va por advices y no por la cadena de middleware porque necesita las dos
    // mitades: cortar ANTES del handler devolviendo una respuesta guardada
    // entera —no un Error, que es lo unico que un middleware puede devolver— y
    // ver DESPUES la respuesta para guardarla.
    App& idempotency(idempotency::Options options = {}) {
        drogon::app().registerPreHandlingAdvice(
            [options](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& respond,
                      drogon::AdviceChainCallback&& next) {
                const auto clave = req->getHeader(options.header);
                const auto metodo = std::string{req->methodString()};

                const bool aplica =
                    !clave.empty() &&
                    std::ranges::find(options.methods, metodo) != options.methods.end();

                if (!aplica) {
                    next();
                    return;
                }

                // La huella va sobre el cuerpo, que es lo que identifica la
                // operacion. Sin ella, la misma clave con otro cuerpo
                // devolveria la respuesta de una operacion distinta.
                const auto huella = idempotency::detail::sha256Hex(req->body());

                // Se guarda en el request para que el advice de salida sepa
                // bajo que clave guardar sin volver a leer la cabecera.
                req->attributes()->insert(std::string{detail::kIdempotencyKey}, clave);

                drogon::async_run([options, clave, huella, respond = std::move(respond),
                                   next = std::move(next)]() mutable -> drogon::Task<> {
                    const auto tomada =
                        co_await idempotency::claim(clave, huella, options.ttl, options.on);

                    switch (tomada.state) {
                        case idempotency::Claim::Replay: {
                            auto resp = drogon::HttpResponse::newHttpResponse();
                            resp->setStatusCode(
                                static_cast<drogon::HttpStatusCode>(tomada.stored.status));
                            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                            resp->setBody(tomada.stored.body);
                            resp->addHeader("Idempotent-Replay", "true");
                            applyResponseChain(resp);
                            respond(resp);
                            co_return;
                        }

                        case idempotency::Claim::InProgress:
                            respond(render(Error{.status  = 409,
                                                 .message = "otra peticion con esa "
                                                            "Idempotency-Key sigue en curso",
                                                 .code    = "idempotency_in_progress"}));
                            co_return;

                        case idempotency::Claim::Mismatch:
                            respond(render(Error{
                                .status  = 422,
                                .message = "esa Idempotency-Key ya se uso con otro cuerpo",
                                .code    = "idempotency_key_reused",
                                .detail  = "una clave identifica UNA operacion: para otra "
                                           "distinta, manda una clave distinta"}));
                            co_return;

                        // Sin Redis no hay proteccion, pero tampoco se cae la
                        // API: se sigue como si la cabecera no estuviera.
                        case idempotency::Claim::Unavailable:
                        case idempotency::Claim::Owned:
                            next();
                            co_return;
                    }
                });
            });

        drogon::app().registerPostHandlingAdvice(
            [options](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
                if (!req->attributes()->find(std::string{detail::kIdempotencyKey})) return;

                const auto clave =
                    req->attributes()->get<std::string>(std::string{detail::kIdempotencyKey});
                const auto status = static_cast<int>(resp->statusCode());

                // Un 5xx no se guarda y ademas suelta la clave: el fallo fue
                // del servidor, asi que el cliente tiene que poder reintentar.
                if (status >= 500) {
                    drogon::async_run([options, clave]() -> drogon::Task<> {
                        co_await idempotency::release(clave, options.on);
                    });
                    return;
                }

                // El 409 y el 422 que genera este mismo mecanismo no se
                // guardan: son respuestas SOBRE la clave, no resultados de la
                // operacion, y guardarlas dejaria la clave envenenada.
                if (status == 409 || status == 422) {
                    const auto body = std::string{resp->body()};
                    if (body.find("idempotency_") != std::string::npos) return;
                }

                idempotency::Stored guardar{
                    .fingerprint = idempotency::detail::sha256Hex(req->body()),
                    .status      = status,
                    .body        = std::string{resp->body()},
                };

                drogon::async_run([options, clave, guardar]() -> drogon::Task<> {
                    co_await idempotency::save(clave, guardar, options.ttl, options.on);
                });
            });
        return *this;
    }

    // Serializacion parcial: `?fields=id,name`.
    //
    // Va por PostHandling, sobre el JSON ya escrito, y no por reflexion en el
    // momento de serializar. La reflexion seria mas barata —los nombres estan
    // en tiempo de compilacion— pero obligaria a meter el request en makeOk, o
    // sea en TODOS los caminos de registro, para una feature que solo se usa
    // cuando el cliente la pide. Esto cuesta un parse extra de la respuesta
    // unicamente en esas peticiones, y cero en las demas.
    App& partial(fields::Options options = {}) {
        drogon::app().registerPostHandlingAdvice(
            [options](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
                // Solo respuestas de exito: filtrarle los campos a un error
                // dejaria al cliente sin el mensaje que explica el error.
                if (resp->statusCode() < drogon::k200OK ||
                    resp->statusCode() >= drogon::k300MultipleChoices) {
                    return;
                }

                const auto raw = req->getParameter(options.param);
                if (raw.empty()) return;

                const auto pedidos = fields::parse(raw);

                bool desconocidos = false;
                const auto filtrado = fields::apply(resp->body(), pedidos, desconocidos);

                if (desconocidos) {
                    const auto error = render(Error{
                        .status  = 400,
                        .message = "ninguno de los campos pedidos existe en este recurso",
                        .code    = "unknown_fields",
                        .detail  = "?" + options.param + "=" + raw});

                    resp->setStatusCode(drogon::k400BadRequest);
                    resp->setBody(std::string{error->body()});
                    return;
                }

                if (filtrado) resp->setBody(*filtrado);
            });
        return *this;
    }

    // Cuatro contadores en /metrics, en el formato de texto de Prometheus.
    //
    // La ruta se sirve en texto plano y no como JSON: es lo que Prometheus
    // sabe leer, y envolverlo en el sobre de la API lo haria inutil para lo
    // unico que existe.
    //
    // No cuenta sus propias peticiones. Prometheus raspa cada 15 segundos, y
    // dejarlo contar haria que el trafico de la grafica fuera, en una API
    // tranquila, casi todo el propio scrape.
    App& metrics(const std::string& path = "/metrics") {
        metricsPath_ = path;

        drogon::app().registerHandler(
            path,
            [](const drogon::HttpRequestPtr&,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                resp->setBody(syrax::metrics::render());
                applyResponseChain(resp);
                cb(resp);
            },
            {drogon::Get});
        return *this;
    }

    // Apaga /docs y /openapi.json (por ejemplo, en produccion).
    App& withoutDocs() {
        docsEnabled_ = false;
        return *this;
    }

    void run(std::uint16_t port = 8080) {
        // El orden importa: la cadena de respuesta tiene que estar poblada
        // antes de construir el 404 estatico y las rutas de documentacion.
        registerMiddlewares();
        if (docsEnabled_) registerDocs();

        // Drogon sirve una pagina HTML para rutas no encontradas. Una API debe
        // responder JSON siempre, incluso cuando el error lo genera el transporte.
        drogon::app().setCustom404Page(
            detail::makeError(404, "route not found"), /*set404=*/true);

        // Una excepcion que se escapa del handler (una consulta contra una
        // tabla que no existe, por ejemplo) salia como un 500 con el cuerpo
        // VACIO: un cliente que espera JSON se encontraba con nada.
        //
        // El what() no viaja al cliente a proposito. Aqui decia "no such
        // table: users", que le describe el esquema a cualquiera que provoque
        // un error. Va al log, que es donde sirve.
        //
        // Antes de rendirse al 500, la excepcion pasa por los traductores que
        // haya registrado la aplicacion con onException(): un UniqueViolation
        // es un 409, y solo el proyecto sabe eso.
        drogon::app().setExceptionHandler(
            [](const std::exception& error, const drogon::HttpRequestPtr& request,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                LOG_ERROR << "excepcion sin atrapar en " << request->path() << ": "
                          << error.what();
                callback(render(errorFrom(error)));
            });

        // El request-id y el log de acceso van puestos de fabrica. Una
        // trazabilidad que hay que acordarse de encender es la que no esta el
        // dia que hace falta; para apagarla esta LOG_ACCESS=0.
        log::install();

        // El conteo se engancha aqui y no en metrics(), para que el orden de
        // las llamadas del proyecto no cambie lo que se mide.
        if (!metricsPath_.empty()) installMetrics();

        // El banner se imprime cuando el listener ya esta arriba, no antes:
        // si el puerto esta ocupado no tiene sentido anunciar una URL que no
        // responde.
        // Con run(0) el puerto lo elige el kernel, asi que el numero real solo
        // se sabe cuando el listener ya esta arriba: preguntarselo a Drogon
        // evita anunciar un ":0" que no lleva a ninguna parte.
        drogon::app().registerBeginningAdvice([this, port] {
            const auto listeners = drogon::app().getListeners();
            banner(listeners.empty() ? port : listeners.front().toPort());
        });

        drogon::app().addListener("0.0.0.0", port).run();
    }

private:
    // Los esquemas salen de los mismos tipos que usan los handlers, asi que la
    // documentacion no puede desincronizarse del codigo.
    void note(drogon::HttpMethod method, const std::string& path,
              std::string requestSchema, std::string responseSchema, int okStatus,
              std::vector<std::string> paramTypes = {}) {
        static const std::unordered_map<int, std::string> kNames{
            {drogon::Get, "get"},     {drogon::Post, "post"},  {drogon::Put, "put"},
            {drogon::Patch, "patch"}, {drogon::Delete, "delete"},
        };

        const auto it = kNames.find(static_cast<int>(method));
        if (it == kNames.end()) return;

        routes_.push_back(RouteInfo{
            .method         = it->second,
            .path           = path,
            .requestSchema  = std::move(requestSchema),
            .responseSchema = std::move(responseSchema),
            .okStatus       = okStatus,
            .paramTypes     = std::move(paramTypes),
        });
    }

    struct Scoped {
        std::string prefix;
        Middleware  fn;
    };

    struct AsyncScoped {
        std::string     prefix;
        AsyncMiddleware fn;
    };

    void installMetrics() {
        const auto exenta = metricsPath_;

        drogon::app().registerPreHandlingAdvice(
            [exenta](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&&,
                     drogon::AdviceChainCallback&& next) {
                if (req->path() != exenta) syrax::metrics::started();
                next();
            });

        drogon::app().registerPostHandlingAdvice(
            [exenta](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
                if (req->path() == exenta) return;

                // El instante de entrada lo deja log::install(), que ya lo
                // necesitaba para la linea de acceso: medirlo dos veces seria
                // dos relojes que se pueden desincronizar.
                std::uint64_t micros = 0;
                if (req->attributes()->find(std::string{log::detail::kStartedKey})) {
                    const auto desde = std::stoull(
                        req->attributes()->get<std::string>(std::string{log::detail::kStartedKey}));
                    const auto hasta = std::stoull(log::detail::micros());
                    if (hasta > desde) micros = hasta - desde;
                }

                syrax::metrics::finished(static_cast<int>(resp->statusCode()), micros);
            });
    }

    void registerMiddlewares() {
        if (cors_) registerCors();

        if (!middlewares_.empty() || !asyncMiddlewares_.empty()) {
            drogon::app().registerPreHandlingAdvice(
                [chain = middlewares_, asyncChain = asyncMiddlewares_](
                    const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& respond,
                    drogon::AdviceChainCallback&& next) {
                    Request    request{req};
                    const auto path = request.path();

                    for (const auto& entry : chain) {
                        if (!entry.prefix.empty() && !path.starts_with(entry.prefix)) continue;

                        if (const auto error = entry.fn(request)) {
                            respond(render(*error));
                            return;
                        }
                    }

                    if (asyncChain.empty()) {
                        next();
                        return;
                    }

                    // Los callbacks de Drogon se pueden llamar desde cualquier
                    // continuacion, asi que la cadena asincrona no obliga a
                    // cambiar nada de la sincrona: solo se le cede el turno.
                    //
                    // Todo va por copia porque la corrutina sobrevive a este
                    // marco. Request lleva un shared_ptr dentro, asi que
                    // copiarlo es barato y mantiene el request vivo.
                    drogon::async_run([asyncChain, request, path,
                                       respond = std::move(respond),
                                       next    = std::move(next)]() mutable -> drogon::Task<> {
                        for (const auto& entry : asyncChain) {
                            if (!entry.prefix.empty() && !path.starts_with(entry.prefix)) continue;

                            if (const auto error = co_await entry.fn(request)) {
                                respond(render(*error));
                                co_return;
                            }
                        }
                        next();
                    });
                });
        }

        detail::responseChain() = responseMiddlewares_;
    }

    void registerCors() {
        const auto& options = *cors_;

        const auto origin      = detail::join(options.origins, ", ");
        const auto methods     = detail::join(options.methods, ", ");
        const auto headers     = detail::join(options.headers, ", ");
        const auto credentials = options.credentials;
        const auto maxAge      = std::to_string(options.maxAge);

        // El preflight se responde antes de llegar al ruteo: OPTIONS no
        // corresponde a ningun handler y terminaria en 404.
        drogon::app().registerPreRoutingAdvice(
            [origin, methods, headers, credentials, maxAge](
                const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& respond,
                drogon::AdviceChainCallback&& next) {
                if (req->method() != drogon::Options) {
                    next();
                    return;
                }

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k204NoContent);
                resp->addHeader("Access-Control-Allow-Origin", origin);
                resp->addHeader("Access-Control-Allow-Methods", methods);
                resp->addHeader("Access-Control-Allow-Headers", headers);
                resp->addHeader("Access-Control-Max-Age", maxAge);
                if (credentials) resp->addHeader("Access-Control-Allow-Credentials", "true");
                applyResponseChain(resp);

                respond(resp);
            });

        useOnResponse([origin, credentials](const drogon::HttpResponsePtr& resp) {
            resp->addHeader("Access-Control-Allow-Origin", origin);
            if (credentials) resp->addHeader("Access-Control-Allow-Credentials", "true");
        });
    }

    // Se escribe localhost y no 0.0.0.0 para que la terminal la reconozca
    // como enlace y se pueda abrir con click.
    void banner(std::uint16_t port) const {
        if (!banner_) return;

        const std::string base = "http://localhost:" + std::to_string(port);

        std::cout << "\n  \033[1m" << title_ << "\033[0m " << version_ << "\n\n"
                  << "  \033[32m->\033[0m  Local:  \033[4m" << base << "/\033[0m\n";

        if (docsEnabled_) {
            std::cout << "  \033[32m->\033[0m  Docs:   \033[4m" << base << "/docs\033[0m\n"
                      << "  \033[32m->\033[0m  Spec:   \033[4m" << base
                      << "/openapi.json\033[0m\n";
        }

        // Prefijos distintos de las rutas registradas: si todas cuelgan de
        // /api/v1, mostrarlo ahorra adivinar.
        std::set<std::string> prefixes;
        for (const auto& route : routes_) {
            const auto second = route.path.find('/', 1);
            if (second != std::string::npos) prefixes.insert(route.path.substr(0, second));
        }
        for (const auto& prefix : prefixes) {
            if (prefix == "/docs") continue;
            std::cout << "  \033[32m->\033[0m  API:    \033[4m" << base << prefix
                      << "\033[0m\n";
        }

        for (const auto& socket : sockets_) {
            std::cout << "  \033[32m->\033[0m  WS:     \033[4mws://localhost:" << port << socket
                      << "\033[0m\n";
        }

        std::cout << "\n  " << routes_.size() << " rutas"
                  << (sockets_.empty() ? std::string{}
                                       : ", " + std::to_string(sockets_.size()) + " sockets")
                  << ".  Ctrl+C para detener.\n\n"
                  << std::flush;
    }

    void registerDocs() {
        const auto spec = buildOpenApi(routes_, title_, version_);
        const auto html = swaggerHtml(title_);

        drogon::app().registerHandler(
            "/openapi.json",
            [spec](const drogon::HttpRequestPtr&, detail::Callback&& cb) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody(spec);
                applyResponseChain(resp);
                cb(resp);
            },
            {drogon::Get});

        drogon::app().registerHandler(
            "/docs",
            [html](const drogon::HttpRequestPtr&, detail::Callback&& cb) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                resp->setBody(html);
                applyResponseChain(resp);
                cb(resp);
            },
            {drogon::Get});
    }

    std::vector<Scoped>             middlewares_;
    std::vector<AsyncScoped>        asyncMiddlewares_;
    std::vector<ResponseMiddleware>  responseMiddlewares_;
    std::optional<CorsOptions>       cors_;
    std::vector<RouteInfo>           routes_;
    std::vector<std::string>         sockets_;
    std::string            title_       = "API";
    std::string            version_     = "1.0.0";
    std::string            base_        = "/api/v1";
    std::string            metricsPath_;
    bool                   docsEnabled_ = true;
    bool                   banner_      = true;

    template <bool AllowBody, typename F>
    App& route(const std::string& path, F&& f, drogon::HttpMethod method, int okStatus) {
        using Raw = typename detail::fn_traits<std::decay_t<F>>::args;

        // Se descarta el Request inicial, si lo hay, antes de repartir el
        // resto entre path params y body.
        constexpr bool kWantsRequest = detail::hasLeadingRequest<Raw>();
        using Args = std::conditional_t<kWantsRequest, detail::DropFirst<Raw>, Raw>;

        constexpr std::size_t kArity = std::tuple_size_v<Args>;

        // Un handler puede pedir path params y body a la vez:
        //   [](int64_t id, UpdateUser body) -> Result<UserResponse>
        // El body, si lo hay, es siempre el ultimo argumento.
        using Value = detail::ResultValueT<typename detail::fn_traits<std::decay_t<F>>::result>;

        if constexpr (AllowBody && detail::hasTrailingBody<Args>()) {
            using Body = std::tuple_element_t<kArity - 1, Args>;

            note(method, path, detail::schemaOf<Body>(), detail::schemaOf<Value>(), okStatus,
                 detail::paramTypeNames(static_cast<detail::DropLast<Args>*>(nullptr)));

            registerWithBody<Body, kWantsRequest>(
                path, std::forward<F>(f), method, okStatus,
                static_cast<detail::DropLast<Args>*>(nullptr));
        } else {
            note(method, path, {}, detail::schemaOf<Value>(), okStatus,
                 detail::paramTypeNames(static_cast<Args*>(nullptr)));

            registerParams<kWantsRequest>(path, std::forward<F>(f), method, okStatus,
                                          static_cast<Args*>(nullptr));
        }
        return *this;
    }

    // Cero o mas path params seguidos de un body JSON.
    template <typename Body, bool WantsRequest, typename F, typename... Params>
    void registerWithBody(const std::string& path, F f, drogon::HttpMethod method,
                          int okStatus, std::tuple<Params...>*) {
        using Ret = typename detail::fn_traits<F>::result;

        if constexpr (detail::kIsTask<Ret>) {
            drogon::app().registerHandler(
                path,
                // Misma firma exacta que exige Drogon para corrutinas.
                [f, okStatus](drogon::HttpRequestPtr req, detail::Callback cb,
                              detail::AsString<Params>... raws) -> drogon::Task<> {
                    std::tuple<std::optional<Params>...> params{
                        detail::convertParam<Params>(raws)...};

                    if (!detail::allPresent(params)) {
                        cb(detail::makeError(400, "invalid path parameter"));
                        co_return;
                    }

                    Body        body{};
                    std::string rawBody{req->getBody()};
                    if (auto ec = glz::read<detail::kStrict>(body, rawBody)) {
                        cb(detail::makeError(422, glz::format_error(ec, rawBody)));
                        co_return;
                    }

                    if (auto invalid = validate(body); !invalid.empty()) {
                        cb(detail::makeValidationError(invalid));
                        co_return;
                    }

                    Request request{req};
                    auto    result = co_await std::apply(
                        [&](const auto&... o) {
                            if constexpr (WantsRequest) {
                                return f(request, *o..., std::move(body));
                            } else {
                                return f(*o..., std::move(body));
                            }
                        },
                        params);
                    detail::respond(cb, result, okStatus);
                    co_return;
                },
                {method});
        } else {
            drogon::app().registerHandler(
                path,
                [f, okStatus](const drogon::HttpRequestPtr& req, detail::Callback&& cb,
                              detail::AsString<Params>... raws) {
                    std::tuple<std::optional<Params>...> params{
                        detail::convertParam<Params>(raws)...};

                    if (!detail::allPresent(params)) {
                        cb(detail::makeError(400, "invalid path parameter"));
                        return;
                    }

                    Body        body{};
                    std::string rawBody{req->getBody()};
                    if (auto ec = glz::read<detail::kStrict>(body, rawBody)) {
                        cb(detail::makeError(422, glz::format_error(ec, rawBody)));
                        return;
                    }

                    if (auto invalid = validate(body); !invalid.empty()) {
                        cb(detail::makeValidationError(invalid));
                        return;
                    }

                    Request request{req};
                    detail::respond(cb,
                                    std::apply(
                                        [&](const auto&... o) {
                                            if constexpr (WantsRequest) {
                                                return f(request, *o..., std::move(body));
                                            } else {
                                                return f(*o..., std::move(body));
                                            }
                                        },
                                        params),
                                    okStatus);
                },
                {method});
        }
    }

    // Solo path params, sin body.
    template <bool WantsRequest, typename F, typename... Args>
    void registerParams(const std::string& path, F f, drogon::HttpMethod method,
                        int okStatus, std::tuple<Args...>*) {
        using Ret = typename detail::fn_traits<F>::result;

        if constexpr (detail::kIsTask<Ret>) {
            drogon::app().registerHandler(
                path,
                // Drogon exige esta firma exacta para handlers corrutina:
                // request y callback POR VALOR, para que vivan en el frame a
                // traves de los puntos de suspension.
                [f, okStatus](drogon::HttpRequestPtr req, detail::Callback cb,
                              detail::AsString<Args>... raws) -> drogon::Task<> {
                    std::tuple<std::optional<Args>...> conv{
                        detail::convertParam<Args>(raws)...};

                    if (!detail::allPresent(conv)) {
                        cb(detail::makeError(400, "invalid path parameter"));
                        co_return;
                    }

                    Request request{req};
                    auto    result = co_await std::apply(
                        [&](const auto&... o) {
                            if constexpr (WantsRequest) return f(request, *o...);
                            else return f(*o...);
                        },
                        conv);
                    detail::respond(cb, result, okStatus);
                    co_return;
                },
                {method});
        } else {
            drogon::app().registerHandler(
                path,
                [f, okStatus](const drogon::HttpRequestPtr& req, detail::Callback&& cb,
                              detail::AsString<Args>... raws) {
                    std::tuple<std::optional<Args>...> conv{
                        detail::convertParam<Args>(raws)...};

                    if (!detail::allPresent(conv)) {
                        cb(detail::makeError(400, "invalid path parameter"));
                        return;
                    }

                    Request request{req};
                    detail::respond(cb,
                                    std::apply(
                                        [&](const auto&... o) {
                                            if constexpr (WantsRequest) return f(request, *o...);
                                            else return f(*o...);
                                        },
                                        conv),
                                    okStatus);
                },
                {method});
        }
    }
};

// Group solo puede reenviar a App una vez App esta completa.
template <typename F> Route Group::get(const std::string& path, F&& f) {
    return app_->get(prefix_ + path, std::forward<F>(f));
}
template <typename F> Route Group::post(const std::string& path, F&& f) {
    return app_->post(prefix_ + path, std::forward<F>(f));
}
template <typename F> Route Group::put(const std::string& path, F&& f) {
    return app_->put(prefix_ + path, std::forward<F>(f));
}
template <typename F> Route Group::patch(const std::string& path, F&& f) {
    return app_->patch(prefix_ + path, std::forward<F>(f));
}
template <typename F> Route Group::del(const std::string& path, F&& f) {
    return app_->del(prefix_ + path, std::forward<F>(f));
}

}  // namespace syrax
