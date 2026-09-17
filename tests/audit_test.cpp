// La auditoria. Lo que hay que probar es que la linea queda escrita con quien,
// que y cuando, y sobre todo que vive o muere con la transaccion que la
// acompaña: una auditoria que registra cosas que no pasaron es peor que no
// tenerla, porque se confia en ella.

#include <catch2/catch_test_macros.hpp>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <syrax/audit.hpp>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace audittest {

// Los clientes que se quedan vivos hasta que muere el proceso. Ver la nota del
// destructor de cada fixture.
inline std::vector<drogon::orm::DbClientPtr>& vivos() {
    static std::vector<drogon::orm::DbClientPtr> clientes;
    return clientes;
}

class TempDb {
public:
    TempDb() : path_{fs::temp_directory_path() / name()} {
        std::error_code ec;
        fs::remove(path_, ec);

        client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);
        client_->execSqlSync("CREATE TABLE pedidos (id INTEGER PRIMARY KEY, precio INTEGER)");

        drogon::sync_wait(syrax::audit::install(client_));
    }
    ~TempDb() {
        // El cliente NO se destruye, a proposito. Cerrar uno de Drogon mientras
        // todavia hay callbacks en vuelo termina ejecutando una consulta sobre
        // una conexion ya cerrada: sale un "Connection is not ready", la
        // BrokenConnection viaja por una corrutina que ya nadie espera y el
        // proceso aborta DESPUES de que el test haya pasado. En CI eso es
        // indistinguible de un fallo real.
        //
        // Es el mismo motivo por el que jobs_test.cpp deja vivo su cliente de
        // Redis. Un binario de tests dura lo que dura, y el archivo se puede
        // borrar igual: en POSIX, unlink sobre un archivo abierto funciona.
        vivos().push_back(client_);
        client_.reset();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    const drogon::orm::DbClientPtr& get() const { return client_; }

private:
    static std::string name() {
        static int counter = 0;
        return "syrax_audit_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".db";
    }

    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

// El dialecto sale del entorno, asi que los tests lo fijan como hace query_test.
struct SqliteDialect {
    SqliteDialect() { syrax::db::activeDialect() = syrax::Dialect::Sqlite; }
};

}  // namespace audittest

using namespace audittest;

TEST_CASE("install crea la tabla y es idempotente", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto tablas = db.get()->execSqlSync(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'syrax_audit'");
    CHECK(tablas.size() == 1);

    // Se llama en cada arranque: volver a llamarla no puede fallar.
    CHECK_NOTHROW(drogon::sync_wait(syrax::audit::install(db.get())));
}

TEST_CASE("una entrada guarda quien, que y sobre que", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    drogon::sync_wait(syrax::audit::record(
        {.action    = "pedido.precio_cambiado",
         .subject   = "pedidos:42",
         .actor     = "7",
         .requestId = "abc-123",
         .data      = R"({"antes":100,"despues":90})"},
        db.get()));

    const auto filas = drogon::sync_wait(syrax::audit::of("pedidos:42", 50, db.get()));

    REQUIRE(filas.size() == 1);
    CHECK(filas.front().action == "pedido.precio_cambiado");
    CHECK(filas.front().actor == "7");
    CHECK(filas.front().request_id == "abc-123");
    CHECK(filas.front().data == R"({"antes":100,"despues":90})");
    CHECK(filas.front().at > 0);
}

TEST_CASE("of() devuelve lo mas reciente primero", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    for (const auto* accion : {"creado", "pagado", "enviado"}) {
        drogon::sync_wait(
            syrax::audit::record({.action = accion, .subject = "pedidos:1"}, db.get()));
    }

    const auto vida = drogon::sync_wait(syrax::audit::of("pedidos:1", 50, db.get()));

    REQUIRE(vida.size() == 3);
    CHECK(vida[0].action == "enviado");
    CHECK(vida[2].action == "creado");
}

TEST_CASE("of() no mezcla recursos", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    drogon::sync_wait(syrax::audit::record({.action = "a", .subject = "pedidos:1"}, db.get()));
    drogon::sync_wait(syrax::audit::record({.action = "b", .subject = "pedidos:2"}, db.get()));

    CHECK(drogon::sync_wait(syrax::audit::of("pedidos:1", 50, db.get())).size() == 1);
}

TEST_CASE("by() contesta la otra mitad de la pregunta", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    drogon::sync_wait(
        syrax::audit::record({.action = "a", .subject = "pedidos:1", .actor = "ada"}, db.get()));
    drogon::sync_wait(
        syrax::audit::record({.action = "b", .subject = "pedidos:2", .actor = "ada"}, db.get()));
    drogon::sync_wait(
        syrax::audit::record({.action = "c", .subject = "pedidos:3", .actor = "alan"}, db.get()));

    CHECK(drogon::sync_wait(syrax::audit::by("ada", 50, db.get())).size() == 2);
    CHECK(drogon::sync_wait(syrax::audit::by("alan", 50, db.get())).size() == 1);
}

TEST_CASE("la entrada se deshace con la transaccion que la acompaña", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<void> {
            co_await tx.client()->execSqlCoro("INSERT INTO pedidos (id, precio) VALUES (1, 100)");

            co_await syrax::audit::record(
                {.action = "pedido.creado", .subject = "pedidos:1"}, tx.client());

            // El cambio no vale, asi que la linea que dice que paso tampoco.
            tx.rollback();
            co_return;
        }));

    CHECK(db.get()->execSqlSync("SELECT id FROM pedidos").empty());
    CHECK(drogon::sync_wait(syrax::audit::of("pedidos:1", 50, db.get())).empty());
}

TEST_CASE("una transaccion que confirma deja las dos cosas", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<void> {
            co_await tx.client()->execSqlCoro("INSERT INTO pedidos (id, precio) VALUES (1, 100)");
            co_await syrax::audit::record(
                {.action = "pedido.creado", .subject = "pedidos:1"}, tx.client());
            co_return;
        }));

    CHECK(db.get()->execSqlSync("SELECT id FROM pedidos").size() == 1);
    CHECK(drogon::sync_wait(syrax::audit::of("pedidos:1", 50, db.get())).size() == 1);
}

TEST_CASE("un actor vacio no es un error", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    // Hay acciones sin nadie detras -un job, una tarea programada- y
    // registrarlas igual es justo el punto.
    drogon::sync_wait(
        syrax::audit::record({.action = "purga.corrida", .subject = "sistema"}, db.get()));

    const auto filas = drogon::sync_wait(syrax::audit::of("sistema", 50, db.get()));
    REQUIRE(filas.size() == 1);
    CHECK(filas.front().actor.empty());
}

TEST_CASE("limit acota lo que vuelve", "[audit]") {
    SqliteDialect dialect;
    TempDb        db;

    for (int i = 0; i < 10; ++i) {
        drogon::sync_wait(syrax::audit::record({.action = "toque", .subject = "pedidos:1"}, db.get()));
    }

    CHECK(drogon::sync_wait(syrax::audit::of("pedidos:1", 3, db.get())).size() == 3);
}
