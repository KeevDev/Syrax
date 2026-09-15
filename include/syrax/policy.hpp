#pragma once

#include <syrax/middleware.hpp>
#include <syrax/result.hpp>

#include <functional>
#include <optional>
#include <string>

namespace syrax {

// Autorizacion: decidir si un actor puede hacer algo sobre un recurso.
//
// Es deliberadamente delgado. No hay registro global de politicas ni
// resolucion por nombre: una politica es una funcion, y se llama desde el
// service. Un sistema de roles y permisos completo es una aplicacion, no un
// framework.
//
//   namespace policies {
//   std::optional<syrax::Error> update(const Actor& actor, const Post& post) {
//       if (actor.role == "admin")   return std::nullopt;
//       if (post.authorId == actor.id) return std::nullopt;
//       return syrax::Forbidden("no puedes editar este post");
//   }
//   }
//
//   if (auto denied = policies::update(actor, post)) co_return *denied;

// Quien hace la peticion. Sale del token y se llena con actorFrom().
struct Actor {
    std::string id;
    std::string role;

    bool authenticated() const { return !id.empty(); }
    bool is(const std::string& expected) const { return role == expected; }
};

// Corta con 403 salvo que la condicion se cumpla.
//
//   if (auto denied = allowIf(actor.is("admin"), "solo administradores"))
//       co_return *denied;
inline std::optional<Error> allowIf(bool condition, std::string message = "forbidden") {
    if (condition) return std::nullopt;
    return Forbidden(std::move(message));
}

inline std::optional<Error> denyIf(bool condition, std::string message = "forbidden") {
    return allowIf(!condition, std::move(message));
}

// Construye el Actor con lo que dejo el middleware auth::bearer() en la
// peticion. Es el unico punto de union entre autenticacion y autorizacion:
// bearer() verifica el token, esto lo convierte en algo sobre lo que decidir.
//
// Sin token, devuelve un Actor no autenticado en vez de fallar: que eso sea
// un 401 o no lo decide la politica, no este helper. Una ruta publica puede
// querer saber quien mira sin exigirlo.
inline Actor actorFrom(const Request& request) {
    return Actor{.id = request.get("auth.sub"), .role = request.get("auth.role")};
}

// Exige que el actor tenga uno de los roles dados.
template <typename... Roles>
std::optional<Error> requireRole(const Actor& actor, Roles&&... roles) {
    if (!actor.authenticated()) return Unauthorized("authentication required");

    const bool allowed = ((actor.role == roles) || ...);
    if (!allowed) return Forbidden("insufficient role");

    return std::nullopt;
}

}  // namespace syrax
