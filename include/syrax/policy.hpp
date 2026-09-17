#pragma once

#include <syrax/middleware.hpp>
#include <syrax/result.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

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

    // Los permisos finos, crudos y separados por espacios tal como vinieron en
    // el token: "pedidos:leer pedidos:escribir". Se guarda sin trocear porque
    // asi es como viaja y como lo escribe quien firma el token; trocearlo en
    // cada comprobacion cuesta menos que mantener dos representaciones.
    std::string scope;

    bool authenticated() const { return !id.empty(); }
    bool is(const std::string& expected) const { return role == expected; }

    // Si tiene ESE permiso exacto. Sin comodines: un "pedidos:*" obliga a
    // decidir si cubre "pedidos:escribir:urgente", y ahi empieza un lenguaje
    // de patrones que no tiene fondo.
    bool can(std::string_view wanted) const {
        if (wanted.empty()) return false;

        // Se recorre en vez de partir en un vector: un token trae tres o
        // cuatro permisos y esto corre en cada peticion.
        std::size_t at = 0;
        while (at < scope.size()) {
            const auto end = scope.find(' ', at);
            const auto len = (end == std::string::npos ? scope.size() : end) - at;

            if (std::string_view{scope}.substr(at, len) == wanted) return true;
            if (end == std::string::npos) break;

            at = end + 1;
        }
        return false;
    }
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
    return Actor{.id    = request.get("auth.sub"),
                 .role  = request.get("auth.role"),
                 .scope = request.get("auth.scope")};
}

// Exige que el actor tenga uno de los roles dados.
template <typename... Roles>
std::optional<Error> requireRole(const Actor& actor, Roles&&... roles) {
    if (!actor.authenticated()) return Unauthorized("authentication required");

    const bool allowed = ((actor.role == roles) || ...);
    if (!allowed) return Forbidden("insufficient role");

    return std::nullopt;
}

// Exige TODOS los permisos dados.
//
//   if (auto denied = requireScope(actor, "pedidos:escribir")) co_return *denied;
//
// Ojo a la asimetria con requireRole, que exige CUALQUIERA de los roles: no es
// un descuido, sale del modelo de datos. Un actor tiene UN rol, asi que listar
// varios solo puede querer decir "alguno de estos"; los permisos son un
// conjunto, y listar varios quiere decir que hacen falta los dos. Cuando no es
// asi, esta requireAnyScope, escrito aparte para que la diferencia se lea en
// el nombre y no haya que recordarla.
template <typename... Scopes>
std::optional<Error> requireScope(const Actor& actor, Scopes&&... scopes) {
    static_assert(sizeof...(scopes) > 0, "syrax: requireScope necesita al menos un permiso");

    if (!actor.authenticated()) return Unauthorized("authentication required");

    const bool allowed = (actor.can(scopes) && ...);
    if (!allowed) return Forbidden("insufficient scope");

    return std::nullopt;
}

// Exige CUALQUIERA de los permisos dados.
template <typename... Scopes>
std::optional<Error> requireAnyScope(const Actor& actor, Scopes&&... scopes) {
    static_assert(sizeof...(scopes) > 0, "syrax: requireAnyScope necesita al menos un permiso");

    if (!actor.authenticated()) return Unauthorized("authentication required");

    const bool allowed = (actor.can(scopes) || ...);
    if (!allowed) return Forbidden("insufficient scope");

    return std::nullopt;
}

}  // namespace syrax
