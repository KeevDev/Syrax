#pragma once

#include <drogon/drogon.h>
#include <json/json.h>

#include <syrax/middleware.hpp>
#include <syrax/result.hpp>

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace syrax {

// El borde HTTP traduce fallos a respuestas de dos maneras, y las dos se
// pueden sustituir desde la aplicacion sin tocar el framework:
//
//   onError      cambia el FORMATO con el que sale un Error.
//   onException  traduce una excepcion que se escapo a un Error.
//
// Las dos tienen el comportamiento actual como valor por defecto, asi que un
// proyecto que no las use no cambia ni un byte de su respuesta.

using ErrorRenderer   = std::function<drogon::HttpResponsePtr(const Error&)>;
using ExceptionMapper = std::function<std::optional<Error>(const std::exception&)>;

namespace detail {

// El formato de fabrica. Los campos opcionales solo aparecen cuando tienen
// algo, para no romper a un cliente que hoy lee error.message y nada mas.
inline drogon::HttpResponsePtr defaultRender(const Error& error) {
    Json::Value body;
    body["error"]["status"]  = error.status;
    body["error"]["message"] = error.message;

    if (!error.code.empty())   body["error"]["code"]   = error.code;
    if (!error.detail.empty()) body["error"]["detail"] = error.detail;

    for (const auto& field : error.fields) {
        Json::Value entry;
        entry["field"]   = field.field;
        entry["message"] = field.message;
        body["error"]["fields"].append(std::move(entry));
    }

    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(error.status));
    return response;
}

inline ErrorRenderer& renderer() {
    static ErrorRenderer fn = defaultRender;
    return fn;
}

inline std::vector<ExceptionMapper>& exceptionMappers() {
    static std::vector<ExceptionMapper> mappers;
    return mappers;
}

}  // namespace detail

// Sustituye el formato de TODOS los errores, incluido el 422 de validacion.
// Que pasen todos por el mismo sitio es el punto: un proyecto que cambia el
// formato y se queda con dos formas de error distintas en la misma API esta
// peor que antes de poder cambiarlo.
inline void onError(ErrorRenderer fn) { detail::renderer() = std::move(fn); }

// Traduce excepciones que se escaparon del handler. Se consultan en el orden
// en que se registraron y gana la primera que devuelva algo; devolver
// std::nullopt significa "esta no la entiendo", y deja el 500 de siempre.
//
//   syrax::onException([](const std::exception& e) -> std::optional<syrax::Error> {
//       if (dynamic_cast<const drogon::orm::UniqueViolation*>(&e))
//           return syrax::Conflict("el recurso ya existe").as("duplicado");
//       return std::nullopt;
//   });
inline void onException(ExceptionMapper fn) {
    detail::exceptionMappers().push_back(std::move(fn));
}

// Para armar una respuesta desde un gancho sin tener que nombrar a Drogon.
inline drogon::HttpResponsePtr jsonResponse(const Json::Value& body, int status) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    return response;
}

// El unico camino de un Error a una respuesta.
inline drogon::HttpResponsePtr render(const Error& error) {
    auto response = detail::renderer()(error);

    // Un gancho que devuelve nullptr es un bug de la aplicacion, pero no es
    // motivo para mandarle al cliente una respuesta vacia.
    if (!response) response = detail::defaultRender(error);

    applyResponseChain(response);
    return response;
}

// Que traduccion le toca a una excepcion. Aislado de render() porque el
// worker de colas tambien lo necesita, y ahi no hay respuesta HTTP.
inline Error errorFrom(const std::exception& thrown) {
    for (const auto& mapper : detail::exceptionMappers()) {
        if (auto mapped = mapper(thrown)) return *mapped;
    }
    return Internal("internal server error");
}

}  // namespace syrax
