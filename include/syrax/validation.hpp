#pragma once

#include <glaze/glaze.hpp>
#include <json/json.h>

#include <syrax/result.hpp>
#include <syrax/traits.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace syrax {

// FieldError se define en result.hpp: un Error lo lleva dentro.

// ------------------------------------------------------------------ reglas
//
// Una regla es cualquier objeto con:
//
//   std::optional<std::string> operator()(const V& valor) const
//
// Devuelve el mensaje si el valor NO cumple, o nullopt si esta bien.
// Opcionalmente puede declarar `void describe(Json::Value&) const` para
// aportar su palabra clave al JSON Schema, y de ahi al Swagger.

namespace rule {

struct MinLen {
    std::size_t n;

    std::optional<std::string> operator()(std::string_view v) const {
        if (v.size() >= n) return std::nullopt;
        return "debe tener al menos " + std::to_string(n) + " caracteres";
    }
    void describe(Json::Value& prop) const { prop["minLength"] = static_cast<Json::UInt64>(n); }
};

struct MaxLen {
    std::size_t n;

    std::optional<std::string> operator()(std::string_view v) const {
        if (v.size() <= n) return std::nullopt;
        return "no puede superar " + std::to_string(n) + " caracteres";
    }
    void describe(Json::Value& prop) const { prop["maxLength"] = static_cast<Json::UInt64>(n); }
};

struct NotEmpty {
    std::optional<std::string> operator()(std::string_view v) const {
        const auto first = v.find_first_not_of(" \t\n\r");
        if (first != std::string_view::npos) return std::nullopt;
        return "no puede estar vacio";
    }
    void describe(Json::Value& prop) const { prop["minLength"] = 1u; }
};

// Deliberadamente no implementa el RFC 5322. Un email sintacticamente valido
// puede no existir, y uno "invalido" segun el RFC puede entregar correo; la
// unica verificacion real es mandar un mensaje. Esto descarta los errores de
// tipeo obvios y nada mas, que es lo que una regla de formulario puede hacer
// con honestidad.
struct Email {
    std::optional<std::string> operator()(std::string_view v) const {
        const auto at = v.find('@');

        const bool ok = at != std::string_view::npos &&      // tiene arroba
                        at > 0 &&                            // y algo antes
                        v.find('@', at + 1) == std::string_view::npos &&  // una sola
                        v.find(' ') == std::string_view::npos &&
                        v.find('.', at) != std::string_view::npos &&  // dominio con punto
                        v.back() != '.' && v.back() != '@';

        if (ok) return std::nullopt;
        return "no es un email valido";
    }
    void describe(Json::Value& prop) const { prop["format"] = "email"; }
};

// El regex se compila una vez, al construir la regla, no en cada request.
class Pattern {
public:
    Pattern(std::string expression, std::string message)
        : source_{std::move(expression)},
          message_{std::move(message)},
          regex_{std::make_shared<std::regex>(source_)} {}

    std::optional<std::string> operator()(const std::string& v) const {
        if (std::regex_match(v, *regex_)) return std::nullopt;
        return message_;
    }
    void describe(Json::Value& prop) const { prop["pattern"] = source_; }

private:
    std::string                 source_;
    std::string                 message_;
    std::shared_ptr<std::regex> regex_;
};

class OneOf {
public:
    OneOf(std::initializer_list<std::string> options) : options_{options} {}

    std::optional<std::string> operator()(std::string_view v) const {
        if (std::ranges::find(options_, v) != options_.end()) return std::nullopt;

        std::string allowed;
        for (const auto& option : options_) {
            if (!allowed.empty()) allowed += ", ";
            allowed += option;
        }
        return "debe ser uno de: " + allowed;
    }
    void describe(Json::Value& prop) const {
        for (const auto& option : options_) prop["enum"].append(option);
    }

private:
    std::vector<std::string> options_;
};

template <typename N>
struct Min {
    N n;

    std::optional<std::string> operator()(N v) const {
        if (v >= n) return std::nullopt;
        return "debe ser mayor o igual que " + std::to_string(n);
    }
    void describe(Json::Value& prop) const { prop["minimum"] = n; }
};

template <typename N>
struct Max {
    N n;

    std::optional<std::string> operator()(N v) const {
        if (v <= n) return std::nullopt;
        return "debe ser menor o igual que " + std::to_string(n);
    }
    void describe(Json::Value& prop) const { prop["maximum"] = n; }
};

template <typename N>
struct Range {
    N lo, hi;

    std::optional<std::string> operator()(N v) const {
        if (v >= lo && v <= hi) return std::nullopt;
        return "debe estar entre " + std::to_string(lo) + " y " + std::to_string(hi);
    }
    void describe(Json::Value& prop) const {
        prop["minimum"] = lo;
        prop["maximum"] = hi;
    }
};

// Escapatoria: cualquier lambda `bool(const V&)` se vuelve una regla.
template <typename F>
struct Satisfies {
    F           predicate;
    std::string message;

    template <typename V>
    std::optional<std::string> operator()(const V& v) const {
        if (predicate(v)) return std::nullopt;
        return message;
    }
};

}  // namespace rule

// ------------------------------------------------------------ declaracion

namespace detail {

template <typename T>
struct Unwrap { using type = T; };

template <typename T>
struct Unwrap<std::optional<T>> { using type = T; };

// El tipo sobre el que opera una regla: para un `std::optional<int>` es `int`,
// porque la regla ve el valor de adentro, no la caja.
template <typename T>
using UnwrapT = typename Unwrap<T>::type;

}  // namespace detail

// Une un miembro con las reglas que debe cumplir.
//
// Se guarda el puntero a miembro, no el nombre: renombrar el campo rompe la
// compilacion en vez de dejar una regla apuntando a algo que ya no existe.
//
// Las reglas se encadenan como metodos y no como funciones sueltas por una
// razon concreta: dentro de `static auto rules()` los nombres de los campos
// tapan a los de namespace, asi que un campo llamado `email` volvia
// inutilizable a una funcion `email()`. Despues de un punto no hay colision
// posible. Es la misma forma que ya tiene Column en las migraciones.
template <typename C, typename M, typename... Rules>
class FieldRule {
public:
    using Value = detail::UnwrapT<M>;

    FieldRule(M C::*member, std::tuple<Rules...> rules)
        : member_{member}, rules_{std::move(rules)} {}

    // --- texto ---
    auto notEmpty() const              { return with(rule::NotEmpty{}); }
    auto minLen(std::size_t n) const   { return with(rule::MinLen{n}); }
    auto maxLen(std::size_t n) const   { return with(rule::MaxLen{n}); }
    auto email() const                 { return with(rule::Email{}); }

    auto oneOf(std::initializer_list<std::string> options) const {
        return with(rule::OneOf{options});
    }
    auto pattern(std::string expression, std::string message) const {
        return with(rule::Pattern{std::move(expression), std::move(message)});
    }

    // --- numeros ---
    auto min(Value n) const            { return with(rule::Min<Value>{n}); }
    auto max(Value n) const            { return with(rule::Max<Value>{n}); }
    auto range(Value lo, Value hi) const { return with(rule::Range<Value>{lo, hi}); }

    // --- escapatoria ---
    template <typename F>
    auto satisfies(F predicate, std::string message) const {
        return with(rule::Satisfies<F>{std::move(predicate), std::move(message)});
    }

    M C::*                      member() const { return member_; }
    const std::tuple<Rules...>& rules() const  { return rules_; }

private:
    // Cada regla agregada produce un tipo nuevo: la lista vive en el tipo, no
    // en memoria, y no hay ni una asignacion dinamica en todo el recorrido.
    template <typename R>
    FieldRule<C, M, Rules..., R> with(R added) const {
        return {member_, std::tuple_cat(rules_, std::make_tuple(std::move(added)))};
    }

    M C::*               member_;
    std::tuple<Rules...> rules_;
};

template <typename C, typename M>
FieldRule<C, M> field(M C::*member) {
    return {member, {}};
}

template <typename... Fields>
std::tuple<Fields...> rules(Fields... fields) {
    return std::make_tuple(std::move(fields)...);
}

// Un tipo se valida si declara `static constexpr auto rules()`. No hay macro
// ni registro: la presencia del metodo es la senal.
template <typename T>
concept Validatable = requires { T::rules(); };

namespace detail {

// Resuelve el nombre JSON de un miembro comparando su direccion contra la de
// cada campo reflejado. Glaze da los nombres en orden de declaracion; la
// comparacion de punteros es lo que los ata al `&T::campo` que escribio el
// usuario, sin que tenga que repetir el nombre como string.
template <typename C, typename M>
std::string fieldName(C& object, M C::*member) {
    const void* target = static_cast<const void*>(std::addressof(object.*member));

    constexpr auto keys  = glz::reflect<C>::keys;
    constexpr auto kSize = glz::reflect<C>::size;

    std::string name;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            auto&& candidate = glz::get_member(object, glz::get<I>(glz::to_tie(object)));
            if (static_cast<const void*>(std::addressof(candidate)) == target) {
                name = std::string{keys[I]};
            }
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    return name;
}

// Un campo opcional ausente no se valida: "no vino" es asunto de presencia,
// no de contenido. Si vino, la regla se aplica al valor de adentro.
template <typename Rule, typename V>
std::optional<std::string> applyRule(const Rule& rule, const V& value) {
    if constexpr (kIsOptional<V>) {
        if (!value.has_value()) return std::nullopt;
        return rule(*value);
    } else {
        return rule(value);
    }
}

template <typename R>
concept Describes = requires(const R& r, Json::Value& j) { r.describe(j); };

// Los elementos de un pack se pasan como ARGUMENTO a estos helpers en vez de
// capturarse en una lambda anidada: GCC no captura un elemento de pack dentro
// de una lambda que vive dentro de una expresion fold.
template <typename T, typename C, typename M, typename Rule, typename V>
void runRule(T& object, M C::*member, const Rule& rule, const V& value,
             std::vector<FieldError>& out) {
    auto message = applyRule(rule, value);
    if (!message) return;

    out.push_back({fieldName(object, member), std::move(*message)});
}

template <typename T, typename C, typename M, typename... Rules>
void checkField(T& object, const FieldRule<C, M, Rules...>& declared,
                std::vector<FieldError>& out) {
    const auto& member = object.*(declared.member());

    std::apply(
        [&](const auto&... rule) { (runRule(object, declared.member(), rule, member, out), ...); },
        declared.rules());
}

template <typename Rule>
void describeOne(const Rule& rule, Json::Value& property) {
    if constexpr (Describes<Rule>) rule.describe(property);
}

template <typename C, typename M, typename... Rules>
void describeField(C& instance, const FieldRule<C, M, Rules...>& declared, Json::Value& schema) {
    const auto name = fieldName(instance, declared.member());
    if (name.empty()) return;

    std::apply(
        [&](const auto&... rule) { (describeOne(rule, schema["properties"][name]), ...); },
        declared.rules());
}

}  // namespace detail

// Corre todas las reglas y devuelve TODOS los fallos, no el primero.
template <typename T>
std::vector<FieldError> validate(T& value) {
    std::vector<FieldError> errors;

    if constexpr (Validatable<T>) {
        std::apply([&](const auto&... declared) { (detail::checkField(value, declared, errors), ...); },
                   T::rules());
    }
    return errors;
}

// Inyecta las palabras clave de cada regla en el JSON Schema que produjo
// Glaze, para que el Swagger muestre los limites reales y no solo los tipos.
template <typename T>
void annotateSchema(Json::Value& schema) {
    if constexpr (Validatable<T>) {
        T instance{};

        std::apply(
            [&](const auto&... declared) { (detail::describeField(instance, declared, schema), ...); },
            T::rules());
    }
}

}  // namespace syrax
