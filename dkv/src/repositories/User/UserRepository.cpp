#include "repositories/User/UserRepository.hpp"

namespace repositories::UserRepository {

using syrax::db::execute;
using syrax::db::findOne;
using syrax::db::query;
using syrax::db::returning;

syrax::Task<std::vector<models::User>> all() {
    co_return co_await query<models::User>(
        "SELECT id, name, email, age FROM users ORDER BY id");
}

syrax::Task<std::optional<models::User>> find(std::int64_t id) {
    co_return co_await findOne<models::User>(
        "SELECT id, name, email, age FROM users WHERE id = $1", id);
}

syrax::Task<bool> emailTaken(std::string email) {
    const auto found = co_await findOne<models::User>(
        "SELECT id, name, email, age FROM users WHERE email = $1", std::move(email));
    co_return found.has_value();
}

syrax::Task<models::User> create(std::string name, std::string email, int age) {
    co_return co_await returning<models::User>(
        "INSERT INTO users (name, email, age) VALUES ($1, $2, $3) "
        "RETURNING id, name, email, age",
        std::move(name), std::move(email), age);
}

syrax::Task<std::optional<models::User>> update(std::int64_t id, std::string name,
                                                std::string email) {
    const auto rows = co_await execute(
        "UPDATE users SET name = $1, email = $2 WHERE id = $3",
        std::move(name), std::move(email), id);

    if (rows == 0) co_return std::nullopt;
    co_return co_await find(id);
}

syrax::Task<bool> remove(std::int64_t id) {
    const auto rows = co_await execute("DELETE FROM users WHERE id = $1", id);
    co_return rows > 0;
}

}  // namespace repositories::UserRepository
