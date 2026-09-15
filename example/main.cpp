#include <syrax/syrax.hpp>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>

using namespace syrax;

struct CreateUser {
    std::string name;
    std::string email;
    int         age;
};

struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
};

// "Base de datos" del spike. No es thread-safe: Drogon corre multihilo en
// produccion. Para M0 no importa, pero no copiar esto a ningun lado.
namespace {
std::map<std::int64_t, User> g_users;
std::int64_t                 g_nextId = 1;

bool emailExists(const std::string& email) {
    for (const auto& [id, u] : g_users) {
        if (u.email == email) return true;
    }
    return false;
}

std::int64_t save(const CreateUser& req) {
    const auto id = g_nextId++;
    g_users[id]   = User{.id = id, .name = req.name, .email = req.email};
    return id;
}

std::optional<User> findUser(std::int64_t id) {
    const auto it = g_users.find(id);
    if (it == g_users.end()) return std::nullopt;
    return it->second;
}
}  // namespace

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 8080);

    App app;

    app.post("/users", [](CreateUser req) -> Result<User> {
        if (emailExists(req.email))
            return Conflict("email already registered");

        return User{.id = save(req), .name = req.name, .email = req.email};
    });

    app.get("/users/{id}", [](std::int64_t id) -> Result<User> {
        auto user = findUser(id);
        if (!user) return NotFound("user not found");
        return *user;
    });

    app.run(port);
}
