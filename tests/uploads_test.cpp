// Subida de archivos con reglas. Se prueba con multipart de verdad, escrito a
// mano, porque lo que hay que comprobar es lo que llega por el cable.

#include "testing_kit_app.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

struct Subido {
    std::string              nombre;
    std::string              extension;
    std::size_t              peso = 0;
    std::vector<std::string> fallos;
};

constexpr auto kBorde = "----syraxtest";

// El multipart mas pequeño que Drogon acepta.
std::string parte(const std::string& campo, const std::string& nombre,
                  const std::string& contenido) {
    std::string out;
    out += std::string{"--"} + kBorde + "\r\n";
    out += "Content-Disposition: form-data; name=\"" + campo + "\"; filename=\"" + nombre + "\"\r\n";
    out += "Content-Type: application/octet-stream\r\n\r\n";
    out += contenido + "\r\n";
    return out;
}

Subido subir(const std::string& cuerpo) {
    api().withoutHeaders().header("Content-Type",
                                  std::string{"multipart/form-data; boundary="} + kBorde);

    const auto response = api().post("/subir", cuerpo + "--" + kBorde + "--\r\n");
    api().withoutHeaders();

    // 201 porque es un POST: el kit no cambia el status de la ruta.
    REQUIRE(response.status == 201);
    return response.json<Subido>();
}

// Un PNG de verdad: los ocho bytes de firma y poco mas.
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "datos";

}  // namespace

TEST_CASE("un archivo valido llega entero", "[uploads]") {
    const auto out = subir(parte("avatar", "foto.png", kPng));

    CHECK(out.fallos.empty());
    CHECK(out.nombre == "foto.png");
    CHECK(out.extension == "png");
    CHECK(out.peso == kPng.size());
}

TEST_CASE("un campo obligatorio que no llega se reporta", "[uploads]") {
    const auto out = subir(parte("otro", "cosa.png", kPng));

    REQUIRE(out.fallos.size() == 1);
    CHECK(out.fallos.front().find("avatar") == 0);
    CHECK(out.fallos.front().find("no se envio") != std::string::npos);
}

TEST_CASE("pasarse de tamaño se reporta con las dos cifras", "[uploads]") {
    // El limite de la ruta es 1024 bytes.
    const auto grande = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(2000, 'x');
    const auto out    = subir(parte("avatar", "grande.png", grande));

    REQUIRE_FALSE(out.fallos.empty());
    CHECK(out.fallos.front().find("maximo") != std::string::npos);
}

TEST_CASE("un .php disfrazado de png no cuela", "[uploads]") {
    // El ataque de manual: el nombre y el tipo declarado dicen imagen, el
    // contenido no. Por eso image() mira los primeros bytes y no la cabecera.
    const auto out = subir(parte("avatar", "malo.png", "<?php system($_GET['c']); ?>"));

    REQUIRE_FALSE(out.fallos.empty());
    CHECK(out.fallos.front().find("no es una imagen") != std::string::npos);
}

TEST_CASE("los cuatro formatos de imagen se reconocen por su firma", "[uploads]") {
    CHECK(subir(parte("avatar", "a.png", std::string("\x89PNG\r\n\x1a\n", 8) + "x")).fallos.empty());
    CHECK(subir(parte("avatar", "a.jpg", std::string("\xFF\xD8\xFF", 3) + "x")).fallos.empty());
    CHECK(subir(parte("avatar", "a.gif", "GIF89a" + std::string("x"))).fallos.empty());
    CHECK(subir(parte("avatar", "a.webp", "RIFF" + std::string(4, '\0') + "WEBPx")).fallos.empty());
}

TEST_CASE("una peticion sin multipart no revienta", "[uploads]") {
    // Un POST con JSON a una ruta que espera archivos: el parser falla y no
    // hay archivos, que es un fallo de validacion y no una excepcion.
    api().withoutHeaders();
    const auto response = api().post("/subir", R"({"no":"soy multipart"})");

    REQUIRE(response.status == 201);
    CHECK_FALSE(response.json<Subido>().fallos.empty());
}
