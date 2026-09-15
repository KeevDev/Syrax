#include <catch2/catch_test_macros.hpp>

#include <syrax/app.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Deja el entorno como estaba: estas variables son globales del proceso y
// otro test podria leerlas despues.
struct ScopedEnv {
    std::string  key;
    bool         had = false;
    std::string  previous;

    explicit ScopedEnv(std::string k) : key{std::move(k)} {
        if (const char* v = std::getenv(key.c_str())) {
            had      = true;
            previous = v;
        }
    }
    void set(const std::string& value) { ::setenv(key.c_str(), value.c_str(), 1); }
    void clear() { ::unsetenv(key.c_str()); }

    ~ScopedEnv() {
        if (had) ::setenv(key.c_str(), previous.c_str(), 1);
        else     ::unsetenv(key.c_str());
    }
};

}  // namespace

TEST_CASE("sin APP_PORT se usa el fallback", "[env]") {
    ScopedEnv guard{"APP_PORT"};
    guard.clear();

    CHECK(syrax::envPort() == 8080);
    CHECK(syrax::envPort(3000) == 3000);
}

TEST_CASE("APP_PORT del entorno manda", "[env]") {
    ScopedEnv guard{"APP_PORT"};
    guard.set("9123");

    CHECK(syrax::envPort() == 9123);
}

TEST_CASE("un APP_PORT invalido cae al fallback y no revienta", "[env]") {
    ScopedEnv guard{"APP_PORT"};

    // Ni texto, ni basura pegada, ni fuera de rango, ni el 0.
    for (const char* bad : {"ocho", "80a", "99999", "0", "-1", ""}) {
        guard.set(bad);
        CHECK(syrax::envPort(8080) == 8080);
    }
}

TEST_CASE("el entorno le gana al .env", "[env]") {
    // loadDotEnv usa setenv con overwrite=0: lo que ya existe no se pisa. Es
    // lo que permite que un despliegue mande sin editar el archivo.
    const auto file = std::filesystem::current_path() / ".env";
    const bool existed = std::filesystem::exists(file);
    if (existed) return;  // no se toca un .env real

    {
        std::ofstream out{file};
        out << "APP_PORT=7777\n";
    }

    {
        ScopedEnv guard{"APP_PORT"};
        guard.clear();
        CHECK(syrax::envPort() == 7777);   // sale del archivo

        guard.set("8888");
        CHECK(syrax::envPort() == 8888);   // el entorno gana
    }
    std::filesystem::remove(file);
}
