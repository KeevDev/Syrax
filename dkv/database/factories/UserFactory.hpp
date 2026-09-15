#pragma once

#include "models/User/User.hpp"

#include <cstdint>
#include <string>

namespace factories {

// Construye usuarios de mentira para pruebas y datos de desarrollo.
//
// Nota honesta: esto todavia no lo usa nadie. Cobra sentido cuando el proyecto
// tenga tests; hoy los datos de ejemplo se cargan con `syrax db:seed`, que
// corre database/seeders/*.sql.
struct UserFactory {
    static models::User make(std::int64_t id = 1) {
        return models::User{
            .id    = id,
            .name  = "User " + std::to_string(id),
            .email = "user" + std::to_string(id) + "@example.com",
            .age   = 20 + static_cast<int>(id % 50),
        };
    }
};

}  // namespace factories
