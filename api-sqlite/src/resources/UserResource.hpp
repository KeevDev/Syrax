#pragma once

#include "models/User.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace resources {

// Lo que sale. Separado del modelo a proposito: es el contrato publico de la
// API y cambia por razones distintas al esquema de la base de datos.
struct UserResource {
    std::int64_t id;
    std::string  name;
    std::string  email;
};

struct DeletedResource {
    std::int64_t id;
    bool         deleted;
};

UserResource              from(const models::User& user);
std::vector<UserResource> from(const std::vector<models::User>& users);

}  // namespace resources
