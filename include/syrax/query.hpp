#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/orm/SqlBinder.h>
#include <glaze/glaze.hpp>

#include <syrax/db.hpp>
#include <syrax/traits.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace syrax {

enum class Dir { Asc, Desc };

namespace detail {

// Cada parametro se guarda como una funcion que sabe enlazarse a si misma.
// Es lo que permite acumular tipos distintos en un vector: un `where` mezcla
// enteros, textos y nulos, y el vector homogeneo que acepta Drogon no sirve.
using ParamBinder = std::function<void(drogon::orm::internal::SqlBinder&)>;

// Resuelve el nombre de la columna a partir del puntero a miembro, comparando
// direcciones contra los campos que refleja Glaze. Es lo que hace que
// &User::emial no compile en vez de fallar en produccion.
template <typename C, typename M>
std::string columnOf(M C::*member) {
    C           probe{};
    const void* target = static_cast<const void*>(std::addressof(probe.*member));

    constexpr auto keys  = glz::reflect<C>::keys;
    constexpr auto kSize = glz::reflect<C>::size;

    std::string name;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            auto&& candidate = glz::get_member(probe, glz::get<I>(glz::to_tie(probe)));
            if (static_cast<const void*>(std::addressof(candidate)) == target) {
                name = std::string{keys[I]};
            }
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    return name;
}

template <typename T>
concept HasTable = requires { T::table; };

template <typename T>
concept HasPrimaryKey = requires { T::primaryKey; };

template <typename T>
std::string tableOf() {
    static_assert(HasTable<T>,
                  "syrax: el modelo necesita `static constexpr auto table = \"...\";` "
                  "para usar Query<T>. Con SQL a mano (db::query) no hace falta.");
    return std::string{T::table};
}

template <typename T>
std::string primaryKeyOf() {
    if constexpr (HasPrimaryKey<T>) return std::string{T::primaryKey};
    else                            return "id";
}

// Postgres numera los parametros, sqlite usa '?' posicional. El builder no
// puede saberlo en compilacion porque el motor sale del .env.
inline std::string placeholder(std::size_t index) {
    return db::dialect() == db::Dialect::Postgres ? "$" + std::to_string(index) : "?";
}

// Ejecuta un SQL con parametros acumulados. Replica lo que hace execSqlCoro
// de Drogon, que no acepta una lista heterogenea.
inline drogon::Task<drogon::orm::Result> run(drogon::orm::DbClientPtr on, std::string sql,
                                             std::vector<ParamBinder> params) {
    auto binder = *on << std::move(sql);
    for (const auto& bind : params) bind(binder);

    co_return co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
}

// La lista de columnas del struct, en orden de declaracion.
template <typename T>
std::string columnList(bool skipPrimaryKey = false) {
    const auto primary = primaryKeyOf<T>();

    constexpr auto keys  = glz::reflect<T>::keys;
    constexpr auto kSize = glz::reflect<T>::size;

    std::string list;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            const std::string key{keys[I]};
            if (skipPrimaryKey && key == primary) return;

            if (!list.empty()) list += ", ";
            list += "\"" + key + "\"";
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    return list;
}

}  // namespace detail

// ---------------------------------------------------------------- Query

// Construye SELECT / DELETE / COUNT sobre un struct plano.
//
//   auto adultos = co_await Query<User>()
//       .where(&User::age, ">", 18)
//       .orderBy(&User::name)
//       .limit(10)
//       .get();
//
// Deliberadamente NO hay joins ni relaciones: ahi un query builder deja de
// tener fondo. Para eso, SQL a mano con db::query, que sigue disponible.
template <typename T>
class Query {
public:
    Query() = default;

    explicit Query(drogon::orm::DbClientPtr on) : client_{std::move(on)} {}

    // --- filtros ---

    template <typename C, typename M, typename V>
    Query& where(M C::*member, std::string op, V value) {
        return condition("AND", detail::columnOf(member), std::move(op), std::move(value));
    }

    template <typename C, typename M, typename V>
    Query& orWhere(M C::*member, std::string op, V value) {
        return condition("OR", detail::columnOf(member), std::move(op), std::move(value));
    }

    template <typename C, typename M, typename V>
    Query& whereIn(M C::*member, std::initializer_list<V> values) {
        return inList("AND", detail::columnOf(member), std::vector<V>{values});
    }

    template <typename C, typename M, typename V>
    Query& whereIn(M C::*member, std::vector<V> values) {
        return inList("AND", detail::columnOf(member), std::move(values));
    }

    template <typename C, typename M>
    Query& whereNull(M C::*member) {
        return bare("AND", "\"" + detail::columnOf(member) + "\" IS NULL");
    }

    template <typename C, typename M>
    Query& whereNotNull(M C::*member) {
        return bare("AND", "\"" + detail::columnOf(member) + "\" IS NOT NULL");
    }

    // --- orden y paginacion ---

    template <typename C, typename M>
    Query& orderBy(M C::*member, Dir direction = Dir::Asc) {
        if (!order_.empty()) order_ += ", ";
        order_ += "\"" + detail::columnOf(member) + "\"";
        order_ += (direction == Dir::Desc) ? " DESC" : " ASC";
        return *this;
    }

    Query& limit(std::size_t count)  { limit_  = count;  return *this; }
    Query& offset(std::size_t count) { offset_ = count;  return *this; }

    // --- ejecucion ---

    drogon::Task<std::vector<T>> get() const {
        const auto result = co_await detail::run(target(), select(), params_);

        std::vector<T> rows;
        rows.reserve(result.size());
        for (const auto& row : result) rows.push_back(db::fromRow<T>(row));
        co_return rows;
    }

    drogon::Task<std::optional<T>> first() const {
        Query copy{*this};
        copy.limit_ = 1;

        const auto rows = co_await copy.get();
        if (rows.empty()) co_return std::nullopt;
        co_return rows.front();
    }

    drogon::Task<std::int64_t> count() const {
        const std::string sql = "SELECT count(*) FROM \"" + detail::tableOf<T>() + "\"" + whereClause();

        const auto result = co_await detail::run(target(), sql, params_);
        if (result.empty() || result.front().size() == 0) co_return 0;

        co_return result.front()[0].template as<std::int64_t>();
    }

    drogon::Task<bool> exists() const { co_return (co_await count()) > 0; }

    // Borra las filas que cumplen los filtros. Devuelve cuantas.
    drogon::Task<std::size_t> del() const {
        const std::string sql = "DELETE FROM \"" + detail::tableOf<T>() + "\"" + whereClause();

        const auto result = co_await detail::run(target(), sql, params_);
        co_return result.affectedRows();
    }

    // El SQL que se generaria, sin ejecutarlo. Para depurar y para tests.
    std::string toSql() const { return select(); }

private:
    drogon::orm::DbClientPtr target() const { return client_ ? client_ : db::client(); }

    template <typename V>
    Query& condition(const char* join, std::string column, std::string op, V value) {
        push(join, "\"" + column + "\" " + op + " " + detail::placeholder(params_.size() + 1));
        params_.push_back([value = std::move(value)](drogon::orm::internal::SqlBinder& b) {
            b << value;
        });
        return *this;
    }

    template <typename V>
    Query& inList(const char* join, std::string column, std::vector<V> values) {
        if (values.empty()) {
            // Un IN vacio no es SQL valido, y "no coincide con nada" es la
            // lectura correcta: mejor eso que devolver la tabla entera.
            return bare(join, "1 = 0");
        }

        std::string slots;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) slots += ", ";
            slots += detail::placeholder(params_.size() + i + 1);
        }
        push(join, "\"" + column + "\" IN (" + slots + ")");

        for (auto& value : values) {
            params_.push_back([value = std::move(value)](drogon::orm::internal::SqlBinder& b) {
                b << value;
            });
        }
        return *this;
    }

    Query& bare(const char* join, std::string expression) {
        push(join, std::move(expression));
        return *this;
    }

    void push(const char* join, std::string expression) {
        if (!where_.empty()) where_ += std::string{" "} + join + " ";
        where_ += std::move(expression);
    }

    std::string whereClause() const {
        return where_.empty() ? std::string{} : " WHERE " + where_;
    }

    std::string select() const {
        std::string sql = "SELECT " + detail::columnList<T>() + " FROM \"" +
                          detail::tableOf<T>() + "\"" + whereClause();

        if (!order_.empty())      sql += " ORDER BY " + order_;
        if (limit_.has_value())   sql += " LIMIT " + std::to_string(*limit_);
        if (offset_.has_value())  sql += " OFFSET " + std::to_string(*offset_);
        return sql;
    }

    drogon::orm::DbClientPtr         client_;
    std::string                      where_;
    std::string                      order_;
    std::optional<std::size_t>       limit_;
    std::optional<std::size_t>       offset_;
    std::vector<detail::ParamBinder> params_;
};

// --------------------------------------------------------- persistencia

// Solo existen para modelos que declaran `table`. La restriccion no es
// decorativa: sin ella, cualquier save(loQueSea) del usuario se resolvia a
// esta plantilla y fallaba por dentro en vez de elegir la suya.
//
// Guarda el objeto: INSERT si la clave primaria viene a cero, UPDATE si no.
// Tras un INSERT, el objeto queda con el id que asigno la base.
//
//   models::User u{.name = "Ada", .email = "ada@x.com", .age = 36};
//   co_await save(u);      // INSERT, y u.id queda relleno
//   u.age = 37;
//   co_await save(u);      // UPDATE
//
// Los campos se sacan del struct por reflection: agregar una columna al
// modelo no obliga a tocar esto.
template <detail::HasTable T>
drogon::Task<void> save(T& value, drogon::orm::DbClientPtr on = nullptr) {
    const auto table   = detail::tableOf<T>();
    const auto primary = detail::primaryKeyOf<T>();
    auto       target  = on ? on : db::client();

    constexpr auto keys  = glz::reflect<T>::keys;
    constexpr auto kSize = glz::reflect<T>::size;

    std::vector<detail::ParamBinder> params;
    std::string                      columns, slots, assignments;
    bool                             isInsert = true;

    // Se mira la clave primaria antes de decidir: cero (o vacia) significa
    // "todavia no existe en la base".
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            if (std::string{keys[I]} != primary) return;

            auto&& field = glz::get_member(value, glz::get<I>(glz::to_tie(value)));
            using Field  = std::remove_cvref_t<decltype(field)>;
            if constexpr (std::is_arithmetic_v<Field>) isInsert = (field == Field{});
            else                                       isInsert = (field == Field{});
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            const std::string key{keys[I]};
            if (key == primary) return;

            auto&& field = glz::get_member(value, glz::get<I>(glz::to_tie(value)));

            if (!columns.empty()) { columns += ", "; slots += ", "; assignments += ", "; }

            const auto slot = detail::placeholder(params.size() + 1);
            columns += "\"" + key + "\"";
            slots   += slot;
            assignments += "\"" + key + "\" = " + slot;

            params.push_back([field](drogon::orm::internal::SqlBinder& b) { b << field; });
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    if (isInsert) {
        const std::string sql = "INSERT INTO \"" + table + "\" (" + columns + ") VALUES (" +
                                slots + ") RETURNING " + detail::columnList<T>();

        const auto result = co_await detail::run(target, sql, std::move(params));
        if (!result.empty()) value = db::fromRow<T>(result.front());
        co_return;
    }

    // El id va al final, despues de los parametros de las columnas.
    const auto slot = detail::placeholder(params.size() + 1);

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            if (std::string{keys[I]} != primary) return;
            auto&& field = glz::get_member(value, glz::get<I>(glz::to_tie(value)));
            params.push_back([field](drogon::orm::internal::SqlBinder& b) { b << field; });
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    const std::string sql = "UPDATE \"" + table + "\" SET " + assignments + " WHERE \"" +
                            primary + "\" = " + slot;

    co_await detail::run(target, sql, std::move(params));
    co_return;
}

// Borra el objeto por su clave primaria. Devuelve si habia algo que borrar.
template <detail::HasTable T>
drogon::Task<bool> remove(const T& value, drogon::orm::DbClientPtr on = nullptr) {
    const auto table   = detail::tableOf<T>();
    const auto primary = detail::primaryKeyOf<T>();
    auto       target  = on ? on : db::client();

    constexpr auto keys  = glz::reflect<T>::keys;
    constexpr auto kSize = glz::reflect<T>::size;

    std::vector<detail::ParamBinder> params;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            if (std::string{keys[I]} != primary) return;

            T    copy  = value;
            auto&& field = glz::get_member(copy, glz::get<I>(glz::to_tie(copy)));
            params.push_back([field](drogon::orm::internal::SqlBinder& b) { b << field; });
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    const std::string sql = "DELETE FROM \"" + table + "\" WHERE \"" + primary + "\" = " +
                            detail::placeholder(1);

    const auto result = co_await detail::run(target, sql, std::move(params));
    co_return result.affectedRows() > 0;
}

}  // namespace syrax
