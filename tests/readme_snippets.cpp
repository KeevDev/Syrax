// Los fragmentos de codigo del README, tal cual estan escritos alli.
//
// No se ejecuta: se compila. Un ejemplo que no compila es peor que no tener
// ejemplo, y dos de los de aqui estaban mal (`app.use(securityHeaders())`
// cuando securityHeaders es un middleware de RESPUESTA, y `.origin` cuando
// el campo es `origins` y es un vector). Los encontro el compilador, no una
// relectura.
//
// Si cambias este archivo, cambia el README, y al reves.
#include <syrax/syrax.hpp>

#include <drogon/orm/Exception.h>

#include <format>

using namespace syrax;

struct Post        { std::int64_t id; std::string authorId; };
struct UpdatePost  { std::string title; };
struct PostResource{ std::int64_t id; std::string title; };

struct SendWelcome {
    std::int64_t userId = 0;
    std::string  email;

    static constexpr auto name  = "send-welcome";
    static constexpr int  tries = 3;

    Task<void> handle() const { co_return; }
};

// --- Errores como valores: tus propios errores ---

namespace errors {
inline syrax::Error saldoInsuficiente(double falta) {
    return syrax::Conflict("saldo insuficiente")
        .as("saldo_insuficiente")
        .explain(std::format("faltan {:.2f}", falta));
}
}  // namespace errors

inline void ganchosDeError() {
    syrax::onError([](const syrax::Error& e) {
        Json::Value body;
        body["ok"]      = false;
        body["code"]    = e.code.empty() ? std::to_string(e.status) : e.code;
        body["message"] = e.message;
        return syrax::jsonResponse(body, e.status);
    });

    syrax::onException([](const std::exception& e) -> std::optional<syrax::Error> {
        if (dynamic_cast<const drogon::orm::UniqueViolation*>(&e))
            return syrax::Conflict("el recurso ya existe").as("duplicado");
        return std::nullopt;
    });
}

inline void trazaDeEjemplo(const syrax::Request& request, std::int64_t id) {
    syrax::log::info("pedido confirmado", {{"pedido", std::to_string(id)},
                                           {"request_id", syrax::log::requestId(request)}});
}

namespace policies {
std::optional<Error> update(const Actor& actor, const Post& post) {
    if (actor.is("admin"))         return std::nullopt;
    if (post.authorId == actor.id) return std::nullopt;
    return Forbidden("no puedes editar este post");
}
}  // namespace policies

namespace controllers {

Task<Result<PostResource>> update(Request req, std::int64_t id, UpdatePost body) {
    const auto actor = actorFrom(req);

    if (auto denied = requireRole(actor, "admin", "editor")) co_return *denied;

    const Post post{.id = id, .authorId = actor.id};
    if (auto denied = policies::update(actor, post)) co_return *denied;

    co_return PostResource{.id = id, .title = body.title};
}

}  // namespace controllers

struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;

    static constexpr auto table = "users";
};

// Los ejemplos del query builder. Como todo lo de aqui: se compila, no corre.
Task<void> queryBuilder(std::string email) {
    const auto adultos = co_await Query<User>()
        .where(&User::age, ">", 18)
        .orderBy(&User::name)
        .limit(10)
        .get();

    const auto ada   = co_await Query<User>().where(&User::email, "=", email).first();
    const auto total = co_await Query<User>().count();
    const bool hay   = co_await Query<User>().whereNotNull(&User::email).exists();
    const auto fuera = co_await Query<User>().where(&User::age, "<", 18).del();

    User u{.name = "Ada", .email = "ada@x.com", .age = 36};
    co_await save(u);
    u.age = 37;
    co_await save(u);
    co_await remove(u);

    (void)adultos; (void)ada; (void)total; (void)hay; (void)fuera;
}

enum class Status : std::int64_t { Pending = 1, PendingPayment = 2, Paid = 3 };

struct Invoice {
    std::int64_t id;
    Status       status;
    int          total;
    bool         vip;

    static constexpr auto table = "invoices";
};

Task<void> gruposYUpdate() {
    const auto grandes = co_await Query<Invoice>()
        .where(&Invoice::status, "=", Status::Pending)
        .whereGroup([](auto& g) {
            g.where(&Invoice::total, ">", 100).orWhere(&Invoice::vip, "=", true);
        })
        .get();

    const auto cambiadas = co_await Query<Invoice>()
        .where(&Invoice::status, "=", Status::PendingPayment)
        .set(&Invoice::status, Status::Paid)
        .update();

    (void)grandes; (void)cambiadas;
}

// El builder dentro de una transaccion, como en el README.
Task<std::optional<User>> altaSiElEmailEstaLibre(std::string name, std::string email, int age) {
    co_return co_await db::transaction(
        [=](const db::Tx& tx) -> Task<std::optional<User>> {
            if (co_await Query<User>(tx.client()).where(&User::email, "=", email).exists())
                co_return std::nullopt;

            User nuevo{.id = 0, .name = name, .email = email, .age = age};
            co_await save(nuevo, tx.client());
            co_return nuevo;
        });
}

int main() {
    const std::string secreto = "s3cr3t0";
    App app;

    app.useOnResponse(securityHeaders());
    app.use(rateLimit(100, std::chrono::minutes{1}));
    app.use("/api/v1/admin", auth::bearer(secreto));
    app.cors({.origins = {"https://mi-front.com"}});
    app.use(requireApiKey("clave"));

    const auto token  = auth::sign({.sub = "42", .role = "admin"}, secreto);
    const auto claims = auth::verify(token, secreto);
    (void)claims;

    const auto hash = auth::hashPassword("secreto");
    const bool ok   = auth::verifyPassword("secreto", hash);
    (void)ok;

    app.post("/posts/{id}", controllers::update);

    app.base(syrax::env("API_BASE", "/api/v1"));

    jobs::connect({.driver = syrax::env("QUEUE_DRIVER", "database")});
    jobs::handle<SendWelcome>();

    db::connect({
        .engine      = syrax::env("DB_ENGINE", "postgres"),
        .host        = syrax::env("DB_HOST", "127.0.0.1"),
        .port        = static_cast<unsigned short>(syrax::envInt("DB_PORT", 0)),
        .database    = syrax::env("DB_NAME", "api"),
        .username    = syrax::env("DB_USER", "postgres"),
        .password    = syrax::env("DB_PASSWORD", "postgres"),
        .connections = static_cast<std::size_t>(syrax::envInt("DB_POOL", 4)),
    });

    if (syrax::envBool("CACHE_ENABLED", false)) {
        cache::connect({
            .host = syrax::env("REDIS_HOST", "127.0.0.1"),
            .port = static_cast<unsigned short>(syrax::envInt("REDIS_PORT", 6379)),
        });
    }

    auto api = app.group("/api/v1");
    api.get("/users/{id}", [](std::int64_t id) -> Task<Result<PostResource>> {
        co_return PostResource{.id = id, .title = "x"};
    }).as("users.show");
    (void)urlFor("users.show", 42);

    Room sala;
    app.ws("/chat", {
        .onOpen    = [&](const Socket& s) { sala.join(s); },
        .onMessage = [&](const Socket&, std::string_view text) { sala.broadcast(text); },
        .onClose   = [&](const Socket& s) { sala.leave(s); },
    });
    return 0;
}
