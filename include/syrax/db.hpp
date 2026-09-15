#pragma once

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/DbConfig.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/Row.h>
#include <glaze/glaze.hpp>

#include <syrax/traits.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace syrax::db {

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

inline std::string env(const char* key, std::string fallback) {
    const char* value = std::getenv(key);
    return (value && *value) ? std::string{value} : std::move(fallback);
}

// Carga un archivo .env al entorno del proceso. Las variables que ya existen
// ganan, para que el entorno real siempre pueda sobreescribir al archivo.
//
// No es un parser completo de dotenv: KEY=VALUE por linea, ignorando
// comentarios y comillas envolventes. Alcanza para credenciales.
inline void loadDotEnv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const auto key   = line.substr(0, eq);
        auto       value = line.substr(eq + 1);

        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }

        ::setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
    }
}

// Que motor se configuro. El query builder lo necesita: postgres numera los
// parametros ($1, $2) y sqlite usa '?' posicional, y el motor sale del .env,
// asi que no se puede decidir en compilacion.
enum class Dialect { Postgres, Sqlite };

inline Dialect& activeDialect() {
    static Dialect dialect = Dialect::Postgres;
    return dialect;
}

inline Dialect dialect() { return activeDialect(); }

// Configura la conexion desde variables de entorno, estilo 12-factor.
//
//   DB_ENGINE            postgres (default) | sqlite
//   postgres             DB_HOST DB_PORT DB_NAME DB_USER DB_PASSWORD
//   sqlite               DB_FILE
//
// Gracias a esto el codigo de la aplicacion es identico con cualquier motor:
// lo unico que cambia es el SQL.
inline void configureFromEnv() {
    loadDotEnv();

    const auto engine = env("DB_ENGINE", "postgres");

    if (engine == "sqlite" || engine == "sqlite3") {
        activeDialect() = Dialect::Sqlite;
        drogon::app().addDbClient(drogon::orm::Sqlite3Config{
            .connectionNumber = 1,
            .filename         = env("DB_FILE", "app.db"),
            .name             = "default",
            .timeout          = -1.0,
        });
        return;
    }

    activeDialect() = Dialect::Postgres;
    drogon::app().addDbClient(drogon::orm::PostgresConfig{
        .host             = env("DB_HOST", "127.0.0.1"),
        .port             = static_cast<unsigned short>(std::stoi(env("DB_PORT", "5432"))),
        .databaseName     = env("DB_NAME", "app"),
        .username         = env("DB_USER", "postgres"),
        .password         = env("DB_PASSWORD", "postgres"),
        .connectionNumber = 4,
        .name             = "default",
        .isFast           = false,
        .characterSet     = "",
        .timeout          = -1.0,
        .autoBatch        = false,
        .connectOptions   = {},
    });
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

}  // namespace syrax::db
