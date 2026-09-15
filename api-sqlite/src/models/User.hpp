#pragma once

#include <cstdint>
#include <string>

namespace models {

// La forma de la tabla `users`. Un struct plano, no una clase de ORM:
// Syrax mapea las columnas a los campos por nombre, en tiempo de compilacion.
//
// passwordHash vive aqui pero no en UserResource, asi que no puede salir en
// una respuesta por accidente.
struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;
};

}  // namespace models
