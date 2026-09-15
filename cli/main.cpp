// syrax — CLI del framework.
//
// Deliberadamente sin dependencias: se compila en segundos y no arrastra
// nada al proyecto. Su unico trabajo es generar proyectos e invocar a cmake.

#include "templates.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kDefaultRepo = "https://github.com/KeevDev/Syrax.git";
constexpr std::string_view kDefaultTag  = "main";

// ------------------------------------------------------------------ utilidades

std::string substitute(std::string_view tpl, std::string_view name,
                       std::string_view repo, std::string_view tag) {
    std::string out{tpl};
    const std::pair<std::string_view, std::string_view> subs[] = {
        {"@NAME@", name}, {"@REPO@", repo}, {"@TAG@", tag}};

    for (const auto& [token, value] : subs) {
        for (auto pos = out.find(token); pos != std::string::npos;
             pos      = out.find(token, pos + value.size())) {
            out.replace(pos, token.size(), value);
        }
    }
    return out;
}

bool writeFile(const fs::path& path, std::string_view content) {
    std::ofstream f(path);
    if (!f) {
        std::cerr << "error: no se pudo escribir " << path << "\n";
        return false;
    }
    f << content;
    return true;
}

int run(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    return (rc == -1) ? 1 : WEXITSTATUS(rc);
}

std::string envOr(const char* key, std::string_view fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string{v} : std::string{fallback};
}

// Lee `project(<nombre>` del CMakeLists.txt para saber que binario correr.
std::string projectName() {
    std::ifstream f("CMakeLists.txt");
    if (!f) return {};

    std::string line;
    while (std::getline(f, line)) {
        const auto pos = line.find("project(");
        if (pos == std::string::npos) continue;

        auto rest = line.substr(pos + 8);
        const auto end = rest.find_first_of(" \t)");
        return (end == std::string::npos) ? rest : rest.substr(0, end);
    }
    return {};
}

bool inProject() {
    if (fs::exists("CMakeLists.txt")) return true;
    std::cerr << "error: aqui no hay un proyecto (falta CMakeLists.txt)\n"
              << "       corre el comando desde la raiz, o crea uno con: syrax new <nombre>\n";
    return false;
}

// ------------------------------------------------------------------- comandos

int cmdNew(const std::string& name) {
    if (name.empty()) {
        std::cerr << "error: falta el nombre.  uso: syrax new <nombre>\n";
        return 1;
    }
    if (fs::exists(name)) {
        std::cerr << "error: '" << name << "' ya existe\n";
        return 1;
    }

    const auto repo = envOr("SYRAX_REPO", kDefaultRepo);
    const auto tag  = envOr("SYRAX_TAG", kDefaultTag);
    const fs::path root{name};

    for (const auto& file : tpl::kProjectFiles) {
        const fs::path out = root / file.path;

        if (out.has_parent_path()) fs::create_directories(out.parent_path());
        if (!writeFile(out, substitute(file.content, name, repo, tag))) return 1;
    }

    std::cout << "creado " << name << "/\n";
    for (const auto& file : tpl::kProjectFiles) {
        std::cout << "  " << file.path << "\n";
    }
    std::cout << "\nsiguiente paso:\n"
              << "  cd " << name << " && syrax serve\n";
    return 0;
}

int cmdBuild() {
    if (!inProject()) return 1;

    const std::string gen = fs::exists("/usr/bin/ninja") ? " -G Ninja" : "";
    if (const int rc = run("cmake -S . -B build -DCMAKE_BUILD_TYPE=Release" + gen); rc != 0) {
        return rc;
    }
    return run("cmake --build build");
}

int cmdServe(const std::string& port) {
    if (const int rc = cmdBuild(); rc != 0) return rc;

    const auto name = projectName();
    if (name.empty()) {
        std::cerr << "error: no pude leer project(...) de CMakeLists.txt\n";
        return 1;
    }

    const fs::path bin = fs::path("build") / name;
    if (!fs::exists(bin)) {
        std::cerr << "error: no encontre el binario en " << bin << "\n";
        return 1;
    }

    std::cout << "\nsyrax: " << bin.string() << " escuchando en :" << port << "\n\n";
    return run("./" + bin.string() + " " + port);
}

int usage() {
    std::cout <<
        "syrax " SYRAX_VERSION "\n"
        "\n"
        "uso:\n"
        "  syrax new <nombre>     crea un proyecto nuevo\n"
        "  syrax build            configura y compila\n"
        "  syrax serve [--port N] compila y levanta el servidor (default 8080)\n"
        "  syrax version          muestra la version\n"
        "\n"
        "variables de entorno:\n"
        "  SYRAX_REPO   origen de syrax para proyectos nuevos (default: GitHub)\n"
        "  SYRAX_TAG    rama o tag a usar (default: main)\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();

    const auto& cmd = args[0];

    if (cmd == "new")   return cmdNew(args.size() > 1 ? args[1] : "");
    if (cmd == "build") return cmdBuild();

    if (cmd == "serve") {
        std::string port = "8080";
        for (std::size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == "--port" || args[i] == "-p") port = args[i + 1];
        }
        return cmdServe(port);
    }

    if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        std::cout << "syrax " SYRAX_VERSION "\n";
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return usage();

    std::cerr << "error: comando desconocido '" << cmd << "'\n\n";
    usage();
    return 1;
}
