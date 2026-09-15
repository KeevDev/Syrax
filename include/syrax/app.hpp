#pragma once

#include <drogon/drogon.h>
#include <glaze/glaze.hpp>

#include <syrax/openapi.hpp>
#include <syrax/result.hpp>
#include <syrax/traits.hpp>

#include <charconv>
#include <cstdint>
#include <optional>
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
    return out;
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

inline drogon::HttpResponsePtr makeError(int status, std::string message) {
    Json::Value body;
    body["error"]["status"]  = status;
    body["error"]["message"] = std::move(message);
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    return resp;
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
    return resp;
}

template <typename T>
void respond(const Callback& cb, const Result<T>& r, int okStatus) {
    if (!r.ok()) {
        cb(makeError(r.error().status, r.error().message));
        return;
    }
    cb(makeOk(r.value(), okStatus));
}

}  // namespace detail

// La aplicacion. Envuelve drogon::app() y traduce entre handlers tipados y
// el mundo de HttpRequestPtr / callbacks.
class App {
public:
    template <typename F> App& get(const std::string& path, F&& f) {
        return route<false>(path, std::forward<F>(f), drogon::Get, 200);
    }
    template <typename F> App& post(const std::string& path, F&& f) {
        return route<true>(path, std::forward<F>(f), drogon::Post, 201);
    }
    template <typename F> App& put(const std::string& path, F&& f) {
        return route<true>(path, std::forward<F>(f), drogon::Put, 200);
    }
    template <typename F> App& patch(const std::string& path, F&& f) {
        return route<true>(path, std::forward<F>(f), drogon::Patch, 200);
    }
    template <typename F> App& del(const std::string& path, F&& f) {
        return route<false>(path, std::forward<F>(f), drogon::Delete, 200);
    }

    // Titulo y version que aparecen en /docs.
    App& docs(std::string title, std::string version = "1.0.0") {
        title_   = std::move(title);
        version_ = std::move(version);
        return *this;
    }

    // Apaga /docs y /openapi.json (por ejemplo, en produccion).
    App& withoutDocs() {
        docsEnabled_ = false;
        return *this;
    }

    void run(std::uint16_t port = 8080) {
        if (docsEnabled_) registerDocs();

        // Drogon sirve una pagina HTML para rutas no encontradas. Una API debe
        // responder JSON siempre, incluso cuando el error lo genera el transporte.
        drogon::app().setCustom404Page(
            detail::makeError(404, "route not found"), /*set404=*/true);

        drogon::app().addListener("0.0.0.0", port).run();
    }

private:
    // Los esquemas salen de los mismos tipos que usan los handlers, asi que la
    // documentacion no puede desincronizarse del codigo.
    void note(drogon::HttpMethod method, const std::string& path,
              std::string requestSchema, std::string responseSchema, int okStatus) {
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
        });
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
                cb(resp);
            },
            {drogon::Get});

        drogon::app().registerHandler(
            "/docs",
            [html](const drogon::HttpRequestPtr&, detail::Callback&& cb) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                resp->setBody(html);
                cb(resp);
            },
            {drogon::Get});
    }

    std::vector<RouteInfo> routes_;
    std::string            title_       = "API";
    std::string            version_     = "1.0.0";
    bool                   docsEnabled_ = true;

    template <bool AllowBody, typename F>
    App& route(const std::string& path, F&& f, drogon::HttpMethod method, int okStatus) {
        using Args                 = typename detail::fn_traits<std::decay_t<F>>::args;
        constexpr std::size_t kArity = std::tuple_size_v<Args>;

        // Un handler puede pedir path params y body a la vez:
        //   [](int64_t id, UpdateUser body) -> Result<UserResponse>
        // El body, si lo hay, es siempre el ultimo argumento.
        using Value = detail::ResultValueT<typename detail::fn_traits<std::decay_t<F>>::result>;

        if constexpr (AllowBody && detail::hasTrailingBody<Args>()) {
            using Body = std::tuple_element_t<kArity - 1, Args>;

            note(method, path, detail::schemaOf<Body>(), detail::schemaOf<Value>(), okStatus);

            registerWithBody<Body>(path, std::forward<F>(f), method, okStatus,
                                   static_cast<detail::DropLast<Args>*>(nullptr));
        } else {
            note(method, path, {}, detail::schemaOf<Value>(), okStatus);

            registerParams(path, std::forward<F>(f), method, okStatus,
                           static_cast<Args*>(nullptr));
        }
        return *this;
    }

    // Cero o mas path params seguidos de un body JSON.
    template <typename Body, typename F, typename... Params>
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

                    auto result = co_await std::apply(
                        [&f, &body](const auto&... o) { return f(*o..., std::move(body)); },
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

                    detail::respond(cb,
                                    std::apply(
                                        [&f, &body](const auto&... o) {
                                            return f(*o..., std::move(body));
                                        },
                                        params),
                                    okStatus);
                },
                {method});
        }
    }

    // Solo path params, sin body.
    template <typename F, typename... Args>
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

                    auto result = co_await std::apply(
                        [&f](const auto&... o) { return f(*o...); }, conv);
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

                    detail::respond(
                        cb, std::apply([&f](const auto&... o) { return f(*o...); }, conv),
                        okStatus);
                },
                {method});
        }
    }
};

}  // namespace syrax
