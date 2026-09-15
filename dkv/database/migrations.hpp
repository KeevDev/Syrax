#pragma once

#include <syrax/syrax.hpp>

// Registra todas las migraciones del proyecto, en orden de aplicacion.
void registerMigrations(syrax::Migrator& migrator);
