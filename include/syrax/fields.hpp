#pragma once

// El cliente movil no quiere los treinta campos del recurso: quiere el id y el
// nombre para pintar una lista. Mandarle los treinta gasta bateria del que
// mira, no del servidor.
//
//   GET /api/v1/users?fields=id,name
//   [{"id":1,"name":"Ada"}, ...]
//
// Se para aqui a proposito. Un `?fields=` plano es finito: una lista de
// nombres, separados por comas, sobre el recurso que ya devuelve la ruta. Lo
// siguiente que pide todo el mundo es anidar —`fields=user{name,posts{title}}`—
// y eso ya no es un parametro, es un lenguaje de consulta, con su parser, su
// profundidad maxima y su problema N+1. Si hace falta eso, hace falta GraphQL,
// y Syrax no va a fingir que lo es.
//
// La regla de donde se aplica, que es lo unico que no es obvio:
//
//   - Un objeto suelto  -> se filtran sus claves.
//   - Un array          -> se filtran las claves de cada elemento.
//   - Un objeto con "data" -> se filtra DENTRO de data, y el sobre se queda
//     entero. Asi una respuesta paginada no pierde `total` ni `page` porque
//     alguien pidio dos campos.
//
// Un nombre que no existe se ignora, como hacen todas las APIs que traen esto.
// Pero si NINGUNO existe, es un 400: un `?fields=nombre` donde el campo se
// llama `name` devolveria objetos vacios, y un cliente que recibe `[{},{},{}]`
// no tiene forma de saber que se equivoco.

#include <syrax/result.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syrax::fields {

struct Options {
    // El parametro de la query. Configurable porque hay APIs que ya usan otro.
    std::string param = "fields";
};

// Parte "id,name, age" en {"id","name","age"}. Los espacios se comen porque
// una URL escrita a mano los trae, y fallar por eso seria pedante.
inline std::vector<std::string> parse(std::string_view raw) {
    std::vector<std::string> out;
    std::string              actual;

    const auto push = [&] {
        const auto first = actual.find_first_not_of(" \t");
        if (first == std::string::npos) return;

        const auto last = actual.find_last_not_of(" \t");
        out.push_back(actual.substr(first, last - first + 1));
        actual.clear();
    };

    for (const char c : raw) {
        if (c == ',') push();
        else          actual += c;
    }
    push();
    return out;
}

namespace detail {

// Deja en el objeto solo las claves pedidas. Devuelve cuantas encontro, para
// distinguir "no pidio nada que exista" de "filtro bien".
inline int keepOnly(glz::generic& node, const std::vector<std::string>& wanted) {
    if (!node.is_object()) return 0;

    auto& objeto = node.get_object();

    // Se construye uno nuevo en vez de borrar del existente: el orden de las
    // claves pasa a ser el que pidio el cliente, que es mas util que el del
    // struct, y no hay que pelearse con la invalidacion de iteradores.
    std::remove_reference_t<decltype(objeto)> filtrado;

    int encontradas = 0;
    for (const auto& name : wanted) {
        const auto it = objeto.find(name);
        if (it == objeto.end()) continue;

        filtrado[name] = it->second;
        ++encontradas;
    }

    objeto = std::move(filtrado);
    return encontradas;
}

inline int applyTo(glz::generic& node, const std::vector<std::string>& wanted) {
    if (node.is_array()) {
        int encontradas = 0;
        for (auto& item : node.get_array()) encontradas += keepOnly(item, wanted);
        return encontradas;
    }
    return keepOnly(node, wanted);
}

}  // namespace detail

// Filtra un cuerpo JSON. Devuelve nullopt si no hay nada que hacer —no es
// JSON, o no hay campos pedidos— y el cuerpo nuevo si lo hubo.
//
// `unknown` sale a true cuando ninguno de los nombres pedidos existia: el que
// llama decide si eso es un 400.
inline std::optional<std::string> apply(std::string_view body,
                                        const std::vector<std::string>& wanted, bool& unknown) {
    unknown = false;
    if (wanted.empty()) return std::nullopt;

    glz::generic doc;
    if (glz::read_json(doc, body)) return std::nullopt;

    int encontradas = 0;

    // El sobre de una pagina se respeta: filtrar sus claves dejaria una
    // respuesta sin `total` ni `page`, que es justo lo que el cliente necesita
    // para pedir la siguiente.
    if (doc.is_object() && doc.contains("data")) {
        encontradas = detail::applyTo(doc.get_object()["data"], wanted);
    } else {
        encontradas = detail::applyTo(doc, wanted);
    }

    // Un array vacio no tiene claves que encontrar, y eso no es un error del
    // cliente: no hay nada que decirle.
    const bool habiaAlgo =
        !(doc.is_array() && doc.get_array().empty()) &&
        !(doc.is_object() && doc.contains("data") && doc.get_object()["data"].is_array() &&
          doc.get_object()["data"].get_array().empty());

    if (encontradas == 0 && habiaAlgo) {
        unknown = true;
        return std::nullopt;
    }

    std::string out;
    if (glz::write_json(doc, out)) return std::nullopt;
    return out;
}

}  // namespace syrax::fields
