#pragma once

#include <syrax/syrax.hpp>

namespace bootstrap {

// Todo lo que hay que preparar antes de atender la primera peticion:
// configuracion, base de datos, documentacion y rutas.
//
// Vive aparte de main.cpp porque esto crece (middleware, plugins, manejadores
// de error) y main deberia seguir cabiendo en una pantalla.
syrax::App create();

}  // namespace bootstrap
