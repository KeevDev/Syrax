#include <syrax/syrax.hpp>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <chrono>
#include <iostream>
#include <optional>
#include <vector>
#include <string>

using namespace syrax;

struct CreateUser {
    std::string name;
    std::string email;
    int         age;
};

struct Whoami {
    std::string sub;
    std::string role;
};

struct Health {
    std::string status;
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

    // Seguridad de ejemplo: CORS, cabeceras, limite de peticiones, y una
    // rama protegida por Bearer token.
    app.cors()
       .useOnResponse(securityHeaders())
       .use(rateLimit(100, std::chrono::seconds{60}))
       .use("/admin", auth::bearer("secreto-de-ejemplo"));

    app.get("/admin/me", [](const Request& req) -> Result<Whoami> {
        return Whoami{.sub = req.get("auth.sub"), .role = req.get("auth.role")};
    });

    // Handler sin argumentos: el caso que rompia la deduccion del body.
    app.get("/users", []() -> Result<std::vector<User>> {
        std::vector<User> out;
        for (const auto& [id, u] : g_users) out.push_back(u);
        return out;
    });

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

    // Guardia de regresion: ejercita el camino de registro de corrutinas,
    // que tiene reglas de vida de parametros distintas al sincrono.
    app.get("/health", []() -> Task<Result<Health>> {
        co_return Health{.status = "ok"};
    });

    app.run(port);
}
