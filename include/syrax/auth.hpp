#pragma once

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <glaze/glaze.hpp>
#include <syrax/middleware.hpp>
#include <syrax/result.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace syrax::auth {

namespace detail {

inline std::string base64UrlEncode(const unsigned char* data, std::size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string out;
    out.reserve((size + 2) / 3 * 4);

    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16) |
                                    (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0) |
                                    (i + 2 < size ? static_cast<std::uint32_t>(data[i + 2]) : 0);

        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        if (i + 1 < size) out += kAlphabet[(chunk >> 6) & 0x3F];
        if (i + 2 < size) out += kAlphabet[chunk & 0x3F];
    }
    return out;  // sin relleno, como manda JWT
}

inline std::string base64UrlEncode(const std::string& text) {
    return base64UrlEncode(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

inline std::string base64UrlDecode(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };

    std::string   out;
    std::uint32_t buffer = 0;
    int           bits   = 0;

    for (const char c : text) {
        const int decoded = value(c);
        if (decoded < 0) continue;

        buffer = (buffer << 6) | static_cast<std::uint32_t>(decoded);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

inline std::string hmacSha256(const std::string& key, const std::string& message) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;

    ::HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest,
           &length);

    return base64UrlEncode(digest, length);
}

// Compara sin filtrar informacion por el tiempo de ejecucion.
inline bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;

    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

inline std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace detail

// Los campos estandar de un JWT mas el sujeto. Para datos propios usa claims.
struct Claims {
    std::string  sub;         // a quien identifica el token
    std::int64_t exp = 0;     // expira en (epoch seconds)
    std::int64_t iat = 0;     // emitido en
    std::string  role;        // opcional, util para autorizacion simple
};

// Firma un JWT con HS256.
//
// HS256 usa un secreto compartido: sirve cuando el mismo servicio emite y
// verifica. Para que terceros verifiquen sin poder firmar hace falta RS256,
// que no esta implementado.
inline std::string sign(const Claims& claims, const std::string& secret,
                        std::chrono::seconds lifetime = std::chrono::hours{24}) {
    Claims payload = claims;
    payload.iat    = detail::now();
    payload.exp    = payload.iat + lifetime.count();

    std::string json;
    (void)glz::write_json(payload, json);

    const std::string header = R"({"alg":"HS256","typ":"JWT"})";
    const std::string body =
        detail::base64UrlEncode(header) + "." + detail::base64UrlEncode(json);

    return body + "." + detail::hmacSha256(secret, body);
}

// Verifica firma y expiracion. Devuelve nullopt si el token no es de fiar.
inline std::optional<Claims> verify(const std::string& token, const std::string& secret) {
    const auto firstDot = token.find('.');
    if (firstDot == std::string::npos) return std::nullopt;

    const auto secondDot = token.find('.', firstDot + 1);
    if (secondDot == std::string::npos) return std::nullopt;

    const auto body      = token.substr(0, secondDot);
    const auto signature = token.substr(secondDot + 1);

    if (!detail::constantTimeEquals(signature, detail::hmacSha256(secret, body))) {
        return std::nullopt;
    }

    Claims     claims;
    const auto json = detail::base64UrlDecode(
        token.substr(firstDot + 1, secondDot - firstDot - 1));

    if (glz::read_json(claims, json)) return std::nullopt;
    if (claims.exp != 0 && claims.exp < detail::now()) return std::nullopt;

    return claims;
}

// --------------------------------------------------------- contrasenas

// Hashea una contrasena con PBKDF2-HMAC-SHA256 y sal aleatoria.
//
// PBKDF2 esta en OpenSSL, que ya es dependencia. Argon2id resiste mejor el
// cracking con GPU, pero anadiria una dependencia nueva.
inline std::string hashPassword(const std::string& password, int iterations = 600000) {
    unsigned char salt[16];
    if (::RAND_bytes(salt, sizeof(salt)) != 1) return {};

    unsigned char derived[32];
    ::PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt,
                        sizeof(salt), iterations, EVP_sha256(), sizeof(derived), derived);

    return "pbkdf2$" + std::to_string(iterations) + "$" +
           detail::base64UrlEncode(salt, sizeof(salt)) + "$" +
           detail::base64UrlEncode(derived, sizeof(derived));
}

inline bool verifyPassword(const std::string& password, const std::string& stored) {
    std::vector<std::string> parts;
    std::size_t              start = 0;

    for (std::size_t i = 0; i <= stored.size(); ++i) {
        if (i == stored.size() || stored[i] == '$') {
            parts.push_back(stored.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 4 || parts[0] != "pbkdf2") return false;

    const int  iterations = std::atoi(parts[1].c_str());
    const auto salt       = detail::base64UrlDecode(parts[2]);

    unsigned char derived[32];
    ::PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        reinterpret_cast<const unsigned char*>(salt.data()),
                        static_cast<int>(salt.size()), iterations, EVP_sha256(),
                        sizeof(derived), derived);

    return detail::constantTimeEquals(parts[3],
                                      detail::base64UrlEncode(derived, sizeof(derived)));
}

// ------------------------------------------------------------ middleware

// Exige un Bearer token valido y deja el sujeto y el rol en el request, para
// que los handlers los lean con req.get("auth.sub").
inline Middleware bearer(std::string secret) {
    return [secret = std::move(secret)](Request& request) -> std::optional<Error> {
        const auto header = request.header("Authorization");
        if (!header.starts_with("Bearer ")) return Unauthorized("missing bearer token");

        const auto claims = verify(header.substr(7), secret);
        if (!claims) return Unauthorized("invalid or expired token");

        request.set("auth.sub", claims->sub);
        request.set("auth.role", claims->role);
        return std::nullopt;
    };
}

}  // namespace syrax::auth
