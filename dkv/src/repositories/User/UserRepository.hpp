#pragma once

#include <syrax/syrax.hpp>

#include "models/User/User.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace repositories {

// SQL y nada mas. Si cambias de motor o de esquema, este es el unico archivo
// que se toca.
namespace UserRepository {

syrax::Task<std::vector<models::User>>   all();
syrax::Task<std::optional<models::User>> find(std::int64_t id);
syrax::Task<bool>                        emailTaken(std::string email);
syrax::Task<models::User>                create(std::string name, std::string email, int age);
syrax::Task<std::optional<models::User>> update(std::int64_t id, std::string name, std::string email);
syrax::Task<bool>                        remove(std::int64_t id);

}  // namespace UserRepository
}  // namespace repositories
