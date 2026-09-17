#pragma once

// Agrupar filas planas en un arbol, y declarar en el modelo que dos tablas se
// relacionan.
//
// Un join devuelve filas planas, como SQL: el padre repetido una vez por hijo.
// Convertir eso en un objeto con un vector dentro es lo que un ORM llama
// hydration, y es el unico trabajo que de verdad se escribe a mano en cada
// endpoint que anide algo.
//
//   const auto users = co_await Query<User>().get();
//   const auto posts = co_await Query<Post>().whereIn(&Post::authorId, ids).get();
//
//   const auto arbol = syrax::groupBy(users, posts, &User::id, &Post::authorId);
//
// **Esto no consulta nada.** Recibe dos vectores que ya trajiste y devuelve el
// arbol. No conoce la base, no puede dispararse solo, y las consultas siguen
// estando a la vista, escritas por ti. Es justo lo que lo separa de las
// relaciones de un ORM: ahi el que navega dispara SQL sin que se vea.
//
// La clave se pasa explicita -&User::id, &Post::authorId- porque adivinar como
// se relacionan dos tablas es declarar relaciones, y para eso esta `relations`
// mas abajo, que lo dice en el modelo y en un solo sitio.

#include <glaze/glaze.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace syrax {

// Un padre con sus hijos. El struct de vuelta por defecto, para cuando no hace
// falta uno propio.
template <typename P, typename C>
struct Grouped {
    P              parent;
    std::vector<C> children;
};

namespace detail {

inline constexpr std::size_t kNoField = static_cast<std::size_t>(-1);

// El tipo del campo I de R, por reflexion.
template <typename R, std::size_t I>
using FieldType =
    std::remove_cvref_t<decltype(glz::get<I>(glz::to_tie(std::declval<R&>())))>;

// El indice del UNICO campo de R que tiene ese tipo. Si hay dos, o ninguno,
// devuelve kNoField: elegir uno seria adivinar, y el mensaje de error es mas
// util que una eleccion silenciosa.
template <typename R, typename Field>
constexpr std::size_t soleFieldOfType() {
    std::size_t found = kNoField;
    int         count = 0;

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            if constexpr (std::is_same_v<FieldType<R, I>, Field>) {
                found = I;
                ++count;
            }
        }(), ...);
    }(std::make_index_sequence<glz::reflect<R>::size>{});

    return count == 1 ? found : kNoField;
}

// Escribe en el campo I de `out`.
template <std::size_t I, typename R, typename V>
void setField(R& out, V value) {
    glz::get<I>(glz::to_tie(out)) = std::move(value);
}

}  // namespace detail

// ------------------------------------------------------------- groupBy

// Agrupa los hijos bajo su padre y rellena el struct que pidas.
//
// R tiene que tener exactamente un campo del tipo del padre y exactamente uno
// del tipo `std::vector<hijo>`. Se deducen por tipo, no por posicion: aqui no
// hay lista de columnas que ordenar, solo dos campos que no se pueden
// confundir entre si.
//
//   struct UserConPosts {
//       User              user;
//       std::vector<Post> posts;
//   };
//
//   groupInto<UserConPosts>(users, posts, &User::id, &Post::authorId);
//
// El orden de los padres se conserva, y el de los hijos dentro de cada padre
// tambien: los dos son el orden en que venian, que es el que puso el ORDER BY
// de quien consulto. Reordenar aqui seria pisar esa decision.
//
// Un padre sin hijos sale con el vector vacio, no desaparece. Es la diferencia
// entre un LEFT JOIN y un INNER, y aqui siempre es la primera: los padres son
// los que trajiste, y esta funcion no esta para descartar ninguno.
template <typename R, typename P, typename C, typename KP, typename KC>
std::vector<R> groupInto(const std::vector<P>& parents, const std::vector<C>& children,
                         KP P::*parentKey, KC C::*childKey) {
    constexpr auto padre = detail::soleFieldOfType<R, P>();
    constexpr auto hijos = detail::soleFieldOfType<R, std::vector<C>>();

    static_assert(padre != detail::kNoField,
                  "syrax: el struct de vuelta necesita exactamente un campo del tipo del "
                  "padre. Con dos del mismo tipo, cual se rellena seria adivinar.");
    static_assert(hijos != detail::kNoField,
                  "syrax: el struct de vuelta necesita exactamente un campo "
                  "`std::vector<Hijo>`.");

    // Los hijos se indexan una vez por su clave, y cada padre se resuelve en
    // tiempo constante. La version ingenua -recorrer los hijos por cada padre-
    // es O(padres x hijos), que con dos paginas de 100 ya son 10.000 vueltas
    // para un trabajo que son 200.
    std::unordered_map<KC, std::vector<C>> porClave;
    for (const auto& child : children) porClave[child.*childKey].push_back(child);

    std::vector<R> out;
    out.reserve(parents.size());

    for (const auto& parent : parents) {
        R fila{};
        detail::setField<padre>(fila, parent);

        if (const auto it = porClave.find(parent.*parentKey); it != porClave.end()) {
            detail::setField<hijos>(fila, it->second);
        }
        out.push_back(std::move(fila));
    }
    return out;
}

// Lo mismo, con el struct de vuelta por defecto.
template <typename P, typename C, typename KP, typename KC>
std::vector<Grouped<P, C>> groupBy(const std::vector<P>& parents, const std::vector<C>& children,
                                   KP P::*parentKey, KC C::*childKey) {
    return groupInto<Grouped<P, C>>(parents, children, parentKey, childKey);
}

// ----------------------------------------------------- relaciones del modelo

// Que dos tablas se relacionan se declara UNA vez, en el modelo, igual que
// `table` o `softDeletes`:
//
//   struct User {
//       std::int64_t id;
//       std::string  name;
//
//       static constexpr auto table     = "users";
//       static constexpr auto relations = syrax::relate(
//           syrax::hasMany(&Post::author_id, &User::id));
//   };
//
// A partir de ahi, `.with<Post>()` no repite las claves.
//
// Va como `static constexpr`, que es lo que la mantiene fuera de la reflexion:
// Glaze refleja los campos NO estaticos, asi que el modelo sigue sirviendo de
// resource, de body validado y de payload de un job, igual que antes.
//
// Se declara en el lado que quiere anidar -el padre-, y solo en ese. Declararla
// en los dos exigiria que cada tipo conociera al otro antes de existir, y la
// que hace falta es la del que consulta.
//
// **Solo hasMany, y solo un nivel.** Anidar dos -user -> posts -> comments- ya
// no es una relacion, es un plan de consultas con su orden y sus claves
// intermedias.
template <typename Child, typename Parent, typename KC, typename KP>
struct HasMany {
    using ChildType  = Child;
    using ParentType = Parent;

    KC Child::*childKey;
    KP Parent::*parentKey;
};

// La clave foranea va primero porque es la que nombra la relacion: `Post` es de
// un `User` por su author_id, no al reves.
template <typename Child, typename Parent, typename KC, typename KP>
constexpr auto hasMany(KC Child::*childKey, KP Parent::*parentKey) {
    return HasMany<Child, Parent, KC, KP>{childKey, parentKey};
}

// Junta las relaciones de un modelo. Es una tupla y nada mas: existe para que
// `relations` pueda llevar varias sin que el modelo tenga que saber de tuplas.
template <typename... Rs>
constexpr auto relate(Rs... rs) {
    return std::tuple{rs...};
}

namespace detail {

template <typename T>
concept HasRelations = requires { T::relations; };

// Busca en las relaciones de Parent la que apunta a Child. Si hay dos hacia el
// mismo tipo -un post tiene autor y revisor, los dos User- no elige: lo dice, y
// en el sitio de uso se pasan las claves a mano.
template <typename Parent, typename Child, typename Tuple>
constexpr std::size_t indexOfRelation(const Tuple&) {
    constexpr auto kSize = std::tuple_size_v<Tuple>;

    std::size_t found = kNoField;
    int         count = 0;

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            using R = std::tuple_element_t<I, Tuple>;
            if constexpr (std::is_same_v<typename R::ChildType, Child>) {
                found = I;
                ++count;
            }
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    return count == 1 ? found : kNoField;
}

}  // namespace detail

}  // namespace syrax
