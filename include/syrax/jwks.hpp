#pragma once

// Verificar un token firmado por OTRO.
//
// Es la mitad finita y util de OAuth2/OIDC. Auth0, Keycloak, Cognito y Entra
// firman con RS256 y publican sus claves publicas en un JWKS; lo unico que
// tiene que hacer tu API es bajarlas y comprobar la firma. *Ser* el proveedor
// -discovery, PKCE, refresh, cuatro flujos y sus modos de fallo- no entra, y
// el motivo esta escrito en el nivel 3 del roadmap.
//
//   app.use(syrax::auth::jwks("https://tu-tenant.auth0.com/.well-known/jwks.json",
//                             {.issuer = "https://tu-tenant.auth0.com/",
//                              .audience = "https://api.tuempresa.com"}));
//
// A partir de ahi el token es un token: `auth.sub`, `auth.role` y `auth.scope`
// quedan en la peticion igual que con bearer(), y requireScope() funciona sobre
// ellos sin traducir nada, porque el claim `scope` de OAuth2 es el formato que
// ya usa policy.hpp.
//
// Tres cosas que hacen que esto sea verificar y no fingir que se verifica:
//
//   - **El algoritmo se exige, no se lee del token.** Un JWT dice en su propia
//     cabecera con que se firmo, y aceptar eso es el agujero clasico: un
//     atacante manda alg=none, o alg=HS256 usando la clave PUBLICA como
//     secreto compartido, y el token pasa. Aqui solo se acepta RS256.
//   - **El `kid` elige la clave, y una desconocida fuerza UNA recarga.** Los
//     proveedores rotan claves; sin recarga, el dia de la rotacion deja de
//     entrar todo el mundo. Con recarga en cada fallo, cualquiera tumba tu API
//     mandando tokens con kid inventado: por eso hay un minimo entre recargas.
//   - **`issuer` y `audience` se comprueban si se piden.** Un token valido de
//     OTRO cliente del mismo proveedor esta perfectamente firmado; lo que dice
//     que no es para ti es el `aud`.

#include <syrax/auth.hpp>
#include <syrax/http.hpp>
#include <syrax/middleware.hpp>

#include <glaze/glaze.hpp>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace syrax::auth {

struct JwksOptions {
    // Quien tiene que haber emitido el token. Vacio no lo comprueba.
    std::string issuer;

    // Para quien es. Vacio no lo comprueba, pero comprobarlo es lo que impide
    // que valga un token de otro cliente del mismo proveedor.
    std::string audience;

    // Minimo entre dos recargas del JWKS. Sin esto, un token con un kid
    // inventado provoca una descarga, y mandar mil tumba tu API contra el
    // proveedor.
    std::chrono::seconds minRefresh{60};

    // Cada cuanto se refrescan las claves aunque todo vaya bien.
    std::chrono::seconds ttl{std::chrono::hours{12}};
};

namespace detail {

// Un JWK trae bastante mas de lo que hace falta -use, x5c, x5t, key_ops- y la
// especificacion dice que hay que ignorar lo que no se entienda, precisamente
// para poder crecer sin romper a nadie. Glaze falla con claves desconocidas por
// defecto, asi que hay que pedirselo: sin esto, un JWKS de Auth0 no parsea y el
// sintoma es un "unknown signing key" que no dice nada del motivo real.
inline constexpr glz::opts kTolerante{.error_on_unknown_keys = false};

// Un JWK de tipo RSA. Solo se leen los campos que hacen falta.
struct Jwk {
    std::string kty;
    std::string kid;
    std::string alg;
    std::string n;
    std::string e;
};

struct JwkSet {
    std::vector<Jwk> keys;
};

using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

// Arma una clave publica RSA desde el modulo y el exponente en base64url.
inline PKeyPtr rsaFrom(const std::string& nB64, const std::string& eB64) {
    PKeyPtr vacia{nullptr, EVP_PKEY_free};

    const auto n = base64UrlDecode(nB64);
    const auto e = base64UrlDecode(eB64);
    if (n.empty() || e.empty()) return vacia;

    BIGNUM* bnN = BN_bin2bn(reinterpret_cast<const unsigned char*>(n.data()),
                            static_cast<int>(n.size()), nullptr);
    BIGNUM* bnE = BN_bin2bn(reinterpret_cast<const unsigned char*>(e.data()),
                            static_cast<int>(e.size()), nullptr);
    if (!bnN || !bnE) {
        BN_free(bnN);
        BN_free(bnE);
        return vacia;
    }

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM*     params = nullptr;
    EVP_PKEY*       pkey = nullptr;
    EVP_PKEY_CTX*   ctx  = nullptr;

    if (bld && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bnN) &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bnE) &&
        (params = OSSL_PARAM_BLD_to_param(bld)) &&
        (ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)) &&
        EVP_PKEY_fromdata_init(ctx) > 0) {
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);
    }

    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(ctx);
    BN_free(bnN);
    BN_free(bnE);

    return PKeyPtr{pkey, EVP_PKEY_free};
}

// Comprueba la firma RS256 de "header.payload" con la clave publica.
inline bool verifyRs256(EVP_PKEY* key, const std::string& signedPart,
                        const std::string& signature) {
    if (!key) return false;

    const auto firma = base64UrlDecode(signature);
    if (firma.empty()) return false;

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!ctx) return false;

    if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) <= 0) return false;

    return EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(firma.data()),
                            firma.size(),
                            reinterpret_cast<const unsigned char*>(signedPart.data()),
                            signedPart.size()) == 1;
}

// La cabecera del JWT: de aqui sale el kid, y el alg se comprueba contra el
// que exigimos en vez de obedecerlo.
struct JwtHeader {
    std::string alg;
    std::string kid;
    std::string typ;
};

// Un claim de texto del payload, o vacio si no esta o no es texto.
inline std::string textClaim(const glz::generic& payload, const std::string& name) {
    if (!payload.is_object() || !payload.contains(name)) return {};

    const auto& value = payload.get_object().find(name)->second;
    return value.is_string() ? value.get_string() : std::string{};
}

inline std::int64_t numberClaim(const glz::generic& payload, const std::string& name) {
    if (!payload.is_object() || !payload.contains(name)) return 0;

    const auto& value = payload.get_object().find(name)->second;
    return value.is_number() ? static_cast<std::int64_t>(value.get_number()) : 0;
}

// `aud` es una cadena en unos proveedores y un array en otros. La
// especificacion permite las dos, asi que hay que aceptar las dos: tratar solo
// una deja fuera a la mitad de los proveedores sin decir por que.
inline bool audienceMatches(const glz::generic& payload, const std::string& expected) {
    if (!payload.is_object() || !payload.contains("aud")) return false;

    const auto& aud = payload.get_object().find("aud")->second;

    if (aud.is_string()) return aud.get_string() == expected;

    if (aud.is_array()) {
        for (const auto& item : aud.get_array()) {
            if (item.is_string() && item.get_string() == expected) return true;
        }
    }
    return false;
}

// El almacen de claves, compartido por todas las peticiones.
class KeyStore {
public:
    KeyStore(std::function<std::string()> url, JwksOptions options)
        : url_{std::move(url)}, options_{options} {}

    // Devuelve la clave de ese kid, recargando si no la conoce. Nunca lanza:
    // un JWKS inalcanzable es un 401, no un 500.
    drogon::Task<EVP_PKEY*> keyFor(std::string kid) {
        if (auto* found = lookup(kid)) co_return found;

        if (!co_await refresh()) co_return nullptr;
        co_return lookup(kid);
    }

    // Vacia la cache. Para los tests.
    void forget() {
        const std::lock_guard lock{mutex_};
        keys_.clear();
        lastRefresh_ = {};
    }

private:
    EVP_PKEY* lookup(const std::string& kid) {
        const std::lock_guard lock{mutex_};

        const auto ahora = std::chrono::steady_clock::now();
        if (lastRefresh_.time_since_epoch().count() != 0 && ahora - lastRefresh_ > options_.ttl) {
            keys_.clear();
            return nullptr;
        }

        // Un JWKS con una sola clave puede venir sin kid: entonces no hay nada
        // que elegir y vale la que hay.
        if (kid.empty() && keys_.size() == 1) return keys_.begin()->second.get();

        const auto it = keys_.find(kid);
        return it == keys_.end() ? nullptr : it->second.get();
    }

    drogon::Task<bool> refresh() {
        {
            const std::lock_guard lock{mutex_};

            // El freno contra el kid inventado: sin el, mil tokens basura son
            // mil descargas contra el proveedor.
            const auto ahora = std::chrono::steady_clock::now();
            if (lastRefresh_.time_since_epoch().count() != 0 &&
                ahora - lastRefresh_ < options_.minRefresh) {
                co_return false;
            }
        }

        // El cliente quiere el origen -esquema, host y puerto- y el path va en
        // la peticion. Pasarle la URL entera como base hace que pida "/" del
        // host y el JWKS no aparezca nunca.
        // La URL se pide AHORA y no se guarda al construir: asi puede venir de
        // algo que todavia no existia cuando se cableo el middleware.
        const auto url = url_();

        const auto trasEsquema = url.find("://");
        const auto inicioPath =
            trasEsquema == std::string::npos ? url.find('/') : url.find('/', trasEsquema + 3);

        const auto origen = inicioPath == std::string::npos ? url : url.substr(0, inicioPath);
        const auto path   = inicioPath == std::string::npos ? "/" : url.substr(inicioPath);

        http::Client cliente{origen, {.timeout = std::chrono::seconds{5}, .retries = 1}};
        const auto   respuesta = co_await cliente.get(path);

        if (!respuesta.ok()) co_return false;

        JwkSet set;
        if (glz::read<detail::kTolerante>(set, respuesta.body)) co_return false;

        const std::lock_guard lock{mutex_};
        keys_.clear();

        for (const auto& jwk : set.keys) {
            if (jwk.kty != "RSA") continue;            // no hay soporte de EC
            if (!jwk.alg.empty() && jwk.alg != "RS256") continue;

            if (auto key = rsaFrom(jwk.n, jwk.e)) keys_.emplace(jwk.kid, std::move(key));
        }

        lastRefresh_ = std::chrono::steady_clock::now();
        co_return !keys_.empty();
    }

    std::function<std::string()>                   url_;
    JwksOptions                                    options_;
    std::mutex                                     mutex_;
    std::unordered_map<std::string, PKeyPtr>       keys_;
    std::chrono::steady_clock::time_point          lastRefresh_{};
};

}  // namespace detail

// El middleware. Es asincrono porque puede tener que bajar las claves, y por
// eso va con useAsync() y no con use().
//
// La URL se da como funcion y no como cadena para que se resuelva cuando haga
// falta, no cuando se cablea: en un proyecto normal sale de la configuracion y
// da igual, pero asi tambien sirve cuando todavia no se conoce al arrancar.
inline AsyncMiddleware jwks(std::function<std::string()> url, JwksOptions options = {}) {
    auto store = std::make_shared<detail::KeyStore>(std::move(url), options);

    return [store, options](Request& request) -> drogon::Task<std::optional<Error>> {
        const auto header = request.header("Authorization");
        if (!header.starts_with("Bearer ")) co_return Unauthorized("missing bearer token");

        const auto token = header.substr(7);

        const auto primero = token.find('.');
        const auto segundo = token.find('.', primero == std::string::npos ? 0 : primero + 1);
        if (primero == std::string::npos || segundo == std::string::npos) {
            co_return Unauthorized("malformed token");
        }

        detail::JwtHeader cabecera;
        if (glz::read<detail::kTolerante>(cabecera,
                                          detail::base64UrlDecode(token.substr(0, primero)))) {
            co_return Unauthorized("malformed token header");
        }

        // El agujero clasico, cerrado: el algoritmo se EXIGE. Obedecer el que
        // dice el token permite alg=none, y alg=HS256 firmando con la clave
        // publica como si fuera un secreto compartido.
        if (cabecera.alg != "RS256") co_return Unauthorized("unsupported token algorithm");

        auto* key = co_await store->keyFor(cabecera.kid);
        if (!key) co_return Unauthorized("unknown signing key");

        if (!detail::verifyRs256(key, token.substr(0, segundo), token.substr(segundo + 1))) {
            co_return Unauthorized("invalid token signature");
        }

        glz::generic payload;
        if (glz::read_json(
                payload, detail::base64UrlDecode(token.substr(primero + 1, segundo - primero - 1)))) {
            co_return Unauthorized("malformed token payload");
        }

        const auto ahora = detail::now();
        const auto exp   = detail::numberClaim(payload, "exp");
        const auto nbf   = detail::numberClaim(payload, "nbf");

        if (exp != 0 && ahora >= exp) co_return Unauthorized("expired token");
        if (nbf != 0 && ahora < nbf) co_return Unauthorized("token not valid yet");

        if (!options.issuer.empty() && detail::textClaim(payload, "iss") != options.issuer) {
            co_return Unauthorized("unexpected token issuer");
        }

        // Un token perfectamente firmado por el mismo proveedor pero emitido
        // para OTRA aplicacion: la firma no dice nada de eso, el aud si.
        if (!options.audience.empty() && !detail::audienceMatches(payload, options.audience)) {
            co_return Unauthorized("token is not for this audience");
        }

        request.set("auth.sub", detail::textClaim(payload, "sub"));
        request.set("auth.role", detail::textClaim(payload, "role"));
        request.set("auth.scope", detail::textClaim(payload, "scope"));
        co_return std::nullopt;
    };
}

inline AsyncMiddleware jwks(std::string url, JwksOptions options = {}) {
    return jwks([url = std::move(url)] { return url; }, options);
}

}  // namespace syrax::auth
