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

# Nadie usa modulos de C++20 aqui, y el escaneo que CMake activa por defecto
# ademas dispara un assert de ninja 1.13 al cambiar la estructura del build.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

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

inline constexpr std::string_view kTestUser = R"T(#include <catch2/catch_test_macros.hpp>

#include "factories/UserFactory.hpp"
#include "http/requests/User/UserRequests.hpp"
#include "http/resources/User/UserResource.hpp"

TEST_CASE("el resource expone solo lo que la API promete") {
    const auto user     = factories::UserFactory::make(7);
    const auto resource = resources::from(user);

    CHECK(resource.id == user.id);
    CHECK(resource.name == user.name);
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
    // Cuatro y no tres: un nombre vacio rompe notEmpty Y minLen(2).
    CHECK(syrax::validate(malo).size() == 4);
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
  --noche:  #071026;
  --abismo: #040a18;
  --oro:    #f0b429;
  --oro-claro: #ffd76a;
  --acero:  #7fa8d9;
  --hueso:  #e8eefc;
  --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
* { box-sizing: border-box; }
html, body { height: 100%; }
body {
  margin: 0; color: var(--hueso);
  background:
    radial-gradient(1200px 600px at 70% -10%, #16325c 0%, transparent 60%),
    radial-gradient(800px 500px at 15% 10%, #0d2148 0%, transparent 55%),
    linear-gradient(180deg, var(--noche) 0%, var(--abismo) 100%);
  background-attachment: fixed;
  font: 15px/1.6 ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  overflow-x: hidden;
}

/* ---------- estrellas ---------- */
.estrellas {
  position: fixed; inset: 0; pointer-events: none; opacity: .5;
  background-image:
    radial-gradient(1px 1px at 12% 22%, #fff, transparent),
    radial-gradient(1px 1px at 78% 14%, #cfe0ff, transparent),
    radial-gradient(1px 1px at 33% 61%, #fff, transparent),
    radial-gradient(1px 1px at 61% 42%, #bcd3ff, transparent),
    radial-gradient(1px 1px at 88% 68%, #fff, transparent),
    radial-gradient(1px 1px at 45% 8%, #fff, transparent),
    radial-gradient(1px 1px at 24% 84%, #cfe0ff, transparent);
}

.hoja { max-width: 900px; margin: 0 auto; padding: 40px 24px 72px; position: relative; }

/* ---------- el dragon ---------- */
.vuelo { display: block; margin: 0 auto; width: min(560px, 92%); height: auto; overflow: visible; }
.dragon { animation: planea 6s ease-in-out infinite; transform-box: view-box; transform-origin: 260px 180px; }
.ala { transform-box: view-box; transform-origin: 290px 168px; }
.ala-lejos { animation: aleteo-lejos 2.6s ease-in-out infinite; }
.ala-cerca { animation: aleteo 2.6s ease-in-out infinite; }
@keyframes aleteo {
  0%, 100% { transform: rotate(6deg) scaleX(.97); }
  50%      { transform: rotate(-22deg) scaleX(1.06); }
}
@keyframes aleteo-lejos {
  0%, 100% { transform: rotate(4deg) scaleX(.95); }
  50%      { transform: rotate(-16deg) scaleX(1.02); }
}
@keyframes planea {
  0%, 100% { transform: translateY(0) rotate(-1deg); }
  50%      { transform: translateY(-14px) rotate(1deg); }
}
.ojo { animation: brasa 3.4s ease-in-out infinite; }
.llama { animation: llamarada 2.9s ease-in-out infinite; transform-box: view-box; transform-origin: 438px 104px; }
@keyframes llamarada {
  0%, 62%, 100% { opacity: 0; transform: scaleX(.5); }
  72%           { opacity: .95; transform: scaleX(1.15); }
  86%           { opacity: .35; transform: scaleX(1.5); }
}
@keyframes brasa { 0%, 100% { opacity: .55; } 50% { opacity: 1; } }

/* ---------- cabecera ---------- */
.marca {
  text-align: center; margin: 4px 0 0;
  font: 400 76px/1 Georgia, "Times New Roman", serif;
  letter-spacing: .22em; text-indent: .22em; text-transform: uppercase;
  color: var(--oro);
  text-shadow: 0 0 28px rgba(240, 180, 41, .35);
}
.lema { text-align: center; color: var(--acero); margin: 14px 0 6px; }
.proyecto {
  text-align: center; font: 12px/1 var(--mono); color: var(--oro-claro);
  letter-spacing: .1em; opacity: .85;
}

/* ---------- tarjetones ---------- */
.tarjetones {
  display: grid; gap: 18px; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
  margin: 54px 0 34px;
}
.tarjeton {
  position: relative; display: block; padding: 30px 28px 26px;
  text-decoration: none; color: inherit; border-radius: 14px;
  background: linear-gradient(180deg, rgba(27, 58, 107, .55), rgba(9, 20, 43, .75));
  border: 1px solid rgba(127, 168, 217, .25);
  transition: transform .18s ease, border-color .18s ease, box-shadow .18s ease;
  overflow: hidden;
}
.tarjeton:hover, .tarjeton:focus-visible {
  transform: translateY(-3px); border-color: var(--oro);
  box-shadow: 0 10px 40px rgba(240, 180, 41, .18);
  outline: none;
}
.tarjeton .sello { font: 12px/1 var(--mono); color: var(--oro); letter-spacing: .14em; }
.tarjeton h2 {
  margin: 14px 0 8px; font: 400 27px/1.15 Georgia, "Times New Roman", serif; color: var(--hueso);
}
.tarjeton p { margin: 0; color: var(--acero); font-size: 14px; }
.tarjeton .flecha { margin-top: 18px; color: var(--oro); font: 13px/1 var(--mono); }

/* ---------- enlaces menores ---------- */
.menores { display: flex; gap: 10px; flex-wrap: wrap; justify-content: center; }
.menor {
  text-decoration: none; color: var(--acero); font: 12px/1 var(--mono);
  padding: 9px 14px; border: 1px solid rgba(127, 168, 217, .2); border-radius: 999px;
  transition: color .15s ease, border-color .15s ease;
}
.menor:hover { color: var(--oro); border-color: rgba(240, 180, 41, .5); }

footer { margin-top: 44px; text-align: center; color: #56719c; font-size: 12px; }
footer code { color: var(--acero); }

@media (prefers-reduced-motion: reduce) {
  .dragon, .ala-cerca, .ala-lejos, .ojo, .llama { animation: none; }
  .llama { opacity: .6; }
}
</style>
</head>
<body>
<div class="estrellas"></div>

<div class="hoja">

  <svg class="vuelo" viewBox="0 0 480 300" role="img" aria-label="Syrax, un dragon dorado en vuelo">
    <defs>
      <linearGradient id="escama" x1="0" y1="0" x2="0.3" y2="1">
        <stop offset="0%" stop-color="#ffd76a"/>
        <stop offset="50%" stop-color="#f0b429"/>
        <stop offset="100%" stop-color="#c2870f"/>
      </linearGradient>
      <linearGradient id="membrana" x1="0.1" y1="0" x2="0.7" y2="1">
        <stop offset="0%" stop-color="#e9b02a"/>
        <stop offset="100%" stop-color="#a46a08"/>
      </linearGradient>
      <radialGradient id="aliento">
        <stop offset="0%" stop-color="#fff3c4"/>
        <stop offset="45%" stop-color="#ff9d3d"/>
        <stop offset="100%" stop-color="rgba(255,120,40,0)"/>
      </radialGradient>
    </defs>

    <g class="dragon">

      <!-- ala del fondo -->
      <g class="ala ala-lejos" opacity=".5" transform="translate(26 -10)">
        <path d="M290 168
                 C 282 128, 268 96, 244 74
                 C 258 106, 262 122, 262 140
                 C 246 122, 226 110, 204 104
                 C 228 128, 242 148, 250 168
                 C 268 168, 280 169, 290 168 Z"
              fill="url(#membrana)"/>
      </g>

      <!-- cuerpo, cuello, cabeza y cola: una sola silueta -->
      <path d="M292 176
               C 316 150, 342 126, 372 110
               C 388 101, 406 94, 428 90
               C 438 90, 442 96, 437 103
               C 426 109, 413 114, 400 120
               C 405 126, 407 133, 405 141
               C 393 134, 380 131, 366 134
               C 346 142, 326 157, 310 176
               C 298 188, 284 196, 266 201
               C 244 208, 224 209, 206 203
               C 166 214, 118 228, 72 240
               L 38 248 L 22 236 L 32 251 L 16 264 L 46 258
               C 96 249, 150 235, 200 220
               C 226 215, 256 207, 276 195
               C 286 189, 291 183, 292 176 Z"
            fill="url(#escama)"/>

      <!-- puas del lomo y el cuello -->
      <path d="M318 162 l 13 -10 l -4 15 z
               M340 142 l 14 -9 l -5 15 z
               M364 124 l 14 -8 l -6 14 z
               M262 198 l 12 -11 l -2 15 z
               M212 210 l 11 -11 l -1 15 z
               M160 224 l 10 -11 l 0 14 z"
            fill="#f5cd5a"/>

      <!-- cuerno -->
      <path d="M392 104 C 374 88, 352 78, 328 76 C 352 86, 372 98, 386 114 Z" fill="#ffd76a"/>

      <!-- ojo -->
      <circle class="ojo" cx="414" cy="104" r="3.4" fill="#0b1633"/>

      <!-- patas recogidas -->
      <path d="M286 196 C 292 210, 286 224, 270 232
               C 286 228, 302 220, 308 206 C 311 198, 304 192, 296 192 Z"
            fill="#c98a12"/>
      <path d="M244 206 C 250 218, 246 230, 234 238
               C 248 234, 260 226, 264 214 C 266 208, 260 202, 252 202 Z"
            fill="#d89a1b"/>

      <!-- aliento -->
      <ellipse class="llama" cx="456" cy="104" rx="18" ry="10" fill="url(#aliento)"/>

      <!-- ala del frente -->
      <g class="ala ala-cerca">
        <path d="M290 168
                 C 276 122, 250 84, 212 62
                 C 176 42, 140 32, 104 30
                 C 138 62, 150 78, 160 96
                 C 136 96, 112 102, 90 114
                 C 126 128, 146 142, 162 158
                 C 142 168, 124 182, 108 200
                 C 158 198, 214 188, 268 172 Z"
              fill="url(#membrana)"/>
        <path d="M290 168 C 258 118, 220 78, 104 30
                 M290 168 C 250 140, 200 118, 90 114
                 M290 168 C 246 176, 190 188, 108 200"
              stroke="#8a5c08" stroke-width="2" fill="none" opacity=".55" stroke-linecap="round"/>
      </g>

    </g>
  </svg>

  <h1 class="marca">Syrax</h1>
  <p class="lema">Un framework de APIs para C++ moderno.</p>
  <p class="proyecto">@NAME@ esta en vuelo</p>

  <div class="tarjetones">
    <a class="tarjeton" href="/docs">
      <span class="sello">01</span>
      <h2>Doc de tu API</h2>
      <p>Los endpoints de @NAME@, con sus esquemas y un boton para probarlos. Sale de tus tipos, no de anotaciones.</p>
      <div class="flecha">/docs -&gt;</div>
    </a>

    <a class="tarjeton" href="/syrax.html">
      <span class="sello">02</span>
      <h2>Doc del framework</h2>
      <p>Como se escribe un endpoint, como se valida, como se habla con la base y que comandos hay. Empieza por aqui.</p>
      <div class="flecha">started -&gt;</div>
    </a>
  </div>

  <div class="menores">
    <a class="menor" href="/openapi.json">openapi.json</a>
    <a class="menor" href="/health">health</a>
    <a class="menor" href="https://github.com/KeevDev/Syrax">github</a>
  </div>

  <footer>
    Esta portada es <code>public/index.html</code>. Editala o borrala: es tuya.
  </footer>

</div>
</body>
</html>
)T";

inline constexpr std::string_view kPublicStarted = R"T(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Empezar con Syrax</title>
<style>
:root {
  --noche: #071026; --abismo: #040a18; --oro: #f0b429; --oro-claro: #ffd76a;
  --acero: #7fa8d9; --hueso: #e8eefc; --tinta: rgba(9,20,43,.72);
  --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
* { box-sizing: border-box; }
body {
  margin: 0; color: var(--hueso); min-height: 100%;
  background:
    radial-gradient(1000px 500px at 80% -10%, #16325c 0%, transparent 60%),
    linear-gradient(180deg, var(--noche) 0%, var(--abismo) 100%);
  background-attachment: fixed;
  font: 15px/1.7 ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
.hoja { max-width: 760px; margin: 0 auto; padding: 44px 24px 90px; }
.volver { display: inline-block; color: var(--acero); text-decoration: none; font: 12px/1 var(--mono); margin-bottom: 34px; }
.volver:hover { color: var(--oro); }
h1 { font: 400 42px/1.1 Georgia, "Times New Roman", serif; color: var(--oro); margin: 0 0 10px; letter-spacing: .01em; }
.entrada { color: var(--acero); margin: 0 0 46px; max-width: 58ch; }
section { margin-bottom: 42px; }
h2 {
  font: 400 24px/1.2 Georgia, "Times New Roman", serif; color: var(--hueso);
  margin: 0 0 6px; display: flex; align-items: baseline; gap: 12px;
}
h2 .n { font: 12px/1 var(--mono); color: var(--oro); letter-spacing: .12em; }
section p { color: var(--acero); margin: 0 0 14px; max-width: 62ch; }
pre {
  margin: 0; padding: 18px 20px; border-radius: 10px; overflow-x: auto;
  background: var(--tinta); border: 1px solid rgba(127,168,217,.2);
  font: 13px/1.75 var(--mono); color: #dbe6fb;
}
pre .c { color: #6f8ab5; }
pre .k { color: var(--oro-claro); }
code { font: 13px/1 var(--mono); color: var(--oro-claro); }
footer { border-top: 1px solid rgba(127,168,217,.18); padding-top: 18px; color: #56719c; font-size: 13px; }
footer a { color: var(--oro); }
</style>
</head>
<body>
<div class="hoja">

  <a class="volver" href="/">&lt;- volver</a>

  <h1>Empezar con Syrax</h1>
  <p class="entrada">
    Lo justo para moverte por el proyecto que acabas de generar. El README del
    repositorio tiene el resto, con los porques.
  </p>

  <section>
    <h2><span class="n">01</span> Un endpoint</h2>
    <p>La firma del handler es la fuente de todo: de ella salen el body a parsear, los path params, el codigo de estado y lo que se documenta.</p>
    <pre><span class="c">// src/routes/v1.cpp</span>
api.get("/users/{id}", user::show).as("users.show");

<span class="c">// src/http/controllers/User/UserController.cpp</span>
Task&lt;Result&lt;UserResource&gt;&gt; show(std::int64_t id) {
    const auto user = co_await service::byId(id);
    if (!user) co_return NotFound("user not found");

    co_return resources::from(*user);
}</pre>
  </section>

  <section>
    <h2><span class="n">02</span> Los errores son valores</h2>
    <p>No hay excepciones de control de flujo: devuelves el error y Syrax lo traduce a una respuesta JSON uniforme.</p>
    <pre>co_return NotFound("user not found");
co_return Conflict("email already registered");
co_return Forbidden("no puedes editar este post");</pre>
  </section>

  <section>
    <h2><span class="n">03</span> Validacion en el tipo</h2>
    <p>Las reglas viven en el struct del request. Un body que no cumple nunca llega al handler: sale un 422 con el detalle por campo, y los limites entran solos al /docs.</p>
    <pre>struct CreateUser {
    std::string name;
    std::string email;
    int         age;

    static auto rules() {
        return syrax::rules(
            syrax::field(&amp;CreateUser::name).notEmpty().minLen(2),
            syrax::field(&amp;CreateUser::email).email(),
            syrax::field(&amp;CreateUser::age).range(0, 130));
    }
};</pre>
  </section>

  <section>
    <h2><span class="n">04</span> La base de datos</h2>
    <p>Un modelo es un struct plano. Con <code>table</code> declarado tienes query builder; el SQL a mano sigue disponible para lo que el builder no cubre.</p>
    <pre>const auto adultos = co_await Query&lt;User&gt;()
    .where(&amp;User::age, "&gt;", 18)
    .orderBy(&amp;User::name)
    .limit(10)
    .get();

User nuevo{.name = "Ada", .email = "ada@x.com", .age = 36};
co_await save(nuevo);      <span class="c">// INSERT, y nuevo.id queda relleno</span></pre>
  </section>

  <section>
    <h2><span class="n">05</span> Migraciones</h2>
    <p>Son C++, asi que un error de esquema lo atrapa el compilador. Viven en <code>database/migrations/</code> y se declaran en <code>database/migrations.cpp</code>.</p>
    <pre>void up(syrax::Schema&amp; schema) override {
    schema.create("users", [](syrax::Table&amp; t) {
        t.id();
        t.string("name", 80);
        t.string("email", 160).unique();
        t.integer("age");
        t.timestamps();
    });
}</pre>
  </section>

  <section>
    <h2><span class="n">06</span> Comandos</h2>
    <pre>syrax serve        <span class="c"># levanta y recompila al guardar</span>
syrax migrate      <span class="c"># aplica lo pendiente</span>
syrax db:seed      <span class="c"># datos de ejemplo</span>
syrax test         <span class="c"># compila y corre tests/</span>
syrax make:model   <span class="c"># modelo de Drogon para Mapper&lt;T&gt;</span></pre>
  </section>

  <footer>
    El resto esta en el <a href="https://github.com/KeevDev/Syrax">README de Syrax</a>.
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

namespace routes::v1 {

void register_(syrax::Group api);

}  // namespace routes::v1
)T";

inline constexpr std::string_view kRoutesV1Cpp = R"T(#include "routes/v1.hpp"

#include "http/controllers/User/UserController.hpp"

namespace routes::v1 {

namespace user = controllers::UserController;

void register_(syrax::Group api) {
    api.get("/users", user::index).as("users.index");
    api.post("/users", user::store).as("users.store");

    api.get("/users/{id}", user::show).as("users.show");
    api.put("/users/{id}", user::update).as("users.update");
    api.del("/users/{id}", user::destroy).as("users.destroy");
}

}  // namespace routes::v1
)T";

inline constexpr std::string_view kRoutesCpp = R"T(#include "routes/routes.hpp"

#include "routes/v1.hpp"

// Con nombre, no anonimo: Glaze no refleja un tipo sin enlace.
namespace health {

struct Status {
    std::string status;
};

}  // namespace health

void registerRoutes(syrax::App& app) {
    app.get("/health", []() -> syrax::Result<health::Status> {
        return health::Status{.status = "ok"};
    });

    routes::v1::register_(app.group("/api/v1"));
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

syrax::Task<std::vector<models::User>> all() {
    co_return co_await syrax::Query<models::User>().orderBy(&models::User::id).get();
}

syrax::Task<std::optional<models::User>> find(std::int64_t id) {
    co_return co_await syrax::Query<models::User>().where(&models::User::id, "=", id).first();
}

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
    {"public/syrax.html",                        kPublicStarted},
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
