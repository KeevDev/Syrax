#include <syrax/syrax.hpp>

#include "routes.hpp"

#include <cstdint>
#include <cstdlib>

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 8080);

    // Lee DB_ENGINE, DB_HOST, DB_NAME... del entorno. Ver .env.example
    syrax::db::configureFromEnv();

    syrax::App app;
    registerRoutes(app);
    app.run(port);
}
