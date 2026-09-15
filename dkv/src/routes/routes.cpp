#include "routes/routes.hpp"

#include "routes/v1.hpp"

void registerRoutes(syrax::App& app) {
    routes::v1::register_(app);

    // routes::v2::register_(app);
}
