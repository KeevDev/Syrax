#include "bootstrap/app.hpp"

#include "routes/routes.hpp"

#include <filesystem>

namespace bootstrap {

syrax::App create() {
    // Ajustes del servidor: hilos, logging, limites. Se versiona.
    if (std::filesystem::exists("config/app.json")) {
        drogon::app().loadConfigFile("config/app.json");
    }

    // Credenciales desde .env y el entorno. NO se versiona.
    syrax::db::configureFromEnv();

    syrax::App app;

    // Titulo y version que se ven en /docs.
    app.docs("dkv", "1.0.0");

    registerRoutes(app);
    return app;
}

}  // namespace bootstrap
