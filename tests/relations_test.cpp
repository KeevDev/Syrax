// Relaciones: agrupar en memoria, traerse los hijos en dos consultas, y
// declarar en el modelo que dos tablas se relacionan.
//
// Lo que hay que probar es que son SIEMPRE dos consultas -no una por padre, que
// es el N+1 que esto viene a cerrar-, que un padre sin hijos no desaparece, y
// que paginar pagina la tabla base y no el producto.

#include <catch2/catch_test_macros.hpp>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <syrax/query.hpp>
#include <syrax/relations.hpp>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Glaze no refleja tipos en namespace anonimo: por eso lleva nombre.
namespace rtest {

// El hijo va primero: la relacion se declara en el padre, y para nombrar
// &Post::author_id el tipo tiene que existir ya.
struct Post {
    std::int64_t id;
    std::string  title;
    std::int64_t author_id;

    static constexpr auto table = "posts";
};

struct User {
    std::int64_t id;
    std::string  name;

    static constexpr auto table     = "users";
    static constexpr auto relations = syrax::relate(
        syrax::hasMany(&Post::author_id, &User::id));
};

// Sin relaciones declaradas: las claves se pasan a mano.
struct Plain {
    std::int64_t id;
    std::string  name;

    static constexpr auto table = "users";
};

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

struct UserConPosts {
    User              user;
    std::vector<Post> posts;
};

struct PlainConPosts {
    Plain             user;
    std::vector<Post> posts;
};

struct ClienteConFacturas {
    Cliente              cliente;
    std::vector<Factura> facturas;
};

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
        client_->execSqlSync("INSERT INTO users (id, name) VALUES "
                             "(1, 'ada'), (2, 'alan'), (3, 'grace')");

        client_->execSqlSync(
            "CREATE TABLE posts (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)");
        // grace (3) no escribe nada: es la que prueba que un padre sin hijos
        // sigue estando.
        client_->execSqlSync("INSERT INTO posts (id, title, author_id) VALUES "
                             "(1, 'primero', 1), (2, 'segundo', 1), (3, 'tercero', 2)");

        client_->execSqlSync("CREATE TABLE clientes (id INTEGER PRIMARY KEY, nombre TEXT, "
                             "tenant_id TEXT)");
        client_->execSqlSync("INSERT INTO clientes (id, nombre, tenant_id) VALUES "
                             "(1, 'de acme', 'acme'), (2, 'de globex', 'globex')");

        client_->execSqlSync("CREATE TABLE facturas (id INTEGER PRIMARY KEY, numero TEXT, "
                             "cliente_id INTEGER, tenant_id TEXT)");
        client_->execSqlSync("INSERT INTO facturas (id, numero, cliente_id, tenant_id) VALUES "
                             "(1, 'F-001', 1, 'acme'), (2, 'F-002', 1, 'acme'), "
                             "(3, 'G-001', 2, 'globex')");
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
        return "syrax_r_" + std::to_string(::getpid()) + "_" + std::to_string(n++) + ".db";
    }
    fs::path                 path_;
    drogon::orm::DbClientPtr client_;
};

struct SqliteDialect {
    syrax::db::Dialect previous = syrax::db::dialect();
    SqliteDialect() { syrax::db::activeDialect() = syrax::db::Dialect::Sqlite; }
    ~SqliteDialect() { syrax::db::activeDialect() = previous; }
};

}  // namespace rtest

using namespace rtest;
using syrax::Query;

// ------------------------------------------------- groupBy, sin tocar la base

TEST_CASE("groupBy agrupa los hijos bajo su padre", "[relations]") {
    const std::vector<User> users{{1, "ada"}, {2, "alan"}, {3, "grace"}};
    const std::vector<Post> posts{{1, "primero", 1}, {2, "segundo", 1}, {3, "tercero", 2}};

    const auto arbol = syrax::groupInto<UserConPosts>(users, posts, &User::id, &Post::author_id);

    REQUIRE(arbol.size() == 3);
    CHECK(arbol[0].user.name == "ada");
    CHECK(arbol[0].posts.size() == 2);
    CHECK(arbol[1].posts.size() == 1);
}

TEST_CASE("un padre sin hijos sale con el vector vacio, no desaparece", "[relations]") {
    const std::vector<User> users{{1, "ada"}, {3, "grace"}};
    const std::vector<Post> posts{{1, "primero", 1}};

    const auto arbol = syrax::groupInto<UserConPosts>(users, posts, &User::id, &Post::author_id);

    // Los padres son los que trajiste: esto no esta para descartar ninguno.
    REQUIRE(arbol.size() == 2);
    CHECK(arbol[1].user.name == "grace");
    CHECK(arbol[1].posts.empty());
}

TEST_CASE("se conserva el orden, el de los padres y el de los hijos", "[relations]") {
    // El orden lo puso el ORDER BY de quien consulto; reordenar aqui seria
    // pisar esa decision.
    const std::vector<User> users{{2, "alan"}, {1, "ada"}};
    const std::vector<Post> posts{{9, "ultimo", 1}, {1, "primero", 1}};

    const auto arbol = syrax::groupInto<UserConPosts>(users, posts, &User::id, &Post::author_id);

    CHECK(arbol[0].user.name == "alan");
    REQUIRE(arbol[1].posts.size() == 2);
    CHECK(arbol[1].posts[0].title == "ultimo");
}

TEST_CASE("sin hijos, todos los padres salen vacios", "[relations]") {
    const std::vector<User> users{{1, "ada"}};

    const auto arbol = syrax::groupInto<UserConPosts>(users, {}, &User::id, &Post::author_id);

    REQUIRE(arbol.size() == 1);
    CHECK(arbol[0].posts.empty());
}

TEST_CASE("groupBy trae el struct por defecto", "[relations]") {
    const std::vector<User> users{{1, "ada"}};
    const std::vector<Post> posts{{1, "primero", 1}};

    const auto arbol = syrax::groupBy(users, posts, &User::id, &Post::author_id);

    REQUIRE(arbol.size() == 1);
    CHECK(arbol[0].parent.name == "ada");
    CHECK(arbol[0].children.size() == 1);
}

// --------------------------------------------- with(), contra sqlite de verdad

TEST_CASE("with trae los hijos y los agrupa", "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto arbol = drogon::sync_wait(Query<User>(db.get())
                                             .orderBy(&User::id)
                                             .with<Post>(&Post::author_id, &User::id)
                                             .get<UserConPosts>());

    REQUIRE(arbol.size() == 3);
    CHECK(arbol[0].user.name == "ada");
    CHECK(arbol[0].posts.size() == 2);
    CHECK(arbol[0].posts[0].title == "primero");
    CHECK(arbol[2].user.name == "grace");
    CHECK(arbol[2].posts.empty());
}

TEST_CASE("with se puede encadenar detras de un filtro", "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    // A diferencia de join(), esto puede ir en cualquier sitio: son dos
    // consultas separadas y no hay ninguna columna que calificar.
    const auto arbol = drogon::sync_wait(Query<User>(db.get())
                                             .where(&User::name, "=", std::string{"ada"})
                                             .with<Post>(&Post::author_id, &User::id)
                                             .get<UserConPosts>());

    REQUIRE(arbol.size() == 1);
    CHECK(arbol[0].posts.size() == 2);
}

TEST_CASE("with sin claves las saca del modelo", "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    // User declara `relations`, asi que aqui no se repiten.
    const auto arbol = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::id).with<Post>().get<UserConPosts>());

    REQUIRE(arbol.size() == 3);
    CHECK(arbol[0].posts.size() == 2);
    CHECK(arbol[2].posts.empty());
}

TEST_CASE("un modelo sin relaciones declaradas sigue sirviendo con las claves a mano",
          "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto arbol = drogon::sync_wait(Query<Plain>(db.get())
                                             .orderBy(&Plain::id)
                                             .with<Post>(&Post::author_id, &Plain::id)
                                             .get<PlainConPosts>());

    REQUIRE(arbol.size() == 3);
    CHECK(arbol[0].posts.size() == 2);
}

TEST_CASE("first trae un padre con sus hijos", "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto uno = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::id).with<Post>().first<UserConPosts>());

    REQUIRE(uno.has_value());
    CHECK(uno->user.name == "ada");
    CHECK(uno->posts.size() == 2);
}

TEST_CASE("first sin filas es nullopt", "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto uno = drogon::sync_wait(Query<User>(db.get())
                                           .where(&User::name, "=", std::string{"nadie"})
                                           .with<Post>()
                                           .first<UserConPosts>());

    CHECK(!uno.has_value());
}

TEST_CASE("paginate pagina la tabla base, no el producto", "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto page = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::id).with<Post>().paginate<UserConPosts>(1, 2));

    // Tres usuarios y tres posts. Con un join, un LIMIT 2 cortaria FILAS del
    // producto y devolveria a ada dos veces; aqui son dos USUARIOS.
    REQUIRE(page.data.size() == 2);
    CHECK(page.data[0].user.name == "ada");
    CHECK(page.data[0].posts.size() == 2);
    CHECK(page.data[1].user.name == "alan");

    // Y el sobre cuenta usuarios, que es lo que el cliente pagina.
    CHECK(page.total == 3);
    CHECK(page.pages == 2);
    CHECK(page.hasMore);
}

TEST_CASE("la segunda pagina solo trae los hijos de sus padres", "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto page = drogon::sync_wait(
        Query<User>(db.get()).orderBy(&User::id).with<Post>().paginate<UserConPosts>(2, 2));

    REQUIRE(page.data.size() == 1);
    CHECK(page.data[0].user.name == "grace");
    CHECK(page.data[0].posts.empty());
    CHECK(!page.hasMore);
}

// ---------------------------------------------------------- multi-tenancy

TEST_CASE("el tenant llega tambien a la consulta hija", "[relations][tenant]") {
    SqliteDialect dialect;
    TempDb        db;

    const auto arbol = drogon::sync_wait(Query<Cliente>(db.get())
                                             .orderBy(&Cliente::id)
                                             .with<Factura>(&Factura::cliente_id, &Cliente::id)
                                             .forTenant("acme")
                                             .get<ClienteConFacturas>());

    // Un solo cliente de acme, con sus dos facturas. Si el tenant no llegara a
    // la consulta hija, esa consulta lanzaria -Factura declara `tenant`-, que
    // es justo el fallo ruidoso que se quiere.
    REQUIRE(arbol.size() == 1);
    CHECK(arbol[0].cliente.nombre == "de acme");
    CHECK(arbol[0].facturas.size() == 2);
}

TEST_CASE("olvidar el tenant sigue lanzando con with", "[relations][tenant]") {
    SqliteDialect dialect;
    TempDb        db;

    CHECK_THROWS_AS(drogon::sync_wait(Query<Cliente>(db.get())
                                          .with<Factura>(&Factura::cliente_id, &Cliente::id)
                                          .get<ClienteConFacturas>()),
                    std::runtime_error);
}

TEST_CASE("sin padres no se consulta a los hijos siquiera", "[relations]") {
    SqliteDialect dialect;
    TempDb        db;

    // Se borra la tabla hija: si `with` la consultara igualmente, esto seria un
    // "no such table". Que no lance es la prueba de que sin claves que buscar
    // no hay segunda consulta -y de paso, de que nunca hay una por padre.
    db.get()->execSqlSync("DROP TABLE posts");

    const auto arbol = drogon::sync_wait(Query<User>(db.get())
                                             .where(&User::name, "=", std::string{"nadie"})
                                             .with<Post>()
                                             .get<UserConPosts>());

    CHECK(arbol.empty());
}
