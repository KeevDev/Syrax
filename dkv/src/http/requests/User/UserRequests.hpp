#pragma once

#include <string>

namespace requests {

// Lo que entra por el body. Syrax lo parsea y valida antes de que el
// controlador se ejecute: si falta un campo o el tipo no cuadra, el cliente
// recibe un 422 y el handler nunca corre.
struct CreateUser {
    std::string name;
    std::string email;
    int         age;
};

struct UpdateUser {
    std::string name;
    std::string email;
};

}  // namespace requests
