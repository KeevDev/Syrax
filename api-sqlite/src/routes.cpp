#include "routes.hpp"

#include "controllers/UserController.hpp"

void registerRoutes(syrax::App& app) {
    controllers::UserController::routes(app);

    // los controladores nuevos se registran aqui
}
