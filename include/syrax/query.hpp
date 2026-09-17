#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/orm/SqlBinder.h>
#include <glaze/glaze.hpp>

#include <syrax/db.hpp>
#include <syrax/relations.hpp>
#include <syrax/traits.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// Como columnOf esta templado por la clase DUEÑA del miembro, &User::name ya
// sabe que su tabla es "users". Calificar sale gratis, y es lo que permite que
// un join no tenga que pedirle al usuario ni un alias ni un string.
template <typename C, typename M>
std::string qualified(M C::*member);

template <typename T>
std::string tableOf() {
    static_assert(HasTable<T>,
                  "syrax: el modelo necesita `static constexpr auto table = \"...\";` "
                  "para usar Query<T>. Con SQL a mano (db::query) no hace falta.");
    return std::string{T::table};
}

template <typename C, typename M>
std::string qualified(M C::*member) {
    return "\"" + tableOf<C>() + "\".\"" + columnOf(member) + "\"";
}

template <typename T>
std::string primaryKeyOf() {
    if constexpr (HasPrimaryKey<T>) return std::string{T::primaryKey};
    else                            return "id";
}

// Lo mismo, en compilacion: lo de arriba devuelve std::string porque se
// concatena con el SQL, y un std::string no vive en un static_assert.
template <typename T>
constexpr std::string_view primaryKeyName() {
    if constexpr (HasPrimaryKey<T>) return std::string_view{T::primaryKey};
    else                            return "id";
}

// Si el struct tiene el campo de su clave primaria.
//
// No tenerlo es perfectamente valido para LEER -una proyeccion sin el id es
// justo para lo que sirve que las columnas salgan del struct-, pero save() y
// remove() no pueden hacer nada sin el: save() decide INSERT o UPDATE mirando
// si la clave viene a cero, y sin campo que mirar se quedaria insertando
// siempre, en silencio y duplicando filas. Por eso esto se comprueba donde se
// escribe, no donde se declara el modelo.
template <typename T>
constexpr bool hasPrimaryKeyField() {
    constexpr auto keys    = glz::reflect<T>::keys;
    constexpr auto primary = primaryKeyName<T>();

    bool found = false;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            if (std::string_view{keys[I]} == primary) found = true;
        }(), ...);
    }(std::make_index_sequence<glz::reflect<T>::size>{});

    return found;
}

// El mensaje de un static_assert tiene que ser un literal, asi que el guardia
// va en una funcion en vez de en una constante: asi se escribe una sola vez y
// los dos sitios que lo necesitan la llaman.
template <typename T>
constexpr void requirePrimaryKeyField() {
    static_assert(hasPrimaryKeyField<T>(),
                  "syrax: el modelo no tiene el campo de su clave primaria, asi que save() y "
                  "remove() no pueden saber sobre que fila actuan: save() se quedaria "
                  "insertando una fila nueva cada vez, en silencio. Para una proyeccion de "
                  "solo lectura -un struct sin el id- eso esta bien, pero entonces no la "
                  "pases por save() ni por remove(): para eso estan Query<T>::update() y "
                  "del(), que filtran por where. Si la columna se llama de otra forma, "
                  "declarala con `static constexpr auto primaryKey = \"doc_id\";`.");
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

// Una consulta sobre dos tablas. Se construye con Query<T>::join(); la
// definicion esta debajo de Query, que es donde se lee.
template <typename T, typename U>
class Join;

// Una consulta que ademas se trae los hijos. Se construye con Query<T>::with().
template <typename T, typename U, typename KU, typename KT>
class With;

// Construye SELECT / UPDATE / DELETE / COUNT sobre un struct plano.
//
//   auto adultos = co_await Query<User>()
//       .where(&User::age, ">", 18)
//       .orderBy(&User::name)
//       .limit(10)
//       .get();
//
// Para unir con otra tabla, join() devuelve un Join<T, U> que trabaja sobre las
// dos y devuelve filas planas. Para traerse los hijos anidados -un User con su
// vector<Post> dentro-, with() devuelve un With<...> que hace DOS consultas y
// los agrupa.
//
// Lo que sigue sin haber es LAZY LOADING, y es la unica linea que importa: un
// post.author() que consulta al navegarlo esconde un numero de consultas que
// depende de los datos, y eso es el N+1. Aqui el numero es siempre el mismo,
// se vea o no la segunda consulta. Tampoco hay identity map ni cascadas: eso
// ultimo lo hace mejor un ON DELETE CASCADE en la migracion.
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

    // --- unir con otra tabla ---

    // Pasa a una consulta de DOS tablas, que devuelve el struct que declares.
    //
    //   Query<Post>().join<User>(&Post::authorId, &User::id)
    //                .get<PostConAutor>(&Post::id, &Post::title, &User::name);
    //
    // Va ANTES que los filtros, y no es capricho: los where de Query<T> se
    // escriben sin calificar -no hace falta, hay una sola tabla-, y en cuanto
    // hay dos, un "id" a secas es ambiguo para el motor. En vez de reescribir
    // lo ya acumulado, que seria adivinar, esto lo dice.
    template <typename U, typename CT, typename MT, typename CU, typename MU>
    Join<T, U> join(MT CT::*local, MU CU::*foreign) const {
        return startJoin<U>(Join<T, U>::Kind::Inner, local, foreign);
    }

    // Conserva las filas de T que no tienen pareja en U.
    template <typename U, typename CT, typename MT, typename CU, typename MU>
    Join<T, U> leftJoin(MT CT::*local, MU CU::*foreign) const {
        return startJoin<U>(Join<T, U>::Kind::Left, local, foreign);
    }

    // --- traerse los hijos ---

    // Anade una segunda consulta que trae los hijos y los agrupa bajo su
    // padre. A diferencia de join(), esto puede ir en cualquier sitio de la
    // cadena: son dos consultas separadas, asi que no hay ninguna columna que
    // calificar.
    //
    //   Query<User>().where(&User::age, ">", 18)
    //                .with<Post>(&Post::author_id, &User::id)
    //                .get<UserConPosts>();
    template <typename U, typename CU, typename MU, typename CT, typename MT>
    With<T, U, MU, MT> with(MU CU::*childKey, MT CT::*parentKey) const {
        static_assert(std::is_same_v<CU, U>,
                      "syrax: la clave foranea tiene que ser del modelo hijo.");
        static_assert(std::is_same_v<CT, T>,
                      "syrax: la clave del padre tiene que ser del modelo de esta consulta.");

        return With<T, U, MU, MT>{*this, childKey, parentKey};
    }

    // Lo mismo, con las claves sacadas del modelo. Ver `relations` en
    // relations.hpp: se declaran una vez, donde se declara la tabla.
    //
    //   Query<User>().with<Post>().get<UserConPosts>();
    template <typename U>
    auto with() const {
        static_assert(detail::HasRelations<T>,
                      "syrax: para llamar a with<U>() sin claves, el modelo tiene que "
                      "declararlas: `static constexpr auto relations = syrax::relate("
                      "syrax::hasMany(&Hijo::padre_id, &Padre::id));`. O pasalas aqui.");

        constexpr auto at = detail::indexOfRelation<T, U>(T::relations);
        static_assert(at != detail::kNoField,
                      "syrax: el modelo no declara UNA relacion hacia ese tipo. Si no hay "
                      "ninguna, agregala; si hay dos hacia el mismo modelo -autor y revisor, "
                      "los dos User-, cual se usa no se puede adivinar: pasa las claves.");

        const auto& relation = std::get<at>(T::relations);
        return with<U>(relation.childKey, relation.parentKey);
    }

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

    // El cliente TAL CUAL, sin resolver al de por defecto: una consulta hija
    // tiene que heredar el de su padre -que puede ser el de una transaccion- y
    // resolverlo aqui la sacaria de ella.
    drogon::orm::DbClientPtr clientOrNull() const { return client_; }

private:
    drogon::orm::DbClientPtr target() const { return client_ ? client_ : db::client(); }

    template <typename U, typename K, typename CT, typename MT, typename CU, typename MU>
    Join<T, U> startJoin(K kind, MT CT::*local, MU CU::*foreign) const {
        static_assert(std::is_same_v<CT, T>,
                      "syrax: la columna local del join tiene que ser de la tabla de esta "
                      "consulta.");
        static_assert(std::is_same_v<CU, U>,
                      "syrax: la columna foranea tiene que ser de la tabla con la que se une.");

        if (!where_.empty() || !order_.empty() || limit_ || offset_ || !sets_.empty()) {
            throw std::runtime_error(
                "syrax: join() va antes que los filtros. Con dos tablas en juego, una columna "
                "sin calificar es ambigua, y reescribir lo que ya se acumulo seria adivinar. "
                "Mueve el .join<U>(...) justo detras del Query<T>().");
        }

        return Join<T, U>{client_, kind,
                          detail::qualified(foreign) + " = " + detail::qualified(local)};
    }

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

// ----------------------------------------------------------------- With

// Una consulta que ademas se trae los hijos, en DOS consultas fijas.
//
//   struct UserConPosts {
//       User              user;
//       std::vector<Post> posts;
//   };
//
//   co_await Query<User>()
//       .where(&User::age, ">", 18)
//       .with<Post>(&Post::author_id, &User::id)
//       .get<UserConPosts>();
//
//   SELECT ... FROM "users" WHERE "age" > $1
//   SELECT ... FROM "posts" WHERE "author_id" IN ($1, $2, $3)
//
// **Esto no es lazy loading, y la diferencia es toda la diferencia.** El lazy
// esconde un numero DESCONOCIDO de consultas: una por objeto, dentro de un
// bucle que no lo dice, y el N+1 aparece en produccion cuando la tabla crece.
// Esto esconde exactamente una, siempre, haya tres filas o tres mil. Esconder
// un numero fijo es empaquetar; esconder uno que depende de los datos es la
// trampa.
//
// **Y aqui paginate() SI existe**, al reves que en un join: se pagina la tabla
// base -donde `total` y `pages` significan lo que dicen- y los hijos se traen
// solo de esa pagina. Es el orden correcto, y es el que un join no permite.
//
// Un padre sin hijos sale con el vector vacio: los padres son los que trajiste
// y esto no descarta ninguno.
//
// Un solo nivel. Anidar dos -user -> posts -> comments- ya no es una relacion,
// es un plan de consultas con su orden y sus claves intermedias.
template <typename T, typename U, typename KU, typename KT>
class With {
public:
    With(Query<T> base, KU U::*childKey, KT T::*parentKey)
        : base_{std::move(base)}, childKey_{childKey}, parentKey_{parentKey} {}

    // Ata las dos consultas al mismo tenant. Una consulta pertenece a un
    // cliente, no a dos.
    With& forTenant(std::string id) {
        static_assert(detail::Tenant<T> || detail::Tenant<U>,
                      "syrax: forTenant() necesita que alguno de los dos modelos declare "
                      "`static constexpr auto tenant = true;`.");
        if constexpr (detail::Tenant<T>) base_.forTenant(id);
        tenant_ = std::move(id);
        return *this;
    }

    template <typename R>
    drogon::Task<std::vector<R>> get() const {
        const auto parents = co_await base_.get();
        co_return co_await hydrate<R>(parents);
    }

    template <typename R>
    drogon::Task<std::optional<R>> first() const {
        Query<T> uno{base_};
        uno.limit(1);

        const auto parents = co_await uno.get();
        if (parents.empty()) co_return std::nullopt;

        const auto filas = co_await hydrate<R>(parents);
        co_return filas.front();
    }

    // Se pagina la tabla BASE, que es donde total y pages significan algo, y
    // los hijos se traen solo de esa pagina. Siguen siendo dos consultas mas
    // la del count.
    template <typename R>
    drogon::Task<Page<R>> paginate(std::int64_t page = 1, std::int64_t perPage = 15) const {
        const auto base = co_await base_.paginate(page, perPage);

        co_return Page<R>{
            .data    = co_await hydrate<R>(base.data),
            .total   = base.total,
            .page    = base.page,
            .perPage = base.perPage,
            .pages   = base.pages,
            .hasMore = base.hasMore,
        };
    }

private:
    // La segunda consulta y el agrupado. Se separa porque get, first y
    // paginate solo se diferencian en como consiguen los padres.
    template <typename R>
    drogon::Task<std::vector<R>> hydrate(const std::vector<T>& parents) const {
        // Sin padres no hay claves que buscar, y un IN vacio seria una ida a la
        // base para no traer nada.
        if (parents.empty()) co_return std::vector<R>{};

        std::vector<KT> keys;
        keys.reserve(parents.size());
        for (const auto& parent : parents) keys.push_back(parent.*parentKey_);

        Query<U> hijos{base_.clientOrNull()};
        hijos.whereIn(childKey_, std::move(keys));
        if constexpr (detail::Tenant<U>) {
            if (tenant_) hijos.forTenant(*tenant_);
        }

        co_return groupInto<R>(parents, co_await hijos.get(), parentKey_, childKey_);
    }

    Query<T>                   base_;
    KU U::*                    childKey_;
    KT T::*                    parentKey_;
    std::optional<std::string> tenant_;
};

// ----------------------------------------------------------------- Join

// Una consulta sobre DOS tablas, que devuelve el struct plano que tu declares.
//
//   struct PostConAutor {
//       std::int64_t id;
//       std::string  title;
//       std::string  author;
//   };
//
//   co_await Query<Post>()
//       .join<User>(&Post::authorId, &User::id)
//       .where(&Post::published, "=", true)
//       .get<PostConAutor>(&Post::id, &Post::title, &User::name);
//
//   SELECT "posts"."id" AS "id", "posts"."title" AS "title",
//          "users"."name" AS "author"
//   FROM "posts" INNER JOIN "users" ON "users"."id" = "posts"."author_id"
//   WHERE "posts"."published" = $1
//
// Los alias salen de los campos del struct de vuelta, POR POSICION: tres
// campos, tres columnas. Si sobra o falta una, no compila. Si escribes
// &User::nmae, tampoco. No hay ni un string de por medio.
//
// **Esto no son relaciones, y la diferencia importa.** Devuelve filas planas,
// como las devuelve SQL. No hay lazy loading, ni identity map, ni un vector de
// hijos dentro del padre: eso exige agrupar N filas en un objeto, y ahi empieza
// el ORM. Para anidar, dos consultas y un mapa en memoria -esta en el README-,
// que ademas es lo que hace por dentro un eager load.
//
// **Y por eso no hay paginate().** Un LIMIT sobre un join corta FILAS, no
// filas de la tabla base: pides 20 posts con su autor y si un post tuviera
// varias filas unidas te vuelven menos de 20 posts. El count(*) contaria lo
// mismo, asi que `total` y `pages` mentirian. Lo correcto es paginar la tabla
// base con Query<T>::paginate y luego traer lo unido de esa pagina.
//
// Dos tablas y se planta. Con tres ya hace falta decidir el orden del join y
// que se une con que, y eso es un planificador.
template <typename T, typename U>
class Join {
public:
    enum class Kind { Inner, Left };

    Join(drogon::orm::DbClientPtr on, Kind kind, std::string onClause)
        : client_{std::move(on)}, kind_{kind}, on_{std::move(onClause)} {}

    // --- filtros, sobre cualquiera de las dos tablas ---

    template <typename C, typename M, typename V>
    Join& where(M C::*member, std::string op, V value) {
        belongs<C>();
        return condition("AND", detail::qualified(member), std::move(op), std::move(value));
    }

    template <typename C, typename M, typename V>
    Join& orWhere(M C::*member, std::string op, V value) {
        belongs<C>();
        return condition("OR", detail::qualified(member), std::move(op), std::move(value));
    }

    template <typename C, typename M, typename V>
    Join& whereIn(M C::*member, std::vector<V> values) {
        belongs<C>();
        return inList(detail::qualified(member), std::move(values));
    }

    template <typename C, typename M, typename V>
    Join& whereIn(M C::*member, std::initializer_list<V> values) {
        return whereIn(member, std::vector<V>{values});
    }

    template <typename C, typename M>
    Join& whereNull(M C::*member) {
        belongs<C>();
        return bare("AND", detail::qualified(member) + " IS NULL");
    }

    template <typename C, typename M>
    Join& whereNotNull(M C::*member) {
        belongs<C>();
        return bare("AND", detail::qualified(member) + " IS NOT NULL");
    }

    template <typename C, typename M>
    Join& orderBy(M C::*member, Dir direction = Dir::Asc) {
        belongs<C>();
        if (!order_.empty()) order_ += ", ";
        order_ += detail::qualified(member) + (direction == Dir::Desc ? " DESC" : " ASC");
        return *this;
    }

    Join& limit(std::size_t count)  { limit_  = count;  return *this; }
    Join& offset(std::size_t count) { offset_ = count;  return *this; }

    // Un solo tenant para las dos tablas: una consulta pertenece a un cliente,
    // no a dos. Si solo una de las dos lo declara, solo esa se filtra.
    Join& forTenant(std::string id) {
        static_assert(detail::Tenant<T> || detail::Tenant<U>,
                      "syrax: forTenant() necesita que alguna de las dos tablas declare "
                      "`static constexpr auto tenant = true;`.");
        tenant_ = std::move(id);
        return *this;
    }

    // --- ejecucion ---

    // Las columnas van en el orden de los campos del struct de vuelta.
    template <typename R, typename... Cols>
    drogon::Task<std::vector<R>> get(Cols... cols) const {
        requireTenant();
        const auto result = co_await detail::run(target(), select<R>(cols...), bound());

        std::vector<R> rows;
        rows.reserve(result.size());
        for (const auto& row : result) rows.push_back(db::fromRow<R>(row));
        co_return rows;
    }

    template <typename R, typename... Cols>
    drogon::Task<std::optional<R>> first(Cols... cols) const {
        Join copy{*this};
        copy.limit_ = 1;

        const auto rows = co_await copy.template get<R>(cols...);
        if (rows.empty()) co_return std::nullopt;
        co_return rows.front();
    }

    // Cuenta FILAS del join, que en un join de varios no es lo mismo que
    // contar filas de la tabla base. Para eso, Query<T>().count().
    drogon::Task<std::int64_t> count() const {
        requireTenant();
        const auto sql = "SELECT count(*) FROM \"" + detail::tableOf<T>() + "\" " +
                         keyword() + " \"" + detail::tableOf<U>() + "\" ON " + on_ +
                         whereClause();

        const auto result = co_await detail::run(target(), sql, bound());
        co_return result.empty() ? 0 : result[0][0].template as<std::int64_t>();
    }

    // El SQL sin ejecutarlo, para ver que salio.
    template <typename R, typename... Cols>
    std::string toSql(Cols... cols) const {
        return select<R>(cols...);
    }

private:
    drogon::orm::DbClientPtr target() const { return client_ ? client_ : db::client(); }

    // El puntero a miembro trae su clase dueña, asi que una columna de una
    // tabla que no esta en el join no compila en vez de generar un SQL que el
    // motor rechaza a mitad de la noche.
    template <typename C>
    static void belongs() {
        static_assert(std::is_same_v<C, T> || std::is_same_v<C, U>,
                      "syrax: esa columna no es de ninguna de las dos tablas del join. "
                      "Un join de tres tablas no entra: haz dos consultas.");
    }

    const char* keyword() const { return kind_ == Kind::Left ? "LEFT JOIN" : "INNER JOIN"; }

    std::string slot(std::size_t ahead = 0) const {
        return detail::placeholder(params_.size() + ahead + 1);
    }

    template <typename V>
    Join& condition(const char* join, std::string column, std::string op, V value) {
        push(join, column + " " + op + " " + slot());
        params_.push_back(detail::binder(std::move(value)));
        return *this;
    }

    template <typename V>
    Join& inList(std::string column, std::vector<V> values) {
        // Igual que en Query: un IN vacio no es SQL valido, y "no coincide con
        // nada" es la lectura correcta.
        if (values.empty()) return bare("AND", "1 = 0");

        std::string slots;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) slots += ", ";
            slots += slot(i);
        }
        push("AND", column + " IN (" + slots + ")");

        for (auto& value : values) params_.push_back(detail::binder(std::move(value)));
        return *this;
    }

    Join& bare(const char* join, std::string expression) {
        push(join, std::move(expression));
        return *this;
    }

    void push(const char* join, std::string expression) {
        if (!where_.empty()) where_ += std::string{" "} + join + " ";
        where_ += std::move(expression);
    }

    // El mismo guardia que en Query, y por el mismo motivo: servirle a un
    // cliente los datos de otro es el fallo que esto no se puede permitir. En
    // un join el riesgo es mayor, no menor: basta que se escape UNA de las dos.
    void requireTenant() const {
        if constexpr (detail::Tenant<T> || detail::Tenant<U>) {
            if (!tenant_) {
                throw std::runtime_error(
                    "syrax: el join entre '" + detail::tableOf<T>() + "' y '" +
                    detail::tableOf<U>() +
                    "' no dice de que tenant es. Alguna de las dos declara `tenant`, asi que "
                    "hay que llamar a forTenant(id) antes de ejecutarlo.");
            }
        }
    }

    // Anade el predicado de una tabla al WHERE, siempre entre parentesis: sin
    // ellos un filtro con OR se combinaria mal y devolveria justo las filas que
    // estos predicados existen para esconder.
    static void andAlso(std::string& clause, const std::string& extra) {
        if (extra.empty()) return;
        clause = clause.empty() ? extra : "(" + clause + ") AND " + extra;
    }

    std::string whereClause() const {
        std::string clause = where_;

        // Los borrados de las DOS tablas. Si se filtrara solo la base, un join
        // contra una fila borrada la resucitaria por la puerta de atras.
        if constexpr (detail::SoftDeletes<T>) {
            andAlso(clause, "\"" + detail::tableOf<T>() + "\".\"" +
                                std::string{detail::kDeletedAt} + "\" IS NULL");
        }
        if constexpr (detail::SoftDeletes<U>) {
            // El mismo predicado sirve para los dos tipos de join, y no por
            // casualidad: en un LEFT JOIN sin pareja el lado derecho viene todo
            // a NULL, asi que "deleted_at IS NULL" tambien es cierto ahi y la
            // fila sin pareja -que es legitima- se conserva.
            andAlso(clause, "\"" + detail::tableOf<U>() + "\".\"" +
                                std::string{detail::kDeletedAt} + "\" IS NULL");
        }

        if (tenant_) {
            if constexpr (detail::Tenant<T>) {
                andAlso(clause, "\"" + detail::tableOf<T>() + "\".\"" +
                                    std::string{detail::kTenantId} + "\" = " +
                                    detail::placeholder(params_.size() + 1));
            }
            if constexpr (detail::Tenant<U>) {
                const auto ahead = detail::Tenant<T> ? 2 : 1;
                andAlso(clause, "\"" + detail::tableOf<U>() + "\".\"" +
                                    std::string{detail::kTenantId} + "\" = " +
                                    detail::placeholder(params_.size() + ahead));
            }
        }
        return clause.empty() ? std::string{} : " WHERE " + clause;
    }

    std::vector<detail::ParamBinder> bound() const {
        auto out = params_;
        if (tenant_) {
            if constexpr (detail::Tenant<T>) out.push_back(detail::binder(*tenant_));
            if constexpr (detail::Tenant<U>) out.push_back(detail::binder(*tenant_));
        }
        return out;
    }

    template <typename R, typename... Cols>
    std::string select(Cols... cols) const {
        static_assert(sizeof...(Cols) == glz::reflect<R>::size,
                      "syrax: el join pide una columna por campo del struct de vuelta, en el "
                      "mismo orden. Si sobran o faltan, el mapeo seria adivinar.");

        constexpr auto keys = glz::reflect<R>::keys;

        std::string  list;
        std::size_t  i = 0;
        ((list += (i ? ", " : "") + detail::qualified(cols) + " AS \"" +
                  std::string{keys[i]} + "\"",
          ++i),
         ...);

        std::string sql = "SELECT " + list + " FROM \"" + detail::tableOf<T>() + "\" " +
                          keyword() + " \"" + detail::tableOf<U>() + "\" ON " + on_ +
                          whereClause();

        if (!order_.empty())     sql += " ORDER BY " + order_;
        if (limit_.has_value())  sql += " LIMIT " + std::to_string(*limit_);
        if (offset_.has_value()) sql += " OFFSET " + std::to_string(*offset_);
        return sql;
    }

    drogon::orm::DbClientPtr         client_;
    Kind                             kind_;
    std::string                      on_;
    std::string                      where_;
    std::string                      order_;
    std::optional<std::size_t>       limit_;
    std::optional<std::size_t>       offset_;
    std::optional<std::string>       tenant_;
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
    detail::requirePrimaryKeyField<T>();

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
    detail::requirePrimaryKeyField<T>();

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
