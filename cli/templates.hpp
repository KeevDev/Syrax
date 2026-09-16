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

# Todo menos main.cpp va a una libreria, para que los tests puedan enlazar
# tus servicios y repositorios. Un ejecutable con main dentro no se puede
# enlazar dos veces.
list(REMOVE_ITEM SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(@NAME@_lib STATIC ${SOURCES})
target_include_directories(@NAME@_lib PUBLIC src database)
target_link_libraries(@NAME@_lib PUBLIC syrax::syrax)

add_executable(@NAME@ src/main.cpp)
target_link_libraries(@NAME@ PRIVATE @NAME@_lib)

# Los tests no entran en el build normal: Catch2 hay que bajarlo y no quieres
# esperarlo cada vez que levantas el servidor. `syrax test` enciende esto.
option(SYRAX_PROJECT_TESTS "Compila los tests del proyecto" OFF)
if(SYRAX_PROJECT_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
)T";

inline constexpr std::string_view kTestsCMake = R"T(include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# Agregar un archivo de test no obliga a tocar este CMake.
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(@NAME@_tests ${TEST_SOURCES})
target_link_libraries(@NAME@_tests PRIVATE @NAME@_lib Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(@NAME@_tests)
)T";

// Un test de verdad, no un assert(true): ejercita el mapeo a resource y las
// reglas del request, que son las dos cosas que se rompen al cambiar un
// modelo. Lo que necesita base de datos va aparte, y por eso no esta aqui.
inline constexpr std::string_view kTestUser = R"T(#include <catch2/catch_test_macros.hpp>

#include "factories/UserFactory.hpp"
#include "http/requests/User/UserRequests.hpp"
#include "http/resources/User/UserResource.hpp"

TEST_CASE("el resource expone solo lo que la API promete") {
    const auto user     = factories::UserFactory::make(7);
    const auto resource = resources::from(user);

    CHECK(resource.id == user.id);
    CHECK(resource.name == user.name);

    // La edad no sale al JSON: si alguien la agrega al resource sin querer,
    // este test no se entera, pero el de abajo si te dice que cambiaste las
    // reglas. Para lo que no debe salir, mira el snapshot del /openapi.json.
}

TEST_CASE("las reglas del request atrapan lo que el tipo no puede") {
    requests::CreateUser valido{.name = "Ada", .email = "ada@example.com", .age = 36};
    CHECK(syrax::validate(valido).empty());

    requests::CreateUser sinArroba{.name = "Ada", .email = "no-es-un-email", .age = 36};
    const auto fallos = syrax::validate(sinArroba);

    REQUIRE(fallos.size() == 1);
    CHECK(fallos.front().field == "email");
}

TEST_CASE("validate devuelve todos los fallos, no el primero") {
    requests::CreateUser malo{.name = "", .email = "tampoco", .age = 999};
    const auto           fallos = syrax::validate(malo);

    // Cuatro y no tres: un nombre vacio rompe notEmpty Y minLen(2). Cada
    // regla que falla es un error, que es lo que hace que el 422 los liste
    // todos y el cliente no tenga que ir descubriendolos de uno en uno.
    CHECK(fallos.size() == 4);
}
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
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>@NAME@ — Syrax</title>
<style>
:root {
  color-scheme: light dark;
  /* Los colores con los que GCC imprime un diagnostico: el error en rojo,
     el caret en verde, la nota en cian. La paleta sale del compilador. */
  --papel:  #f3f4f7;  --panel:  #ffffff;  --linea: #dfe1e8;
  --tinta:  #14161c;  --gris:   #666c7a;
  --error:  #c62828;  --caret:  #2e7d32;  --nota:  #0f6f86;
  --sombra: 0 1px 2px rgba(20, 22, 28, .05);
  --mono: ui-monospace, SFMono-Regular, "JetBrains Mono", Menlo, Consolas, monospace;
}
@media (prefers-color-scheme: dark) {
  :root {
    --papel: #101218; --panel: #171a22; --linea: #272b36;
    --tinta: #e8eaf0; --gris:  #9aa1b1;
    --error: #ef6b62; --caret: #78c47e; --nota:  #5cc0da;
    --sombra: none;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--papel); color: var(--tinta);
  font: 15px/1.65 ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  -webkit-font-smoothing: antialiased;
}
.hoja { max-width: 780px; margin: 0 auto; padding: 64px 24px 96px; }
a { color: var(--nota); }
a:focus-visible, .enlace:focus-visible { outline: 2px solid var(--nota); outline-offset: 3px; }

/* ---- cabecera ---- */
.prompt { font: 12px/1 var(--mono); color: var(--gris); margin-bottom: 26px; }
.prompt b { color: var(--caret); font-weight: 500; }
.marca {
  font: 600 46px/1 var(--mono); letter-spacing: -0.045em; margin: 0;
}
.tesis { color: var(--gris); margin: 12px 0 0; max-width: 46ch; font-size: 16px; }

/* ---- el diagnostico: la pieza central ---- */
.diagnostico {
  margin: 38px 0 12px; padding: 22px 24px; background: var(--panel);
  border: 1px solid var(--linea); border-left: 3px solid var(--error);
  border-radius: 4px; box-shadow: var(--sombra);
  font: 13px/1.85 var(--mono); overflow-x: auto;
}
.diagnostico .codigo { white-space: pre; }
.diagnostico .mal { color: var(--error); }
.subraya {
  display: block; color: var(--caret); white-space: pre;
  animation: crece .5s .35s steps(12, end) both;
}
@keyframes crece { from { clip-path: inset(0 100% 0 0); } to { clip-path: inset(0 0 0 0); } }
.veredicto { margin-top: 12px; color: var(--error); white-space: pre-wrap; }
.veredicto .sug { color: var(--caret); }
@media (prefers-reduced-motion: reduce) { .subraya { animation: none; } }

.pie-diagnostico { color: var(--gris); font-size: 14px; margin: 0 0 56px; max-width: 54ch; }

/* ---- secciones ---- */
h2 {
  font: 600 12px/1 var(--mono); letter-spacing: .14em; text-transform: uppercase;
  color: var(--gris); margin: 0 0 16px;
}
section { margin-bottom: 52px; }
pre {
  margin: 0; padding: 20px 22px; background: var(--panel); border: 1px solid var(--linea);
  border-radius: 4px; overflow-x: auto; font: 13px/1.8 var(--mono); box-shadow: var(--sombra);
}
pre .c { color: var(--gris); }
pre .k { color: var(--nota); }

/* lo que escribes / lo que no */
.cuentas { display: flex; gap: 28px; flex-wrap: wrap; align-items: flex-end; margin-top: 18px; }
.cuenta { flex: 1 1 200px; }
.cifra { font: 600 34px/1 var(--mono); letter-spacing: -.03em; display: block; }
.cifra.propia { color: var(--caret); }
.cifra.ajena  { color: var(--gris); text-decoration: line-through; text-decoration-thickness: 1px; }
.cuenta span.que { display: block; color: var(--gris); font-size: 13px; margin-top: 6px; }

.hace { list-style: none; padding: 0; margin: 16px 0 0; }
.hace li {
  padding: 9px 0 9px 22px; border-top: 1px solid var(--linea); position: relative;
  font-size: 14px; color: var(--gris);
}
.hace li::before {
  content: "->"; position: absolute; left: 0; color: var(--caret);
  font: 12px/1.9 var(--mono);
}
.hace li b { color: var(--tinta); font-weight: 500; }

.enlaces { display: grid; gap: 10px; grid-template-columns: repeat(auto-fit, minmax(210px, 1fr)); }
.enlace {
  display: block; padding: 15px 16px; background: var(--panel); border: 1px solid var(--linea);
  border-radius: 4px; text-decoration: none; color: inherit; box-shadow: var(--sombra);
  transition: border-color .15s ease;
}
.enlace:hover { border-color: var(--nota); }
.enlace b { display: block; font: 600 13px/1.4 var(--mono); }
.enlace span { color: var(--gris); font-size: 13px; }

footer { border-top: 1px solid var(--linea); padding-top: 18px; color: var(--gris); font-size: 13px; }
</style>
</head>
<body>
<div class="hoja">

  <p class="prompt">~/@NAME@ <b>$</b> syrax serve</p>
  <h1 class="marca">syrax</h1>
  <p class="tesis">Un framework de APIs para C++ moderno donde el compilador es la red de seguridad.</p>

  <div class="diagnostico">
    <div class="codigo">co_await Query&lt;User&gt;().where(&amp;User::<span class="mal">emial</span>, "=", email);
<span class="subraya">                                    ^~~~~</span></div>
    <div class="veredicto">error: 'emial' is not a member of 'User'<span class="sug">; did you mean 'email'?</span></div>
  </div>
  <p class="pie-diagnostico">
    El nombre de una columna es un puntero a miembro, no un string. La errata no
    llega a produccion porque no llega ni a compilar. Lo mismo vale para el body
    de una peticion, el esquema del OpenAPI y la forma de una fila.
  </p>

  <section>
    <h2>Una firma, y el resto sale solo</h2>
    <pre><span class="k">app</span>.post("/users", [](requests::CreateUser body)
        -&gt; Task&lt;Result&lt;UserResource&gt;&gt; {
    <span class="c">// el body ya llego parseado y validado</span>
    co_return resources::from(co_await service::create(body));
});</pre>
    <ul class="hace">
      <li><b>Parsea</b> el JSON en tu struct, por reflexion, sin macros</li>
      <li><b>Valida</b> las reglas del tipo y responde 422 con detalle por campo</li>
      <li><b>Serializa</b> la respuesta y fija el codigo de estado</li>
      <li><b>Documenta</b> la ruta en /docs sin que anotes nada</li>
    </ul>
  </section>

  <section>
    <h2>Un modelo son quince lineas</h2>
    <div class="cuentas">
      <div class="cuenta">
        <span class="cifra propia">15</span>
        <span class="que">el struct que escribes tu</span>
      </div>
      <div class="cuenta">
        <span class="cifra ajena">1.632</span>
        <span class="que">las que genera un ORM para la misma tabla de 6 columnas</span>
      </div>
    </div>
  </section>

  <section>
    <h2>A donde ir</h2>
    <div class="enlaces">
      <a class="enlace" href="/docs"><b>/docs</b><span>tu API, endpoint por endpoint</span></a>
      <a class="enlace" href="/openapi.json"><b>/openapi.json</b><span>el contrato, para generar clientes</span></a>
      <a class="enlace" href="https://github.com/KeevDev/Syrax"><b>github</b><span>el README completo del framework</span></a>
    </div>
  </section>

  <section>
    <h2>Mientras tanto</h2>
    <pre>syrax migrate      <span class="c"># crea las tablas</span>
syrax db:seed      <span class="c"># datos de ejemplo</span>
syrax test         <span class="c"># los tests que ya trae tests/</span>
syrax serve        <span class="c"># recompila solo al guardar</span></pre>
  </section>

  <footer>
    Esta pagina es <code>public/index.html</code>. No la sirve el framework: es un
    archivo estatico tuyo. Editala, o borrala.
  </footer>

</div>
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

#include <string>

namespace routes::v1 {

namespace user = controllers::UserController;

// El mapa de la API: una linea por endpoint, y a la derecha quien lo atiende.
// Se lee de un vistazo que expone esta version, sin abrir ningun controlador.
void register_(syrax::App& app) {
    const std::string base{kPrefix};

    app.get(base + "/users", user::index);
    app.post(base + "/users", user::store);

    app.get(base + "/users/{id}", user::show);
    app.put(base + "/users/{id}", user::update);
    app.del(base + "/users/{id}", user::destroy);
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

#include "http/requests/User/UserRequests.hpp"
#include "http/resources/User/UserResource.hpp"

#include <cstdint>
#include <vector>

namespace controllers::UserController {

// Un handler por accion, con la firma que declara lo que necesita: Syrax
// deduce de ahi el path param, el body a parsear y el esquema que documenta.
//
// Que ruta lleva a cada uno esta en routes/v1.cpp. Aqui esta el que hacer,
// no el donde: cambiar la URL o la version de la API no toca este archivo.
syrax::Task<syrax::Result<std::vector<resources::UserResource>>> index();

syrax::Task<syrax::Result<resources::UserResource>> show(std::int64_t id);

syrax::Task<syrax::Result<resources::UserResource>> store(requests::CreateUser body);

syrax::Task<syrax::Result<resources::UserResource>> update(std::int64_t id,
                                                           requests::UpdateUser body);

syrax::Task<syrax::Result<resources::DeletedResource>> destroy(std::int64_t id);

}  // namespace controllers::UserController
)T";

inline constexpr std::string_view kControllerUserCpp = R"T(#include "http/controllers/User/UserController.hpp"

#include "services/User/UserService.hpp"

using namespace syrax;

namespace controllers::UserController {

namespace service = services::UserService;

// El controlador traduce entre HTTP y el dominio, y nada mas: entra un id o
// un body ya validado, sale un resource o un Error. La regla de negocio esta
// en el service; el SQL, mas abajo.

Task<Result<std::vector<resources::UserResource>>> index() {
    co_return resources::from(co_await service::list());
}

Task<Result<resources::UserResource>> show(std::int64_t id) {
    const auto user = co_await service::byId(id);
    if (!user) co_return NotFound("user not found");

    co_return resources::from(*user);
}

Task<Result<resources::UserResource>> store(requests::CreateUser body) {
    const auto user = co_await service::create(std::move(body));
    if (!user) co_return Conflict("email already registered");

    co_return resources::from(*user);
}

Task<Result<resources::UserResource>> update(std::int64_t id, requests::UpdateUser body) {
    const auto user = co_await service::update(id, std::move(body));
    if (!user) co_return NotFound("user not found");

    co_return resources::from(*user);
}

Task<Result<resources::DeletedResource>> destroy(std::int64_t id) {
    if (!co_await service::remove(id)) co_return NotFound("user not found");

    co_return resources::DeletedResource{.id = id, .deleted = true};
}

}  // namespace controllers::UserController
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
    {"tests/CMakeLists.txt",                     kTestsCMake},
    {"tests/user_test.cpp",                      kTestUser},

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
