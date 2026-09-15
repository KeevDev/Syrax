#include "test_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using testsrv::kPort;
using Fixture = testsrv::Server;

namespace {

// --------------------------------------------------------- cliente crudo
//
// Se habla el protocolo a mano por la misma razon que en http_test: usar el
// cliente de Drogon exigiria otro event loop dentro del proceso de tests.

std::string base64(const unsigned char* data, int length) {
    std::string out(4 * ((length + 2) / 3) + 1, '\0');
    const int   written =
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, length);
    out.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return out;
}

struct Client {
    int fd = -1;

    ~Client() {
        if (fd >= 0) ::close(fd);
    }

    // Devuelve false si el servidor no acepto el handshake.
    bool connect(const std::string& path) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        // Sin timeout, un fallo de protocolo cuelga el test en vez de fallarlo.
        timeval timeout{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(kPort);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;

        const unsigned char key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const std::string   encoded = base64(key, sizeof(key));

        const std::string request = "GET " + path + " HTTP/1.1\r\n"
                                    "Host: 127.0.0.1\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: " + encoded + "\r\n"
                                    "Sec-WebSocket-Version: 13\r\n\r\n";
        ::send(fd, request.data(), request.size(), 0);

        std::string response;
        char        buffer[1024];
        while (response.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) return false;
            response.append(buffer, static_cast<std::size_t>(n));
        }
        return response.find(" 101 ") != std::string::npos;
    }

    void sendText(const std::string& text) const {
        // Un frame de cliente SIEMPRE va enmascarado; el servidor descarta
        // los que no lo esten.
        std::vector<unsigned char> frame;
        frame.push_back(0x81);  // FIN + opcode texto
        frame.push_back(static_cast<unsigned char>(0x80 | text.size()));

        const unsigned char mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        frame.insert(frame.end(), mask, mask + 4);

        for (std::size_t i = 0; i < text.size(); ++i) {
            frame.push_back(static_cast<unsigned char>(text[i]) ^ mask[i % 4]);
        }
        ::send(fd, frame.data(), frame.size(), 0);
    }

    // Lee un frame de texto del servidor (sin mascara, payload corto).
    std::string receiveText() const {
        unsigned char header[2];
        if (::recv(fd, header, 2, MSG_WAITALL) != 2) return {};

        std::size_t length = header[1] & 0x7F;
        if (length == 126) {
            unsigned char extended[2];
            if (::recv(fd, extended, 2, MSG_WAITALL) != 2) return {};
            length = (static_cast<std::size_t>(extended[0]) << 8) | extended[1];
        }

        std::string payload(length, '\0');
        if (length > 0 &&
            ::recv(fd, payload.data(), length, MSG_WAITALL) != static_cast<ssize_t>(length)) {
            return {};
        }
        return payload;
    }
};

}  // namespace

TEST_CASE_METHOD(Fixture, "el handshake se completa", "[ws]") {
    Client client;
    CHECK(client.connect("/echo"));
}

TEST_CASE_METHOD(Fixture, "onOpen corre y su estado llega a onMessage", "[ws]") {
    Client client;
    REQUIRE(client.connect("/echo"));

    client.sendText("mundo");

    // El "hola" lo puso onOpen con s.set(): confirma que el contexto por
    // conexion sobrevive entre callbacks.
    CHECK(client.receiveText() == "hola:mundo");
}

TEST_CASE_METHOD(Fixture, "cada path despacha a sus propios handlers", "[ws]") {
    Client uno, dos;
    REQUIRE(uno.connect("/room"));
    REQUIRE(dos.connect("/room"));

    // Lo manda uno y lo reciben los dos: es un broadcast, no un eco.
    uno.sendText("a todos");

    CHECK(uno.receiveText() == "a todos");
    CHECK(dos.receiveText() == "a todos");
}

TEST_CASE_METHOD(Fixture, "un socket cerrado sale del room", "[ws]") {
    {
        Client temporal;
        REQUIRE(temporal.connect("/room"));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (testsrv::room().size() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(testsrv::room().size() >= 1);
    }

    // Al cerrarse el socket, onClose tiene que haberlo sacado: si no, el Room
    // acumula conexiones muertas para siempre.
    const auto before   = testsrv::room().size();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (testsrv::room().size() >= before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(testsrv::room().size() < before);
}

TEST_CASE_METHOD(Fixture, "onClose corre al desconectar", "[ws]") {
    const int before = testsrv::closeCount();
    {
        Client temporal;
        REQUIRE(temporal.connect("/echo"));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (testsrv::closeCount() == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(testsrv::closeCount() > before);
}

TEST_CASE_METHOD(Fixture, "sendJson serializa igual que una respuesta HTTP", "[ws]") {
    // No hace falta red para esto: lo que se comprueba es que el mismo struct
    // que sirve un endpoint REST sirve un mensaje de socket.
    std::string out;
    REQUIRE_FALSE(glz::write_json(testsrv::Payload{.valor = "x"}, out));
    CHECK(out == R"({"valor":"x"})");
}
