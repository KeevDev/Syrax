#pragma once

// Un test que vale la pena es el que recorre las capas de verdad: ruta ->
// controller -> service -> repositorio -> SQL. Comprobar que el resource copia
// bien un campo no dice nada de si la API funciona.
//
// Esto levanta la aplicacion REAL del proyecto —la que arma bootstrap::create()—
// contra una sqlite temporal, y le habla por TCP. Dos consecuencias que valen
// la pena:
//
//   - No hay una aplicacion "de pruebas" que se desincronice de la de verdad.
//     Si una ruta no esta registrada, el test lo nota.
//   - La peticion pasa por el transporte: cabeceras, codigos, serializacion y
//     el 404 y el 422 que genera el borde. Un cliente que inyecta al router se
//     salta justo la parte donde viven esos fallos.
//
// El uso, desde el test de un proyecto generado:
//
//   #include <syrax/testing.hpp>
//   #include "bootstrap/app.hpp"
//   #include "migrations.hpp"
//
//   static syrax::testing::App app{{
//       .create     = bootstrap::create,
//       .migrations = registerMigrations,
//   }};
//
//   TEST_CASE("POST /users crea, y GET lo devuelve") {
//       app.fresh();
//
//       const auto creado = app.post("/users", R"({"name":"Ada",...})");
//       REQUIRE(creado.status == 201);
//   }

#include <syrax/app.hpp>
#include <syrax/db.hpp>
#include <syrax/env.hpp>
#include <syrax/migration.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace syrax::testing {

// ----------------------------------------------------------------- Response

struct Header {
    std::string name;
    std::string value;
};

struct Response {
    int                 status = 0;
    std::string         body;
    std::vector<Header> headers;

    bool ok() const { return status >= 200 && status < 300; }

    // Las cabeceras HTTP no distinguen mayusculas, y quien escribe el test no
    // deberia tener que acordarse de como las capitaliza Drogon.
    std::string header(std::string_view name) const {
        const auto equal = [](std::string_view a, std::string_view b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) ==
                              std::tolower(static_cast<unsigned char>(y));
                   });
        };

        for (const auto& h : headers) {
            if (equal(h.name, name)) return h.value;
        }
        return {};
    }

    // El cuerpo como el tipo que la API promete. Falla con el error de glaze
    // en vez de devolver un T vacio: un test que compara contra basura pasa
    // por razones equivocadas.
    template <typename T>
    T json() const {
        T value{};
        if (auto ec = glz::read_json(value, body)) {
            throw std::runtime_error("syrax::testing: el cuerpo no es un " +
                                     std::string{glz::name_v<T>} + ": " +
                                     glz::format_error(ec, body));
        }
        return value;
    }
};

// -------------------------------------------------------------------- App

struct Options {
    // La fabrica de la aplicacion del proyecto. Es bootstrap::create.
    std::function<syrax::App()> create;

    // El registro de migraciones del proyecto. Es registerMigrations.
    std::function<void(Migrator&)> migrations;

    // Que hacer con el log durante los tests. Por defecto calla: el ruido de
    // una linea de acceso por peticion tapa el fallo que se esta buscando.
    bool quiet = true;
};

namespace detail {

inline std::filesystem::path tempDbPath() {
    static std::atomic<int> counter{0};
    return std::filesystem::temp_directory_path() /
           ("syrax_testing_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++) + ".db");
}

}  // namespace detail

// Drogon es un singleton: un solo app().run() por proceso, y no se puede
// volver a arrancar despues de quit(). Asi que esto es uno por binario de
// test, no uno por caso. La instancia va en el ambito del archivo y los
// TEST_CASE la comparten; fresh() es lo que los aisla entre si.
class App {
public:
    // El constructor no arranca nada a proposito. La instancia vive en el
    // ambito del archivo, asi que se construye ANTES de main, y ahi las tablas
    // estaticas de Drogon —el mapa de mime types, entre otras— todavia no
    // existen: construir una respuesta ahi revienta con un out_of_range que no
    // dice de donde viene. Ademas, asi `--list-tests` no levanta un servidor ni
    // migra una base solo para enumerar casos.
    explicit App(Options options) : options_{std::move(options)} {
        if (!options_.create) {
            throw std::invalid_argument(
                "syrax::testing::App: falta .create. Es la fabrica de la aplicacion "
                "del proyecto, normalmente bootstrap::create.");
        }
    }

    ~App() {
        stop();

        if (dbFile_.empty()) return;

        std::error_code ec;
        std::filesystem::remove(dbFile_, ec);
    }

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    std::uint16_t port() {
        ensureStarted();
        return port_;
    }

    // El cliente de la base, para sembrar filas o comprobar el estado despues
    // de una peticion. Es la misma base contra la que corre la aplicacion.
    drogon::orm::DbClientPtr db() {
        ensureStarted();
        return db_;
    }

    // Deja la base como recien migrada: borra las filas de todas las tablas y
    // reinicia los autoincrementos, sin volver a correr el esquema. Va al
    // principio de cada caso que toque datos.
    //
    // No borra ni recrea el archivo a proposito: la aplicacion lo tiene
    // abierto, y cambiarlo debajo le deja un descriptor apuntando a un inodo
    // que ya no existe.
    void fresh() {
        ensureStarted();

        const auto tablas = db_->execSqlSync(
            "SELECT name FROM sqlite_master WHERE type = 'table' "
            "AND name NOT LIKE 'sqlite_%' AND name <> 'syrax_migrations'");

        for (const auto& row : tablas) {
            db_->execSqlSync("DELETE FROM " + row["name"].as<std::string>());
        }

        // sqlite guarda el contador de cada AUTOINCREMENT aqui. Sin esto, el
        // primer id de un test depende de cuantas filas creo el anterior, que
        // es justo la clase de acoplamiento que fresh() viene a quitar.
        const auto sequence = db_->execSqlSync(
            "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'sqlite_sequence'");
        if (!sequence.empty()) db_->execSqlSync("DELETE FROM sqlite_sequence");
    }

    // Una cabecera que se manda en todas las peticiones siguientes. Para el
    // Authorization de una suite que corre autenticada.
    App& header(std::string name, std::string value) {
        defaults_.push_back({std::move(name), std::move(value)});
        return *this;
    }

    // Quita las cabeceras por defecto.
    App& withoutHeaders() {
        defaults_.clear();
        return *this;
    }

    Response get(std::string_view path) { return request("GET", path); }
    Response del(std::string_view path) { return request("DELETE", path); }

    Response post(std::string_view path, std::string_view body = {}) {
        return request("POST", path, body);
    }
    Response put(std::string_view path, std::string_view body = {}) {
        return request("PUT", path, body);
    }
    Response patch(std::string_view path, std::string_view body = {}) {
        return request("PATCH", path, body);
    }

    // El mismo cuerpo, escrito desde el tipo en vez de a mano. Evita que un
    // JSON mal tecleado se lea como un fallo de la API.
    //
    // El requires no es adorno: sin el, un literal de cadena prefiere esta
    // plantilla antes que la sobrecarga de string_view —const char[N] es una
    // coincidencia exacta y el string_view una conversion— y el cuerpo sale
    // codificado DOS veces. La API contesta 422 y el test acusa al handler de
    // un fallo que estaba en el cliente.
    template <typename T>
        requires(!std::convertible_to<const T&, std::string_view>)
    Response post(std::string_view path, const T& body) {
        return request("POST", path, toJson(body));
    }
    template <typename T>
        requires(!std::convertible_to<const T&, std::string_view>)
    Response put(std::string_view path, const T& body) {
        return request("PUT", path, toJson(body));
    }
    template <typename T>
        requires(!std::convertible_to<const T&, std::string_view>)
    Response patch(std::string_view path, const T& body) {
        return request("PATCH", path, toJson(body));
    }

    // La ruta sigue la regla de las URL, para no tener que adivinar nada:
    //
    //   "widgets"          relativa   -> /api/v1/widgets
    //   "/health"          absoluta   -> /health
    //
    // Asi el caso comun no repite el prefijo de la API en cada linea, y lo que
    // cuelga fuera de ella —/health, /docs, /metrics— sigue siendo alcanzable
    // sin una segunda funcion.
    Response request(std::string_view method, std::string_view path,
                     std::string_view body = {}) {
        ensureStarted();
        return send(method, resolve(path), body);
    }

private:
    // Drogon es un singleton: un solo app().run() por proceso, y no se puede
    // volver a arrancar despues de quit(). El guardia esta aqui y no en el
    // constructor para que salte cuando se usa la segunda instancia, que es
    // donde el mensaje sirve de algo.
    // A quien apaga el handler de atexit. Nulo mientras no haya servidor.
    static App*& instance() {
        static App* value = nullptr;
        return value;
    }

    static std::atomic<bool>& running() {
        static std::atomic<bool> value{false};
        return value;
    }

    void ensureStarted() {
        if (live_) return;

        if (running().exchange(true)) {
            throw std::logic_error(
                "syrax::testing::App: ya hay una corriendo en este proceso. Drogon solo "
                "admite un servidor por proceso, asi que la instancia va en el ambito "
                "del archivo y los casos la comparten; fresh() es lo que los aisla. "
                "Si de verdad hacen falta dos aplicaciones, van en binarios distintos.");
        }
        live_ = true;

        dbFile_ = detail::tempDbPath();

        // Un proceso anterior con este mismo pid pudo morir sin limpiar. Abrir
        // encima de su archivo mezclaria las filas de los dos, y el test falla
        // una vez cada muchas sin explicacion.
        std::error_code ec;
        std::filesystem::remove(dbFile_, ec);

        redirectEnvToSqlite();
        migrate();
        start();
    }

    template <typename T>
    static std::string toJson(const T& value) {
        std::string out;
        if (auto ec = glz::write_json(value, out)) {
            throw std::runtime_error("syrax::testing: no se pudo serializar el cuerpo: " +
                                     glz::format_error(ec, out));
        }
        return out;
    }

    // El seam es el entorno, no un gancho nuevo: loadDotEnv() escribe con
    // overwrite=0, asi que lo que se ponga aqui le gana al .env del proyecto y
    // bootstrap::database() conecta a la sqlite temporal por su camino normal.
    void redirectEnvToSqlite() {
        ::setenv("DB_ENGINE", "sqlite", /*overwrite=*/1);
        ::setenv("DB_FILE", dbFile_.c_str(), /*overwrite=*/1);

        // Sin esto, un proyecto con CACHE_ENABLED=1 en su .env intenta hablar
        // con un Redis que el test no tiene por que estar corriendo.
        ::setenv("CACHE_ENABLED", "0", /*overwrite=*/1);
        ::setenv("QUEUE_DRIVER", "database", /*overwrite=*/1);

        if (options_.quiet) ::setenv("LOG_ACCESS", "0", /*overwrite=*/1);
    }

    void migrate() {
        if (!options_.migrations) return;

        Migrator migrator{db::Connection{.engine = "sqlite", .file = dbFile_.string()}};
        migrator.quiet(options_.quiet);
        options_.migrations(migrator);

        if (migrator.migrate() != 0) {
            throw std::runtime_error("syrax::testing: las migraciones fallaron sobre " +
                                     dbFile_.string());
        }
    }

    void start() {
        std::atomic<bool> ready{false};

        thread_ = std::thread([this, &ready] {
            auto app = options_.create();
            if (options_.quiet) app.quiet();
            base_ = app.base();

            // Corre con el listener ya arriba, que es cuando Drogon sabe que
            // puerto le dio el kernel.
            drogon::app().registerBeginningAdvice([this, &ready] {
                const auto listeners = drogon::app().getListeners();
                if (!listeners.empty()) port_ = listeners.front().toPort();
                ready = true;
            });

            app.run(0);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!ready || port_ == 0) {
            stop();
            throw std::runtime_error(
                "syrax::testing: la aplicacion no llego a escuchar en 10s. Suele ser "
                "que create() lanzo: correr el test con LOG_ACCESS=1 y quiet=false "
                "deja ver el error.");
        }

        // El destructor de esta clase NO basta para apagar el servidor. La
        // instancia vive en el ambito del archivo, asi que se destruye durante
        // la destruccion de estaticos, cuando Drogon ya empezo a desmontar los
        // suyos: el quit() llega tarde, trantor aborta con "forbidden to run
        // loop on threads other than event-loop thread" y llama a exit(1). El
        // binario sale con 1 aunque todos los tests hayan pasado, que en CI es
        // indistinguible de un fallo de verdad.
        //
        // atexit se registra aqui, con main ya corriendo, asi que su handler va
        // por delante del destructor de cualquier estatico anterior a main. Y
        // no ata el kit a Catch2, que es la otra forma de conseguir el mismo
        // gancho.
        instance() = this;
        std::atexit([] {
            if (auto* self = instance()) self->stop();
        });

        // El aviso de arranque no basta para dar el servidor por listo. En
        // Linux, Drogon abre UN listener por hilo de IO con SO_REUSEPORT, y el
        // kernel reparte las conexiones entre todos: una que caiga en un
        // socket que todavia no llamo a listen() se rechaza con
        // ECONNREFUSED. El sintoma es un test que falla una vez cada muchas y
        // solo cuando la maquina va cargada, que es el peor tipo de fallo.
        //
        // Se comprueba conectando de verdad en vez de dormir un rato fijo: un
        // sleep que alcanza en un portatil no alcanza en un CI cargado, y uno
        // que alcanza siempre le suma ese tiempo a cada binario de test.
        waitUntilAccepting();

        // El cliente de la base se pide despues de run(), que es cuando Drogon
        // crea los que registro bootstrap::database().
        db_ = db::client();
        if (!db_) {
            throw std::runtime_error(
                "syrax::testing: la aplicacion no registro ninguna conexion 'default'. "
                "Comprueba que create() llame a la funcion que hace db::connect().");
        }
    }

    // Conecta y cuelga, hasta que el servidor acepte de verdad. Se piden
    // varias seguidas a proposito: con SO_REUSEPORT el kernel reparte entre
    // los listeners, asi que una sola que funcione no prueba que esten todos.
    void waitUntilAccepting() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        int seguidas = 0;
        while (seguidas < 8 && std::chrono::steady_clock::now() < deadline) {
            if (probe()) {
                ++seguidas;
                continue;
            }
            seguidas = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (seguidas < 8) {
            throw std::runtime_error("syrax::testing: el servidor levanto en el puerto " +
                                     std::to_string(port_) + " pero no acepta conexiones");
        }
    }

    bool probe() const {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = ::htons(port_);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        ::close(fd);
        return ok;
    }

    void stop() {
        if (!thread_.joinable()) return;
        live_      = false;
        instance() = nullptr;

        // El cliente NO se suelta aqui. Soltarlo mientras Drogon todavia tiene
        // callbacks en vuelo -un async_run de la idempotencia, un job- termina
        // ejecutando una consulta sobre una conexion ya cerrada: sale un
        // "Connection is not ready" y el proceso aborta DESPUES de que los
        // tests hayan pasado, que en CI es indistinguible de un fallo real.
        //
        // Es el mismo motivo por el que los fixtures de sqlite y el cliente de
        // Redis de jobs_test se quedan vivos. El proceso esta terminando.
        drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
        thread_.join();
    }

    std::string resolve(std::string_view path) const {
        if (path.starts_with('/')) return std::string{path};
        return base_ + "/" + std::string{path};
    }

    // Deliberadamente tonto: sockets a pelo y Connection: close, para leer
    // hasta EOF sin parsear chunked. Los tests no deberian arrastrar el
    // cliente de Drogon, que querria su propio event loop.
    Response send(std::string_view method, const std::string& path, std::string_view body) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("syrax::testing: no se pudo abrir el socket");

        // Sin timeout, un fallo de protocolo cuelga la suite en vez de fallarla.
        timeval timeout{.tv_sec = 10, .tv_usec = 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = ::htons(port_);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            const int fallo = errno;
            ::close(fd);

            // El mensaje lleva errno porque los dos motivos habituales piden
            // cosas distintas: un ECONNREFUSED es que el servidor no esta, y un
            // EADDRNOTAVAIL es que la maquina se quedo sin puertos efimeros.
            throw std::runtime_error("syrax::testing: no se pudo conectar al puerto " +
                                     std::to_string(port_) + ": " + std::strerror(fallo));
        }

        std::string request;
        request += std::string{method} + " " + path + " HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\n";
        request += "Content-Type: application/json\r\n";
        for (const auto& h : defaults_) request += h.name + ": " + h.value + "\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        request += "Connection: close\r\n\r\n";
        request += std::string{body};

        ::send(fd, request.data(), request.size(), 0);

        std::string raw;
        char        buffer[4096];
        for (ssize_t n; (n = ::recv(fd, buffer, sizeof(buffer), 0)) > 0;) {
            raw.append(buffer, static_cast<std::size_t>(n));
        }
        ::close(fd);

        return parse(raw);
    }

    static Response parse(const std::string& raw) {
        Response response;

        const auto endOfHead = raw.find("\r\n\r\n");
        if (endOfHead == std::string::npos) return response;

        response.body = raw.substr(endOfHead + 4);

        const auto endOfStatus = raw.find("\r\n");
        if (const auto space = raw.find(' '); space != std::string::npos && space < endOfStatus) {
            response.status = std::atoi(raw.c_str() + space + 1);
        }

        for (auto at = endOfStatus + 2; at < endOfHead;) {
            const auto eol = raw.find("\r\n", at);
            const auto sep = raw.find(':', at);

            if (sep != std::string::npos && sep < eol) {
                auto value = raw.substr(sep + 1, eol - sep - 1);
                const auto first = value.find_first_not_of(" \t");
                response.headers.push_back(
                    {raw.substr(at, sep - at),
                     first == std::string::npos ? "" : value.substr(first)});
            }
            at = eol + 2;
        }
        return response;
    }

    Options                  options_;
    bool                     live_ = false;
    std::filesystem::path    dbFile_;
    std::string              base_;
    std::thread              thread_;
    std::atomic<std::uint16_t> port_{0};
    drogon::orm::DbClientPtr db_;
    std::vector<Header>      defaults_;
};

}  // namespace syrax::testing
