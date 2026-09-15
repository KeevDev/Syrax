#pragma once

// Plantillas que genera `syrax new`.
//
// La estructura es por FEATURE (src/users/) y no por CAPA (controllers/,
// models/, resources/). Cuando trabajas en usuarios tocas una carpeta, no
// cinco. Nada de esto lo conoce el framework: son archivos C++ normales que
// puedes renombrar o borrar.

#include <string_view>

namespace tpl {

struct File {
    std::string_view path;
    std::string_view content;
};

// --------------------------------------------------------------- raiz

inline constexpr std::string_view kCMake = R"T(cmake_minimum_required(VERSION 3.25)
project(@NAME@ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(syrax
    GIT_REPOSITORY @REPO@
    GIT_TAG        @TAG@
)
FetchContent_MakeAvailable(syrax)

# CONFIGURE_DEPENDS hace que agregar un modulo nuevo no requiera tocar este
# archivo: cmake reescanea las fuentes en cada build.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_executable(@NAME@ ${SOURCES})
target_include_directories(@NAME@ PRIVATE src)
target_link_libraries(@NAME@ PRIVATE syrax::syrax)
)T";

inline constexpr std::string_view kGitignore = R"T(build/
.cache/
compile_commands.json
uploads/
)T";

inline constexpr std::string_view kReadme = R"T(# @NAME@

API construida con [Syrax](https://github.com/KeevDev/Syrax).

```bash
syrax serve           # compila y levanta en :8080
syrax serve --port N  # otro puerto
```

## Estructura

```
src/
├── main.cpp          arranque
├── routes.hpp/.cpp   donde se arma la API
└── users/            un modulo = una carpeta
    ├── dto.hpp       lo que entra y lo que sale
    ├── store.*       acceso a datos
    └── handlers.*    los endpoints
```

Organizado **por feature**, no por capa. Trabajar en usuarios toca una
carpeta, no cinco. Ninguno de estos nombres lo conoce Syrax: son archivos
C++ normales, renombralos o borralos si no te sirven.

## Agregar un modulo

1. `cp -r src/users src/products` y renombra el namespace
2. Registra su `registerRoutes` en `src/routes.cpp`

No hay que tocar el `CMakeLists.txt`.

## La base de datos

Syrax no trae ORM a proposito. `users/store.cpp` guarda en memoria y es la
costura donde enchufas la tuya: cambia esa implementacion por Postgres o
SQLite y los handlers no se enteran.

## Endpoints

| | |
|---|---|
| `GET /users` | listado |
| `GET /users/{id}` | uno |
| `POST /users` | crear |
| `PUT /users/{id}` | actualizar |
| `DELETE /users/{id}` | borrar |
)T";

// ---------------------------------------------------------------- src/

inline constexpr std::string_view kMain = R"T(#include <syrax/syrax.hpp>

#include "routes.hpp"

#include <cstdint>
#include <cstdlib>

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 8080);

    syrax::App app;
    registerRoutes(app);
    app.run(port);
}
)T";

inline constexpr std::string_view kRoutesH = R"T(#pragma once

#include <syrax/syrax.hpp>

// Punto unico donde se arma la API. Cada modulo expone su propio
// registerRoutes y se engancha aqui.
void registerRoutes(syrax::App& app);
)T";

inline constexpr std::string_view kRoutesCpp = R"T(#include "routes.hpp"

#include "users/handlers.hpp"

void registerRoutes(syrax::App& app) {
    users::registerRoutes(app);

    // los modulos nuevos se enganchan aqui
}
)T";

// --------------------------------------------------------- src/users/

inline constexpr std::string_view kUsersDto = R"T(#pragma once

#include <cstdint>
#include <string>

namespace users {

// --- lo que entra ---------------------------------------------------------

struct CreateUser {
    std::string name;
    std::string email;
    int         age;
};

struct UpdateUser {
    std::string name;
    std::string email;
};

// --- lo que sale ----------------------------------------------------------
//
// Separado del modelo interno a proposito: User (ver store.hpp) tiene
// passwordHash y UserResponse no. Esa es toda la razon de tener dos tipos:
// hace imposible filtrar un campo privado por accidente.

struct UserResponse {
    std::int64_t id;
    std::string  name;
    std::string  email;
};

struct DeletedResponse {
    std::int64_t id;
    bool         deleted;
};

}  // namespace users
)T";

inline constexpr std::string_view kUsersStoreH = R"T(#pragma once

#include "dto.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace users {

// El modelo interno. passwordHash nunca llega al cliente porque
// UserResponse simplemente no tiene ese campo.
struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;
    std::string  passwordHash;
};

// Syrax no trae ORM a proposito. Esto es la costura donde enchufas tu base
// de datos: reemplaza la implementacion de store.cpp por Postgres, SQLite o
// lo que uses y los handlers no cambian.
//
// La implementacion por defecto guarda en memoria y toma un mutex porque
// Drogon atiende peticiones en varios hilos a la vez.
class Store {
public:
    static Store& instance();

    std::vector<User>   all() const;
    std::optional<User> find(std::int64_t id) const;
    bool                emailTaken(const std::string& email) const;

    User                create(const CreateUser& input);
    std::optional<User> update(std::int64_t id, const UpdateUser& input);
    bool                remove(std::int64_t id);

private:
    mutable std::mutex mutex_;
    std::vector<User>  users_;
    std::int64_t       nextId_ = 1;
};

UserResponse toResponse(const User& user);

}  // namespace users
)T";

inline constexpr std::string_view kUsersStoreCpp = R"T(#include "store.hpp"

#include <algorithm>

namespace users {

Store& Store::instance() {
    static Store store;
    return store;
}

std::vector<User> Store::all() const {
    const std::lock_guard lock{mutex_};
    return users_;
}

std::optional<User> Store::find(std::int64_t id) const {
    const std::lock_guard lock{mutex_};

    const auto it = std::ranges::find(users_, id, &User::id);
    if (it == users_.end()) return std::nullopt;
    return *it;
}

bool Store::emailTaken(const std::string& email) const {
    const std::lock_guard lock{mutex_};
    return std::ranges::any_of(users_,
                               [&](const User& u) { return u.email == email; });
}

User Store::create(const CreateUser& input) {
    const std::lock_guard lock{mutex_};

    const User user{
        .id           = nextId_++,
        .name         = input.name,
        .email        = input.email,
        .age          = input.age,
        .passwordHash = {},
    };
    users_.push_back(user);
    return user;
}

std::optional<User> Store::update(std::int64_t id, const UpdateUser& input) {
    const std::lock_guard lock{mutex_};

    const auto it = std::ranges::find(users_, id, &User::id);
    if (it == users_.end()) return std::nullopt;

    it->name  = input.name;
    it->email = input.email;
    return *it;
}

bool Store::remove(std::int64_t id) {
    const std::lock_guard lock{mutex_};

    const auto removed = std::erase_if(users_, [id](const User& u) { return u.id == id; });
    return removed > 0;
}

UserResponse toResponse(const User& user) {
    return UserResponse{
        .id    = user.id,
        .name  = user.name,
        .email = user.email,
    };
}

}  // namespace users
)T";

inline constexpr std::string_view kUsersHandlersH = R"T(#pragma once

#include <syrax/syrax.hpp>

namespace users {

void registerRoutes(syrax::App& app);

}  // namespace users
)T";

inline constexpr std::string_view kUsersHandlersCpp = R"T(#include "handlers.hpp"

#include "dto.hpp"
#include "store.hpp"

#include <cstdint>
#include <vector>

using namespace syrax;

namespace users {

void registerRoutes(App& app) {

    app.get("/users", []() -> Result<std::vector<UserResponse>> {
        std::vector<UserResponse> out;
        for (const auto& user : Store::instance().all()) {
            out.push_back(toResponse(user));
        }
        return out;
    });

    app.get("/users/{id}", [](std::int64_t id) -> Result<UserResponse> {
        const auto user = Store::instance().find(id);
        if (!user) return NotFound("user not found");

        return toResponse(*user);
    });

    app.post("/users", [](CreateUser body) -> Result<UserResponse> {
        if (Store::instance().emailTaken(body.email))
            return Conflict("email already registered");

        return toResponse(Store::instance().create(body));
    });

    // Path param y body a la vez: el body es siempre el ultimo argumento.
    app.put("/users/{id}", [](std::int64_t id, UpdateUser body) -> Result<UserResponse> {
        const auto user = Store::instance().update(id, body);
        if (!user) return NotFound("user not found");

        return toResponse(*user);
    });

    app.del("/users/{id}", [](std::int64_t id) -> Result<DeletedResponse> {
        if (!Store::instance().remove(id)) return NotFound("user not found");

        return DeletedResponse{.id = id, .deleted = true};
    });
}

}  // namespace users
)T";

// ------------------------------------------------------------------ indice

inline constexpr File kProjectFiles[] = {
    {"CMakeLists.txt",         kCMake},
    {".gitignore",             kGitignore},
    {"README.md",              kReadme},
    {"src/main.cpp",           kMain},
    {"src/routes.hpp",         kRoutesH},
    {"src/routes.cpp",         kRoutesCpp},
    {"src/users/dto.hpp",      kUsersDto},
    {"src/users/store.hpp",    kUsersStoreH},
    {"src/users/store.cpp",    kUsersStoreCpp},
    {"src/users/handlers.hpp", kUsersHandlersH},
    {"src/users/handlers.cpp", kUsersHandlersCpp},
};

}  // namespace tpl
