#include <catch2/catch_test_macros.hpp>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <syrax/query.hpp>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Glaze no refleja tipos en namespace anonimo: por eso lleva nombre.
namespace qtest {

struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;

    static constexpr auto table = "users";
};

// Clave primaria con otro nombre, para comprobar que no esta cableado a "id".
struct Doc {
    std::int64_t doc_id;
    std::string  title;

    static constexpr auto table      = "docs";
    static constexpr auto primaryKey = "doc_id";
};

// Un estado como enum: la columna guarda el entero, el struct los nombres.
enum class Status : std::int64_t { Draft = 1, Sent = 2, Paid = 3 };

struct Invoice {
    std::int64_t id;
    Status       status;
    int          total;

    static constexpr auto table = "invoices";
};

// Los clientes que se quedan vivos hasta que muere el proceso. Ver la nota del
// destructor de cada fixture.
inline std::vector<drogon::orm::DbClientPtr>& vivos() {
    static std::vector<drogon::orm::DbClientPtr> clientes;
    return clientes;
}

class TempDb {
public:
    TempDb() : path_{fs::temp_directory_path() / name()} {
        // Un proceso anterior con este mismo pid pudo morir sin limpiar. Abrir
        // encima de su archivo dejaria las filas de los dos, y el test falla
        // una vez cada muchas sin explicacion.
        std::error_code ec;
        fs::remove(path_, ec);

        client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);

        client_->execSqlSync(
            "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, "
            "email TEXT, age INTEGER)");
        client_->execSqlSync(
            "INSERT INTO users (name, email, age) VALUES "
            "('ada', 'ada@x.com', 36), ('alan', 'alan@x.com', 41), "
            "('grace', 'grace@x.com', 45), ('linus', NULL, 17)");
        client_->execSqlSync(
            "CREATE TABLE docs (doc_id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT)");
        client_->execSqlSync(
            "CREATE TABLE invoices (id INTEGER PRIMARY KEY AUTOINCREMENT, status INTEGER, "
            "total INTEGER)");
        client_->execSqlSync(
            "INSERT INTO invoices (status, total) VALUES (1, 100), (2, 200), (3, 300)");
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
        static int n = 0;
        return "syrax_q_" + std::to_string(::getpid()) + "_" + std::to_string(n++) + ".db";
    }
    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

// El dialecto es estado global que fija configureFromEnv(); en los tests se
// pone a mano y se restaura.
struct SqliteDialect {
    syrax::db::Dialect previous = syrax::db::dialect();
    SqliteDialect()  { syrax::db::activeDialect() = syrax::db::Dialect::Sqlite; }
    ~SqliteDialect() { syrax::db::activeDialect() = previous; }
};

}  // namespace qtest

using namespace qtest;
using syrax::Dir;
using syrax::Query;

TEST_CASE("el nombre de la columna sale del puntero a miembro", "[query]") {
    CHECK(syrax::detail::columnOf(&User::email) == "email");
    CHECK(syrax::detail::columnOf(&User::age) == "age");
    CHECK(syrax::detail::columnOf(&Doc::doc_id) == "doc_id");
}

TEST_CASE("where genera el SQL esperado", "[query]") {
    SqliteDialect dialect;

    const auto sql = Query<User>().where(&User::age, ">", 18).orderBy(&User::name).toSql();

    CHECK(sql == R"(SELECT "id", "name", "email", "age" FROM "users" WHERE "age" > ? )"
                 R"(ORDER BY "name" ASC)");
}

TEST_CASE("los filtros se encadenan con AND y OR", "[query]") {
    SqliteDialect dialect;

    const auto sql = Query<User>()
                         .where(&User::age, ">", 18)
                         .orWhere(&User::name, "LIKE", std::string{"a%"})
                         .whereNull(&User::email)
                         .toSql();

    CHECK(sql.find(R"("age" > ? OR "name" LIKE ? AND "email" IS NULL)") != std::string::npos);
}

TEST_CASE("get devuelve los structs mapeados", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto adultos = drogon::sync_wait(
        Query<User>(db.get()).where(&User::age, ">", 18).orderBy(&User::age).get());

    REQUIRE(adultos.size() == 3);
    CHECK(adultos[0].name == "ada");
    CHECK(adultos[2].name == "grace");
}

TEST_CASE("first devuelve una sola fila o nada", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto mayor = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::age, Dir::Desc).first());

    REQUIRE(mayor.has_value());
    CHECK(mayor->name == "grace");

    const auto ninguno = drogon::sync_wait(
        Query<User>(db.get()).where(&User::age, ">", 200).first());
    CHECK_FALSE(ninguno.has_value());
}

TEST_CASE("count y exists no traen las filas", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 4);
    CHECK(drogon::sync_wait(Query<User>(db.get()).where(&User::age, "<", 18).count()) == 1);
    CHECK(drogon::sync_wait(Query<User>(db.get()).where(&User::age, ">", 100).exists()) == false);
}

TEST_CASE("whereIn acepta una lista", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto algunos = drogon::sync_wait(
        Query<User>(db.get()).whereIn(&User::name, {std::string{"ada"}, std::string{"grace"}})
            .orderBy(&User::name).get());

    REQUIRE(algunos.size() == 2);
    CHECK(algunos[0].name == "ada");
    CHECK(algunos[1].name == "grace");
}

TEST_CASE("un whereIn vacio no devuelve la tabla entera", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    // El riesgo real: generar "IN ()" o ignorar el filtro y devolverlo todo.
    const auto ninguno = drogon::sync_wait(
        Query<User>(db.get()).whereIn(&User::name, std::vector<std::string>{}).get());

    CHECK(ninguno.empty());
}

TEST_CASE("whereNull distingue NULL de vacio", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto sinEmail = drogon::sync_wait(Query<User>(db.get()).whereNull(&User::email).get());
    REQUIRE(sinEmail.size() == 1);
    CHECK(sinEmail.front().name == "linus");

    CHECK(drogon::sync_wait(Query<User>(db.get()).whereNotNull(&User::email).count()) == 3);
}

TEST_CASE("limit y offset paginan", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto pagina = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::age).limit(2).offset(1).get());

    REQUIRE(pagina.size() == 2);
    CHECK(pagina[0].name == "ada");
    CHECK(pagina[1].name == "alan");
}

TEST_CASE("del borra solo lo filtrado", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto borradas =
        drogon::sync_wait(Query<User>(db.get()).where(&User::age, "<", 18).del());

    CHECK(borradas == 1);
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 3);
}

TEST_CASE("save inserta y rellena la clave primaria", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    User nuevo{.id = 0, .name = "hedy", .email = "hedy@x.com", .age = 30};
    drogon::sync_wait(syrax::save(nuevo, db.get()));

    // El id lo asigna la base y vuelve al objeto: eso es lo que lo hace
    // utilizable como objeto y no como un DTO de ida.
    CHECK(nuevo.id > 0);
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 5);
}

TEST_CASE("save actualiza si la clave primaria ya viene puesta", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    auto ada = drogon::sync_wait(
        Query<User>(db.get()).where(&User::name, "=", std::string{"ada"}).first());
    REQUIRE(ada.has_value());

    ada->age = 99;
    drogon::sync_wait(syrax::save(*ada, db.get()));

    const auto releida = drogon::sync_wait(
        Query<User>(db.get()).where(&User::name, "=", std::string{"ada"}).first());

    REQUIRE(releida.has_value());
    CHECK(releida->age == 99);
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 4);  // no duplico
}

TEST_CASE("remove borra por clave primaria", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    auto ada = drogon::sync_wait(
        Query<User>(db.get()).where(&User::name, "=", std::string{"ada"}).first());
    REQUIRE(ada.has_value());

    CHECK(drogon::sync_wait(syrax::remove(*ada, db.get())));
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 3);
}

TEST_CASE("la clave primaria no esta cableada a 'id'", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    Doc doc{.doc_id = 0, .title = "acta"};
    drogon::sync_wait(syrax::save(doc, db.get()));
    CHECK(doc.doc_id > 0);

    doc.title = "acta revisada";
    drogon::sync_wait(syrax::save(doc, db.get()));

    const auto releido = drogon::sync_wait(Query<Doc>(db.get()).first());
    REQUIRE(releido.has_value());
    CHECK(releido->title == "acta revisada");
}

TEST_CASE("postgres numera los parametros y sqlite no", "[query]") {
    const auto previo = syrax::db::dialect();

    syrax::db::activeDialect() = syrax::db::Dialect::Postgres;
    const auto pg = Query<User>().where(&User::age, ">", 18)
                        .where(&User::name, "=", std::string{"ada"}).toSql();
    CHECK(pg.find("\"age\" > $1") != std::string::npos);
    CHECK(pg.find("\"name\" = $2") != std::string::npos);

    syrax::db::activeDialect() = syrax::db::Dialect::Sqlite;
    const auto lite = Query<User>().where(&User::age, ">", 18)
                          .where(&User::name, "=", std::string{"ada"}).toSql();
    CHECK(lite.find("\"age\" > ?") != std::string::npos);

    syrax::db::activeDialect() = previo;
}

// ------------------------------------------------------------ agrupacion

TEST_CASE("whereGroup pone los parentesis", "[query]") {
    SqliteDialect dialect;

    const auto sql = Query<User>()
                         .where(&User::age, ">", 18)
                         .whereGroup([](auto& g) {
                             g.where(&User::name, "LIKE", std::string{"a%"})
                                 .orWhere(&User::email, "IS", nullptr);
                         })
                         .toSql();

    CHECK(sql.find(R"("age" > ? AND ("name" LIKE ? OR "email" IS ?))") != std::string::npos);
}

TEST_CASE("un grupo no rompe la numeracion de los que vienen detras", "[query]") {
    const auto previo = syrax::db::dialect();
    syrax::db::activeDialect() = syrax::db::Dialect::Postgres;

    const auto sql = Query<User>()
                         .where(&User::age, ">", 18)
                         .whereGroup([](auto& g) {
                             g.where(&User::name, "=", std::string{"ada"})
                                 .orWhere(&User::name, "=", std::string{"grace"});
                         })
                         .where(&User::age, "<", 99)
                         .toSql();

    CHECK(sql.find(R"("age" > $1 AND ("name" = $2 OR "name" = $3) AND "age" < $4)") !=
          std::string::npos);

    syrax::db::activeDialect() = previo;
}

TEST_CASE("un grupo vacio no deja un parentesis suelto", "[query]") {
    SqliteDialect dialect;

    const auto sql = Query<User>().where(&User::age, ">", 18).whereGroup([](auto&) {}).toSql();

    CHECK(sql.find("()") == std::string::npos);
    CHECK(sql.find(R"(WHERE "age" > ?)") != std::string::npos);
}

TEST_CASE("agrupar cambia que filas vuelven, no solo el SQL", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    // Encadenado plano: SQL aplica AND antes que OR, asi que esto es
    // (age > 18 AND name = 'ada') OR name = 'linus' -> entran los dos.
    const auto plano = drogon::sync_wait(Query<User>(db.get())
                                             .where(&User::age, ">", 18)
                                             .where(&User::name, "=", std::string{"ada"})
                                             .orWhere(&User::name, "=", std::string{"linus"})
                                             .get());
    CHECK(plano.size() == 2);

    // Agrupado: age > 18 AND (name = 'ada' OR name = 'linus'). Linus tiene 17.
    const auto agrupado = drogon::sync_wait(Query<User>(db.get())
                                                .where(&User::age, ">", 18)
                                                .whereGroup([](auto& g) {
                                                    g.where(&User::name, "=", std::string{"ada"})
                                                        .orWhere(&User::name, "=",
                                                                 std::string{"linus"});
                                                })
                                                .get());
    REQUIRE(agrupado.size() == 1);
    CHECK(agrupado.front().name == "ada");
}

// --------------------------------------------------------------- update

TEST_CASE("update cambia las filas filtradas en una sola consulta", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto tocadas = drogon::sync_wait(Query<User>(db.get())
                                               .where(&User::age, "<", 18)
                                               .set(&User::name, std::string{"menor"})
                                               .set(&User::age, 0)
                                               .update());
    CHECK(tocadas == 1);

    // Si el orden de enlace fuera el otro, sqlite habria puesto el 18 en el
    // nombre o filtrado por "menor": el numero de filas ya no cuadraria.
    const auto menor = drogon::sync_wait(
        Query<User>(db.get()).where(&User::name, "=", std::string{"menor"}).first());
    REQUIRE(menor.has_value());
    CHECK(menor->age == 0);

    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 4);
    CHECK(drogon::sync_wait(
              Query<User>(db.get()).where(&User::name, "=", std::string{"ada"}).count()) == 1);
}

TEST_CASE("un update sin set no toca nada", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    // "UPDATE users SET  WHERE ..." no es SQL: el builder ni lo intenta.
    CHECK(drogon::sync_wait(Query<User>(db.get()).where(&User::age, "<", 18).update()) == 0);
    CHECK(drogon::sync_wait(
              Query<User>(db.get()).where(&User::name, "=", std::string{"linus"}).count()) == 1);
}

TEST_CASE("en postgres el SET se numera detras del WHERE", "[query]") {
    const auto previo = syrax::db::dialect();
    syrax::db::activeDialect() = syrax::db::Dialect::Postgres;

    const auto sql = Query<User>()
                         .where(&User::age, "<", 18)
                         .set(&User::name, std::string{"menor"})
                         .toUpdateSql();

    CHECK(sql == R"(UPDATE "users" SET "name" = $2 WHERE "age" < $1)");

    syrax::db::activeDialect() = previo;
}

// ---------------------------------------------------------------- enums

TEST_CASE("un enum se enlaza como su tipo subyacente", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    CHECK(drogon::sync_wait(
              Query<Invoice>(db.get()).where(&Invoice::status, "=", Status::Paid).count()) == 1);

    const auto pendientes = drogon::sync_wait(
        Query<Invoice>(db.get())
            .whereIn(&Invoice::status, {Status::Draft, Status::Sent})
            .orderBy(&Invoice::total)
            .get());

    REQUIRE(pendientes.size() == 2);
    CHECK(pendientes.front().status == Status::Draft);
}

TEST_CASE("un campo enum vuelve del SELECT con su nombre", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    Invoice nueva{.id = 0, .status = Status::Sent, .total = 42};
    drogon::sync_wait(syrax::save(nueva, db.get()));
    REQUIRE(nueva.id > 0);
    CHECK(nueva.status == Status::Sent);

    drogon::sync_wait(Query<Invoice>(db.get())
                          .where(&Invoice::id, "=", nueva.id)
                          .set(&Invoice::status, Status::Paid)
                          .update());

    const auto releida = drogon::sync_wait(
        Query<Invoice>(db.get()).where(&Invoice::id, "=", nueva.id).first());

    REQUIRE(releida.has_value());
    CHECK(releida->status == Status::Paid);
    CHECK(releida->total == 42);
}

// ---------------------------------------------------------- transacciones

TEST_CASE("el builder corre dentro de una transaccion", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    // Comprobar y luego insertar, las dos cosas sobre la misma transaccion:
    // es el caso que antes obligaba a bajar a SQL a mano.
    const auto creado = drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<std::optional<User>> {
            const bool tomado = co_await Query<User>(tx.client())
                                    .where(&User::email, "=", std::string{"hedy@x.com"})
                                    .exists();
            if (tomado) co_return std::nullopt;

            User nuevo{.id = 0, .name = "hedy", .email = "hedy@x.com", .age = 30};
            co_await syrax::save(nuevo, tx.client());
            co_return nuevo;
        }));

    REQUIRE(creado.has_value());
    CHECK(creado->id > 0);
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 5);
}

TEST_CASE("lo que hace el builder en una transaccion se deshace con ella", "[query][db]") {
    SqliteDialect dialect;
    TempDb        db;

    drogon::sync_wait(syrax::db::transactionOn(
        db.get(), [](const syrax::db::Tx& tx) -> drogon::Task<void> {
            User nuevo{.id = 0, .name = "hedy", .email = "hedy@x.com", .age = 30};
            co_await syrax::save(nuevo, tx.client());

            co_await Query<User>(tx.client()).where(&User::age, "<", 18).del();

            // Abortar como decision de negocio: ni el alta ni el borrado valen.
            tx.rollback();
            co_return;
        }));

    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 4);
    CHECK(drogon::sync_wait(
              Query<User>(db.get()).where(&User::name, "=", std::string{"linus"}).count()) == 1);
}

// ------------------------------------------------------------- paginacion

TEST_CASE("paginate devuelve la pagina y el total", "[query][paginate]") {
    SqliteDialect dialect;
    TempDb        db;

    // Hay 4 usuarios en el fixture.
    const auto pagina = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::id).paginate(/*page=*/1, /*perPage=*/2));

    CHECK(pagina.data.size() == 2);
    CHECK(pagina.data.front().name == "ada");

    // El total es el de TODAS las filas que cumplen el filtro, no el de la
    // pagina: sin eso no se puede pintar un paginador.
    CHECK(pagina.total == 4);
    CHECK(pagina.page == 1);
    CHECK(pagina.perPage == 2);
    CHECK(pagina.pages == 2);
    CHECK(pagina.hasMore);
}

TEST_CASE("la ultima pagina dice que no hay mas", "[query][paginate]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto ultima = drogon::sync_wait(Query<User>(db.get()).orderBy(&User::id).paginate(2, 2));

    CHECK(ultima.data.size() == 2);
    CHECK(ultima.data.front().name == "grace");
    CHECK_FALSE(ultima.hasMore);
}

TEST_CASE("los filtros cuentan para el total, no solo para la pagina", "[query][paginate]") {
    SqliteDialect dialect;
    TempDb        db;

    // Tres mayores de edad de los cuatro.
    const auto pagina =
        drogon::sync_wait(Query<User>(db.get()).where(&User::age, ">", 18).orderBy(&User::id).paginate(1, 2));

    CHECK(pagina.total == 3);
    CHECK(pagina.pages == 2);
    CHECK(pagina.data.size() == 2);
}

TEST_CASE("una pagina pasada del final sale vacia, no rota", "[query][paginate]") {
    SqliteDialect dialect;
    TempDb        db;

    // Es lo que pasa cuando alguien borra filas mientras otro pagina.
    const auto vacia = drogon::sync_wait(Query<User>(db.get()).paginate(99, 10));

    CHECK(vacia.data.empty());
    CHECK(vacia.total == 4);
    CHECK_FALSE(vacia.hasMore);
}

TEST_CASE("una tabla vacia da una pagina vacia y cero paginas", "[query][paginate]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto vacia = drogon::sync_wait(Query<Doc>(db.get()).paginate(1, 10));

    CHECK(vacia.data.empty());
    CHECK(vacia.total == 0);
    CHECK(vacia.pages == 0);
    CHECK_FALSE(vacia.hasMore);
}

TEST_CASE("una pagina que no existe se corrige en vez de romper el SQL", "[query][paginate]") {
    SqliteDialect dialect;
    TempDb        db;

    // Un ?page=0 es siempre un parametro mal leido, y un OFFSET negativo seria
    // un error de SQL en vez de una respuesta.
    const auto cero = drogon::sync_wait(Query<User>(db.get()).orderBy(&User::id).paginate(0, 2));

    CHECK(cero.page == 1);
    CHECK(cero.data.size() == 2);

    const auto negativa = drogon::sync_wait(Query<User>(db.get()).paginate(-5, -5));
    CHECK(negativa.page == 1);
    CHECK(negativa.perPage == 1);
}

TEST_CASE("el numero de paginas redondea hacia arriba", "[query][paginate]") {
    SqliteDialect dialect;
    TempDb        db;

    // 4 filas de 3 en 3 son dos paginas, no una: es la division entera que en
    // el cliente alguien redondea mal.
    CHECK(drogon::sync_wait(Query<User>(db.get()).paginate(1, 3)).pages == 2);
    CHECK(drogon::sync_wait(Query<User>(db.get()).paginate(1, 4)).pages == 1);
    CHECK(drogon::sync_wait(Query<User>(db.get()).paginate(1, 1)).pages == 4);
}

// ----------------------------------------------- soft deletes y timestamps

namespace qtest {

struct Note {
    std::int64_t               id;
    std::string                body;
    std::optional<std::string> created_at;
    std::optional<std::string> updated_at;
    std::optional<std::string> deleted_at;

    static constexpr auto table       = "notes";
    static constexpr auto timestamps  = true;
    static constexpr auto softDeletes = true;
};

// La misma tabla, sin declarar nada: para comprobar que sin los marcadores el
// comportamiento es el de siempre.
struct Plain {
    std::int64_t id;
    std::string  body;

    static constexpr auto table = "notes";
};

class NotesDb {
public:
    NotesDb() : path_{fs::temp_directory_path() / name()} {
        std::error_code ec;
        fs::remove(path_, ec);

        client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);
        client_->execSqlSync(
            "CREATE TABLE notes (id INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT, "
            "created_at TEXT, updated_at TEXT, deleted_at TEXT)");
    }
    ~NotesDb() {
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
        return "syrax_notes_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".db";
    }

    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

}  // namespace qtest

TEST_CASE("save rellena created_at y updated_at al insertar", "[query][timestamps]") {
    SqliteDialect dialect;
    NotesDb       db;

    Note nota{.id = 0, .body = "primera"};
    drogon::sync_wait(syrax::save(nota, db.get()));

    // Vuelven rellenos por el RETURNING: el modelo los declara, asi que se
    // leen aunque no se escriban desde el struct.
    REQUIRE(nota.created_at.has_value());
    REQUIRE(nota.updated_at.has_value());
    CHECK_FALSE(nota.created_at->empty());
}

TEST_CASE("un campo de timestamp vacio no pisa el de la base", "[query][timestamps]") {
    SqliteDialect dialect;
    NotesDb       db;

    // El caso que rompe una implementacion ingenua: el struct trae el campo a
    // nullopt y, si se escribiera desde ahi, el created_at quedaria NULL.
    Note nota{.id = 0, .body = "x", .created_at = std::nullopt};
    drogon::sync_wait(syrax::save(nota, db.get()));

    const auto fila = db.get()->execSqlSync("SELECT created_at FROM notes");
    REQUIRE(fila.size() == 1);
    CHECK_FALSE(fila.front()["created_at"].isNull());
}

TEST_CASE("update() toca updated_at sin que nadie lo pida", "[query][timestamps]") {
    SqliteDialect dialect;
    NotesDb       db;

    db.get()->execSqlSync(
        "INSERT INTO notes (body, created_at, updated_at) VALUES ('vieja', '2020-01-01', '2020-01-01')");

    drogon::sync_wait(Query<Note>(db.get()).set(&Note::body, std::string{"nueva"}).update());

    const auto fila = db.get()->execSqlSync("SELECT body, updated_at FROM notes");
    CHECK(fila.front()["body"].as<std::string>() == "nueva");

    // Una fila que cambia y deja el updated_at viejo tiene un campo mintiendo.
    CHECK(fila.front()["updated_at"].as<std::string>() != "2020-01-01");
}

TEST_CASE("del() marca en vez de borrar, y la fila desaparece", "[query][softdeletes]") {
    SqliteDialect dialect;
    NotesDb       db;

    Note nota{.id = 0, .body = "borrame"};
    drogon::sync_wait(syrax::save(nota, db.get()));

    CHECK(drogon::sync_wait(Query<Note>(db.get()).del()) == 1);

    // Ya no se ve...
    CHECK(drogon::sync_wait(Query<Note>(db.get()).count()) == 0);

    // ...pero sigue en la tabla, que es justo el punto.
    CHECK(db.get()->execSqlSync("SELECT id FROM notes").size() == 1);
}

TEST_CASE("withTrashed las incluye y onlyTrashed deja solo esas", "[query][softdeletes]") {
    SqliteDialect dialect;
    NotesDb       db;

    Note viva{.id = 0, .body = "viva"};
    Note muerta{.id = 0, .body = "muerta"};
    drogon::sync_wait(syrax::save(viva, db.get()));
    drogon::sync_wait(syrax::save(muerta, db.get()));

    drogon::sync_wait(Query<Note>(db.get()).where(&Note::body, "=", std::string{"muerta"}).del());

    CHECK(drogon::sync_wait(Query<Note>(db.get()).count()) == 1);
    CHECK(drogon::sync_wait(Query<Note>(db.get()).withTrashed().count()) == 2);
    CHECK(drogon::sync_wait(Query<Note>(db.get()).onlyTrashed().count()) == 1);

    const auto papelera = drogon::sync_wait(Query<Note>(db.get()).onlyTrashed().get());
    REQUIRE(papelera.size() == 1);
    CHECK(papelera.front().body == "muerta");
}

TEST_CASE("el filtro de borrados no rompe un where con OR", "[query][softdeletes]") {
    SqliteDialect dialect;
    NotesDb       db;

    Note a{.id = 0, .body = "a"};
    Note b{.id = 0, .body = "b"};
    drogon::sync_wait(syrax::save(a, db.get()));
    drogon::sync_wait(syrax::save(b, db.get()));
    drogon::sync_wait(Query<Note>(db.get()).where(&Note::body, "=", std::string{"b"}).del());

    // Sin parentesis alrededor del where del usuario, esto se leeria como
    // "a = 'a' OR (b = 'b' AND no borrado)" y devolveria la fila borrada.
    const auto encontradas = drogon::sync_wait(Query<Note>(db.get())
                                                   .where(&Note::body, "=", std::string{"a"})
                                                   .orWhere(&Note::body, "=", std::string{"b"})
                                                   .get());

    REQUIRE(encontradas.size() == 1);
    CHECK(encontradas.front().body == "a");
}

TEST_CASE("restore deshace el borrado", "[query][softdeletes]") {
    SqliteDialect dialect;
    NotesDb       db;

    Note nota{.id = 0, .body = "vuelve"};
    drogon::sync_wait(syrax::save(nota, db.get()));
    drogon::sync_wait(Query<Note>(db.get()).del());
    REQUIRE(drogon::sync_wait(Query<Note>(db.get()).count()) == 0);

    CHECK(drogon::sync_wait(Query<Note>(db.get()).restore()) == 1);
    CHECK(drogon::sync_wait(Query<Note>(db.get()).count()) == 1);
}

TEST_CASE("forceDelete borra de verdad", "[query][softdeletes]") {
    SqliteDialect dialect;
    NotesDb       db;

    Note nota{.id = 0, .body = "adios"};
    drogon::sync_wait(syrax::save(nota, db.get()));

    CHECK(drogon::sync_wait(Query<Note>(db.get()).forceDelete()) == 1);
    CHECK(db.get()->execSqlSync("SELECT id FROM notes").empty());
}

TEST_CASE("sin los marcadores, del() borra como siempre", "[query][softdeletes]") {
    SqliteDialect dialect;
    NotesDb       db;

    db.get()->execSqlSync("INSERT INTO notes (body) VALUES ('cualquiera')");

    // La tabla TIENE deleted_at, pero el modelo no declara softDeletes: el
    // comportamiento no puede cambiar por como se llame una columna.
    CHECK(drogon::sync_wait(Query<Plain>(db.get()).del()) == 1);
    CHECK(db.get()->execSqlSync("SELECT id FROM notes").empty());
}

// ------------------------------------------------------------ multi-tenancy

namespace qtest {

struct Factura {
    std::int64_t id;
    std::string  tenant_id;
    int          total;

    static constexpr auto table  = "facturas";
    static constexpr auto tenant = true;
};

class TenantDb {
public:
    TenantDb() : path_{fs::temp_directory_path() / name()} {
        std::error_code ec;
        fs::remove(path_, ec);

        client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);
        client_->execSqlSync(
            "CREATE TABLE facturas (id INTEGER PRIMARY KEY AUTOINCREMENT, tenant_id TEXT, "
            "total INTEGER)");
        client_->execSqlSync(
            "INSERT INTO facturas (tenant_id, total) VALUES "
            "('acme', 100), ('acme', 200), ('globex', 300)");
    }
    ~TenantDb() {
        vivos().push_back(client_);
        client_.reset();

        std::error_code ec;
        fs::remove(path_, ec);
    }

    const drogon::orm::DbClientPtr& get() const { return client_; }

private:
    static std::string name() {
        static int counter = 0;
        return "syrax_tenant_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".db";
    }

    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

}  // namespace qtest

TEST_CASE("forTenant deja ver solo las filas de ese tenant", "[query][tenant]") {
    SqliteDialect dialect;
    TenantDb      db;

    CHECK(drogon::sync_wait(Query<Factura>(db.get()).forTenant("acme").count()) == 2);
    CHECK(drogon::sync_wait(Query<Factura>(db.get()).forTenant("globex").count()) == 1);
    CHECK(drogon::sync_wait(Query<Factura>(db.get()).forTenant("nadie").count()) == 0);
}

TEST_CASE("olvidar el tenant LANZA, no devuelve todo", "[query][tenant]") {
    SqliteDialect dialect;
    TenantDb      db;

    // Es la decision central. Devolver las tres filas seria servirle a un
    // cliente los datos de otro, en silencio y en produccion. Un 500 ruidoso
    // en la primera prueba convierte un fallo de seguridad en uno de
    // programacion normal.
    CHECK_THROWS_AS(drogon::sync_wait(Query<Factura>(db.get()).count()), std::runtime_error);
    CHECK_THROWS_AS(drogon::sync_wait(Query<Factura>(db.get()).get()), std::runtime_error);
    CHECK_THROWS_AS(drogon::sync_wait(Query<Factura>(db.get()).del()), std::runtime_error);
    CHECK_THROWS_AS(
        drogon::sync_wait(Query<Factura>(db.get()).set(&Factura::total, 1).update()),
        std::runtime_error);
}

TEST_CASE("el filtro de tenant no se rompe con un where con OR", "[query][tenant]") {
    SqliteDialect dialect;
    TenantDb      db;

    // Sin parentesis, esto seria "total = 100 OR (total = 300 AND tenant =
    // acme)" y devolveria la factura de globex.
    const auto encontradas = drogon::sync_wait(Query<Factura>(db.get())
                                                   .forTenant("acme")
                                                   .where(&Factura::total, "=", 100)
                                                   .orWhere(&Factura::total, "=", 300)
                                                   .get());

    REQUIRE(encontradas.size() == 1);
    CHECK(encontradas.front().total == 100);
}

TEST_CASE("un borrado no toca las filas de otro tenant", "[query][tenant]") {
    SqliteDialect dialect;
    TenantDb      db;

    CHECK(drogon::sync_wait(Query<Factura>(db.get()).forTenant("acme").del()) == 2);

    // La de globex sigue ahi.
    CHECK(db.get()->execSqlSync("SELECT id FROM facturas").size() == 1);
}

TEST_CASE("un update no toca las filas de otro tenant", "[query][tenant]") {
    SqliteDialect dialect;
    TenantDb      db;

    drogon::sync_wait(Query<Factura>(db.get()).forTenant("acme").set(&Factura::total, 0).update());

    const auto globex = db.get()->execSqlSync(
        "SELECT total FROM facturas WHERE tenant_id = 'globex'");
    REQUIRE(globex.size() == 1);
    CHECK(globex.front()["total"].as<int>() == 300);
}

TEST_CASE("paginar respeta el tenant", "[query][tenant]") {
    SqliteDialect dialect;
    TenantDb      db;

    const auto pagina =
        drogon::sync_wait(Query<Factura>(db.get()).forTenant("acme").orderBy(&Factura::id).paginate(1, 10));

    // El total es el del tenant, no el de la tabla: si no, el paginador del
    // cliente mostraria paginas que no existen para el.
    CHECK(pagina.total == 2);
    CHECK(pagina.data.size() == 2);
}

TEST_CASE("un modelo sin tenant no cambia en nada", "[query][tenant]") {
    SqliteDialect dialect;
    TempDb        db;

    // Sin el marcador no hay guardia ni filtro: el comportamiento de siempre.
    CHECK(drogon::sync_wait(Query<User>(db.get()).count()) == 4);
}
