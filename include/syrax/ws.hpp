#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <glaze/glaze.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace syrax {

// Una conexion abierta. Envuelve la de Drogon para que el codigo de
// aplicacion no tenga que nombrarla, igual que Request hace con HttpRequest.
class Socket {
public:
    explicit Socket(drogon::WebSocketConnectionPtr connection)
        : connection_{std::move(connection)} {}

    void send(std::string_view text) const { connection_->send(text.data(), text.size()); }

    // Serializa con Glaze, como las respuestas HTTP: el mismo struct sirve
    // para un endpoint REST y para un mensaje de socket.
    template <typename T>
    void sendJson(const T& value) const {
        std::string out;
        if (glz::write_json(value, out)) return;
        send(out);
    }

    void close() const { connection_->shutdown(); }
    bool open() const  { return !connection_->disconnected(); }

    std::string ip() const { return connection_->peerAddr().toIp(); }

    // Estado por conexion. Lo tipico es guardar aqui el usuario que quedo
    // autenticado en el handshake.
    void set(const std::string& key, std::string value) const {
        attributes()[key] = std::move(value);
    }
    std::string get(const std::string& key) const {
        const auto& all = attributes();
        const auto  it   = all.find(key);
        return it == all.end() ? std::string{} : it->second;
    }
    bool has(const std::string& key) const { return attributes().contains(key); }

    const drogon::WebSocketConnectionPtr& drogon() const { return connection_; }

    // Dos Socket que envuelven la misma conexion son el mismo socket: es lo
    // que permite guardarlos en un Room y sacarlos despues.
    bool operator==(const Socket& other) const { return connection_ == other.connection_; }

private:
    std::unordered_map<std::string, std::string>& attributes() const;

    drogon::WebSocketConnectionPtr connection_;
};

// Que hacer en cada momento de la vida de una conexion. Los tres son
// opcionales: un endpoint que solo emite no necesita onMessage.
struct SocketHandlers {
    std::function<void(const Socket&)>                   onOpen;
    std::function<void(const Socket&, std::string_view)> onMessage;
    std::function<void(const Socket&)>                   onClose;
};

// ------------------------------------------------------------------ Room

// Un grupo de conexiones al que se puede emitir de una vez. Es lo minimo que
// hace falta para que los WebSockets sirvan de algo: sin esto, cada usuario
// tendria que rehacer el registro de conexiones y su sincronizacion.
//
// Drogon reparte las conexiones entre varios event loops, asi que el acceso
// va bajo mutex: join, leave y broadcast corren en hilos distintos.
class Room {
public:
    void join(const Socket& socket) {
        std::lock_guard lock{mutex_};
        members_.insert(socket.drogon());
    }

    void leave(const Socket& socket) {
        std::lock_guard lock{mutex_};
        members_.erase(socket.drogon());
    }

    std::size_t size() const {
        std::lock_guard lock{mutex_};
        return members_.size();
    }

    void broadcast(std::string_view text) const {
        // Se copia la lista y se suelta el lock antes de enviar: send() puede
        // encolar en otro event loop, y sostener el mutex mientras tanto
        // serializaria todas las conexiones contra la mas lenta.
        for (const auto& member : snapshot()) {
            if (!member->disconnected()) member->send(text.data(), text.size());
        }
    }

    template <typename T>
    void broadcastJson(const T& value) const {
        std::string out;
        if (glz::write_json(value, out)) return;
        broadcast(out);
    }

private:
    std::vector<drogon::WebSocketConnectionPtr> snapshot() const {
        std::lock_guard lock{mutex_};
        return {members_.begin(), members_.end()};
    }

    mutable std::mutex                                     mutex_;
    std::unordered_set<drogon::WebSocketConnectionPtr>     members_;
};

namespace detail {

// El estado que Syrax cuelga de cada conexion: a que ruta pertenece (para
// despachar) y los atributos del usuario.
struct SocketState {
    std::string                                  path;
    std::unordered_map<std::string, std::string> attributes;
};

inline std::unordered_map<std::string, SocketHandlers>& wsRegistry() {
    static std::unordered_map<std::string, SocketHandlers> registry;
    return registry;
}

// Drogon instancia UN controlador por nombre de clase y le manda todas las
// rutas registradas con ese nombre. Por eso este puente no puede cerrar sobre
// los handlers: tiene que despachar por path en tiempo de ejecucion, y el
// path se guarda en el contexto de la conexion en el handshake, que es el
// unico momento en que hay un HttpRequest a mano.
class WsBridge : public drogon::WebSocketController<WsBridge> {
public:
    void handleNewConnection(const drogon::HttpRequestPtr&         request,
                             const drogon::WebSocketConnectionPtr& connection) override {
        auto state   = std::make_shared<SocketState>();
        state->path  = request->path();
        connection->setContext(state);

        if (const auto* handlers = lookup(state->path); handlers && handlers->onOpen) {
            handlers->onOpen(Socket{connection});
        }
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
                          std::string&&                         message,
                          const drogon::WebSocketMessageType&    type) override {
        // Ping, pong y close los gestiona Drogon; aqui solo interesa lo que
        // mando la aplicacion del otro lado.
        if (type != drogon::WebSocketMessageType::Text &&
            type != drogon::WebSocketMessageType::Binary) {
            return;
        }

        if (const auto* handlers = lookup(pathOf(connection)); handlers && handlers->onMessage) {
            handlers->onMessage(Socket{connection}, message);
        }
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) override {
        if (const auto* handlers = lookup(pathOf(connection)); handlers && handlers->onClose) {
            handlers->onClose(Socket{connection});
        }
    }

    static void initPathRouting() {}

    // registerSelf__ es protected en la base; esto lo expone para que App
    // pueda registrar una ruta sin la macro WS_PATH_ADD, que exige conocer
    // todas las rutas en tiempo de compilacion.
    static void addPath(const std::string& path) { registerSelf__(path, {}); }

private:
    static std::string pathOf(const drogon::WebSocketConnectionPtr& connection) {
        if (!connection->hasContext()) return {};
        return connection->getContextRef<SocketState>().path;
    }

    static const SocketHandlers* lookup(const std::string& path) {
        const auto& registry = wsRegistry();
        const auto  it       = registry.find(path);
        return it == registry.end() ? nullptr : &it->second;
    }
};

}  // namespace detail

inline std::unordered_map<std::string, std::string>& Socket::attributes() const {
    return connection_->getContextRef<detail::SocketState>().attributes;
}

}  // namespace syrax
