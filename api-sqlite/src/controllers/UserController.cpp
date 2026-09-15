#include "controllers/UserController.hpp"

#include "requests/UserRequests.hpp"
#include "resources/UserResource.hpp"
#include "services/UserService.hpp"

#include <cstdint>
#include <vector>

using namespace syrax;

namespace controllers::UserController {

namespace service = services::UserService;

// Los controladores son delgados a proposito: reciben, delegan, y traducen
// el resultado a HTTP. Ninguna regla de negocio vive aqui.
void routes(App& app) {

    app.get("/users", []() -> Task<Result<std::vector<resources::UserResource>>> {
        co_return resources::from(co_await service::list());
    });

    app.get("/users/{id}", [](std::int64_t id) -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::byId(id);
        if (!user) co_return NotFound("user not found");

        co_return resources::from(*user);
    });

    app.post("/users", [](requests::CreateUser body)
                 -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::create(std::move(body));
        if (!user) co_return Conflict("email already registered");

        co_return resources::from(*user);
    });

    app.put("/users/{id}", [](std::int64_t id, requests::UpdateUser body)
                 -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::update(id, std::move(body));
        if (!user) co_return NotFound("user not found");

        co_return resources::from(*user);
    });

    app.del("/users/{id}", [](std::int64_t id)
                -> Task<Result<resources::DeletedResource>> {
        if (!co_await service::remove(id)) co_return NotFound("user not found");

        co_return resources::DeletedResource{.id = id, .deleted = true};
    });
}

}  // namespace controllers::UserController
