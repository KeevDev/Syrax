#pragma once

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/DbConfig.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/Row.h>
#include <glaze/glaze.hpp>

// Drogon compila el soporte de cada motor solo si encontro su libreria de
// cliente. config.h dice cuales quedaron dentro; sin el archivo asumimos que
// estan todos y que el error lo dara Drogon.
#if defined(__has_include)
#if __has_include(<drogon/config.h>)
#include <drogon/config.h>
#define SYRAX_HAS_POSTGRES USE_POSTGRESQL
#define SYRAX_HAS_MYSQL    USE_MYSQL
#define SYRAX_HAS_SQLITE   USE_SQLITE3
#endif
#endif

#ifndef SYRAX_HAS_POSTGRES
#define SYRAX_HAS_POSTGRES 1
#define SYRAX_HAS_MYSQL    1
#define SYRAX_HAS_SQLITE   1
#endif

#include <syrax/env.hpp>
#include <syrax/traits.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace syrax {

// Que motor hay detras. Es la lista completa de lo que habla el ORM de
// Drogon; no hay un cuarto. El resto del framework consulta el dialecto en
// vez de preguntar por un motor concreto, porque lo que cambia entre ellos es
// la sintaxis, no el motor en si.
enum class Dialect { Postgres, Mysql, Sqlite };

namespace db {

// El entorno se lee igual desde la configuracion de la app que desde aqui.
using syrax::env;
using syrax::envBool;
using syrax::envInt;
using syrax::loadDotEnv;
using syrax::Dialect;

// Mapea una fila de base de datos a un struct plano.
//
// Glaze extrae los nombres de campo en tiempo de compilacion, asi que el
// vinculo "columna name -> campo name" se resuelve solo. Sin reflection esto
// habria que escribirlo a mano por cada modelo: es exactamente el boilerplate
// que drogon_ctl genera en ~500 lineas por tabla.
//
// Una columna NULL deja el campo en su valor por defecto. Usa std::optional
// si necesitas distinguir NULL de "cero".
template <typename T>
T fromRow(const drogon::orm::Row& row) {
    T out{};

    constexpr auto keys  = glz::reflect<T>::keys;
    constexpr auto kSize = glz::reflect<T>::size;

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (
            [&] {
                auto&& member = glz::get_member(out, glz::get<I>(glz::to_tie(out)));
                using Member  = std::remove_cvref_t<decltype(member)>;

                const auto field = row[std::string{keys[I]}];
                if (field.isNull()) return;

                if constexpr (syrax::detail::kIsOptional<Member>) {
                    using Value = typename Member::value_type;
                    if constexpr (std::is_enum_v<Value>) {
                        member = static_cast<Value>(
                            field.template as<std::underlying_type_t<Value>>());
                    } else {
                        member = field.template as<Value>();
                    }
                } else if constexpr (std::is_enum_v<Member>) {
                    // La columna guarda un entero y el struct lo declara con
                    // nombres. Drogon no sabe leer un enum, asi que se lee el
                    // tipo subyacente y se convierte.
                    member = static_cast<Member>(
                        field.template as<std::underlying_type_t<Member>>());
                } else {
                    member = field.template as<Member>();
                }
            }(),
            ...);
    }(std::make_index_sequence<kSize>{});

    return out;
}

// El dialecto activo. El query builder lo necesita: postgres numera los
// parametros ($1, $2) y mysql y sqlite usan '?' posicional. Sale de la
// configuracion, asi que no se puede decidir en compilacion.
inline Dialect& activeDialect() {
    static Dialect dialect = Dialect::Postgres;
    return dialect;
}

inline Dialect dialect() { return activeDialect(); }

// Si este binario habla ese motor. Drogon puede conocer mysql y aun asi no
// traerlo dentro, porque al compilarlo no estaba la libreria del cliente:
// preguntarlo aqui ahorra rastrear un LOG_FATAL a media ejecucion.
inline bool supports(Dialect engine) {
    switch (engine) {
        case Dialect::Postgres: return SYRAX_HAS_POSTGRES;
        case Dialect::Mysql:    return SYRAX_HAS_MYSQL;
        case Dialect::Sqlite:   return SYRAX_HAS_SQLITE;
    }
    return false;
}

// La libreria de cliente que Drogon necesita para hablar ese motor. Sale en
// el error de arriba, que sin esto deja al lector adivinando que instalar.
inline std::string engineLibrary(Dialect engine) {
    switch (engine) {
        case Dialect::Postgres: return "libpq";
        case Dialect::Mysql:    return "libmysqlclient (o mariadb-connector-c)";
        case Dialect::Sqlite:   return "sqlite3";
    }
    return "?";
}

inline std::string engineName(Dialect engine) {
    switch (engine) {
        case Dialect::Postgres: return "postgres";
        case Dialect::Mysql:    return "mysql";
        case Dialect::Sqlite:   return "sqlite";
    }
    return "?";
}

// El nombre del motor tal como se escribe en una configuracion. Acepta los
// alias de cada quien porque es un valor escrito a mano, no una constante.
inline Dialect engineFromName(std::string name) {
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (name == "postgres" || name == "postgresql" || name == "pgsql" || name == "pg") {
        return Dialect::Postgres;
    }
    if (name == "mysql" || name == "mariadb") return Dialect::Mysql;
    if (name == "sqlite" || name == "sqlite3") return Dialect::Sqlite;

    throw std::invalid_argument("syrax: motor '" + name +
                                "' desconocido. Drogon habla postgres, mysql y sqlite.");
}

inline unsigned short defaultPort(Dialect engine) {
    switch (engine) {
        case Dialect::Postgres: return 5432;
        case Dialect::Mysql:    return 3306;
        case Dialect::Sqlite:   return 0;
    }
    return 0;
}

// Los datos de una conexion, sin decidir de donde salen. La aplicacion arma
// esto en su configuracion (de un .env, de un JSON, o a mano) y llama a
// connect(). Los campos que no aplican al motor elegido se ignoran, para que
// una misma estructura describa cualquiera de los tres.
struct Connection {
    std::string    engine      = "postgres";
    std::string    host        = "127.0.0.1";
    unsigned short port        = 0;  // 0 toma el puerto habitual del motor
    std::string    database    = "app";
    std::string    username    = "postgres";
    std::string    password    = "postgres";
    std::string    file        = "app.db";  // sqlite
    std::string    charset     = "";        // mysql
    std::size_t    connections = 4;
    double         timeout     = -1.0;
    bool           fast        = false;
    bool           autoBatch   = false;  // postgres
    std::string    name        = "default";

    std::unordered_map<std::string, std::string> options;  // postgres
};

namespace detail {

// Quien fija el dialecto global cuando hay varias conexiones. La llamada
// "default" manda; si no hay ninguna con ese nombre, vale la primera.
inline void adoptDialect(Dialect engine, const std::string& name) {
    static bool fixed = false;
    if (fixed) return;

    activeDialect() = engine;
    if (name == "default") fixed = true;
}

}  // namespace detail

// Registra una conexion en Drogon. Se puede llamar varias veces con nombres
// distintos: la aplicacion pide cada cliente por su nombre con client("...").
inline void connect(const Connection& conn) {
    const auto engine = engineFromName(conn.engine);

    if (!supports(engine)) {
        throw std::runtime_error("syrax: este Drogon se compilo sin soporte de " +
                                 engineName(engine) + ". Instala " + engineLibrary(engine) +
                                 " y vuelve a compilar (borra build/ para que Drogon se "
                                 "reconfigure), o cambia el motor de la conexion '" +
                                 conn.name + "'.");
    }

    detail::adoptDialect(engine, conn.name);

    const auto port = conn.port != 0 ? conn.port : defaultPort(engine);

    switch (engine) {
        case Dialect::Postgres:
            drogon::app().addDbClient(drogon::orm::PostgresConfig{
                .host             = conn.host,
                .port             = port,
                .databaseName     = conn.database,
                .username         = conn.username,
                .password         = conn.password,
                .connectionNumber = conn.connections,
                .name             = conn.name,
                .isFast           = conn.fast,
                .characterSet     = conn.charset,
                .timeout          = conn.timeout,
                .autoBatch        = conn.autoBatch,
                .connectOptions   = conn.options,
            });
            return;

        case Dialect::Mysql:
            drogon::app().addDbClient(drogon::orm::MysqlConfig{
                .host             = conn.host,
                .port             = port,
                .databaseName     = conn.database,
                .username         = conn.username,
                .password         = conn.password,
                .connectionNumber = conn.connections,
                .name             = conn.name,
                .isFast           = conn.fast,
                .characterSet     = conn.charset,
                .timeout          = conn.timeout,
            });
            return;

        case Dialect::Sqlite:
            // Un archivo no admite concurrencia real: mas conexiones solo
            // reparten esperas sobre el mismo lock.
            drogon::app().addDbClient(drogon::orm::Sqlite3Config{
                .connectionNumber = 1,
                .filename         = conn.file,
                .name             = conn.name,
                .timeout          = conn.timeout,
            });
            return;
    }
}

// La conexion que describe el entorno, estilo 12-factor:
//
//   DB_ENGINE   postgres (default) | mysql | sqlite
//   postgres    DB_HOST DB_PORT DB_NAME DB_USER DB_PASSWORD DB_POOL
//   mysql       DB_HOST DB_PORT DB_NAME DB_USER DB_PASSWORD DB_POOL DB_CHARSET
//   sqlite      DB_FILE
inline Connection envConnection(const std::string& name = "default") {
    Connection conn;
    conn.engine      = env("DB_ENGINE", conn.engine);
    conn.host        = env("DB_HOST", conn.host);
    conn.port        = static_cast<unsigned short>(envInt("DB_PORT", 0));
    conn.database    = env("DB_NAME", conn.database);
    conn.username    = env("DB_USER", conn.username);
    conn.password    = env("DB_PASSWORD", conn.password);
    conn.file        = env("DB_FILE", conn.file);
    conn.charset     = env("DB_CHARSET", conn.charset);
    conn.connections = static_cast<std::size_t>(envInt("DB_POOL", 4));
    conn.name        = name;
    return conn;
}

// El atajo de siempre: leer el .env y conectar. Un proyecto que quiera
// decidir mas (varias conexiones, valores que no vienen del entorno) arma la
// Connection el mismo y llama a connect().
inline void configureFromEnv() {
    loadDotEnv();
    connect(envConnection());
}

inline drogon::orm::DbClientPtr client(const std::string& name = "default") {
    return drogon::app().getDbClient(name);
}

// Las cuatro operaciones van contra un cliente cualquiera. Una Transaction
// de Drogon ES un DbClient, asi que lo mismo sirve dentro y fuera de una
// transaccion sin duplicar nada.
namespace detail {

template <typename T, typename... Args>
drogon::Task<std::vector<T>> queryOn(drogon::orm::DbClientPtr on, std::string sql, Args... args) {
    const auto result = co_await on->execSqlCoro(sql, std::move(args)...);

    std::vector<T> rows;
    rows.reserve(result.size());
    for (const auto& row : result) {
        rows.push_back(fromRow<T>(row));
    }
    co_return rows;
}

template <typename T, typename... Args>
drogon::Task<std::optional<T>> findOneOn(drogon::orm::DbClientPtr on, std::string sql,
                                         Args... args) {
    const auto result = co_await on->execSqlCoro(sql, std::move(args)...);
    if (result.empty()) co_return std::nullopt;

    co_return fromRow<T>(result.front());
}

template <typename... Args>
drogon::Task<std::size_t> executeOn(drogon::orm::DbClientPtr on, std::string sql, Args... args) {
    const auto result = co_await on->execSqlCoro(sql, std::move(args)...);
    co_return result.affectedRows();
}

template <typename T, typename... Args>
drogon::Task<T> returningOn(drogon::orm::DbClientPtr on, std::string sql, Args... args) {
    const auto result = co_await on->execSqlCoro(sql, std::move(args)...);
    co_return fromRow<T>(result.front());
}

}  // namespace detail

// SELECT que devuelve varias filas.
//
//   auto users = co_await db::query<User>("SELECT * FROM users WHERE age > $1", 18);
template <typename T, typename... Args>
drogon::Task<std::vector<T>> query(std::string sql, Args... args) {
    co_return co_await detail::queryOn<T>(client(), std::move(sql), std::move(args)...);
}

// SELECT que devuelve una fila o ninguna.
template <typename T, typename... Args>
drogon::Task<std::optional<T>> findOne(std::string sql, Args... args) {
    co_return co_await detail::findOneOn<T>(client(), std::move(sql), std::move(args)...);
}

// INSERT / UPDATE / DELETE. Devuelve las filas afectadas.
template <typename... Args>
drogon::Task<std::size_t> execute(std::string sql, Args... args) {
    co_return co_await detail::executeOn(client(), std::move(sql), std::move(args)...);
}

// INSERT ... RETURNING, que es como se recupera la fila recien creada.
template <typename T, typename... Args>
drogon::Task<T> returning(std::string sql, Args... args) {
    co_return co_await detail::returningOn<T>(client(), std::move(sql), std::move(args)...);
}

// Un unico valor: count(*), max(x), exists(...). Sin esto hay que declarar un
// struct de un campo para cada agregado, y encima no puede ir dentro de la
// funcion porque Glaze no refleja tipos locales.
//
//   const auto total = co_await db::scalar<std::int64_t>("SELECT count(*) FROM users");
template <typename T, typename... Args>
drogon::Task<T> scalar(std::string sql, Args... args) {
    const auto result = co_await client()->execSqlCoro(sql, std::move(args)...);
    if (result.empty() || result.front().size() == 0) co_return T{};

    const auto field = result.front()[0];
    if (field.isNull()) co_return T{};

    co_return field.template as<T>();
}

// ------------------------------------------------------------ transaccion

// Las mismas cuatro operaciones, pero dentro de una transaccion.
//
// Sin esto, un "comprobar y luego insertar" es una condicion de carrera: dos
// peticiones simultaneas pueden ver el email libre las dos y crear el usuario
// las dos. Con transaccion y la restriccion UNIQUE en la tabla, una gana y la
// otra falla limpio.
// Drogon confirma la transaccion cuando se destruye, en otro hilo, y el
// resultado solo se sabe por callback: sin esperarlo, un COMMIT que falla
// —un deadlock, un serialization failure, un constraint diferido— llega
// despues de que el handler ya respondio 201.
struct CommitFailed : std::runtime_error {
    CommitFailed() : std::runtime_error{"syrax: la transaccion no se pudo confirmar"} {}
};

namespace detail {

struct CommitState {
    std::mutex              mutex;
    bool                    finished = false;
    bool                    ok       = false;
    std::coroutine_handle<> waiter;
};

// El callback llega desde el hilo de la base: la corrutina se reanuda alli,
// igual que ya pasa con el resultado de cualquier consulta.
inline void settleCommit(const std::shared_ptr<CommitState>& state, bool ok) {
    std::coroutine_handle<> waiter;
    {
        const std::lock_guard lock{state->mutex};
        state->ok       = ok;
        state->finished = true;
        waiter          = state->waiter;
        state->waiter   = {};
    }
    if (waiter) waiter.resume();
}

struct CommitAwaiter {
    std::shared_ptr<CommitState> state;

    bool await_ready() const {
        const std::lock_guard lock{state->mutex};
        return state->finished;
    }

    // Puede haber llegado entre el await_ready y esto: si ya esta, no se
    // suspende, porque nadie va a volver a reanudarla.
    bool await_suspend(std::coroutine_handle<> handle) const {
        const std::lock_guard lock{state->mutex};
        if (state->finished) return false;

        state->waiter = handle;
        return true;
    }

    bool await_resume() const {
        const std::lock_guard lock{state->mutex};
        return state->ok;
    }
};

inline drogon::Task<void> awaitCommit(std::shared_ptr<CommitState> state) {
    if (!co_await CommitAwaiter{std::move(state)}) throw CommitFailed{};
}

template <typename T>
struct TaskValue;

template <typename T>
struct TaskValue<drogon::Task<T>> {
    using type = T;
};

}  // namespace detail

class Tx {
public:
    explicit Tx(std::shared_ptr<drogon::orm::Transaction> transaction,
                std::shared_ptr<bool> aborted = nullptr)
        : transaction_{std::move(transaction)}, aborted_{std::move(aborted)} {}

    template <typename T, typename... Args>
    drogon::Task<std::vector<T>> query(std::string sql, Args... args) const {
        co_return co_await detail::queryOn<T>(transaction_, std::move(sql), std::move(args)...);
    }

    template <typename T, typename... Args>
    drogon::Task<std::optional<T>> findOne(std::string sql, Args... args) const {
        co_return co_await detail::findOneOn<T>(transaction_, std::move(sql), std::move(args)...);
    }

    template <typename... Args>
    drogon::Task<std::size_t> execute(std::string sql, Args... args) const {
        co_return co_await detail::executeOn(transaction_, std::move(sql), std::move(args)...);
    }

    template <typename T, typename... Args>
    drogon::Task<T> returning(std::string sql, Args... args) const {
        co_return co_await detail::returningOn<T>(transaction_, std::move(sql),
                                                  std::move(args)...);
    }

    template <typename T, typename... Args>
    drogon::Task<T> scalar(std::string sql, Args... args) const {
        const auto result = co_await transaction_->execSqlCoro(sql, std::move(args)...);
        if (result.empty() || result.front().size() == 0) co_return T{};

        const auto field = result.front()[0];
        if (field.isNull()) co_return T{};

        co_return field.template as<T>();
    }

    // Deshace lo hecho hasta aqui sin lanzar. Util cuando abortar es una
    // decision de negocio y no un error.
    void rollback() const {
        if (aborted_) *aborted_ = true;
        transaction_->rollback();
    }

    // El Transaction de Drogon ES un DbClient —hereda de el—, asi que todo lo
    // que acepta un cliente vale aqui dentro sin una ruta aparte:
    //
    //   co_await Query<User>(tx.client()).where(...).exists();
    //   co_await save(user, tx.client());
    //
    // Sin esto, lo unico que quedaba fuera de la transaccion era justamente el
    // query builder, que es donde mas falta hace.
    drogon::orm::DbClientPtr client() const { return transaction_; }

private:
    std::shared_ptr<drogon::orm::Transaction> transaction_;
    std::shared_ptr<bool>                     aborted_;
};

// Corre el cuerpo dentro de una transaccion.
//
//   const auto user = co_await db::transaction([](const db::Tx& tx)
//                                              -> drogon::Task<models::User> {
//       co_await tx.execute("...");
//       co_return co_await tx.returning<models::User>("...");
//   });
//
// Drogon confirma la transaccion cuando se destruye el objeto. Si el cuerpo
// lanza, aqui se deshace explicitamente antes de propagar: dejar que el
// destructor confirme a medias seria peor que el error original.
template <typename F>
auto transactionOn(drogon::orm::DbClientPtr on, F body)
    -> decltype(body(std::declval<const Tx&>())) {
    using Value = typename detail::TaskValue<
        std::remove_cvref_t<decltype(body(std::declval<const Tx&>()))>>::type;

    auto handle  = co_await on->newTransactionCoro();
    auto aborted = std::make_shared<bool>(false);
    auto state   = std::make_shared<detail::CommitState>();

    handle->setCommitCallback([state](bool ok) { detail::settleCommit(state, ok); });

    // El COMMIT lo dispara el destructor de la transaccion, asi que hay que
    // soltar TODAS las referencias —la de Tx tambien, de ahi el bloque— antes
    // de poder esperarlo.
    //
    // Tras un rollback no se espera nada: Drogon no llama al callback si la
    // transaccion ya se deshizo, y quedarse esperandolo colgaria la corrutina.
    if constexpr (std::is_void_v<Value>) {
        try {
            const Tx tx{handle, aborted};
            co_await body(tx);
        } catch (...) {
            handle->rollback();
            handle.reset();
            throw;
        }

        handle.reset();
        if (!*aborted) co_await detail::awaitCommit(std::move(state));
        co_return;
    } else {
        // optional porque el valor no tiene por que ser construible por
        // defecto, y tiene que sobrevivir hasta despues del COMMIT.
        std::optional<Value> value;

        try {
            const Tx tx{handle, aborted};
            value.emplace(co_await body(tx));
        } catch (...) {
            handle->rollback();
            handle.reset();
            throw;
        }

        handle.reset();
        if (!*aborted) co_await detail::awaitCommit(std::move(state));
        co_return std::move(*value);
    }
}

template <typename F>
auto transaction(F body) -> decltype(body(std::declval<const Tx&>())) {
    co_return co_await transactionOn(client(), std::move(body));
}

}  // namespace db
}  // namespace syrax
