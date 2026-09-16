// syrax — CLI del framework.
//
// Deliberadamente sin dependencias: se compila en segundos y no arrastra
// nada al proyecto. Su unico trabajo es generar proyectos e invocar a cmake.

#include "templates.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
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

// Abreviaturas. Una letra para lo que se teclea a diario; banderas largas
// para lo meta (--version, --help).
struct Alias {
    std::string_view from;
    std::string_view to;
};

constexpr Alias kAliases[] = {
    {"n", "new"},
    {"b", "build"},
    {"s", "serve"},
    {"m", "migrate"},
    {"m:r", "migrate:rollback"},   {"rollback", "migrate:rollback"},
    {"m:s", "migrate:status"},     {"status", "migrate:status"},
    {"seed", "db:seed"},           {"db:s", "db:seed"},
    {"work", "queue:work"},        {"q:w", "queue:work"},
    {"q:f", "queue:failed"},       {"q:r", "queue:retry"},
    {"t", "test"},
    {"u", "upgrade"},              {"-u", "upgrade"},      {"--upgrade", "upgrade"},
    {"v", "version"},              {"-v", "version"},      {"--version", "version"},
    {"h", "help"},                 {"-h", "help"},         {"--help", "help"},
};

std::string canonical(const std::string& cmd) {
    for (const auto& alias : kAliases) {
        if (cmd == alias.from) return std::string{alias.to};
    }
    return cmd;
}

constexpr std::string_view kDefaultRepo = "https://github.com/KeevDev/Syrax.git";
// Se fija una version en vez de apuntar a main: con un blanco movil, cada
// push al framework invalida el build de todos los proyectos y recompila
// Drogon entero. SYRAX_TAG lo sobreescribe para desarrollo.
constexpr std::string_view kDefaultTag  = "v" SYRAX_VERSION;

// ------------------------------------------------------------------ utilidades

std::string substitute(std::string_view tpl, std::string_view name,
                       std::string_view repo, std::string_view tag,
                       tpl::Engine engine) {
    const bool pg    = (engine == tpl::Engine::Postgres);
    const bool mysql = (engine == tpl::Engine::Mysql);

    const std::string setup =
        (pg || mysql) ? "docker compose up -d" : "# sqlite no necesita nada";

    const std::pair<std::string_view, std::string> subs[] = {
        {"@NAME@",     std::string{name}},
        {"@REPO@",     std::string{repo}},
        {"@TAG@",      std::string{tag}},
        {"@ENGINE@",   pg ? "PostgreSQL" : mysql ? "MySQL" : "SQLite"},
        {"@DBENGINE@", pg ? "postgres" : mysql ? "mysql" : "sqlite"},
        {"@DBUSER@",   pg ? "postgres" : mysql ? "root" : ""},
        {"@SETUP@",    setup},
        // Postgres numera los parametros; mysql y sqlite usan '?' posicional.
        {"@P1@",       pg ? "$1" : "?"},
        {"@P2@",       pg ? "$2" : "?"},
        {"@P3@",       pg ? "$3" : "?"},
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

// Los binarios del andamiaje viajan en base64 dentro del propio ejecutable:
// el CLI no baja nada de la red al generar un proyecto.
std::string decodeBase64(const std::string& texto) {
    static constexpr std::string_view kAlfabeto =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string  salida;
    std::uint32_t acumulado = 0;
    int           bits      = -8;

    for (const unsigned char c : texto) {
        const auto pos = kAlfabeto.find(static_cast<char>(c));
        if (pos == std::string_view::npos) continue;

        acumulado = (acumulado << 6) + static_cast<std::uint32_t>(pos);
        bits += 6;

        if (bits >= 0) {
            salida.push_back(static_cast<char>((acumulado >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return salida;
}

bool writeBinary(const fs::path& path, const tpl::BinaryFile& file) {
    std::string base64;
    for (std::size_t i = 0; i < file.count; ++i) base64 += file.chunks[i];

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "error: no pude escribir " << path << "\n";
        return false;
    }

    const auto bytes = decodeBase64(base64);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
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
                     "(cambialo con --db mysql|sqlite)\n";
        return tpl::Engine::Postgres;
    }

    std::cout << "motor de base de datos:\n"
              << "  1) postgres  — trae docker-compose.yml listo\n"
              << "  2) mysql     — trae docker-compose.yml listo\n"
              << "  3) sqlite    — sin dependencias, arranca solo\n"
              << "eleccion [1]: " << std::flush;

    std::string line;
    std::getline(std::cin, line);

    if (line == "2" || line == "mysql" || line == "mariadb") return tpl::Engine::Mysql;
    if (line == "3" || line == "sqlite" || line == "sqlite3") return tpl::Engine::Sqlite;
    return tpl::Engine::Postgres;
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
    const bool mysql  = (engine == "mysql" || engine == "mariadb");

    const auto at = [&](const char* k, const char* d) {
        return env.count(k) ? env.at(k) : std::string{d};
    };

    std::string prefix;
    if (sqlite) {
        prefix = "sqlite3 '" + at("DB_FILE", "app.db") + "' < ";
    } else if (mysql) {
        prefix = "mysql --host=" + at("DB_HOST", "127.0.0.1") +
                 " --port=" + at("DB_PORT", "3306") + " --user=" + at("DB_USER", "root") +
                 " --password=" + at("DB_PASSWORD", "root") + " " + at("DB_NAME", "app") + " < ";
    } else {
        prefix = "psql 'postgres://" + at("DB_USER", "postgres") + ":" +
                 at("DB_PASSWORD", "postgres") + "@" + at("DB_HOST", "127.0.0.1") + ":" +
                 at("DB_PORT", "5432") + "/" + at("DB_NAME", "app") +
                 "' -v ON_ERROR_STOP=1 -q -f ";
    }

    std::cout << label << " (" << engine << ")\n";

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

    for (const auto& file : tpl::kBinaryProjectFiles) {
        const fs::path out = root / file.path;
        if (out.has_parent_path()) fs::create_directories(out.parent_path());

        if (!writeBinary(out, file)) return 1;
        written.emplace_back(file.path);
    }

    const bool contenedor = (engine != tpl::Engine::Sqlite);

    const std::string motor = engine == tpl::Engine::Postgres ? "postgres"
                              : engine == tpl::Engine::Mysql  ? "mysql"
                                                              : "sqlite";

    std::cout << "\ncreado " << name << "/  (" << motor << ")\n";
    for (const auto& path : written) std::cout << "  " << path << "\n";

    std::cout << "\nsiguiente paso:\n"
              << "  cd " << name << "\n";
    if (contenedor) {
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

// --------------------------------------------------- servidor recargable

// El estado de la terminal es global a proposito: hay que devolverlo como
// estaba pase lo que pase —incluido un Ctrl+C— o la shell se queda sin eco.
termios               gTerminal{};
bool                  gRawMode     = false;
volatile sig_atomic_t gInterrupted = 0;

void restoreTerminal() {
    if (!gRawMode) return;
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &gTerminal);
    gRawMode = false;
}

void onInterrupt(int) { gInterrupted = 1; }

// Lee tecla a tecla en vez de linea a linea. Solo se tocan ICANON y ECHO:
// el mapeo de saltos de linea a la salida se queda como esta, para que lo
// que imprima cmake y el servidor siga viendose bien.
bool enableRawMode() {
    if (!::isatty(STDIN_FILENO)) return false;
    if (::tcgetattr(STDIN_FILENO, &gTerminal) != 0) return false;

    termios raw = gTerminal;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;   // read() no espera indefinidamente: vuelve cada
    raw.c_cc[VTIME] = 1;   // decima de segundo para mirar como esta el hijo

    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;

    gRawMode = true;
    std::atexit(restoreTerminal);
    return true;
}

pid_t spawnServer(const std::string& bin, const std::string& port) {
    std::cout.flush();

    const pid_t pid = ::fork();
    if (pid != 0) return pid;

    // El hijo no hereda stdin: las teclas son para el CLI. Si los dos leen
    // del mismo sitio, cada pulsacion se la lleva quien llegue primero.
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::close(devnull);
    }

    if (port.empty()) ::execl(bin.c_str(), bin.c_str(), static_cast<char*>(nullptr));
    else ::execl(bin.c_str(), bin.c_str(), port.c_str(), static_cast<char*>(nullptr));

    std::cerr << "error: no pude ejecutar " << bin << "\n";
    ::_exit(127);
}

void stopServer(pid_t pid) {
    if (pid <= 0) return;

    ::kill(pid, SIGTERM);

    // Drogon cierra en cuanto recibe la senal. Se le dan cinco segundos por
    // si esta terminando una peticion, y si no, se le acaba el plazo.
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(pid, nullptr, WNOHANG) == pid) return;
        ::usleep(100000);
    }

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

// inotify no es recursivo: hay que registrar cada directorio, y volver a
// hacerlo cuando aparecen nuevos. Registrar dos veces el mismo no duplica
// nada —devuelve el mismo descriptor de vigilancia—, asi que se puede
// reescanear sin llevar la cuenta.
void addWatches(int fd) {
    if (fd < 0) return;

    constexpr std::uint32_t kMask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;

    ::inotify_add_watch(fd, ".", kMask);   // por el CMakeLists de la raiz

    for (const char* root : {"src", "database"}) {
        if (!fs::exists(root)) continue;

        ::inotify_add_watch(fd, root, kMask);

        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
            if (entry.is_directory(ec)) ::inotify_add_watch(fd, entry.path().c_str(), kMask);
        }
    }
}

// Un editor guarda de muchas formas —escribir en el sitio, o escribir un
// temporal y renombrarlo—, y ademas toca archivos que no compilan nada.
bool looksLikeSource(std::string_view name) {
    for (const auto* ext : {".cpp", ".hpp", ".cc", ".h", ".hxx", ".cxx"}) {
        if (name.ends_with(ext)) return true;
    }
    return name == "CMakeLists.txt";
}

// Devuelve si algo que importa cambio, y vacia la cola en cualquier caso.
bool drainEvents(int fd) {
    if (fd < 0) return false;

    alignas(inotify_event) char buffer[4096];
    bool                        interesa = false;

    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got <= 0) return interesa;

        for (ssize_t offset = 0; offset < got;) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);

            if (event->len > 0 && looksLikeSource(event->name)) interesa = true;
            offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
        }
    }
}

int serveWithReload(const std::string& bin, const std::string& port, bool watch) {
    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);

    int inotify = -1;
    if (watch) {
        inotify = ::inotify_init1(IN_NONBLOCK);
        addWatches(inotify);
    }

    pid_t child = spawnServer(bin, port);

    std::cout << (inotify >= 0 ? "\n  vigilando src/ y database/   "
                               : "\n  ")
              << "r  recompila y reinicia      q  salir\n\n";

    // Recompila y cambia el binario en caliente. Devuelve el pid que toca
    // seguir vigilando: si no compila, el de siempre.
    const auto reload = [&](pid_t current) {
        std::cout << "\nrecompilando...\n";

        if (const int rc = cmdBuild(); rc != 0) {
            // El servidor anterior sigue vivo: un error de compilacion no te
            // deja sin servidor, que es justo cuando mas falta hace.
            std::cout << "\nno compila; sigue corriendo el binario anterior.\n\n";
            return current;
        }

        stopServer(current);
        std::cout << "\nreiniciando...\n\n";
        return spawnServer(bin, port);
    };

    // Un guardado dispara varios eventos, y guardar tres archivos seguidos no
    // deberia ser tres compilaciones: se espera a que amaine.
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> pendiente;

    for (;;) {
        if (gInterrupted) {
            stopServer(child);
            restoreTerminal();
            std::cout << "\n";
            return 0;
        }

        // Si el servidor se cayo solo —un puerto ocupado, un fallo al
        // arrancar— no tiene sentido seguir escuchando.
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child) {
            restoreTerminal();
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }

        pollfd fds[2]  = {{STDIN_FILENO, POLLIN, 0}, {inotify, POLLIN, 0}};
        const int nfds = inotify >= 0 ? 2 : 1;
        ::poll(fds, static_cast<nfds_t>(nfds), 100);

        if (inotify >= 0 && (fds[1].revents & POLLIN) && drainEvents(inotify)) {
            pendiente = Clock::now() + std::chrono::milliseconds{250};
        }

        if (pendiente && Clock::now() >= *pendiente) {
            pendiente.reset();
            child = reload(child);

            // Lo que se guardo mientras compilaba no cuenta como cambio nuevo,
            // y los directorios recien creados hay que registrarlos.
            drainEvents(inotify);
            addWatches(inotify);
            continue;
        }

        if (!(fds[0].revents & POLLIN)) continue;

        char          key = 0;
        const ssize_t got = ::read(STDIN_FILENO, &key, 1);
        if (got <= 0) continue;

        if (key == 'q' || key == 'Q') {
            stopServer(child);
            restoreTerminal();
            std::cout << "\n";
            return 0;
        }

        if (key == 'r' || key == 'R') {
            child = reload(child);
            drainEvents(inotify);
            addWatches(inotify);
        }
    }
}

int cmdServe(const std::string& port, bool watch = true) {
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

    // Sin terminal interactiva (un pipe, un contenedor, CI) no hay a quien
    // escuchar: se ejecuta y se espera, como siempre.
    if (!enableRawMode()) return run("./" + bin.string() + " " + port);

    return serveWithReload("./" + bin.string(), port, watch);
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

int cmdTest() {
    if (!inProject()) return 1;

    // Los tests del proyecto viven detras de una opcion para que el build de
    // todos los dias no arrastre Catch2. Encenderla aqui es lo que hace que
    // `syrax test` funcione sin que tengas que acordarte del -D.
    const std::string gen = fs::exists("/usr/bin/ninja") ? " -G Ninja" : "";
    if (const int rc = run("cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
                           " -DSYRAX_PROJECT_TESTS=ON" + gen);
        rc != 0) {
        return rc;
    }

    if (const int rc = run("cmake --build build"); rc != 0) return rc;

    return run("ctest --test-dir build --output-on-failure");
}

// Lee un KEY=VALUE del .env del proyecto. No es un parser completo; es el
// mismo subconjunto que carga syrax::db::loadDotEnv.
std::string dotEnv(const std::string& key, std::string fallback = {}) {
    std::ifstream file(".env");
    if (!file) return fallback;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos || line.substr(0, eq) != key) continue;

        auto value = line.substr(eq + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return fallback;
}

// drogon_ctl no viene con syrax: hay que construirlo desde el Drogon que ya
// bajo FetchContent. Tarda, asi que se guarda en el proyecto y se reutiliza.
fs::path findOrBuildCtl() {
    if (run("command -v drogon_ctl > /dev/null 2>&1") == 0) return "drogon_ctl";

    const fs::path cached = "build/_ctl/drogon_ctl/drogon_ctl";
    if (fs::exists(cached)) return fs::absolute(cached);

    const fs::path source = "build/_deps/drogon-src";
    if (!fs::exists(source)) {
        std::cerr << "error: falta " << source << "\n"
                  << "       corre `syrax build` primero, que es quien baja Drogon.\n";
        return {};
    }

    std::cout << "drogon_ctl no esta compilado. Construyendolo una sola vez;\n"
              << "esto tarda varios minutos porque arrastra Drogon entero.\n\n";

    const std::string configure =
        "cmake -S " + source.string() + " -B build/_ctl -G Ninja "
        "-DCMAKE_BUILD_TYPE=Release -DBUILD_CTL=ON -DBUILD_EXAMPLES=OFF "
        "-DBUILD_TESTING=OFF -DBUILD_ORM=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.10";

    if (run(configure) != 0) return {};
    if (run("cmake --build build/_ctl --target drogon_ctl") != 0) return {};

    if (!fs::exists(cached)) {
        std::cerr << "error: drogon_ctl no aparecio donde se esperaba\n";
        return {};
    }
    return fs::absolute(cached);
}

// Genera el modelo de Drogon (el que usa Mapper<T>) para una tabla concreta.
//
// No se hace en `syrax new` a proposito: drogon_ctl lee el esquema de la base,
// que en ese momento todavia no existe. Y son ~270 lineas por columna, que no
// se le meten a nadie sin pedirlas.
int cmdMakeModel(const std::string& table) {
    if (!inProject()) return 1;

    if (table.empty()) {
        std::cerr << "uso: syrax make:model <tabla>\n";
        return 1;
    }

    const auto engine = dotEnv("DB_ENGINE", "postgres");
    const bool sqlite = (engine == "sqlite" || engine == "sqlite3");
    const bool mysql  = (engine == "mysql" || engine == "mariadb");

    const fs::path dir = "src/models/generated";
    fs::create_directories(dir);

    std::ofstream config(dir / "model.json");
    if (!config) {
        std::cerr << "error: no se pudo escribir " << (dir / "model.json") << "\n";
        return 1;
    }

    config << "{\n";
    if (sqlite) {
        config << "    \"rdbms\": \"sqlite3\",\n"
               << "    \"filename\": \"" << dotEnv("DB_FILE", "app.db") << "\",\n";
    } else if (mysql) {
        config << "    \"rdbms\": \"mysql\",\n"
               << "    \"host\": \"" << dotEnv("DB_HOST", "127.0.0.1") << "\",\n"
               << "    \"port\": " << dotEnv("DB_PORT", "3306") << ",\n"
               << "    \"dbname\": \"" << dotEnv("DB_NAME", "app") << "\",\n"
               << "    \"user\": \"" << dotEnv("DB_USER", "root") << "\",\n"
               << "    \"password\": \"" << dotEnv("DB_PASSWORD", "") << "\",\n";
    } else {
        config << "    \"rdbms\": \"postgresql\",\n"
               << "    \"host\": \"" << dotEnv("DB_HOST", "127.0.0.1") << "\",\n"
               << "    \"port\": " << dotEnv("DB_PORT", "5432") << ",\n"
               << "    \"dbname\": \"" << dotEnv("DB_NAME", "app") << "\",\n"
               << "    \"schema\": \"public\",\n"
               << "    \"user\": \"" << dotEnv("DB_USER", "postgres") << "\",\n"
               << "    \"password\": \"" << dotEnv("DB_PASSWORD", "") << "\",\n";
    }
    config << "    \"tables\": [\"" << table << "\"],\n"
           << "    \"convert\": { \"enabled\": false },\n"
           << "    \"relationships\": { \"enabled\": false },\n"
           << "    \"restful_api_controllers\": { \"enabled\": false }\n"
           << "}\n";
    config.close();

    const auto ctl = findOrBuildCtl();
    if (ctl.empty()) return 1;

    std::cout << "generando el modelo de '" << table << "' desde la base...\n";

    // drogon_ctl pregunta antes de sobreescribir; se responde que si.
    if (run("yes y 2>/dev/null | " + ctl.string() + " create model " + dir.string()) != 0) {
        std::cerr << "\nerror: drogon_ctl fallo. Comprueba que la base este levantada y\n"
                  << "       que " << (dir / "model.json") << " tenga las credenciales correctas.\n";
        return 1;
    }

    std::cout << "\nlisto. Para usarlo:\n"
              << "  #include <drogon/orm/CoroMapper.h>\n"
              << "  #include \"models/generated/" << table << ".h\"   (o el nombre que genero)\n\n"
              << "  CoroMapper<...> mapper(syrax::db::client());\n";
    return 0;
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
        "  new <nombre> [--db postgres|mysql|sqlite]  n  crea un proyecto\n"
        "  build                                 b    configura y compila\n"
        "  serve [--port N] [--no-watch]         s    levanta y recompila al guardar; q sale\n"
        "\n"
        "  migrate                               m    aplica las migraciones pendientes\n"
        "  migrate:rollback                      m:r  revierte la ultima\n"
        "  migrate:status                        m:s  muestra cuales estan aplicadas\n"
        "  db:seed                               seed carga database/seeders/*.sql\n"
        "  make:model <tabla>                    m:m  genera el modelo de Drogon (Mapper<T>)\n"
        "\n"
        "  queue:work                            work corre los jobs encolados\n"
        "  queue:failed                          q:f  lista los que se rindieron\n"
        "  queue:retry                           q:r  devuelve los fallidos a la cola\n"
        "  test                                  t    compila y corre los tests\n"
        "\n"
        "  upgrade                               -u   recompila e instala la ultima version\n"
        "  version                               -v   muestra la version\n"
        "  help                                  -h   esta ayuda\n"
        "\n"
        "Sin --db, `new` pregunta el motor si hay terminal interactiva.\n"
        "\n"
        "variables de entorno:\n"
        "  SYRAX_REPO   origen de syrax para proyectos nuevos (default: GitHub)\n"
        "  SYRAX_TAG    rama o tag a usar (default: v" SYRAX_VERSION ")\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();

    const auto cmd = canonical(args[0]);

    if (cmd == "new") {
        std::string name;
        auto        engine      = tpl::Engine::Postgres;
        bool        engineGiven = false;

        for (std::size_t i = 1; i < args.size(); ++i) {
            if ((args[i] == "--db" || args[i] == "-d") && i + 1 < args.size()) {
                const auto& value = args[++i];
                if (value == "sqlite" || value == "sqlite3") {
                    engine = tpl::Engine::Sqlite;
                } else if (value == "mysql" || value == "mariadb") {
                    engine = tpl::Engine::Mysql;
                } else if (value == "postgres" || value == "postgresql" || value == "pg") {
                    engine = tpl::Engine::Postgres;
                } else {
                    std::cerr << "error: --db acepta 'postgres', 'mysql' o 'sqlite', no '"
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

    if (cmd == "build")            return cmdBuild();
    if (cmd == "migrate")          return cmdMigrate("migrate");
    if (cmd == "migrate:rollback") return cmdMigrate("migrate:rollback");
    if (cmd == "migrate:status")   return cmdMigrate("migrate:status");
    if (cmd == "db:seed")          return cmdSeed();

    // El worker y el resto de la cola corren dentro del binario del proyecto,
    // que es quien conoce los jobs. El CLI solo compila y delega.
    if (cmd == "queue:work")       return cmdMigrate("queue:work");
    if (cmd == "queue:failed")     return cmdMigrate("queue:failed");
    if (cmd == "queue:retry")      return cmdMigrate("queue:retry");
    if (cmd == "make:model" || cmd == "m:m") {
        return cmdMakeModel(argc > 2 ? argv[2] : "");
    }
    if (cmd == "test")             return cmdTest();
    if (cmd == "upgrade")          return cmdUpgrade();
    if (cmd == "help")             return usage();

    if (cmd == "serve") {
        // Sin --port no se pasa argumento ninguno: el binario resuelve
        // APP_PORT desde el .env. Pasarle un 8080 por defecto dejaba muerta
        // esa variable, porque el argumento siempre le gana al archivo.
        std::string port;
        bool        watch = true;

        for (std::size_t i = 1; i < args.size(); ++i) {
            if ((args[i] == "--port" || args[i] == "-p") && i + 1 < args.size()) {
                port = args[i + 1];
            }
            if (args[i] == "--no-watch") watch = false;
        }
        return cmdServe(port, watch);
    }

    if (cmd == "version") {
        std::cout << "syrax " SYRAX_VERSION "  (build " __DATE__ ")\n";

        const std::string source = SYRAX_SOURCE_DIR;
        if (!source.empty()) std::cout << "origen: " << source << "\n";

        std::cout << "actualizar con: syrax upgrade\n";
        return 0;
    }

    std::cerr << "error: comando desconocido '" << args[0] << "'\n\n";
    usage();
    return 1;
}
