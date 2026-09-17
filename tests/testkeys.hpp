#pragma once

// Un proveedor de identidad de mentira para probar el middleware de JWKS.
//
// Genera una pareja RSA al vuelo y sabe firmar RS256, porque para comprobar que
// la verificacion funciona hace falta un token firmado de verdad: uno escrito a
// mano no probaria nada mas que el parser.
//
// La parte de FIRMAR vive aqui, en los tests, y no en el framework: Syrax
// verifica tokens de terceros pero no los emite, y meter aqui el firmado seria
// empezar a ser el proveedor por la puerta de atras.

#include <syrax/auth.hpp>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace testkeys {

using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

// El puerto donde la app publica su JWKS. Lo rellena el test cuando el
// servidor ya escucha; el middleware lo lee al bajar las claves, no al
// cablearse, que es justo para lo que existe la URL perezosa.
inline std::atomic<std::uint16_t>& puerto() {
    static std::atomic<std::uint16_t> value{0};
    return value;
}

inline constexpr const char* kKid = "clave-de-prueba";

inline std::string kid() { return kKid; }

// Una sola pareja para todo el binario: generar RSA de 2048 bits cuesta, y
// hacerlo por caso alargaria la suite sin probar nada nuevo.
inline EVP_PKEY* key() {
    static PKeyPtr almacenada = [] {
        EVP_PKEY*     pkey = nullptr;
        EVP_PKEY_CTX* ctx  = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);

        if (ctx && EVP_PKEY_keygen_init(ctx) > 0 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0) {
            EVP_PKEY_generate(ctx, &pkey);
        }
        EVP_PKEY_CTX_free(ctx);
        return PKeyPtr{pkey, EVP_PKEY_free};
    }();
    return almacenada.get();
}

namespace detail {

inline std::string paramB64(const char* name) {
    BIGNUM* bn = nullptr;
    if (EVP_PKEY_get_bn_param(key(), name, &bn) <= 0 || !bn) return {};

    std::string bytes(static_cast<std::size_t>(BN_num_bytes(bn)), '\0');
    BN_bn2bin(bn, reinterpret_cast<unsigned char*>(bytes.data()));
    BN_free(bn);

    return syrax::auth::detail::base64UrlEncode(bytes);
}

}  // namespace detail

inline std::string modulusB64() { return detail::paramB64(OSSL_PKEY_PARAM_RSA_N); }
inline std::string exponentB64() { return detail::paramB64(OSSL_PKEY_PARAM_RSA_E); }

// Firma con RS256 lo que se le dé. Devuelve la firma en base64url.
inline std::string signRs256(const std::string& data) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!ctx) return {};

    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key()) <= 0) return {};

    std::size_t len = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &len,
                       reinterpret_cast<const unsigned char*>(data.data()), data.size()) <= 0) {
        return {};
    }

    std::string firma(len, '\0');
    if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(firma.data()), &len,
                       reinterpret_cast<const unsigned char*>(data.data()), data.size()) <= 0) {
        return {};
    }
    firma.resize(len);

    return syrax::auth::detail::base64UrlEncode(firma);
}

// Un token completo. `header` y `payload` van como JSON crudo para que un test
// pueda mandar un alg=none o un aud equivocado sin pelearse con un tipo.
inline std::string token(const std::string& headerJson, const std::string& payloadJson,
                         bool firmar = true) {
    const auto cuerpo = syrax::auth::detail::base64UrlEncode(headerJson) + "." +
                        syrax::auth::detail::base64UrlEncode(payloadJson);

    return cuerpo + "." + (firmar ? signRs256(cuerpo) : std::string{"firma-inventada"});
}

}  // namespace testkeys
