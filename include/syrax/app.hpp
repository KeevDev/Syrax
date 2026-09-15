#pragma once

#include <drogon/drogon.h>
#include <glaze/glaze.hpp>

#include <syrax/result.hpp>
#include <syrax/traits.hpp>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>

namespace syrax {

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

    void run(std::uint16_t port = 8080) {
        // Drogon sirve una pagina HTML para rutas no encontradas. Una API debe
        // responder JSON siempre, incluso cuando el error lo genera el transporte.
        drogon::app().setCustom404Page(
            detail::makeError(404, "route not found"), /*set404=*/true);

        drogon::app().addListener("0.0.0.0", port).run();
    }

private:
    template <bool AllowBody, typename F>
    App& route(const std::string& path, F&& f, drogon::HttpMethod method, int okStatus) {
        using Args                 = typename detail::fn_traits<std::decay_t<F>>::args;
        constexpr std::size_t kArity = std::tuple_size_v<Args>;

        // Un handler puede pedir path params y body a la vez:
        //   [](int64_t id, UpdateUser body) -> Result<UserResponse>
        // El body, si lo hay, es siempre el ultimo argumento.
        if constexpr (AllowBody && kArity >= 1 &&
                      detail::BodyLike<std::tuple_element_t<kArity - 1, Args>>) {
            registerWithBody<std::tuple_element_t<kArity - 1, Args>>(
                path, std::forward<F>(f), method, okStatus,
                static_cast<detail::DropLast<Args>*>(nullptr));
        } else {
            registerParams(path, std::forward<F>(f), method, okStatus,
                           static_cast<Args*>(nullptr));
        }
        return *this;
    }

    // Cero o mas path params seguidos de un body JSON.
    template <typename Body, typename F, typename... Params>
    void registerWithBody(const std::string& path, F f, drogon::HttpMethod method,
                          int okStatus, std::tuple<Params...>*) {
        drogon::app().registerHandler(
            path,
            [f, okStatus](const drogon::HttpRequestPtr& req, detail::Callback&& cb,
                          detail::AsString<Params>... raws) {
                std::tuple<std::optional<Params>...> params{
                    detail::convertParam<Params>(raws)...};

                // Con cero params el fold sobre pack vacio da true.
                const bool paramsOk = std::apply(
                    [](const auto&... o) { return (o.has_value() && ...); }, params);
                if (!paramsOk) {
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

    // GET/DELETE: los argumentos salen de los path params, convertidos por Drogon.
    template <typename F, typename... Args>
    void registerParams(const std::string& path, F f, drogon::HttpMethod method,
                        int okStatus, std::tuple<Args...>*) {
        drogon::app().registerHandler(
            path,
            [f, okStatus](const drogon::HttpRequestPtr& req, detail::Callback&& cb,
                          detail::AsString<Args>... raws) {
                std::tuple<std::optional<Args>...> conv{detail::convertParam<Args>(raws)...};

                const bool allOk = std::apply(
                    [](const auto&... o) { return (o.has_value() && ...); }, conv);
                if (!allOk) {
                    cb(detail::makeError(400, "invalid path parameter"));
                    return;
                }

                detail::respond(
                    cb, std::apply([&f](const auto&... o) { return f(*o...); }, conv),
                    okStatus);
            },
            {method});
    }
};

}  // namespace syrax
