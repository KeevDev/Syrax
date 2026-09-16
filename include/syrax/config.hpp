#pragma once

// Hoy la configuracion de un proyecto es una colección de `env("DB_POOL", "4")`
// repartidos por bootstrap. Eso tiene tres agujeros, y los tres se pagan tarde:
//
//   - Todo es string. `envInt` convierte, pero cada sitio decide por su cuenta
//     que hacer si el valor no vale.
//   - Un nombre mal escrito no falla: `env("DB_POOOL", "4")` devuelve el 4 de
//     siempre y la aplicacion arranca con el pool por defecto, en silencio.
//   - Nadie valida. `DB_POOL=0` es aceptable para el compilador y absurdo para
//     el programa; se descubre cuando la primera consulta se queda esperando.
//
// La alternativa es declarar la configuracion como un tipo, llenarlo del
// entorno y **validarlo al arrancar**:
//
//   struct Config {
//       std::string dbEngine = "postgres";
//       int         dbPool   = 4;
//       bool        docs     = true;
//
//       static auto rules() {
//           return syrax::rules(syrax::field(&Config::dbPool).range(1, 64),
//                               syrax::field(&Config::dbEngine).notEmpty());
//       }
//   };
//
//   const auto cfg = syrax::config::load<Config>();
//
// El nombre de cada variable sale del campo: `dbPool` lee `DB_POOL`. No hay
// macro ni lista que mantener al lado, asi que no se pueden desincronizar.
//
// Y si algo no cuadra, `load()` lanza con el nombre del campo, la variable y el
// valor que traia. Es a proposito: un arranque que falla con una frase se
// arregla en un minuto, y uno que arranca con el valor equivocado se convierte
// en una noche de depuracion tres semanas despues.
//
// Esto NO sustituye a `env()`. Un proyecto pequeño con cuatro variables no
// necesita el tipo; el tipo empieza a pagar cuando son veinte y alguna importa.

#include <syrax/env.hpp>
#include <syrax/result.hpp>
#include <syrax/validation.hpp>

#include <glaze/glaze.hpp>

#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace syrax::config {

// `dbPool` -> `DB_POOL`, `apiBase` -> `API_BASE`, `port` -> `PORT`.
//
// La conversion es la convencion de toda la casa —el .env que genera `syrax
// new` ya se escribe asi— y va en una sola direccion: del campo a la variable.
// Al reves nunca hace falta, que es lo que la mantiene sin ambigüedades.
inline std::string envName(std::string_view field) {
    std::string out;
    out.reserve(field.size() + 4);

    for (std::size_t i = 0; i < field.size(); ++i) {
        const auto c = static_cast<unsigned char>(field[i]);

        // El guion bajo va ANTES de la mayuscula, no despues de la minuscula:
        // asi `dbPool` da DB_POOL y no DB__POOL cuando ya venia separado.
        if (std::isupper(c) && i > 0 && field[i - 1] != '_') out += '_';
        out += static_cast<char>(std::toupper(c));
    }
    return out;
}

// Lo que no cuadro al leer el entorno. Se juntan todos antes de lanzar: quien
// tiene tres variables mal quiere verlas de una, no arrancar tres veces.
struct Problem {
    std::string field;     // dbPool
    std::string variable;  // DB_POOL
    std::string value;     // lo que traia, vacio si el fallo es de una regla
    std::string message;
};

class Invalid : public std::runtime_error {
public:
    explicit Invalid(std::vector<Problem> problems)
        : std::runtime_error{describe(problems)}, problems_{std::move(problems)} {}

    const std::vector<Problem>& problems() const { return problems_; }

private:
    static std::string describe(const std::vector<Problem>& problems) {
        std::string out = "syrax: la configuracion no es valida\n";

        for (const auto& p : problems) {
            out += "\n  " + p.variable;
            if (!p.value.empty()) out += "='" + p.value + "'";
            out += "\n    " + p.message;
        }
        return out + "\n";
    }

    std::vector<Problem> problems_;
};

namespace detail {

// Un campo se queda con su valor por defecto cuando la variable no esta. Es
// deliberado: el valor por defecto ya esta escrito en el struct, que es donde
// alguien lo va a buscar, y no repetido en una llamada a env().
template <typename F>
void assign(F& field, const std::string& raw, const std::string& name,
            const std::string& variable, std::vector<Problem>& problems) {
    const auto fail = [&](std::string expected) {
        problems.push_back({name, variable, raw, "no es " + std::move(expected)});
    };

    if constexpr (std::is_same_v<F, std::string>) {
        field = raw;
    } else if constexpr (std::is_same_v<F, bool>) {
        auto lower = raw;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
            field = true;
        } else if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
            field = false;
        } else {
            fail("un booleano (1/0, true/false, yes/no, on/off)");
        }
    } else if constexpr (std::is_integral_v<F>) {
        try {
            const auto value = std::stoll(raw);
            const auto shrunk = static_cast<F>(value);

            // stoll acepta 70000 para un unsigned short y lo trunca a 4464.
            // Un puerto que no es el que pediste es peor que un arranque que
            // falla, asi que la conversion con perdida es un error.
            if (static_cast<long long>(shrunk) != value) {
                fail("representable en este campo (se sale del rango del tipo)");
            } else {
                field = shrunk;
            }
        } catch (const std::exception&) {
            fail("un entero");
        }
    } else if constexpr (std::is_floating_point_v<F>) {
        try {
            field = static_cast<F>(std::stod(raw));
        } catch (const std::exception&) {
            fail("un numero");
        }
    } else {
        static_assert(sizeof(F) == 0,
                      "syrax::config: solo se leen del entorno string, bool, enteros y "
                      "flotantes. Un campo compuesto no tiene una representacion obvia "
                      "en una variable, y adivinar una seria inventar un formato mas.");
    }
}

}  // namespace detail

// Llena el tipo desde el entorno, sin validar. Existe aparte de load() para
// los tests, que a veces quieren ver el resultado crudo.
template <typename T>
T read(std::vector<Problem>& problems) {
    T value{};

    constexpr auto keys  = glz::reflect<T>::keys;
    constexpr auto kSize = glz::reflect<T>::size;

    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            const std::string name     = std::string{keys[I]};
            const std::string variable = envName(name);

            const char* raw = std::getenv(variable.c_str());
            if (!raw || !*raw) return;  // sin variable, gana el valor por defecto

            // El mismo acceso por indice que usa db::fromRow: to_tie da las
            // referencias a los campos en el orden de los keys.
            detail::assign(glz::get_member(value, glz::get<I>(glz::to_tie(value))),
                           std::string{raw}, name, variable, problems);
        }(), ...);
    }(std::make_index_sequence<kSize>{});

    return value;
}

// Lee el entorno y aplica las reglas del tipo. Lanza `Invalid` con TODOS los
// problemas juntos si algo no cuadra.
//
// Se llama al arrancar, antes de conectar nada: el momento de descubrir que
// DB_POOL es "cuatro" es antes de abrir el pool, no en la primera consulta.
template <typename T>
T load() {
    loadDotEnv();

    std::vector<Problem> problems;
    T                    value = read<T>(problems);

    // Las reglas solo corren sobre lo que se leyo bien. Validar el rango de un
    // campo que ni siquiera se pudo convertir daria dos quejas del mismo fallo.
    if (problems.empty()) {
        for (const auto& invalid : validate(value)) {
            problems.push_back({invalid.field, envName(invalid.field), {}, invalid.message});
        }
    }

    if (!problems.empty()) throw Invalid{std::move(problems)};
    return value;
}

}  // namespace syrax::config
