#pragma once

#include <tuple>
#include <type_traits>

namespace syrax::detail {

// Extrae tipo de retorno y tipos de argumento de cualquier callable.
// Es la pieza que permite que el handler declare su propia firma y que
// Syrax deduzca de ahi que parsear, que validar y que documentar.
template <typename T>
struct fn_traits : fn_traits<decltype(&std::remove_reference_t<T>::operator())> {};

template <typename C, typename R, typename... A>
struct fn_traits<R (C::*)(A...) const> {
    using result = R;
    using args   = std::tuple<std::decay_t<A>...>;
};

template <typename C, typename R, typename... A>
struct fn_traits<R (C::*)(A...)> {
    using result = R;
    using args   = std::tuple<std::decay_t<A>...>;
};

template <typename R, typename... A>
struct fn_traits<R (*)(A...)> {
    using result = R;
    using args   = std::tuple<std::decay_t<A>...>;
};

}  // namespace syrax::detail
