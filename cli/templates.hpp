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

# ccache acelera muchisimo las recompilaciones de las dependencias: Drogon
# son ~200 objetos y sin cache se rehacen enteros ante cualquier cambio de
# configuracion.
find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
    message(STATUS "ccache: ${CCACHE}")
endif()

include(FetchContent)
FetchContent_Declare(syrax
    GIT_REPOSITORY @REPO@
    GIT_TAG        @TAG@
)
# Drogon trae un trantor que declara cmake_minimum_required(3.5), y CMake 4
# avisa de que esa compatibilidad se va. Esto le aplica un minimo de politicas
# sin tocar su codigo; en CMake < 4 la variable no existe y se ignora. Se
# restaura despues para que tu proyecto siga viendo sus propios avisos.
set(_policy_min_backup "${CMAKE_POLICY_VERSION_MINIMUM}")
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

FetchContent_MakeAvailable(syrax)

set(CMAKE_POLICY_VERSION_MINIMUM "${_policy_min_backup}")
unset(_policy_min_backup)

# CONFIGURE_DEPENDS hace que agregar archivos no requiera tocar este CMake:
# cmake reescanea las fuentes en cada build.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/database/*.cpp)

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
logs/*
!logs/.gitkeep
)T";

inline constexpr std::string_view kEnvPostgres = R"T(# Puerto donde escucha la app. Un argumento en la linea de comandos lo pisa.
APP_PORT=8080

DB_ENGINE=postgres
DB_HOST=127.0.0.1

# Lo usan la app Y el docker-compose. Si el puerto esta ocupado por otro
# postgres local, cambialo aqui y los dos quedan de acuerdo.
DB_PORT=5432
DB_NAME=@NAME@
DB_USER=postgres
DB_PASSWORD=postgres
)T";

inline constexpr std::string_view kEnvSqlite = R"T(# Puerto donde escucha la app. Un argumento en la linea de comandos lo pisa.
APP_PORT=8080

DB_ENGINE=sqlite
DB_FILE=app.db
)T";

inline constexpr std::string_view kCompose = R"T(# docker compose lee el .env de este directorio, asi que DB_PORT es la unica
# fuente de verdad: la cambias ahi y la app y el contenedor quedan de acuerdo.
#
# Si el puerto ya esta ocupado (es comun tener varios postgres locales),
# cambia DB_PORT en .env por uno libre. El 5432 de la derecha es el interno
# del contenedor y no se toca.
services:
  db:
    image: postgres:17-alpine
    environment:
      POSTGRES_DB: ${DB_NAME:-@NAME@}
      POSTGRES_USER: ${DB_USER:-postgres}
      POSTGRES_PASSWORD: ${DB_PASSWORD:-postgres}
    ports:
      - "${DB_PORT:-5432}:5432"
    volumes:
      - pgdata:/var/lib/postgresql/data
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U ${DB_USER:-postgres}"]
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

inline constexpr std::string_view kBootstrapH = R"T(#pragma once

#include <syrax/syrax.hpp>

namespace bootstrap {

syrax::App create();

}  // namespace bootstrap
)T";

inline constexpr std::string_view kBootstrapCpp = R"T(#include "bootstrap/app.hpp"

#include "routes/routes.hpp"

#include <filesystem>

namespace bootstrap {

syrax::App create() {
    if (std::filesystem::exists("config/app.json")) {
        drogon::app().loadConfigFile("config/app.json");
    }

    syrax::db::configureFromEnv();

    syrax::App app;

    app.docs("@NAME@", "1.0.0");

    registerRoutes(app);
    return app;
}

}  // namespace bootstrap
)T";

inline constexpr std::string_view kGitkeep = R"T()T";

inline constexpr std::string_view kPublicIndex = R"T(<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>@NAME@</title></head>
<body>
  <h1>@NAME@</h1>
  <p>Archivos estaticos salen de esta carpeta. La API vive en <code>/api/v1</code>.</p>
  <p><a href="/docs">Documentacion</a></p>
</body>
</html>
)T";

inline constexpr std::string_view kDockerfile = R"T(# Build y runtime separados: la imagen final no carga compilador ni fuentes.
# Ambas etapas usan la misma base para que las versiones de las librerias
# compartidas coincidan.
#
# Este Dockerfile se construye en cada push desde el CI de Syrax, que ademas
# arranca la imagen y le pega a /health. Si falla en tu maquina y no en el CI,
# lo primero que hay que mirar es si tu demonio de Docker alcanza los repos
# de Debian.
#
# Si `apt-get install` falla en la etapa runtime por un nombre de paquete,
# comprueba el soname en tu version de Debian:
#     docker run --rm debian:trixie-slim sh -c "apt-get update && apt-cache search jsoncpp"
# El sufijo de libjsoncpp cambia entre releases (25 en bookworm, 26 en trixie).
FROM debian:trixie AS build

RUN apt-get update && apt-get install -y --no-install-recommends         g++ cmake ninja-build git ca-certificates pkg-config         libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev         libpq-dev libsqlite3-dev libc-ares-dev libbrotli-dev     && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release     && cmake --build build

FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends         libjsoncpp26 libuuid1 zlib1g libssl3t64 libpq5 libsqlite3-0         libc-ares2 libbrotli1 ca-certificates     && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/@NAME@ /app/@NAME@
COPY --from=build /src/config /app/config
COPY --from=build /src/public /app/public

EXPOSE 8080
CMD ["/app/@NAME@"]
)T";

inline constexpr std::string_view kDockerignore = R"T(build/
.git/
.env
*.db
logs/
)T";

inline constexpr std::string_view kMigrationUsers = R"T(#pragma once

#include <syrax/syrax.hpp>

#include <string>

struct CreateUsersTable : syrax::Migration {
    std::string name() const override { return "001_create_users"; }

    void up(syrax::Schema& schema) override {
        schema.create("users", [](syrax::Blueprint& table) {
            table.id();
            table.string("name");
            table.string("email").unique();
            table.integer("age").defaultTo("0");
            table.timestamps();
        });
    }

    void down(syrax::Schema& schema) override {
        schema.drop("users");
    }
};
)T";

inline constexpr std::string_view kMigrationsH = R"T(#pragma once

#include <syrax/syrax.hpp>

void registerMigrations(syrax::Migrator& migrator);
)T";

inline constexpr std::string_view kMigrationsCpp = R"T(#include "migrations.hpp"

#include "migrations/001_create_users.hpp"

void registerMigrations(syrax::Migrator& migrator) {
    migrator.add<CreateUsersTable>();
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
├── migrations.cpp        que migraciones existen y en que orden
├── migrations/           las migraciones    -> syrax migrate
├── seeders/              datos demo         -> syrax db:seed
└── factories/            objetos de mentira para tests

src/
├── main.cpp              arranque y conexion a la BD
├── routes/               el mapa de la API
│   ├── routes.cpp        engancha las versiones
│   └── v1.cpp            rutas de /api/v1
├── http/                 TODO lo atado al transporte
│   ├── controllers/User/ recibe, delega, responde
│   ├── requests/User/    lo que entra
│   └── resources/User/   lo que sale
├── services/User/        logica de negocio
├── repositories/User/    SQL. lo unico que sabe de la BD
└── models/User/          la forma de la tabla
```

**Por que existe `http/`:** marca un limite real. Si manana expones la misma
logica por gRPC, esa carpeta se tira entera y `services/`, `repositories/` y
`models/` siguen sirviendo sin tocarse.

Cada capa se subdivide por recurso (`User/`, `Order/`) para que con veinte
entidades ninguna carpeta sea un basurero plano.

## Versionar la API

`src/routes/v1.cpp` monta todo bajo `/api/v1`. Para una v2: copia ese archivo,
cambia `kPrefix`, y registralo en `routes.cpp`. Las dos versiones conviven y
pueden apuntar a controladores distintos.

**Por que models/ y resources/ estan separados:** `User` tiene `passwordHash`
y `UserResource` no. Un campo privado no puede filtrarse por accidente porque
el tipo que se serializa simplemente no lo tiene.

## Agregar un recurso

1. Migracion en `database/migrations/` y su linea en `database/migrations.cpp`
2. `models/Product/Product.hpp` — un struct plano con los campos de la tabla
3. `repositories/Product/ProductRepository.*` — el SQL
4. `services/Product/ProductService.*` — las reglas
5. `http/requests/Product/` y `http/resources/Product/`
6. `http/controllers/Product/` y registralo en `src/routes/v1.cpp`

No hay que tocar el `CMakeLists.txt`.

## Sobre el mapeo

Syrax convierte filas a structs reflejando los nombres de campo en tiempo de
compilacion: el campo `email` se llena con la columna `email`. No hay que
escribir ese mapeo ni generar modelos de 500 lineas.
)T";

// ================================================================== src/

inline constexpr std::string_view kMain = R"T(#include <syrax/syrax.hpp>

#include "bootstrap/app.hpp"
#include "migrations.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

int migrations(const std::string& command) {
    syrax::Migrator migrator;
    registerMigrations(migrator);

    if (command == "migrate")          return migrator.migrate();
    if (command == "migrate:rollback") return migrator.rollback();
    return migrator.status();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string arg = (argc > 1) ? argv[1] : "";

    if (arg == "migrate" || arg == "migrate:rollback" || arg == "migrate:status") {
        return migrations(arg);
    }

    const auto port = arg.empty() ? syrax::envPort()
                                  : static_cast<std::uint16_t>(std::atoi(arg.c_str()));

    auto app = bootstrap::create();
    app.run(port);
    return 0;
}
)T";

inline constexpr std::string_view kRoutesH = R"T(#pragma once

#include <syrax/syrax.hpp>

void registerRoutes(syrax::App& app);
)T";

inline constexpr std::string_view kRoutesV1H = R"T(#pragma once

#include <syrax/syrax.hpp>

#include <string_view>

namespace routes::v1 {

inline constexpr std::string_view kPrefix = "/api/v1";

void register_(syrax::App& app);

}  // namespace routes::v1
)T";

inline constexpr std::string_view kRoutesV1Cpp = R"T(#include "routes/v1.hpp"

#include "http/controllers/User/UserController.hpp"

namespace routes::v1 {

void register_(syrax::App& app) {
    controllers::user::routes(app, kPrefix);

}

}  // namespace routes::v1
)T";

inline constexpr std::string_view kRoutesCpp = R"T(#include "routes/routes.hpp"

#include "routes/v1.hpp"

// Con nombre, no anonimo: Glaze refleja este struct para serializarlo, y un
// tipo sin enlace no se puede reflejar bajo clang.
namespace health {

struct Status {
    std::string status;
};

}  // namespace health

void registerRoutes(syrax::App& app) {
    app.get("/health", []() -> syrax::Result<health::Status> {
        return health::Status{.status = "ok"};
    });

    routes::v1::register_(app);

}
)T";

// ============================================================== models/

inline constexpr std::string_view kModelUser = R"T(#pragma once

#include <cstdint>
#include <string>

namespace models {

struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;

    // Con esto el modelo se puede usar con Query<User>, save() y remove().
    // Sin esto sigue sirviendo para db::query con SQL a mano.
    static constexpr auto table = "users";
};

}  // namespace models
)T";

// ============================================================ requests/

inline constexpr std::string_view kRequestsUser = R"T(#pragma once

#include <syrax/syrax.hpp>

#include <string>

namespace requests {

struct CreateUser {
    std::string name;
    std::string email;
    int         age;

    static auto rules() {
        return syrax::rules(syrax::field(&CreateUser::name).notEmpty().minLen(2).maxLen(80),
                            syrax::field(&CreateUser::email).email(),
                            syrax::field(&CreateUser::age).range(0, 130));
    }
};

struct UpdateUser {
    std::string name;
    std::string email;

    static auto rules() {
        return syrax::rules(syrax::field(&UpdateUser::name).notEmpty().minLen(2).maxLen(80),
                            syrax::field(&UpdateUser::email).email());
    }
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

inline constexpr std::string_view kResourceUserCpp = R"T(#include "http/resources/User/UserResource.hpp"

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

namespace UserRepository {
syrax::Task<std::vector<models::User>>   all();
syrax::Task<std::optional<models::User>> find(std::int64_t id);
syrax::Task<bool>                        emailTaken(std::string email);
syrax::Task<std::optional<models::User>> createIfEmailFree(std::string name,
                                                           std::string email, int age);
syrax::Task<std::optional<models::User>> update(std::int64_t id, std::string name, std::string email);
syrax::Task<bool>                        remove(std::int64_t id);

}  // namespace UserRepository
}  // namespace repositories
)T";

inline constexpr std::string_view kRepoUserCpp = R"T(#include "repositories/User/UserRepository.hpp"

namespace repositories::UserRepository {

using syrax::db::execute;
using syrax::db::findOne;
using syrax::db::returning;

// Con el query builder: las columnas se verifican en compilacion, asi que
// &models::User::nombre_mal no compila en vez de fallar en produccion.
syrax::Task<std::vector<models::User>> all() {
    co_return co_await syrax::Query<models::User>().orderBy(&models::User::id).get();
}

syrax::Task<std::optional<models::User>> find(std::int64_t id) {
    co_return co_await syrax::Query<models::User>().where(&models::User::id, "=", id).first();
}

// Con SQL a mano: sigue disponible, y es lo que usarias para un JOIN o
// cualquier cosa que el builder no cubre.
syrax::Task<bool> emailTaken(std::string email) {
    const auto found = co_await findOne<models::User>(
        "SELECT id, name, email, age FROM users WHERE email = @P1@", std::move(email));
    co_return found.has_value();
}

// Comprobar y luego insertar en dos consultas sueltas es una condicion de
// carrera: dos peticiones simultaneas ven el email libre las dos. Dentro de
// una transaccion, y con el UNIQUE de la tabla detras, una gana y la otra
// recibe nullopt.
syrax::Task<std::optional<models::User>> createIfEmailFree(std::string name,
                                                           std::string email, int age) {
    co_return co_await syrax::db::transaction(
        [name = std::move(name), email = std::move(email), age](const syrax::db::Tx& tx)
            -> syrax::Task<std::optional<models::User>> {
            // tx.client() es lo que mete al query builder dentro de la
            // transaccion: sin eso, estas dos consultas irian por fuera y la
            // carrera seguiria abierta.
            const bool tomado = co_await syrax::Query<models::User>(tx.client())
                                    .where(&models::User::email, "=", email)
                                    .exists();
            if (tomado) co_return std::nullopt;

            // La clave primaria a cero es lo que le dice a save() que esto
            // es un alta y no una actualizacion.
            models::User nuevo{.id = 0, .name = name, .email = email, .age = age};
            co_await syrax::save(nuevo, tx.client());
            co_return nuevo;
        });
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
#include "http/requests/User/UserRequests.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace services {

namespace UserService {
syrax::Task<std::vector<models::User>>   list();
syrax::Task<std::optional<models::User>> byId(std::int64_t id);

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
    co_return co_await repo::createIfEmailFree(std::move(input.name),
                                               std::move(input.email), input.age);
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

void routes(syrax::App& app, std::string_view prefix);

}  // namespace controllers::user
)T";

inline constexpr std::string_view kControllerUserCpp = R"T(#include "http/controllers/User/UserController.hpp"

#include "http/requests/User/UserRequests.hpp"
#include "http/resources/User/UserResource.hpp"
#include "services/User/UserService.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace syrax;

namespace controllers::user {

namespace service = services::UserService;

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

    {"database/migrations.hpp",                  kMigrationsH},
    {"database/migrations.cpp",                  kMigrationsCpp},
    {"database/migrations/001_create_users.hpp", kMigrationUsers},
    {"database/seeders/001_users.sql",           kSeederPostgres,    Engine::Postgres},
    {"database/seeders/001_users.sql",           kSeederSqlite,      Engine::Sqlite},
    {"database/factories/UserFactory.hpp",       kUserFactory},

    {"docker/Dockerfile",                        kDockerfile},
    {".dockerignore",                            kDockerignore},
    {"public/index.html",                        kPublicIndex},
    {"logs/.gitkeep",                            kGitkeep},

    {"src/main.cpp",                             kMain},
    {"src/bootstrap/app.hpp",                    kBootstrapH},
    {"src/bootstrap/app.cpp",                    kBootstrapCpp},
    {"src/routes/routes.hpp",                    kRoutesH},
    {"src/routes/routes.cpp",                    kRoutesCpp},
    {"src/routes/v1.hpp",                        kRoutesV1H},
    {"src/routes/v1.cpp",                        kRoutesV1Cpp},

    {"src/models/User/User.hpp",                 kModelUser},
    {"src/http/requests/User/UserRequests.hpp",  kRequestsUser},
    {"src/http/resources/User/UserResource.hpp", kResourceUserH},
    {"src/http/resources/User/UserResource.cpp", kResourceUserCpp},
    {"src/repositories/User/UserRepository.hpp", kRepoUserH},
    {"src/repositories/User/UserRepository.cpp", kRepoUserCpp},
    {"src/services/User/UserService.hpp",        kServiceUserH},
    {"src/services/User/UserService.cpp",        kServiceUserCpp},
    {"src/http/controllers/User/UserController.hpp", kControllerUserH},
    {"src/http/controllers/User/UserController.cpp", kControllerUserCpp},
};

}  // namespace tpl
