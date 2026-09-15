// syrax — CLI del framework.
//
// Deliberadamente sin dependencias: se compila en segundos y no arrastra
// nada al proyecto. Su unico trabajo es generar proyectos e invocar a cmake.

#include "templates.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

// install.sh graba aqui la ruta del checkout del que salio este binario,
// para que `syrax upgrade` sepa de donde recompilarse.
#ifndef SYRAX_SOURCE_DIR
#define SYRAX_SOURCE_DIR ""
#endif

constexpr std::string_view kDefaultRepo = "https://github.com/KeevDev/Syrax.git";
constexpr std::string_view kDefaultTag  = "main";

// ------------------------------------------------------------------ utilidades

std::string substitute(std::string_view tpl, std::string_view name,
                       std::string_view repo, std::string_view tag,
                       tpl::Engine engine) {
    const bool pg = (engine == tpl::Engine::Postgres);

    const std::string setup =
        pg ? "docker compose up -d" : "# sqlite no necesita nada";

    const std::pair<std::string_view, std::string> subs[] = {
        {"@NAME@",   std::string{name}},
        {"@REPO@",   std::string{repo}},
        {"@TAG@",    std::string{tag}},
        {"@ENGINE@", pg ? "PostgreSQL" : "SQLite"},
        {"@SETUP@",  setup},
        // Postgres numera los parametros; SQLite usa '?' posicional.
        {"@P1@",     pg ? "$1" : "?"},
        {"@P2@",     pg ? "$2" : "?"},
        {"@P3@",     pg ? "$3" : "?"},
    };

    std::string out{tpl};
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
    std::cout.flush();
    std::cerr.flush();

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

        auto       rest = line.substr(pos + 8);
        const auto end  = rest.find_first_of(" \t)");
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

// Pregunta el motor solo si hay terminal. En un script o CI usa el default
// sin bloquearse esperando una respuesta que nunca llega.
tpl::Engine promptEngine() {
    if (!isatty(STDIN_FILENO)) {
        std::cout << "sin terminal interactiva: usando postgres "
                     "(cambialo con --db sqlite)\n";
        return tpl::Engine::Postgres;
    }

    std::cout << "motor de base de datos:\n"
              << "  1) postgres  — trae docker-compose.yml listo\n"
              << "  2) sqlite    — sin dependencias, arranca solo\n"
              << "eleccion [1]: " << std::flush;

    std::string line;
    std::getline(std::cin, line);

    return (line == "2" || line == "sqlite") ? tpl::Engine::Sqlite
                                             : tpl::Engine::Postgres;
}

// Lee el .env del proyecto. No es un parser completo de dotenv: KEY=VALUE,
// una por linea, ignorando comentarios. Alcanza para saber a que base apuntar.
std::map<std::string, std::string> readEnv() {
    std::map<std::string, std::string> env;

    std::ifstream f(".env");
    if (!f) return env;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        env[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return env;
}

// Ejecuta en orden los .sql de un directorio.
//
// Delega en los clientes de linea de comandos (sqlite3 / psql) en vez de
// enlazar contra libpq: mantiene el CLI en 3 segundos de compilacion y sin
// dependencias. Los .sql deben ser idempotentes (IF NOT EXISTS, ON CONFLICT):
// esto corre todo cada vez, no lleva registro de lo aplicado.
int runSqlDir(const std::string& dir, const std::string& label) {
    if (!fs::is_directory(dir)) {
        std::cerr << "error: no existe " << dir << "\n";
        return 1;
    }

    const auto env    = readEnv();
    const auto engine = env.count("DB_ENGINE") ? env.at("DB_ENGINE") : "postgres";

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".sql") files.push_back(entry.path());
    }
    if (files.empty()) {
        std::cout << "no hay archivos .sql en " << dir << "\n";
        return 0;
    }
    std::ranges::sort(files);

    const bool sqlite = (engine == "sqlite" || engine == "sqlite3");

    std::string prefix;
    if (sqlite) {
        const auto file = env.count("DB_FILE") ? env.at("DB_FILE") : "app.db";
        prefix          = "sqlite3 '" + file + "' < ";
    } else {
        const auto at = [&](const char* k, const char* d) {
            return env.count(k) ? env.at(k) : std::string{d};
        };
        prefix = "psql 'postgres://" + at("DB_USER", "postgres") + ":" +
                 at("DB_PASSWORD", "postgres") + "@" + at("DB_HOST", "127.0.0.1") + ":" +
                 at("DB_PORT", "5432") + "/" + at("DB_NAME", "app") +
                 "' -v ON_ERROR_STOP=1 -q -f ";
    }

    std::cout << label << " (" << (sqlite ? "sqlite" : "postgres") << ")\n";

    for (const auto& file : files) {
        std::cout << "  " << file.filename().string() << std::flush;

        if (const int rc = run(prefix + "'" + file.string() + "'"); rc != 0) {
            std::cout << "  FALLO\n";
            return rc;
        }
        std::cout << "  ok\n";
    }
    return 0;
}

// ------------------------------------------------------------------- comandos

int cmdNew(const std::string& name, tpl::Engine engine, bool engineGiven) {
    if (name.empty()) {
        std::cerr << "error: falta el nombre.  uso: syrax new <nombre>\n";
        return 1;
    }
    if (fs::exists(name)) {
        std::cerr << "error: '" << name << "' ya existe\n";
        return 1;
    }

    if (!engineGiven) engine = promptEngine();

    const auto     repo = envOr("SYRAX_REPO", kDefaultRepo);
    const auto     tag  = envOr("SYRAX_TAG", kDefaultTag);
    const fs::path root{name};

    std::vector<std::string> written;

    for (const auto& file : tpl::kProjectFiles) {
        if (file.engine != tpl::Engine::Any && file.engine != engine) continue;

        const fs::path out = root / file.path;
        if (out.has_parent_path()) fs::create_directories(out.parent_path());

        if (!writeFile(out, substitute(file.content, name, repo, tag, engine))) {
            return 1;
        }
        written.emplace_back(file.path);
    }

    const bool pg = (engine == tpl::Engine::Postgres);

    std::cout << "\ncreado " << name << "/  (" << (pg ? "postgres" : "sqlite") << ")\n";
    for (const auto& path : written) std::cout << "  " << path << "\n";

    std::cout << "\nsiguiente paso:\n"
              << "  cd " << name << "\n";
    if (pg) {
        std::cout << "  docker compose up -d\n";
    }
    std::cout << "  syrax migrate\n"
              << "  syrax serve\n";
    return 0;
}

int cmdBuild() {
    if (!inProject()) return 1;

    const std::string gen = fs::exists("/usr/bin/ninja") ? " -G Ninja" : "";
    if (const int rc = run("cmake -S . -B build -DCMAKE_BUILD_TYPE=Release" + gen);
        rc != 0) {
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

// Recompila e reinstala desde el checkout de origen. Si el binario se
// instalo desde GitHub, clona de nuevo.
int cmdUpgrade() {
    const std::string source = SYRAX_SOURCE_DIR;

    if (!source.empty() && fs::exists(fs::path(source) / "install.sh")) {
        std::cout << "actualizando desde " << source << "\n\n";
        return run("cd '" + source + "' && git pull --ff-only 2>/dev/null; ./install.sh");
    }

    std::cout << "actualizando desde " << kDefaultRepo << "\n\n";
    const std::string tmp = "/tmp/syrax-upgrade-$$";
    return run("rm -rf " + tmp + " && git clone --depth 1 " + std::string{kDefaultRepo} +
               " " + tmp + " && cd " + tmp + " && ./install.sh && rm -rf " + tmp);
}

// Las migraciones son C++, asi que hay que compilarlas antes de correrlas.
// Es mas lento que ejecutar .sql sueltos, pero a cambio el schema builder
// valida en tiempo de compilacion y el mismo codigo sirve para postgres y
// sqlite.
int cmdMigrate(const std::string& sub) {
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

    std::cout << "\n" << sub << "\n\n";
    return run("./" + bin.string() + " " + sub);
}

int cmdSeed() {
    if (!inProject()) return 1;
    return runSqlDir("database/seeders", "cargando seeders");
}

int usage() {
    std::cout <<
        "syrax " SYRAX_VERSION "\n"
        "\n"
        "uso:\n"
        "  syrax new <nombre> [--db postgres|sqlite]   crea un proyecto\n"
        "  syrax build                                 configura y compila\n"
        "  syrax migrate                               aplica las migraciones pendientes\n"
        "  syrax migrate:rollback                      revierte la ultima\n"
        "  syrax migrate:status                        muestra cuales estan aplicadas\n"
        "  syrax db:seed                               carga database/seeders/*.sql\n"
        "  syrax serve [--port N]                      compila y levanta (default 8080)\n"
        "  syrax upgrade                               recompila e instala la ultima version\n"
        "  syrax version                               muestra la version\n"
        "\n"
        "Sin --db, `new` pregunta el motor si hay terminal interactiva.\n"
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

    if (cmd == "new") {
        std::string name;
        auto        engine      = tpl::Engine::Postgres;
        bool        engineGiven = false;

        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--db" && i + 1 < args.size()) {
                const auto& value = args[++i];
                if (value == "sqlite" || value == "sqlite3") {
                    engine = tpl::Engine::Sqlite;
                } else if (value == "postgres" || value == "postgresql") {
                    engine = tpl::Engine::Postgres;
                } else {
                    std::cerr << "error: --db acepta 'postgres' o 'sqlite', no '"
                              << value << "'\n";
                    return 1;
                }
                engineGiven = true;
            } else if (name.empty()) {
                name = args[i];
            }
        }
        return cmdNew(name, engine, engineGiven);
    }

    if (cmd == "build") return cmdBuild();
    if (cmd == "migrate")          return cmdMigrate("migrate");
    if (cmd == "migrate:rollback") return cmdMigrate("migrate:rollback");
    if (cmd == "migrate:status")   return cmdMigrate("migrate:status");
    if (cmd == "db:seed" || cmd == "seed") return cmdSeed();

    if (cmd == "serve") {
        std::string port = "8080";
        for (std::size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == "--port" || args[i] == "-p") port = args[i + 1];
        }
        return cmdServe(port);
    }

    if (cmd == "upgrade") return cmdUpgrade();

    if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        std::cout << "syrax " SYRAX_VERSION "  (build " __DATE__ ")\n";

        const std::string source = SYRAX_SOURCE_DIR;
        if (!source.empty()) std::cout << "origen: " << source << "\n";

        std::cout << "actualizar con: syrax upgrade\n";
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return usage();

    std::cerr << "error: comando desconocido '" << cmd << "'\n\n";
    usage();
    return 1;
}
