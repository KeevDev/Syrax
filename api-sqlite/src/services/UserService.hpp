#pragma once

#include <syrax/syrax.hpp>

#include "models/User.hpp"
#include "requests/UserRequests.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace services {

// Las reglas de negocio. El controlador no decide nada; pregunta aqui.
namespace UserService {

syrax::Task<std::vector<models::User>>   list();
syrax::Task<std::optional<models::User>> byId(std::int64_t id);

// Devuelve nullopt si el email ya existe.
syrax::Task<std::optional<models::User>> create(requests::CreateUser input);
syrax::Task<std::optional<models::User>> update(std::int64_t id, requests::UpdateUser input);
syrax::Task<bool>                        remove(std::int64_t id);

}  // namespace UserService
}  // namespace services
