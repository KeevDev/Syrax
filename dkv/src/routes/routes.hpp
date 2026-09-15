#pragma once

#include <syrax/syrax.hpp>

// Punto unico donde se arma la API. Cada version vive en su propio archivo.
void registerRoutes(syrax::App& app);
