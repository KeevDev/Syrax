#include "http/controllers/User/UserController.hpp"

#include "http/requests/User/UserRequests.hpp"
#include "http/resources/User/UserResource.hpp"
#include "services/User/UserService.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace syrax;

namespace controllers::user {

namespace service = services::UserService;

// Los controladores son delgados a proposito: reciben, delegan, y traducen
// el resultado a HTTP. Ninguna regla de negocio vive aqui.
void routes(App& app, std::string_view prefix) {
    const std::string base{prefix};

    app.get(base + "/users", []() -> Task<Result<std::vector<resources::UserResource>>> {
        co_return resources::from(co_await service::list());
    });

    app.get(base + "/users/{id}", [](std::int64_t id) -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::byId(id);
        if (!user) co_return NotFound("user not found");

        co_return resources::from(*user);
    });

    app.post(base + "/users", [](requests::CreateUser body)
                 -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::create(std::move(body));
        if (!user) co_return Conflict("email already registered");

        co_return resources::from(*user);
    });

    app.put(base + "/users/{id}", [](std::int64_t id, requests::UpdateUser body)
                 -> Task<Result<resources::UserResource>> {
        const auto user = co_await service::update(id, std::move(body));
        if (!user) co_return NotFound("user not found");

        co_return resources::from(*user);
    });

    app.del(base + "/users/{id}", [](std::int64_t id)
                -> Task<Result<resources::DeletedResource>> {
        if (!co_await service::remove(id)) co_return NotFound("user not found");

        co_return resources::DeletedResource{.id = id, .deleted = true};
    });
}

}  // namespace controllers::user
