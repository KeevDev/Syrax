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

using namespace syrax;

struct Post        { std::int64_t id; std::string authorId; };
struct UpdatePost  { std::string title; };
struct PostResource{ std::int64_t id; std::string title; };

namespace policies {
std::optional<Error> update(const Actor& actor, const Post& post) {
    if (actor.is("admin"))         return std::nullopt;
    if (post.authorId == actor.id) return std::nullopt;
    return Forbidden("no puedes editar este post");
}
}  // namespace policies

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

    app.post("/posts/{id}", [](Request req, std::int64_t id, UpdatePost body)
                             -> Task<Result<PostResource>> {
        const auto actor = actorFrom(req);

        if (auto denied = requireRole(actor, "admin", "editor")) co_return *denied;

        const Post post{.id = id, .authorId = actor.id};
        if (auto denied = policies::update(actor, post)) co_return *denied;

        co_return PostResource{.id = id, .title = body.title};
    });

    Room sala;
    app.ws("/chat", {
        .onOpen    = [&](const Socket& s) { sala.join(s); },
        .onMessage = [&](const Socket&, std::string_view text) { sala.broadcast(text); },
        .onClose   = [&](const Socket& s) { sala.leave(s); },
    });
    return 0;
}
