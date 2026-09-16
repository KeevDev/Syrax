#pragma once

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace syrax {

// Carga un archivo .env al entorno del proceso. Las variables que ya existen
// ganan, para que el entorno real siempre pueda sobreescribir al archivo.
//
// No es un parser completo de dotenv: KEY=VALUE por linea, ignorando
// comentarios y comillas envolventes. Alcanza para credenciales.
inline void loadDotEnv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const auto key   = line.substr(0, eq);
        auto       value = line.substr(eq + 1);

        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }

        ::setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
    }
}

inline std::string env(const char* key, std::string fallback) {
    const char* value = std::getenv(key);
    return (value && *value) ? std::string{value} : std::move(fallback);
}

namespace detail {

// Un valor mal escrito no se ignora en silencio: quien puso DB_PORT=cinco
// quiso decir algo, y arrancar con el valor por defecto le esconde el error.
inline void badEnv(const char* key, const std::string& raw, const std::string& fallback) {
    std::cerr << "syrax: " << key << "='" << raw << "' no es valido, usando " << fallback << "\n";
}

}  // namespace detail

inline long long envInt(const char* key, long long fallback) {
    const auto raw = env(key, "");
    if (raw.empty()) return fallback;

    long long  value  = 0;
    const auto end    = raw.data() + raw.size();
    const auto parsed = std::from_chars(raw.data(), end, value);

    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        detail::badEnv(key, raw, std::to_string(fallback));
        return fallback;
    }
    return value;
}

// Acepta lo que escribe la gente en un .env, no solo "true"/"false".
inline bool envBool(const char* key, bool fallback) {
    auto raw = env(key, "");
    if (raw.empty()) return fallback;

    for (auto& c : raw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on")   return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off")  return false;

    detail::badEnv(key, raw, fallback ? "true" : "false");
    return fallback;
}

}  // namespace syrax
