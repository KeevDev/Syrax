#pragma once

// La aplicacion con la que se prueban el kit de tests y /health: una completa
// de verdad —migracion, modelo, repositorio, rutas— por la que pasa una
// peticion de principio a fin.
//
// Vive en un header porque varios archivos de test la comparten, y con una
// funcion en vez de una variable porque Drogon solo admite un servidor por
// proceso: todos tienen que ver LA MISMA.

#include "testkeys.hpp"

#include <syrax/syrax.hpp>
#include <syrax/testing.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ------------------------------------------------------ la aplicacion de prueba

namespace models {

struct Widget {
    std::int64_t id;
    std::string  name;
    int          size;

    static constexpr auto table = "widgets";
};

}  // namespace models

namespace requests {

struct CreateWidget {
    std::string name;
    int         size;

    static auto rules() {
        return syrax::rules(syrax::field(&CreateWidget::name).notEmpty().minLen(2),
                            syrax::field(&CreateWidget::size).range(1, 100));
    }
};

}  // namespace requests

struct CreateWidgetsTable : syrax::Migration {
    std::string name() const override { return "001_create_widgets"; }

    void up(syrax::Schema& schema) override {
        schema.create("widgets", [](syrax::Blueprint& table) {
            table.id();
            table.string("name").unique();
            table.integer("size").defaultTo("0");
        });
    }

    void down(syrax::Schema& schema) override { schema.drop("widgets"); }
};

inline void registerMigrations(syrax::Migrator& migrator) {
    migrator.add<CreateWidgetsTable>();
}

// El repositorio va contra db::client() sin decir cual: es lo que hace que el
// test compruebe que el kit redirigio la conexion de verdad.
namespace repository {

inline syrax::Task<std::vector<models::Widget>> all() {
    co_return co_await syrax::Query<models::Widget>().orderBy(&models::Widget::id).get();
}

inline syrax::Task<std::optional<models::Widget>> find(std::int64_t id) {
    co_return co_await syrax::Query<models::Widget>().where(&models::Widget::id, "=", id).first();
}

inline syrax::Task<std::optional<models::Widget>> create(std::string name, int size) {
    const bool tomado =
        co_await syrax::Query<models::Widget>().where(&models::Widget::name, "=", name).exists();
    if (tomado) co_return std::nullopt;

    models::Widget nuevo{.id = 0, .name = std::move(name), .size = size};
    co_await syrax::save(nuevo);
    co_return nuevo;
}

}  // namespace repository

// Cuantas veces se ha llamado a las rutas de prueba del cliente HTTP. Lo unico
// que distingue un reintento de una llamada suelta es este numero.
inline std::atomic<int>& hits() {
    static std::atomic<int> n{0};
    return n;
}

namespace bootstrap {

// Crear un cliente de Drogon contra un puerto muerto y soltarlo termina
// uniendo el hilo del loop consigo mismo y abortando el proceso, asi que se
// pregunta con un socket pelado antes de conectar.
inline bool redisReachable() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = ::htons(static_cast<std::uint16_t>(syrax::envInt("REDIS_TEST_PORT", 6379)));
    ::inet_pton(AF_INET, syrax::env("REDIS_TEST_HOST", "127.0.0.1").c_str(), &addr.sin_addr);

    const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

inline syrax::App create() {
    syrax::db::connect({
        .engine = syrax::env("DB_ENGINE", "postgres"),
        .file   = syrax::env("DB_FILE", "app.db"),
    });

    syrax::App app;
    app.base("/api/v1");
    app.etag();

    // Sin Redis, claim() devuelve Unavailable y la peticion sigue como si la
    // cabecera no estuviera: se puede montar siempre.
    app.idempotency();
    app.partial();
    app.metrics();

    auto api = app.api();

    api.get("/widgets", []() -> syrax::Task<syrax::Result<std::vector<models::Widget>>> {
        co_return co_await repository::all();
    }).as("widgets.index");

    api.get("/paginados",
            [](const syrax::Request& request) -> syrax::Task<syrax::Result<syrax::Page<models::Widget>>> {
                const auto page = request.query("page");
                const auto per  = request.query("per_page");

                co_return co_await syrax::Query<models::Widget>()
                    .orderBy(&models::Widget::id)
                    .paginate(page.empty() ? 1 : std::stoll(page),
                              per.empty() ? 2 : std::stoll(per));
            })
        .as("widgets.paginados");

    api.get("/widgets/{id}",
            [](std::int64_t id) -> syrax::Task<syrax::Result<models::Widget>> {
                const auto found = co_await repository::find(id);
                if (!found) co_return syrax::NotFound("widget no encontrado");
                co_return *found;
            })
        .as("widgets.show");

    api.post("/widgets",
             [](requests::CreateWidget body) -> syrax::Task<syrax::Result<models::Widget>> {
                 const auto creado = co_await repository::create(body.name, body.size);
                 if (!creado) {
                     co_return syrax::Conflict("ese nombre ya existe").as("nombre_duplicado");
                 }
                 co_return *creado;
             })
        .as("widgets.store");

    // Fuera de la base de la API, asi que tambien comprueba que resolve() no le
    // pega el prefijo.
    app.health();

    // --- rutas para el cliente HTTP -------------------------------------

    struct Eco {
        int         hits = 0;
        std::string requestId;
        std::string authorization;
    };

    // Siempre 500: sirve para contar cuantas veces se reintenta.
    app.get("/falla", []() -> syrax::Result<Eco> {
        ++hits();
        return syrax::Error{.status = 500, .message = "siempre falla"};
    });

    // Sin cuerpo tipado a proposito: con uno, un cuerpo que no encaje daria
    // 422 antes de llegar al handler y el contador no veria la llamada.
    app.post("/falla", []() -> syrax::Result<Eco> {
        ++hits();
        return syrax::Error{.status = 500, .message = "siempre falla"};
    });

    // 404: un 4xx no merece otro intento, porque repetirlo da lo mismo.
    app.get("/no-esta", []() -> syrax::Result<Eco> {
        ++hits();
        return syrax::Error{.status = 404, .message = "no esta"};
    });

    // Devuelve lo que recibio, para ver que cabeceras cruzaron.
    app.get("/eco", [](const syrax::Request& request) -> syrax::Result<Eco> {
        ++hits();
        return Eco{
            .hits          = hits(),
            .requestId     = request.header("X-Request-Id"),
            .authorization = request.header("Authorization"),
        };
    });

    // --- subida de archivos ---------------------------------------------

    struct Subido {
        std::string              nombre;
        std::string              extension;
        std::size_t              peso = 0;
        std::vector<std::string> fallos;
    };

    app.post("/subir", [](const syrax::Request& request) -> syrax::Result<Subido> {
        const syrax::Uploads archivos{request};

        const auto fallos = archivos.check({
            syrax::upload("avatar").required().maxSize(1024).image(),
        });

        Subido out;
        for (const auto& fallo : fallos) out.fallos.push_back(fallo.field + ": " + fallo.message);

        if (const auto avatar = archivos.file("avatar")) {
            out.nombre    = avatar->filename;
            out.extension = avatar->extension;
            out.peso      = avatar->size;
        }
        return out;
    });

    // --- JWKS ------------------------------------------------------------
    //
    // La app hace de proveedor de identidad para el test: publica su JWKS y
    // tiene una ruta protegida detras del middleware.

    struct Jwk {
        std::string kty = "RSA";
        std::string alg = "RS256";
        std::string use = "sig";
        std::string kid;
        std::string n;
        std::string e;
    };
    struct JwkSet {
        std::vector<Jwk> keys;
    };

    app.get("/jwks.json", []() -> syrax::Result<JwkSet> {
        ++hits();
        return JwkSet{.keys = {Jwk{.kid = testkeys::kid(),
                                   .n   = testkeys::modulusB64(),
                                   .e   = testkeys::exponentB64()}}};
    });

    // La URL va como funcion y no como cadena: el puerto no existe todavia
    // -lo asigna el kernel al arrancar- y una cadena lo congelaria en 0.
    app.useAsync("/protegido",
                 syrax::auth::jwks(
                     [] {
                         return "http://127.0.0.1:" + std::to_string(testkeys::puerto()) +
                                "/jwks.json";
                     },
                     {.issuer     = "https://emisor.de.prueba/",
                      .audience   = "api-de-prueba",
                      .minRefresh = std::chrono::seconds{0}}));

    app.get("/protegido", [](const syrax::Request& request) -> syrax::Result<Eco> {
        const auto actor = syrax::actorFrom(request);
        return Eco{.hits = 0, .requestId = actor.id, .authorization = actor.scope};
    });

    app.get("/cuenta", []() -> syrax::Result<Eco> { return Eco{.hits = hits()}; });
    app.get("/reinicia", []() -> syrax::Result<Eco> {
        hits() = 0;
        return Eco{.hits = 0};
    });

    // El limitador compartido, solo si hay Redis. Va con prefijo para que no
    // le corte las peticiones a los demas tests del binario, y cuenta por una
    // cabecera en vez de por IP: todos los casos salen del mismo 127.0.0.1, y
    // con la IP como clave el primero en correr se llevaria la cuota entera.
    if (redisReachable()) {
        syrax::cache::connect({
            .host    = syrax::env("REDIS_TEST_HOST", "127.0.0.1"),
            .port    = static_cast<unsigned short>(syrax::envInt("REDIS_TEST_PORT", 6379)),
            .timeout = 3.0,
        });

        app.useAsync("/limitado",
                     syrax::rateLimitShared(3, std::chrono::seconds{60},
                                            [](const syrax::Request& request) {
                                                return request.header("X-Cliente");
                                            }));

        app.get("/limitado", []() -> syrax::Result<models::Widget> {
            return models::Widget{.id = 0, .name = "pasa", .size = 0};
        });
    }

    return app;
}

}  // namespace bootstrap

// ------------------------------------------------------------------ el kit

// Estatico de funcion, no variable global: se construye en el primer uso, ya
// dentro de main. Una instancia en el ambito del namespace se construiria
// ANTES, y ahi las tablas estaticas de Drogon todavia no existen.
inline syrax::testing::App& api() {
    static syrax::testing::App app{{
        .create     = bootstrap::create,
        .migrations = registerMigrations,
    }};
    return app;
}
