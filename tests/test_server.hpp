#pragma once

#include <syrax/syrax.hpp>

#include <cstdint>
#include <string>

// Drogon es un singleton: solo puede haber UN app().run() por proceso. Por
// eso el servidor de los tests vive aqui y no en cada archivo — con dos, el
// segundo run() se pisa con el primero y el binario aborta al salir.
namespace testsrv {

// El puerto lo elige el kernel, no nosotros. Con un numero fijo, dos binarios
// de test corriendo a la vez —lo que hace `ctest -j8`— pelean por el mismo y el
// segundo no llega a escuchar. Solo tiene valor despues de ensureServer().
std::uint16_t port();

struct CreateThing {
    std::string name;
    int         size;
};

struct Thing {
    std::int64_t id;
    std::string  name;
};

struct Echo {
    std::string value;
};

struct Payload {
    std::string valor;
};

struct Signup {
    std::string name;
    std::string email;
    int         age;

    static auto rules() {
        return syrax::rules(syrax::field(&Signup::name).notEmpty().minLen(3),
                            syrax::field(&Signup::email).email(),
                            syrax::field(&Signup::age).range(18, 120));
    }
};

// Arranca el servidor la primera vez que se llama y espera a que escuche.
void ensureServer();

// Estado observable de los endpoints WebSocket.
syrax::Room& room();
int          closeCount();

// Fixture comun: heredar de aqui garantiza que el servidor este arriba.
struct Server {
    Server() { ensureServer(); }
};

}  // namespace testsrv
