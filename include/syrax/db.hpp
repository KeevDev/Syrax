#pragma once

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/DbConfig.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/Row.h>
#include <glaze/glaze.hpp>

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace syrax::db {

namespace detail {

template <typename T>
struct IsOptional : std::false_type {};

template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

template <typename T>
inline constexpr bool kIsOptional = IsOptional<T>::value;

}  // namespace detail

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

                if constexpr (detail::kIsOptional<Member>) {
                    member = field.template as<typename Member::value_type>();
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

// Configura la conexion desde variables de entorno, estilo 12-factor.
//
//   DB_ENGINE            postgres (default) | sqlite
//   postgres             DB_HOST DB_PORT DB_NAME DB_USER DB_PASSWORD
//   sqlite               DB_FILE
//
// Gracias a esto el codigo de la aplicacion es identico con cualquier motor:
// lo unico que cambia es el SQL.
inline void configureFromEnv() {
    const auto engine = env("DB_ENGINE", "postgres");

    if (engine == "sqlite" || engine == "sqlite3") {
        drogon::app().addDbClient(drogon::orm::Sqlite3Config{
            .connectionNumber = 1,
            .filename         = env("DB_FILE", "app.db"),
            .name             = "default",
            .timeout          = -1.0,
        });
        return;
    }

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

// SELECT que devuelve varias filas.
//
//   auto users = co_await db::query<User>("SELECT * FROM users WHERE age > $1", 18);
template <typename T, typename... Args>
drogon::Task<std::vector<T>> query(std::string sql, Args... args) {
    const auto result = co_await client()->execSqlCoro(sql, std::move(args)...);

    std::vector<T> rows;
    rows.reserve(result.size());
    for (const auto& row : result) {
        rows.push_back(fromRow<T>(row));
    }
    co_return rows;
}

// SELECT que devuelve una fila o ninguna.
template <typename T, typename... Args>
drogon::Task<std::optional<T>> findOne(std::string sql, Args... args) {
    const auto result = co_await client()->execSqlCoro(sql, std::move(args)...);
    if (result.empty()) co_return std::nullopt;

    co_return fromRow<T>(result.front());
}

// INSERT / UPDATE / DELETE. Devuelve las filas afectadas.
template <typename... Args>
drogon::Task<std::size_t> execute(std::string sql, Args... args) {
    const auto result = co_await client()->execSqlCoro(sql, std::move(args)...);
    co_return result.affectedRows();
}

// INSERT ... RETURNING, que es como se recupera la fila recien creada.
template <typename T, typename... Args>
drogon::Task<T> returning(std::string sql, Args... args) {
    const auto result = co_await client()->execSqlCoro(sql, std::move(args)...);
    co_return fromRow<T>(result.front());
}

}  // namespace syrax::db
