#pragma once

#include <syrax/syrax.hpp>

#include <string_view>

namespace routes::v1 {

// Todas las rutas de esta version cuelgan de aqui. Para sacar una v2 se copia
// este archivo, se cambia el prefijo, y las dos conviven.
inline constexpr std::string_view kPrefix = "/api/v1";

void register_(syrax::App& app);

}  // namespace routes::v1
