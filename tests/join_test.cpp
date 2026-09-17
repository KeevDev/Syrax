// Joins: dos tablas, y de vuelta el struct plano que declara quien consulta.
//
// Lo que hay que probar no es que sepa concatenar "JOIN" -eso es texto- sino
// las tres cosas que lo separan de escribir el SQL a mano: que las columnas
// salgan calificadas con su tabla, que los borrados y el tenant de LAS DOS
// tablas se apliquen solos, y que el struct de vuelta se rellene por posicion.

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
namespace jtest {

struct User {
    std::int64_t id;
    std::string  name;

    static constexpr auto table = "users";
};

struct Post {
    std::int64_t id;
    std::string  title;
    std::int64_t author_id;

    static constexpr auto table = "posts";
};

// Con borrado logico, para ver que el join no resucita filas.
struct Articulo {
    std::int64_t id;
    std::string  title;
    std::int64_t author_id;

    static constexpr auto table       = "articulos";
    static constexpr auto softDeletes = true;
};

struct Autor {
    std::int64_t id;
    std::string  name;

    static constexpr auto table       = "autores";
    static constexpr auto softDeletes = true;
};

// Multi-tenant por fila, las dos.
struct Factura {
    std::int64_t id;
    std::string  numero;
    std::int64_t cliente_id;
    std::string  tenant_id;

    static constexpr auto table  = "facturas";
    static constexpr auto tenant = true;
};

struct Cliente {
    std::int64_t id;
    std::string  nombre;
    std::string  tenant_id;

    static constexpr auto table  = "clientes";
    static constexpr auto tenant = true;
};

// --- lo que vuelve ---

struct PostConAutor {
    std::int64_t id;
    std::string  title;
    std::string  author;
};

struct SoloTitulo {
    std::string title;
};

struct FacturaConCliente {
    std::string numero;
    std::string cliente;
};

// Los clientes que se quedan vivos hasta que muere el proceso: cerrar uno de
// Drogon con callbacks en vuelo aborta el proceso DESPUES de que el test pase.
// Mismo motivo que en query_test.cpp.
inline std::vector<drogon::orm::DbClientPtr>& vivos() {
    static std::vector<drogon::orm::DbClientPtr> lista;
    return lista;
}

class TempDb {
public:
    TempDb() : path_{fs::temp_directory_path() / name()} {
        std::error_code ec;
        fs::remove(path_, ec);

        client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);

        client_->execSqlSync("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)");
        client_->execSqlSync(
            "INSERT INTO users (id, name) VALUES (1, 'ada'), (2, 'alan'), (3, 'grace')");

        client_->execSqlSync(
            "CREATE TABLE posts (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)");
        // grace (3) no escribe nada: es la que prueba el LEFT JOIN.
        client_->execSqlSync(
            "INSERT INTO posts (id, title, author_id) VALUES "
            "(1, 'primero', 1), (2, 'segundo', 1), (3, 'tercero', 2), (4, 'huerfano', 99)");

        client_->execSqlSync(
            "CREATE TABLE autores (id INTEGER PRIMARY KEY, name TEXT, deleted_at TEXT)");
        client_->execSqlSync(
            "INSERT INTO autores (id, name, deleted_at) VALUES "
            "(1, 'viva', NULL), (2, 'borrada', '2026-01-01')");

        client_->execSqlSync("CREATE TABLE articulos (id INTEGER PRIMARY KEY, title TEXT, "
                             "author_id INTEGER, deleted_at TEXT)");
        client_->execSqlSync(
            "INSERT INTO articulos (id, title, author_id, deleted_at) VALUES "
            "(1, 'de autor vivo', 1, NULL), "
            "(2, 'borrado', 1, '2026-01-01'), "
            "(3, 'de autor borrado', 2, NULL)");

        client_->execSqlSync("CREATE TABLE clientes (id INTEGER PRIMARY KEY, nombre TEXT, "
                             "tenant_id TEXT)");
        client_->execSqlSync(
            "INSERT INTO clientes (id, nombre, tenant_id) VALUES "
            "(1, 'cliente de acme', 'acme'), (2, 'cliente de globex', 'globex')");

        client_->execSqlSync("CREATE TABLE facturas (id INTEGER PRIMARY KEY, numero TEXT, "
                             "cliente_id INTEGER, tenant_id TEXT)");
        client_->execSqlSync(
            "INSERT INTO facturas (id, numero, cliente_id, tenant_id) VALUES "
            "(1, 'F-001', 1, 'acme'), (2, 'F-002', 1, 'acme'), (3, 'G-001', 2, 'globex')");
    }
    ~TempDb() {
        vivos().push_back(client_);
        client_.reset();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    const drogon::orm::DbClientPtr& get() const { return client_; }

private:
    static std::string name() {
        static int n = 0;
        return "syrax_j_" + std::to_string(::getpid()) + "_" + std::to_string(n++) + ".db";
    }
    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

struct SqliteDialect {
    syrax::db::Dialect previous = syrax::db::dialect();
    SqliteDialect() { syrax::db::activeDialect() = syrax::db::Dialect::Sqlite; }
    ~SqliteDialect() { syrax::db::activeDialect() = previous; }
};

}  // namespace jtest

using namespace jtest;
using syrax::Dir;
using syrax::Query;

// ------------------------------------------------------------- el SQL

TEST_CASE("el join califica cada columna con su tabla", "[join]") {
    SqliteDialect dialect;

    const auto sql = Query<Post>()
                         .join<User>(&Post::author_id, &User::id)
                         .toSql<PostConAutor>(&Post::id, &Post::title, &User::name);

    CHECK(sql == R"(SELECT "posts"."id" AS "id", "posts"."title" AS "title", )"
                 R"("users"."name" AS "author" FROM "posts" )"
                 R"(INNER JOIN "users" ON "users"."id" = "posts"."author_id")");
}

TEST_CASE("los alias salen de los campos del struct de vuelta", "[join]") {
    SqliteDialect dialect;

    // El struct pide `title`, la columna se llama `name`: el alias lo pone el
    // campo, que es lo que hace que Glaze sepa rellenarlo.
    const auto sql = Query<Post>()
                         .join<User>(&Post::author_id, &User::id)
                         .toSql<SoloTitulo>(&User::name);

    CHECK(sql.find(R"("users"."name" AS "title")") != std::string::npos);
}

TEST_CASE("leftJoin dice LEFT JOIN", "[join]") {
    SqliteDialect dialect;

    const auto sql = Query<Post>()
                         .leftJoin<User>(&Post::author_id, &User::id)
                         .toSql<PostConAutor>(&Post::id, &Post::title, &User::name);

    CHECK(sql.find("LEFT JOIN") != std::string::npos);
}

TEST_CASE("se puede filtrar y ordenar por cualquiera de las dos tablas", "[join]") {
    SqliteDialect dialect;

    const auto sql = Query<Post>()
                         .join<User>(&Post::author_id, &User::id)
                         .where(&User::name, "=", std::string{"ada"})
                         .where(&Post::title, "!=", std::string{"borrador"})
                         .orderBy(&User::name)
                         .orderBy(&Post::id, Dir::Desc)
                         .toSql<PostConAutor>(&Post::id, &Post::title, &User::name);

    CHECK(sql.find(R"(WHERE "users"."name" = ? AND "posts"."title" != ?)") != std::string::npos);
    CHECK(sql.find(R"(ORDER BY "users"."name" ASC, "posts"."id" DESC)") != std::string::npos);
}

TEST_CASE("limit y offset van al final", "[join]") {
    SqliteDialect dialect;

    const auto sql = Query<Post>()
                         .join<User>(&Post::author_id, &User::id)
                         .limit(10)
                         .offset(20)
                         .toSql<PostConAutor>(&Post::id, &Post::title, &User::name);

    CHECK(sql.find("LIMIT 10 OFFSET 20") != std::string::npos);
}

TEST_CASE("un IN vacio no coincide con nada, en vez de ser SQL invalido", "[join]") {
    SqliteDialect dialect;

    const auto sql = Query<Post>()
                         .join<User>(&Post::author_id, &User::id)
                         .whereIn(&User::id, std::vector<std::int64_t>{})
                         .toSql<PostConAutor>(&Post::id, &Post::title, &User::name);

    CHECK(sql.find("WHERE 1 = 0") != std::string::npos);
}

TEST_CASE("join va antes que los filtros, y decirlo es mejor que adivinar", "[join]") {
    SqliteDialect dialect;

    // Los where de Query<T> se escriben sin calificar. Con dos tablas, un "id"
    // a secas es ambiguo: reescribir lo ya acumulado seria adivinar.
    CHECK_THROWS_AS(Query<Post>().where(&Post::id, ">", 1).join<User>(&Post::author_id, &User::id),
                    std::runtime_error);
}

// -------------------------------------------------- contra sqlite de verdad

TEST_CASE("el join trae las filas emparejadas", "[join]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto filas = drogon::sync_wait(Query<Post>(db.get())
                                             .join<User>(&Post::author_id, &User::id)
                                             .orderBy(&Post::id)
                                             .get<PostConAutor>(&Post::id, &Post::title,
                                                                &User::name));

    // El post 4 apunta a un autor que no existe: un INNER JOIN lo deja fuera.
    REQUIRE(filas.size() == 3);
    CHECK(filas[0].title == "primero");
    CHECK(filas[0].author == "ada");
    CHECK(filas[2].title == "tercero");
    CHECK(filas[2].author == "alan");
}

TEST_CASE("el leftJoin conserva la fila sin pareja", "[join]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto filas = drogon::sync_wait(Query<Post>(db.get())
                                             .leftJoin<User>(&Post::author_id, &User::id)
                                             .orderBy(&Post::id)
                                             .get<PostConAutor>(&Post::id, &Post::title,
                                                                &User::name));

    REQUIRE(filas.size() == 4);
    CHECK(filas[3].title == "huerfano");
    // Sin pareja, la columna viene NULL y el campo se queda en su valor por
    // defecto: es la misma regla que en el resto del mapeo.
    CHECK(filas[3].author.empty());
}

TEST_CASE("un filtro sobre la tabla unida recorta el resultado", "[join]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto filas = drogon::sync_wait(Query<Post>(db.get())
                                             .join<User>(&Post::author_id, &User::id)
                                             .where(&User::name, "=", std::string{"ada"})
                                             .get<PostConAutor>(&Post::id, &Post::title,
                                                                &User::name));

    CHECK(filas.size() == 2);
}

TEST_CASE("first devuelve una sola fila", "[join]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto fila = drogon::sync_wait(Query<Post>(db.get())
                                            .join<User>(&Post::author_id, &User::id)
                                            .orderBy(&Post::id)
                                            .first<PostConAutor>(&Post::id, &Post::title,
                                                                 &User::name));

    REQUIRE(fila.has_value());
    CHECK(fila->title == "primero");
}

TEST_CASE("count cuenta las filas del join", "[join]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto total = drogon::sync_wait(
        Query<Post>(db.get()).join<User>(&Post::author_id, &User::id).count());

    CHECK(total == 3);
}

// ------------------------------------------------------- borrado logico

TEST_CASE("el join no devuelve filas borradas de la tabla base", "[join]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto filas = drogon::sync_wait(Query<Articulo>(db.get())
                                             .join<Autor>(&Articulo::author_id, &Autor::id)
                                             .orderBy(&Articulo::id)
                                             .get<PostConAutor>(&Articulo::id, &Articulo::title,
                                                                &Autor::name));

    // De tres articulos: uno borrado, y otro cuyo AUTOR esta borrado.
    REQUIRE(filas.size() == 1);
    CHECK(filas[0].title == "de autor vivo");
}

TEST_CASE("una fila borrada de la tabla unida no resucita por el join", "[join]") {
    SqliteDialect dialect;
    TempDb        db;

    // El articulo 3 esta vivo, pero su autor no. Filtrar solo la tabla base
    // dejaria pasar el nombre de una fila que se dio por borrada.
    const auto sql = Query<Articulo>()
                         .join<Autor>(&Articulo::author_id, &Autor::id)
                         .toSql<PostConAutor>(&Articulo::id, &Articulo::title, &Autor::name);

    CHECK(sql.find(R"("articulos"."deleted_at" IS NULL)") != std::string::npos);
    CHECK(sql.find(R"("autores"."deleted_at" IS NULL)") != std::string::npos);
}

// ---------------------------------------------------------- multi-tenancy

TEST_CASE("olvidar el tenant en un join lanza", "[join][tenant]") {
    SqliteDialect dialect;
    TempDb        db;

    // El mismo guardia que en Query, y por mas motivo: en un join basta que se
    // escape UNA de las dos tablas para servir datos de otro cliente.
    CHECK_THROWS_AS(drogon::sync_wait(Query<Factura>(db.get())
                                          .join<Cliente>(&Factura::cliente_id, &Cliente::id)
                                          .get<FacturaConCliente>(&Factura::numero,
                                                                  &Cliente::nombre)),
                    std::runtime_error);
}

TEST_CASE("forTenant filtra las dos tablas", "[join][tenant]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto sql = Query<Factura>()
                         .join<Cliente>(&Factura::cliente_id, &Cliente::id)
                         .forTenant("acme")
                         .toSql<FacturaConCliente>(&Factura::numero, &Cliente::nombre);

    CHECK(sql.find(R"("facturas"."tenant_id" = ?)") != std::string::npos);
    CHECK(sql.find(R"("clientes"."tenant_id" = ?)") != std::string::npos);
}

TEST_CASE("cada tenant ve lo suyo y nada mas", "[join][tenant]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto acme = drogon::sync_wait(Query<Factura>(db.get())
                                            .join<Cliente>(&Factura::cliente_id, &Cliente::id)
                                            .forTenant("acme")
                                            .orderBy(&Factura::id)
                                            .get<FacturaConCliente>(&Factura::numero,
                                                                    &Cliente::nombre));

    REQUIRE(acme.size() == 2);
    CHECK(acme[0].numero == "F-001");
    CHECK(acme[0].cliente == "cliente de acme");

    const auto globex = drogon::sync_wait(Query<Factura>(db.get())
                                              .join<Cliente>(&Factura::cliente_id, &Cliente::id)
                                              .forTenant("globex")
                                              .get<FacturaConCliente>(&Factura::numero,
                                                                      &Cliente::nombre));

    REQUIRE(globex.size() == 1);
    CHECK(globex[0].numero == "G-001");

    const auto otro = drogon::sync_wait(Query<Factura>(db.get())
                                            .join<Cliente>(&Factura::cliente_id, &Cliente::id)
                                            .forTenant("nadie")
                                            .get<FacturaConCliente>(&Factura::numero,
                                                                    &Cliente::nombre));

    CHECK(otro.empty());
}
