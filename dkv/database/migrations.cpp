#include "migrations.hpp"

#include "migrations/001_create_users.hpp"

// El orden de esta lista es el orden en que se aplican. Agregar una migracion
// son dos pasos: crear el archivo y anadir una linea aqui.
//
// El registro es explicito a proposito: nada de macros ni de auto-registro por
// inicializacion estatica, cuyo orden no esta garantizado entre unidades de
// traduccion.
void registerMigrations(syrax::Migrator& migrator) {
    migrator.add<CreateUsersTable>();
}
