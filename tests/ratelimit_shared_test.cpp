// El limite de peticiones compartido entre instancias, y la cadena de
// middleware asincrona sobre la que se apoya.
//
// Va en el binario del kit porque necesita una aplicacion levantada: un
// middleware que no corre dentro de una peticion de verdad no prueba lo unico
// que importa de el, que es que la corta.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace {

// Si hay algo escuchando, con un socket pelado. Crear un cliente de Drogon
// contra un puerto muerto y soltarlo termina abortando el proceso, asi que se
// pregunta antes.
bool redisReachable() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = ::htons(static_cast<std::uint16_t>(syrax::envInt("REDIS_TEST_PORT", 6379)));
    ::inet_pton(AF_INET, syrax::env("REDIS_TEST_HOST", "127.0.0.1").c_str(), &addr.sin_addr);

    const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

}  // namespace

namespace {

// El contador vive en Redis y sobrevive al proceso, asi que cada caso cuenta
// bajo una clave propia: si no, correr la suite dos veces en el mismo minuto
// haria fallar el segundo intento sin que nada este roto.
syrax::testing::Response pedir(const std::string& cliente) {
    api().withoutHeaders().header("X-Cliente", cliente);
    return api().get("/limitado");
}

std::string unico(const std::string& prefijo) {
    return prefijo + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace

TEST_CASE("la cadena asincrona corta la peticion igual que la sincrona", "[middleware]") {
    if (!redisReachable()) {
        WARN("sin Redis en 127.0.0.1:6379, se salta");
        return;
    }

    const auto cliente = unico("corta");

    // El limite de /limitado es 3 por minuto: la cuarta no pasa.
    for (int i = 1; i <= 3; ++i) {
        INFO("peticion " << i);
        CHECK(pedir(cliente).status == 200);
    }

    CHECK(pedir(cliente).status == 429);

    api().withoutHeaders();
}

TEST_CASE("cada clave tiene su propia cuota", "[middleware]") {
    if (!redisReachable()) {
        WARN("sin Redis en 127.0.0.1:6379, se salta");
        return;
    }

    // Con margen de sobra sobre el limite de 3. El limitador falla ABIERTO a
    // proposito -si Redis no contesta, la peticion pasa-, y con ocho procesos
    // de test golpeando el mismo Redis un tropiezo suelto es posible. Cuatro
    // peticiones justas harian que ese tropiezo pareciera un fallo del
    // limitador; con diez, la cuota se agota igual.
    const auto gastado = unico("gastado");
    for (int i = 0; i < 10; ++i) pedir(gastado);
    REQUIRE(pedir(gastado).status == 429);

    // Otro cliente no paga lo que gasto el primero, que es la diferencia
    // entre un limitador y un interruptor.
    CHECK(pedir(unico("nuevo")).status == 200);

    api().withoutHeaders();
}

TEST_CASE("el 429 compartido llega con su codigo estable", "[middleware]") {
    if (!redisReachable()) {
        WARN("sin Redis en 127.0.0.1:6379, se salta");
        return;
    }

    const auto cliente = unico("codigo");

    syrax::testing::Response ultima;
    for (int i = 0; i < 5; ++i) ultima = pedir(cliente);

    REQUIRE(ultima.status == 429);
    CHECK(ultima.body.find("\"code\":\"rate_limited\"") != std::string::npos);

    api().withoutHeaders();
}

TEST_CASE("una ruta fuera del prefijo no pasa por el limitador", "[middleware]") {
    // El middleware esta montado con prefijo: si se aplicara a todo, los demas
    // tests de este binario empezarian a fallar con 429 sin motivo aparente.
    for (int i = 0; i < 10; ++i) {
        CHECK(api().get("widgets").status == 200);
    }
}
