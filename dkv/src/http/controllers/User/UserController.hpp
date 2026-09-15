#pragma once

#include <syrax/syrax.hpp>

#include <string_view>

namespace controllers::user {

// El prefijo lo decide la version de rutas que lo registra, no el controlador.
void routes(syrax::App& app, std::string_view prefix);

}  // namespace controllers::user
