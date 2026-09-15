#pragma once

// Plantillas que genera `syrax new`.
//
// La estructura es por capas (controllers/, services/, repositories/...)
// porque es la organizacion que mas gente reconoce de inmediato. Ninguno de
// esos nombres lo conoce el framework: son archivos C++ normales.
//
// Tokens sustituidos: @NAME@ @REPO@ @TAG@ @ENGINE@ @P1@..@P3@

#include <string_view>

namespace tpl {

enum class Engine { Any, Postgres, Sqlite };

struct File {
    std::string_view path;
    std::string_view content;
    Engine           engine = Engine::Any;
};

// ================================================================== raiz

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

# CONFIGURE_DEPENDS hace que agregar archivos no requiera tocar este CMake:
# cmake reescanea las fuentes en cada build.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_executable(@NAME@ ${SOURCES})
target_include_directories(@NAME@ PRIVATE src database)
target_link_libraries(@NAME@ PRIVATE syrax::syrax)
)T";

inline constexpr std::string_view kGitignore = R"T(build/
.cache/
compile_commands.json
uploads/
.env
*.db
)T";

inline constexpr std::string_view kEnvPostgres = R"T(DB_ENGINE=postgres
DB_HOST=127.0.0.1
DB_PORT=5432
DB_NAME=@NAME@
DB_USER=postgres
DB_PASSWORD=postgres
)T";

inline constexpr std::string_view kEnvSqlite = R"T(DB_ENGINE=sqlite
DB_FILE=app.db
)T";

inline constexpr std::string_view kCompose = R"T(services:
  db:
    image: postgres:17-alpine
    environment:
      POSTGRES_DB: @NAME@
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: postgres
    ports:
      - "5432:5432"
    volumes:
      - pgdata:/var/lib/postgresql/data
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U postgres"]
      interval: 5s
      retries: 10

volumes:
  pgdata:
)T";

inline constexpr std::string_view kMigrationPostgres = R"T(CREATE TABLE IF NOT EXISTS users (
    id         BIGSERIAL PRIMARY KEY,
    name       TEXT        NOT NULL,
    email      TEXT        NOT NULL UNIQUE,
    age        INTEGER     NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
)T";

inline constexpr std::string_view kMigrationSqlite = R"T(CREATE TABLE IF NOT EXISTS users (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT    NOT NULL,
    email      TEXT    NOT NULL UNIQUE,
    age        INTEGER NOT NULL DEFAULT 0,
    created_at TEXT    NOT NULL DEFAULT (datetime('now'))
);
)T";

inline constexpr std::string_view kAppConfig = R"T({
    "app": {
        "number_of_threads": 0,
        "enable_session": false,
        "document_root": "./public",
        "max_connections": 100000,
        "client_max_body_size": "1M"
    },
    "log": {
        "log_level": "INFO"
    }
}
)T";

inline constexpr std::string_view kSeederPostgres = R"T(-- Datos de ejemplo. Se corre con: syrax db:seed
INSERT INTO users (name, email, age) VALUES
    ('Ada Lovelace',  'ada@example.com',  36),
    ('Alan Turing',   'alan@example.com', 41),
    ('Grace Hopper',  'grace@example.com', 85)
ON CONFLICT (email) DO NOTHING;
)T";

inline constexpr std::string_view kSeederSqlite = R"T(-- Datos de ejemplo. Se corre con: syrax db:seed
INSERT OR IGNORE INTO users (name, email, age) VALUES
    ('Ada Lovelace',  'ada@example.com',  36),
    ('Alan Turing',   'alan@example.com', 41),
    ('Grace Hopper',  'grace@example.com', 85);
)T";

inline constexpr std::string_view kUserFactory = R"T(#pragma once

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
)T";

inline constexpr std::string_view kReadme = R"T(# @NAME@

API construida con [Syrax](https://github.com/KeevDev/Syrax). Motor: **@ENGINE@**.

## Arrancar

```bash
@SETUP@
syrax migrate
syrax db:seed     # opcional: datos de ejemplo
syrax serve
```

```bash
curl localhost:8080/users
curl -X POST localhost:8080/users -H 'Content-Type: application/json' \
     -d '{"name":"Kevin","email":"kev@example.com","age":30}'
```

## Estructura

```
config/app.json           ajustes del servidor (versionado)
.env                      credenciales (NO versionado)

database/
├── migrations/           esquema     -> syrax migrate
├── seeders/              datos demo  -> syrax db:seed
└── factories/            objetos de mentira para tests

src/
├── main.cpp              arranque y conexion a la BD
├── routes/
│   ├── routes.cpp        engancha las versiones
│   └── v1.cpp            rutas de /api/v1
├── controllers/User/     HTTP: recibe, delega, responde
├── services/User/        logica de negocio
├── repositories/User/    SQL. lo unico que sabe de la BD
├── models/User/          la forma de la tabla
├── requests/User/        lo que entra
└── resources/User/       lo que sale
```

Cada capa se subdivide por recurso (`controllers/User/`, `controllers/Order/`)
para que con veinte entidades ninguna carpeta sea un basurero plano.

## Versionar la API

`src/routes/v1.cpp` monta todo bajo `/api/v1`. Para una v2: copia ese archivo,
cambia `kPrefix`, y registralo en `routes.cpp`. Las dos versiones conviven y
pueden apuntar a controladores distintos.

**Por que models/ y resources/ estan separados:** `User` tiene `passwordHash`
y `UserResource` no. Un campo privado no puede filtrarse por accidente porque
el tipo que se serializa simplemente no lo tiene.

## Agregar un recurso

1. Migracion en `migrations/`
2. `models/Product.hpp` — un struct plano con los campos de la tabla
3. `repositories/ProductRepository.*` — el SQL
4. `services/ProductService.*` — las reglas
5. `resources/ProductResource.*` y `requests/ProductRequests.hpp`
6. `controllers/ProductController.*` y registralo en `src/routes.cpp`

No hay que tocar el `CMakeLists.txt`.

## Sobre el mapeo

Syrax convierte filas a structs reflejando los nombres de campo en tiempo de
compilacion: el campo `email` se llena con la columna `email`. No hay que
escribir ese mapeo ni generar modelos de 500 lineas.
)T";

// ================================================================== src/

inline constexpr std::string_view kMain = R"T(#include <syrax/syrax.hpp>

#include "routes/routes.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 8080);

    // Ajustes del servidor: hilos, logging, limites. Sin secretos: esto se
    // versiona.
    if (std::filesystem::exists("config/app.json")) {
        drogon::app().loadConfigFile("config/app.json");
    }

    // Credenciales y conexion, desde el entorno. Esto NO se versiona.
    // Ver .env
    syrax::db::configureFromEnv();

    syrax::App app;
    registerRoutes(app);
    app.run(port);
}
)T";

inline constexpr std::string_view kRoutesH = R"T(#pragma once

#include <syrax/syrax.hpp>

// Punto unico donde se arma la API. Cada version vive en su propio archivo.
void registerRoutes(syrax::App& app);
)T";

inline constexpr std::string_view kRoutesV1H = R"T(#pragma once

#include <syrax/syrax.hpp>

#include <string_view>

namespace routes::v1 {

// Todas las rutas de esta version cuelgan de aqui. Para sacar una v2 se copia
// este archivo, se cambia el prefijo, y las dos conviven.
inline constexpr std::string_view kPrefix = "/api/v1";

void register_(syrax::App& app);

}  // namespace routes::v1
)T";

inline constexpr std::string_view kRoutesV1Cpp = R"T(#include "routes/v1.hpp"

#include "controllers/User/UserController.hpp"

namespace routes::v1 {

void register_(syrax::App& app) {
    controllers::user::routes(app, kPrefix);

    // los controladores de esta version se registran aqui
}

}  // namespace routes::v1
)T";

inline constexpr std::string_view kRoutesCpp = R"T(#include "routes/routes.hpp"

#include "routes/v1.hpp"

void registerRoutes(syrax::App& app) {
    routes::v1::register_(app);

    // routes::v2::register_(app);
}
)T";

// ============================================================== models/

inline constexpr std::string_view kModelUser = R"T(#pragma once

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
)T";

// ============================================================ requests/

inline constexpr std::string_view kRequestsUser = R"T(#pragma once

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
)T";

// =========================================================== resources/

inline constexpr std::string_view kResourceUserH = R"T(#pragma once

#include "models/User/User.hpp"

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
)T";

inline constexpr std::string_view kResourceUserCpp = R"T(#include "resources/User/UserResource.hpp"

namespace resources {

UserResource from(const models::User& user) {
    return UserResource{
        .id    = user.id,
        .name  = user.name,
        .email = user.email,
    };
}

std::vector<UserResource> from(const std::vector<models::User>& users) {
    std::vector<UserResource> out;
    out.reserve(users.size());
    for (const auto& user : users) {
        out.push_back(from(user));
    }
    return out;
}

}  // namespace resources
)T";

// ======================================================== repositories/

inline constexpr std::string_view kRepoUserH = R"T(#pragma once

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
)T";

inline constexpr std::string_view kRepoUserCpp = R"T(#include "repositories/User/UserRepository.hpp"

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
        "SELECT id, name, email, age FROM users WHERE id = @P1@", id);
}

syrax::Task<bool> emailTaken(std::string email) {
    const auto found = co_await findOne<models::User>(
        "SELECT id, name, email, age FROM users WHERE email = @P1@", std::move(email));
    co_return found.has_value();
}

syrax::Task<models::User> create(std::string name, std::string email, int age) {
    co_return co_await returning<models::User>(
        "INSERT INTO users (name, email, age) VALUES (@P1@, @P2@, @P3@) "
        "RETURNING id, name, email, age",
        std::move(name), std::move(email), age);
}

syrax::Task<std::optional<models::User>> update(std::int64_t id, std::string name,
                                                std::string email) {
    const auto rows = co_await execute(
        "UPDATE users SET name = @P1@, email = @P2@ WHERE id = @P3@",
        std::move(name), std::move(email), id);

    if (rows == 0) co_return std::nullopt;
    co_return co_await find(id);
}

syrax::Task<bool> remove(std::int64_t id) {
    const auto rows = co_await execute("DELETE FROM users WHERE id = @P1@", id);
    co_return rows > 0;
}

}  // namespace repositories::UserRepository
)T";

// ============================================================= services/

inline constexpr std::string_view kServiceUserH = R"T(#pragma once

#include <syrax/syrax.hpp>

#include "models/User/User.hpp"
#include "requests/User/UserRequests.hpp"

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
)T";

inline constexpr std::string_view kServiceUserCpp = R"T(#include "services/User/UserService.hpp"

#include "repositories/User/UserRepository.hpp"

namespace services::UserService {

namespace repo = repositories::UserRepository;

syrax::Task<std::vector<models::User>> list() {
    co_return co_await repo::all();
}

syrax::Task<std::optional<models::User>> byId(std::int64_t id) {
    co_return co_await repo::find(id);
}

syrax::Task<std::optional<models::User>> create(requests::CreateUser input) {
    // La regla de negocio vive aqui, no en el controlador ni en el SQL.
    if (co_await repo::emailTaken(input.email)) co_return std::nullopt;

    co_return co_await repo::create(std::move(input.name), std::move(input.email),
                                    input.age);
}

syrax::Task<std::optional<models::User>> update(std::int64_t id,
                                                requests::UpdateUser input) {
    co_return co_await repo::update(id, std::move(input.name), std::move(input.email));
}

syrax::Task<bool> remove(std::int64_t id) {
    co_return co_await repo::remove(id);
}

}  // namespace services::UserService
)T";

// ========================================================== controllers/

inline constexpr std::string_view kControllerUserH = R"T(#pragma once

#include <syrax/syrax.hpp>

#include <string_view>

namespace controllers::user {

// El prefijo lo decide la version de rutas que lo registra, no el controlador.
void routes(syrax::App& app, std::string_view prefix);

}  // namespace controllers::user
)T";

inline constexpr std::string_view kControllerUserCpp = R"T(#include "controllers/User/UserController.hpp"

#include "requests/User/UserRequests.hpp"
#include "resources/User/UserResource.hpp"
#include "services/User/UserService.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace syrax;

namespace controllers::user {

namespace service = services::UserService;

// Los controladores son delgados a proposito: reciben, delegan, y traducen
// el resultado a HTTP. Ninguna regla de negocio vive aqui.
void routes(App& app, std::string_view prefix) {
    const std::string base{prefix};

    app.get(base + "/users", []() -> Task<Result<std::vector<resources::UserResource>>> {
        co_return resources::from(co_await service::list());
    });

    app.get(base + "/users/{id}", [](std::int64_t id) -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::byId(id);
        if (!user) co_return NotFound("user not found");

        co_return resources::from(*user);
    });

    app.post(base + "/users", [](requests::CreateUser body)
                 -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::create(std::move(body));
        if (!user) co_return Conflict("email already registered");

        co_return resources::from(*user);
    });

    app.put(base + "/users/{id}", [](std::int64_t id, requests::UpdateUser body)
                 -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::update(id, std::move(body));
        if (!user) co_return NotFound("user not found");

        co_return resources::from(*user);
    });

    app.del(base + "/users/{id}", [](std::int64_t id)
                -> Task<Result<resources::DeletedResource>> {
        if (!co_await service::remove(id)) co_return NotFound("user not found");

        co_return resources::DeletedResource{.id = id, .deleted = true};
    });
}

}  // namespace controllers::user
)T";

// ================================================================ indice

inline constexpr File kProjectFiles[] = {
    {"CMakeLists.txt",                           kCMake},
    {".gitignore",                               kGitignore},
    {"README.md",                                kReadme},
    {"config/app.json",                          kAppConfig},
    {".env.example",                             kEnvPostgres,       Engine::Postgres},
    {".env.example",                             kEnvSqlite,         Engine::Sqlite},
    {".env",                                     kEnvPostgres,       Engine::Postgres},
    {".env",                                     kEnvSqlite,         Engine::Sqlite},
    {"docker-compose.yml",                       kCompose,           Engine::Postgres},

    {"database/migrations/001_create_users.sql", kMigrationPostgres, Engine::Postgres},
    {"database/migrations/001_create_users.sql", kMigrationSqlite,   Engine::Sqlite},
    {"database/seeders/001_users.sql",           kSeederPostgres,    Engine::Postgres},
    {"database/seeders/001_users.sql",           kSeederSqlite,      Engine::Sqlite},
    {"database/factories/UserFactory.hpp",       kUserFactory},

    {"src/main.cpp",                             kMain},
    {"src/routes/routes.hpp",                    kRoutesH},
    {"src/routes/routes.cpp",                    kRoutesCpp},
    {"src/routes/v1.hpp",                        kRoutesV1H},
    {"src/routes/v1.cpp",                        kRoutesV1Cpp},

    {"src/models/User/User.hpp",                 kModelUser},
    {"src/requests/User/UserRequests.hpp",       kRequestsUser},
    {"src/resources/User/UserResource.hpp",      kResourceUserH},
    {"src/resources/User/UserResource.cpp",      kResourceUserCpp},
    {"src/repositories/User/UserRepository.hpp", kRepoUserH},
    {"src/repositories/User/UserRepository.cpp", kRepoUserCpp},
    {"src/services/User/UserService.hpp",        kServiceUserH},
    {"src/services/User/UserService.cpp",        kServiceUserCpp},
    {"src/controllers/User/UserController.hpp",  kControllerUserH},
    {"src/controllers/User/UserController.cpp",  kControllerUserCpp},
};

}  // namespace tpl
