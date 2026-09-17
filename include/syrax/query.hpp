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
#include <type_traits>
#include <utility>
#include <vector>

namespace syrax {

enum class Dir { Asc, Desc };

namespace detail {

// Cada parametro se guarda como una funcion que sabe enlazarse a si misma.
// Es lo que permite acumular tipos distintos en un vector: un `where` mezcla
// enteros, textos y nulos, y el vector homogeneo que acepta Drogon no sirve.
using ParamBinder = std::function<void(drogon::orm::internal::SqlBinder&)>;

// Drogon no sabe enlazar un enum: su SqlBinder corta con un static_assert que
// ni siquiera lo menciona. La columna guarda el entero de todas formas, asi
// que se enlaza el tipo subyacente y el enum sigue siendo el nombre.
template <typename V>
auto bindable(V value) {
    if constexpr (std::is_enum_v<V>) return static_cast<std::underlying_type_t<V>>(value);
    else                             return value;
}

template <typename V>
ParamBinder binder(V value) {
    return [value = bindable(std::move(value))](drogon::orm::internal::SqlBinder& b) {
        b << value;
    };
}

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

// Las dos conveniencias van declaradas en el modelo, no deducidas de que
// existan las columnas. Deducirlas seria mas corto de escribir y peor de vivir:
// alguien agrega un `deleted_at` para su propia contabilidad y de pronto
// DELETE deja de borrar, sin que nada en su codigo lo diga. Asi se lee en el
// modelo, que es donde se va a buscar.
//
//   static constexpr auto timestamps  = true;   // created_at / updated_at
//   static constexpr auto softDeletes = true;   // deleted_at
template <typename T>
concept Timestamps = requires { T::timestamps; };

template <typename T>
concept SoftDeletes = requires { T::softDeletes; };

// Multi-tenancy por fila. Solo el modelo de fila: el de esquema y el de base
// por tenant son decisiones que no se pueden desandar, y un framework no
// deberia elegirlas por ti.
//
//   static constexpr auto tenant = true;   // usa la columna tenant_id
template <typename T>
concept Tenant = requires { T::tenant; };

// Los nombres son fijos. Hacerlos configurables anade tres puntos de
// configuracion para ahorrar un rename en la migracion, y estos tres nombres
// son los que usa todo el mundo.
inline constexpr std::string_view kCreatedAt = "created_at";
inline constexpr std::string_view kUpdatedAt = "updated_at";
inline constexpr std::string_view kDeletedAt = "deleted_at";
inline constexpr std::string_view kTenantId  = "tenant_id";

// La hora la pone la BASE, no el proceso. Con varias instancias, los relojes
// de las maquinas difieren y dos filas creadas en orden pueden quedar con
// timestamps cruzados; el servidor de base es uno solo.
inline constexpr std::string_view kNow = "CURRENT_TIMESTAMP";

// Si una columna la gestiona el framework, para no escribirla tambien desde el
// struct: si el modelo la declara como campo se lee, pero no se escribe.
template <typename T>
bool managedColumn(const std::string& key) {
    if constexpr (Timestamps<T>) {
        if (key == kCreatedAt || key == kUpdatedAt) return true;
    }
    if constexpr (SoftDeletes<T>) {
        if (key == kDeletedAt) return true;
    }
    return false;
}

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

// El sobre de una pagina. Cada API lo reinventa —unas con `meta`, otras con
// `pagination`, otras con `_links`— y el cliente acaba escribiendo un adaptador
// por servicio. Con uno solo, la forma la conoce el que consume y la documenta
// OpenAPI sin que nadie la escriba a mano.
//
// Se queda en lo que hace falta para pintar un paginador: los datos, cuantos
// hay en total, en que pagina estamos y cuantas hay. `pages` y `hasMore` salen
// de los otros tres, pero calcularlos en el cliente es la clase de division
// entera que alguien redondea mal.
template <typename T>
struct Page {
    std::vector<T> data;
    std::int64_t   total   = 0;
    std::int64_t   page    = 1;
    std::int64_t   perPage = 0;
    std::int64_t   pages   = 0;
    bool           hasMore = false;
};

// Construye SELECT / UPDATE / DELETE / COUNT sobre un struct plano.
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

    // Agrupa filtros entre parentesis.
    //
    //   .where(&Invoice::status, "=", Status::Pending)
    //   .whereGroup([](auto& g) {
    //       g.where(&Invoice::total, ">", 100).orWhere(&Invoice::vip, "=", true);
    //   })
    //   // WHERE "status" = $1 AND ("total" > $2 OR "vip" = $3)
    //
    // Sin esto, mezclar where y orWhere deja mandando la precedencia de SQL
    // —AND aprieta mas que OR—, que no es como se lee la cadena de llamadas.
    template <typename F>
    Query& whereGroup(F&& build) { return group("AND", std::forward<F>(build)); }

    template <typename F>
    Query& orWhereGroup(F&& build) { return group("OR", std::forward<F>(build)); }

    // --- orden y paginacion ---

    template <typename C, typename M>
    Query& orderBy(M C::*member, Dir direction = Dir::Asc) {
        if (!order_.empty()) order_ += ", ";
        order_ += "\"" + detail::columnOf(member) + "\"";
        order_ += (direction == Dir::Desc) ? " DESC" : " ASC";
        return *this;
    }

    // Ata la consulta a un tenant. Obligatorio en un modelo que lo declara:
    // ver el guardia de abajo.
    Query& forTenant(std::string id) {
        static_assert(detail::Tenant<T>,
                      "syrax: forTenant() necesita `static constexpr auto tenant = true;` en el "
                      "modelo. Sin eso no hay columna que filtrar.");
        tenant_ = std::move(id);
        return *this;
    }

    // Incluye tambien las filas borradas. Solo tiene sentido en un modelo con
    // softDeletes; en los demas no hay nada que incluir y no compila, que es
    // mejor que pasar desapercibido.
    Query& withTrashed() {
        static_assert(detail::SoftDeletes<T>,
                      "syrax: withTrashed() necesita `static constexpr auto softDeletes = true;` "
                      "en el modelo. Sin eso no hay filas borradas que incluir.");
        trashed_ = Trashed::With;
        return *this;
    }

    // Solo las borradas: la papelera.
    Query& onlyTrashed() {
        static_assert(detail::SoftDeletes<T>,
                      "syrax: onlyTrashed() necesita `static constexpr auto softDeletes = true;` "
                      "en el modelo.");
        trashed_ = Trashed::Only;
        return *this;
    }

    Query& limit(std::size_t count)  { limit_  = count;  return *this; }
    Query& offset(std::size_t count) { offset_ = count;  return *this; }

    // --- que columnas cambia un update() ---

    template <typename C, typename M, typename V>
    Query& set(M C::*member, V value) {
        sets_.push_back({detail::columnOf(member), detail::binder(std::move(value))});
        return *this;
    }

    // --- ejecucion ---

    drogon::Task<std::vector<T>> get() const {
        requireTenant();
        const auto result = co_await detail::run(target(), select(), bound());

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
        requireTenant();

        const std::string sql = "SELECT count(*) FROM \"" + detail::tableOf<T>() + "\"" + whereClause();

        const auto result = co_await detail::run(target(), sql, bound());
        if (result.empty() || result.front().size() == 0) co_return 0;

        co_return result.front()[0].template as<std::int64_t>();
    }

    drogon::Task<bool> exists() const { co_return (co_await count()) > 0; }

    // Una pagina con su total. Son DOS consultas —el count y el select— y no
    // hay forma de hacerlo en una sin window functions, que sqlite y mysql
    // viejos no tienen. Se dice aqui porque en una tabla grande el count(*) es
    // el caro de los dos, y quien lo sepa puede decidir no pedirlo.
    //
    //   co_return co_await syrax::Query<models::User>()
    //                 .orderBy(&models::User::id)
    //                 .paginate(page, perPage);
    //
    // Sin orderBy el orden lo decide el motor y puede cambiar entre paginas:
    // la fila que estaba en la 1 aparece otra vez en la 2. Va sin imponerlo
    // porque la clave de orden es del que consulta, no del framework.
    drogon::Task<Page<T>> paginate(std::int64_t page = 1, std::int64_t perPage = 15) const {
        // Una pagina 0 o negativa es siempre un parametro mal leido, y devolver
        // un offset negativo seria un error de SQL en vez de una respuesta.
        if (page < 1) page = 1;
        if (perPage < 1) perPage = 1;

        Page<T> out{.page = page, .perPage = perPage};
        out.total = co_await count();
        out.pages = (out.total + perPage - 1) / perPage;

        // Una pagina mas alla del final devuelve vacio, no un error: es lo que
        // pasa cuando alguien borra filas mientras otro pagina.
        if (out.total > 0 && (page - 1) * perPage < out.total) {
            Query copy{*this};
            copy.limit_  = static_cast<std::size_t>(perPage);
            copy.offset_ = static_cast<std::size_t>((page - 1) * perPage);

            out.data = co_await copy.get();
        }

        out.hasMore = page < out.pages;
        co_return out;
    }

    // Borra las filas que cumplen los filtros. Devuelve cuantas.
    //
    // Con softDeletes no borra: marca `deleted_at`, y a partir de ahi esas
    // filas dejan de aparecer en las consultas normales. La fila sigue en la
    // tabla, que es el punto: un borrado de verdad no se deshace, y la mitad
    // de las veces que alguien borra algo en produccion querria deshacerlo.
    drogon::Task<std::size_t> del() const {
        requireTenant();

        if constexpr (detail::SoftDeletes<T>) {
            const std::string sql = "UPDATE \"" + detail::tableOf<T>() + "\" SET \"" +
                                    std::string{detail::kDeletedAt} + "\" = " +
                                    std::string{detail::kNow} + whereClause();

            const auto result = co_await detail::run(target(), sql, bound());
            co_return result.affectedRows();
        } else {
            const std::string sql =
                "DELETE FROM \"" + detail::tableOf<T>() + "\"" + whereClause();

            const auto result = co_await detail::run(target(), sql, bound());
            co_return result.affectedRows();
        }
    }

    // Borra de verdad, aunque el modelo tenga softDeletes. Existe porque
    // "borrar para siempre" es una operacion legitima -el RGPD la exige- y
    // esconderla obligaria a escribir el DELETE a mano, sin los filtros.
    drogon::Task<std::size_t> forceDelete() const {
        requireTenant();

        const std::string sql = "DELETE FROM \"" + detail::tableOf<T>() + "\"" + whereClause();

        const auto result = co_await detail::run(target(), sql, bound());
        co_return result.affectedRows();
    }

    // Deshace el borrado. Solo toca filas borradas, sin que haya que pedir
    // onlyTrashed(): restaurar una fila viva no significa nada.
    drogon::Task<std::size_t> restore() const {
        static_assert(detail::SoftDeletes<T>,
                      "syrax: restore() necesita `static constexpr auto softDeletes = true;` "
                      "en el modelo.");

        requireTenant();

        Query copy{*this};
        copy.trashed_ = Trashed::Only;

        const std::string sql = "UPDATE \"" + detail::tableOf<T>() + "\" SET \"" +
                                std::string{detail::kDeletedAt} + "\" = NULL" +
                                copy.whereClause();

        const auto result = co_await detail::run(target(), sql, bound());
        co_return result.affectedRows();
    }

    // Aplica los set() a todas las filas que cumplen los filtros, en una sola
    // consulta. Devuelve cuantas cambiaron. Sin filtros toca la tabla entera,
    // igual que el SQL a mano.
    drogon::Task<std::size_t> update() const {
        requireTenant();
        if (sets_.empty()) co_return 0;

        auto plan = updatePlan();

        const auto result = co_await detail::run(target(), std::move(plan.first),
                                                 std::move(plan.second));
        co_return result.affectedRows();
    }

    // El SQL que se generaria, sin ejecutarlo. Para depurar y para tests.
    std::string toSql() const { return select(); }

    std::string toUpdateSql() const { return updatePlan().first; }

private:
    drogon::orm::DbClientPtr target() const { return client_ ? client_ : db::client(); }

    // Los parametros de un grupo continuan la numeracion del padre, no
    // empiezan de cero: por eso el indice no es solo params_.size().
    std::string slot(std::size_t ahead = 0) const {
        return detail::placeholder(paramBase_ + params_.size() + ahead + 1);
    }

    template <typename V>
    Query& condition(const char* join, std::string column, std::string op, V value) {
        push(join, "\"" + column + "\" " + op + " " + slot());
        params_.push_back(detail::binder(std::move(value)));
        return *this;
    }

    template <typename F>
    Query& group(const char* join, F&& build) {
        Query sub;
        sub.paramBase_ = paramBase_ + params_.size();
        build(sub);

        // Un grupo sin filtros no deja un "()" que ningun motor acepta:
        // simplemente no existe.
        if (sub.where_.empty()) return *this;

        push(join, "(" + sub.where_ + ")");
        for (auto& param : sub.params_) params_.push_back(std::move(param));
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
            slots += slot(i);
        }
        push(join, "\"" + column + "\" IN (" + slots + ")");

        for (auto& value : values) params_.push_back(detail::binder(std::move(value)));
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

    // El fallo que esta feature no se puede permitir es servirle a un cliente
    // los datos de otro. Y con el tenant pasandose a mano, basta olvidarlo UNA
    // vez en un repositorio para que eso pase, en silencio y en produccion.
    //
    // Por eso olvidarlo no devuelve todas las filas: lanza. Un 500 ruidoso en
    // la primera prueba es infinitamente mejor que una fuga que nadie ve, y
    // convierte un fallo de seguridad en un fallo de programacion normal.
    void requireTenant() const {
        if constexpr (detail::Tenant<T>) {
            if (!tenant_) {
                throw std::runtime_error(
                    "syrax: la consulta sobre '" + detail::tableOf<T>() +
                    "' no dice de que tenant es. El modelo declara `tenant`, asi que hay que "
                    "llamar a forTenant(id) antes de ejecutarla: devolver las filas de todos "
                    "los tenants seria una fuga de datos.");
            }
        }
    }

    std::string whereClause() const {
        std::string clause = where_;

        if constexpr (detail::Tenant<T>) {
            if (tenant_) {
                // Va con parentesis por lo mismo que el filtro de borrados: sin
                // ellos, un where con OR se combinaria mal y devolveria filas
                // de otro tenant.
                const auto extra = "\"" + std::string{detail::kTenantId} + "\" = " +
                                   detail::placeholder(paramBase_ + params_.size() + 1);

                clause = clause.empty() ? extra : "(" + clause + ") AND " + extra;
            }
        }

        if constexpr (detail::SoftDeletes<T>) {
            const std::string column = "\"" + std::string{detail::kDeletedAt} + "\"";

            std::string extra;
            if (trashed_ == Trashed::Without)   extra = column + " IS NULL";
            else if (trashed_ == Trashed::Only) extra = column + " IS NOT NULL";

            if (!extra.empty()) {
                // Los parentesis no son cosmeticos: sin ellos, un filtro con OR
                // -"a = 1 OR b = 2"- se combinaria como "a = 1 OR (b = 2 AND no
                // borrado)" y devolveria filas borradas.
                clause = clause.empty() ? extra : "(" + clause + ") AND " + extra;
            }
        }
        return clause.empty() ? std::string{} : " WHERE " + clause;
    }

    // Postgres numera los parametros, asi que el SET puede quedarse con los
    // numeros que sobran detras del WHERE y enlazarse al final. SQLite usa '?'
    // posicional, donde manda el orden de aparicion en el SQL y el SET va
    // delante. De ahi que el orden de enlace dependa del motor.
    std::pair<std::string, std::vector<detail::ParamBinder>> updatePlan() const {
        const bool numbered = db::dialect() == db::Dialect::Postgres;

        // El del tenant ya ocupa un hueco detras de los del where, asi que los
        // del SET empiezan despues de el.
        const auto previos = bound().size();

        std::string assignments;
        for (std::size_t i = 0; i < sets_.size(); ++i) {
            if (i) assignments += ", ";
            assignments += "\"" + sets_[i].column + "\" = " +
                           detail::placeholder(numbered ? previos + i + 1 : i + 1);
        }

        // Una fila que cambia y no actualiza su updated_at deja el campo
        // mintiendo, que es peor que no tenerlo. Va sin parametro porque la
        // hora la pone la base.
        if constexpr (detail::Timestamps<T>) {
            assignments += ", \"" + std::string{detail::kUpdatedAt} + "\" = " +
                           std::string{detail::kNow};
        }

        auto                             where = bound();
        std::vector<detail::ParamBinder> params;
        params.reserve(where.size() + sets_.size());

        if (numbered) {
            params = where;
            for (const auto& assignment : sets_) params.push_back(assignment.bind);
        } else {
            for (const auto& assignment : sets_) params.push_back(assignment.bind);
            params.insert(params.end(), where.begin(), where.end());
        }

        return {"UPDATE \"" + detail::tableOf<T>() + "\" SET " + assignments + whereClause(),
                std::move(params)};
    }

    // Los del where mas, si lo hay, el del tenant. Va al final porque su
    // predicado tambien se escribe al final del WHERE, y en sqlite el orden de
    // enlace es el de aparicion en el SQL.
    std::vector<detail::ParamBinder> bound() const {
        auto out = params_;
        if constexpr (detail::Tenant<T>) {
            if (tenant_) out.push_back(detail::binder(*tenant_));
        }
        return out;
    }

    std::string select() const {
        std::string sql = "SELECT " + detail::columnList<T>() + " FROM \"" +
                          detail::tableOf<T>() + "\"" + whereClause();

        if (!order_.empty())      sql += " ORDER BY " + order_;
        if (limit_.has_value())   sql += " LIMIT " + std::to_string(*limit_);
        if (offset_.has_value())  sql += " OFFSET " + std::to_string(*offset_);
        return sql;
    }

    struct Assignment {
        std::string         column;
        detail::ParamBinder bind;
    };

    enum class Trashed { Without, With, Only };

    drogon::orm::DbClientPtr         client_;
    std::string                      where_;
    std::string                      order_;
    std::optional<std::size_t>       limit_;
    std::optional<std::size_t>       offset_;
    std::optional<std::string>       tenant_;
    std::vector<detail::ParamBinder> params_;
    std::vector<Assignment>          sets_;
    std::size_t                      paramBase_ = 0;
    Trashed                          trashed_   = Trashed::Without;
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

            // created_at, updated_at y deleted_at los lleva el framework. Si
            // el modelo los declara como campos se LEEN -vuelven rellenos del
            // RETURNING-, pero no se escriben desde el struct: si no, un objeto
            // recien construido con el campo vacio los pisaria con basura.
            if (detail::managedColumn<T>(key)) return;

            auto&& field = glz::get_member(value, glz::get<I>(glz::to_tie(value)));

            if (!columns.empty()) { columns += ", "; slots += ", "; assignments += ", "; }

            const auto slot = detail::placeholder(params.size() + 1);
            columns += "\"" + key + "\"";
            slots   += slot;
            assignments += "\"" + key + "\" = " + slot;

            params.push_back(detail::binder(field));
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    if constexpr (detail::Timestamps<T>) {
        if (isInsert) {
            if (!columns.empty()) { columns += ", "; slots += ", "; }

            columns += "\"" + std::string{detail::kCreatedAt} + "\", \"" +
                       std::string{detail::kUpdatedAt} + "\"";
            slots += std::string{detail::kNow} + ", " + std::string{detail::kNow};
        } else {
            if (!assignments.empty()) assignments += ", ";
            assignments += "\"" + std::string{detail::kUpdatedAt} + "\" = " +
                           std::string{detail::kNow};
        }
    }

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
            params.push_back(detail::binder(field));
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
            params.push_back(detail::binder(field));
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    const std::string sql = "DELETE FROM \"" + table + "\" WHERE \"" + primary + "\" = " +
                            detail::placeholder(1);

    const auto result = co_await detail::run(target, sql, std::move(params));
    co_return result.affectedRows() > 0;
}

}  // namespace syrax
