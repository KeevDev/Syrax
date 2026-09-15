#include <syrax/syrax.hpp>

#include "bootstrap/app.hpp"
#include "migrations.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

int migrations(const std::string& command) {
    syrax::Migrator migrator;
    registerMigrations(migrator);

    if (command == "migrate")          return migrator.migrate();
    if (command == "migrate:rollback") return migrator.rollback();
    return migrator.status();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string arg = (argc > 1) ? argv[1] : "";

    if (arg == "migrate" || arg == "migrate:rollback" || arg == "migrate:status") {
        return migrations(arg);
    }

    // Sin subcomando, el argumento es el puerto.
    const auto port = static_cast<std::uint16_t>(arg.empty() ? 8080 : std::atoi(arg.c_str()));

    auto app = bootstrap::create();
    app.run(port);
    return 0;
}
