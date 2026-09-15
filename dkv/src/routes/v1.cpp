#include "routes/v1.hpp"

#include "http/controllers/User/UserController.hpp"

namespace routes::v1 {

void register_(syrax::App& app) {
    controllers::user::routes(app, kPrefix);

    // los controladores de esta version se registran aqui
}

}  // namespace routes::v1
