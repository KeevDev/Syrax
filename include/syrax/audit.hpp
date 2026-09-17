#pragma once

// Quien hizo que, sobre que, y cuando.
//
// Es la pregunta que llega siempre y siempre tarde: "¿quien cambio el precio de
// este pedido?". Cuando llega, o esta escrito en algun sitio o no se sabe, y
// reconstruirlo desde los logs de acceso no funciona porque un log dice que
// hubo un PUT, no que cambio.
//
//   co_await syrax::audit::record(request, {
//       .action  = "pedido.precio_cambiado",
//       .subject = "pedidos:" + std::to_string(id),
//       .data    = R"({"antes":100,"despues":90})",
//   });
//
// De la trazabilidad del nivel 1 sale gratis la mitad: el actor lo pone el
// token y el request-id ya viaja en la peticion, asi que la entrada queda
// cosida a la traza sin que nadie los copie a mano. Una linea de auditoria con
// su request-id se puede cruzar con el log de acceso de esa misma peticion.
//
// **Dentro de la transaccion que hace el cambio**, que es la parte que
// importa:
//
//   co_await syrax::db::transaction([&](const syrax::db::Tx& tx) -> syrax::Task<void> {
//       co_await repo::bajarPrecio(id, 90, tx.client());
//       co_await syrax::audit::record(request, {...}, tx.client());
//   });
//
// Si el cambio se deshace, la linea de auditoria tambien: una auditoria que
// registra cosas que no pasaron es peor que no tenerla, porque se confia en
// ella.
//
// **Es append-only por contrato, no por magia.** Aqui no hay nada que
// actualice ni borre una entrada, y eso es todo lo que puede prometer un
// framework: que nadie con acceso a la base la toque se consigue con permisos
// o con un trigger, y eso es del despliegue.

#include <syrax/db.hpp>
#include <syrax/log.hpp>
#include <syrax/middleware.hpp>
#include <syrax/policy.hpp>

#include <drogon/drogon.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace syrax::audit {

inline constexpr std::string_view kTable = "syrax_audit";

// Una linea del registro. Los cuatro primeros campos son la pregunta que se
// viene a contestar; `data` es para el detalle que solo conoce el proyecto.
struct Entry {
    std::string action;   // "pedido.precio_cambiado"
    std::string subject;  // "pedidos:42"
    std::string actor;    // quien, del token; vacio si fue anonimo
    std::string requestId;
    std::string data;     // JSON libre, o vacio
};

// Lo que se lee de vuelta. Lleva el id y la fecha que puso la base.
struct Record {
    std::int64_t id = 0;
    std::string  action;
    std::string  subject;
    std::string  actor;
    std::string  request_id;
    std::string  data;
    std::int64_t at = 0;

    static constexpr auto table = "syrax_audit";
};

namespace detail {

inline std::string ddl(Dialect dialect) {
    const std::string id = dialect == Dialect::Postgres ? "BIGSERIAL PRIMARY KEY"
                           : dialect == Dialect::Mysql  ? "BIGINT AUTO_INCREMENT PRIMARY KEY"
                                                        : "INTEGER PRIMARY KEY AUTOINCREMENT";
    // MySQL no indexa un TEXT sin longitud, y los tres campos por los que se
    // busca -actor, subject, request_id- van indexados justo por eso.
    const std::string text = dialect == Dialect::Mysql ? "VARCHAR(191)" : "TEXT";

    return "CREATE TABLE IF NOT EXISTS " + std::string{kTable} +
           " ("
           "id " + id + ", "
           "action " + text + " NOT NULL, "
           "subject " + text + " NOT NULL, "
           "actor " + text + " NOT NULL, "
           "request_id " + text + " NOT NULL, "
           "data TEXT NOT NULL, "
           "at BIGINT NOT NULL)";
}

inline std::string exists(Dialect dialect) {
    switch (dialect) {
        case Dialect::Postgres:
            return "SELECT 1 FROM information_schema.tables "
                   "WHERE table_schema = current_schema() AND table_name = '" +
                   std::string{kTable} + "'";
        case Dialect::Mysql:
            return "SELECT 1 FROM information_schema.tables "
                   "WHERE table_schema = DATABASE() AND table_name = '" +
                   std::string{kTable} + "'";
        case Dialect::Sqlite:
            return "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = '" +
                   std::string{kTable} + "'";
    }
    return "";
}

inline std::vector<std::string> indexes() {
    const std::string table{kTable};
    return {
        "CREATE INDEX IF NOT EXISTS idx_" + table + "_subject ON " + table + " (subject)",
        "CREATE INDEX IF NOT EXISTS idx_" + table + "_actor ON " + table + " (actor)",
        "CREATE INDEX IF NOT EXISTS idx_" + table + "_request ON " + table + " (request_id)",
    };
}

inline std::string arg(std::size_t n) {
    return db::dialect() == Dialect::Postgres ? "$" + std::to_string(n) : "?";
}

inline std::string args(std::size_t n) {
    std::string out;
    for (std::size_t i = 1; i <= n; ++i) {
        if (i > 1) out += ", ";
        out += arg(i);
    }
    return out;
}

inline std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace detail

// Crea la tabla si no esta. Se llama una vez, en bootstrap.
//
// Se pregunta antes de crear en vez de un CREATE IF NOT EXISTS a ciegas:
// Postgres avisa por consola cada vez que uno de esos no hace nada, y eso
// seria en cada arranque.
inline drogon::Task<void> install(drogon::orm::DbClientPtr on = nullptr) {
    auto db = on ? on : db::client();

    const auto ya = co_await db->execSqlCoro(detail::exists(db::dialect()));
    if (!ya.empty()) co_return;

    co_await db->execSqlCoro(detail::ddl(db::dialect()));
    for (const auto& index : detail::indexes()) co_await db->execSqlCoro(index);

    co_return;
}

// Escribe una linea. `on` acepta el cliente de una transaccion en curso, que
// es como se consigue que la auditoria y el cambio auditado vivan o mueran
// juntos.
inline drogon::Task<void> record(Entry entry, drogon::orm::DbClientPtr on = nullptr) {
    auto db = on ? on : db::client();

    co_await db->execSqlCoro("INSERT INTO " + std::string{kTable} +
                                 " (action, subject, actor, request_id, data, at) VALUES (" +
                                 detail::args(6) + ")",
                             entry.action, entry.subject, entry.actor, entry.requestId,
                             entry.data, detail::now());
    co_return;
}

// La forma normal: el actor y el request-id salen de la peticion, y quien
// escribe el handler solo dice que paso.
//
// Un actor vacio no es un error: hay acciones que ocurren sin nadie detras -un
// job, una tarea programada- y registrarlas igual es el punto.
inline drogon::Task<void> record(const Request& request, Entry entry,
                                 drogon::orm::DbClientPtr on = nullptr) {
    if (entry.actor.empty())     entry.actor     = actorFrom(request).id;
    if (entry.requestId.empty()) entry.requestId = log::requestId(request);

    co_await record(std::move(entry), std::move(on));
    co_return;
}

// Lo que paso sobre un recurso, de lo mas reciente a lo mas antiguo. Es la
// consulta que se hace el 90% de las veces: "cuentame la vida de este pedido".
inline drogon::Task<std::vector<Record>> of(std::string subject, std::int64_t limit = 50,
                                            drogon::orm::DbClientPtr on = nullptr) {
    auto db = on ? on : db::client();

    const auto result = co_await db->execSqlCoro(
        "SELECT id, action, subject, actor, request_id, data, at FROM " + std::string{kTable} +
            " WHERE subject = " + detail::arg(1) + " ORDER BY id DESC LIMIT " +
            std::to_string(limit),
        std::move(subject));

    std::vector<Record> rows;
    rows.reserve(result.size());
    for (const auto& row : result) rows.push_back(db::fromRow<Record>(row));

    co_return rows;
}

// Lo que hizo un actor. La otra mitad de la pregunta.
inline drogon::Task<std::vector<Record>> by(std::string actor, std::int64_t limit = 50,
                                            drogon::orm::DbClientPtr on = nullptr) {
    auto db = on ? on : db::client();

    const auto result = co_await db->execSqlCoro(
        "SELECT id, action, subject, actor, request_id, data, at FROM " + std::string{kTable} +
            " WHERE actor = " + detail::arg(1) + " ORDER BY id DESC LIMIT " +
            std::to_string(limit),
        std::move(actor));

    std::vector<Record> rows;
    rows.reserve(result.size());
    for (const auto& row : result) rows.push_back(db::fromRow<Record>(row));

    co_return rows;
}

}  // namespace syrax::audit
