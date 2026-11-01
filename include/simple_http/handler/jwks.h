#pragma once

// JSON Web Key Set (RFC 7517) support for the JWT verifier.
//
// The middleware accepts a static JWKS document (`JwtConfig::jwks_json`) and
// selects a key by the token's `kid`, or falls back to the single key when the
// set has exactly one. Supported key types:
//
//   * RSA  — `kty:"RSA"`, `n`, `e`
//   * EC   — `kty:"EC"`, `crv:"P-256"`, `x`, `y`
//   * oct  — `kty:"oct"`, `k` (an HS* shared secret)
//
// Fetching is deliberately a small helper, not hidden inside the middleware:
// the caller fetches the document (usually at startup) and passes the JSON to
// JwtConfig. That keeps the middleware synchronous and makes key rotation /
// refresh an explicit policy:
//
//     auto body = co_await simple_http::fetch_jwks_json(client, jwks_url);
//     if (body)
//         server.use(middleware::jwt(JwtConfig{
//             .algorithms = {"RS256"},
//             .jwks_json = *body,
//             .issuer = "...", .audience = "...",
//         }));
//
// The document is parsed once, at middleware construction. A JWKS refresh means
// constructing a new middleware (or recreating the server's chain); automatic
// background refresh is not part of this layer.

#include <atomic>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include "../client/http.h" // fetch_jwks_json (Client; no handler dependency)
#include "../core/base64.h"
#include "../core/logging.h"

namespace simple_http {

struct JwkDocument {
    struct Key {
        std::string kty;
        std::string kid;
        std::string alg;
        std::string use;
        std::string n;
        std::string e;
        std::string crv;
        std::string x;
        std::string y;
        std::string k;
    };
    std::vector<Key> keys;
};

struct JwkKey {
    std::string kid;
    std::string alg; // empty = usable for any allowed algorithm of its type
    std::string kty;
    std::shared_ptr<EVP_PKEY> public_key; // RSA / EC
    std::string secret;                   // oct
};

class Jwks {
  public:
    Jwks() = default;

    static std::expected<Jwks, std::string> parse(std::string_view json) {
        JwkDocument document{};
        if (auto ec = glz::read_json(document, json); ec) {
            return std::unexpected(std::string{"invalid JWKS JSON"});
        }

        Jwks out;
        for (auto &jwk : document.keys) {
            JwkKey key;
            key.kid = jwk.kid;
            key.alg = jwk.alg;
            key.kty = jwk.kty;
            if (jwk.kty == "RSA") {
                key.public_key = make_rsa(jwk.n, jwk.e);
            } else if (jwk.kty == "EC") {
                key.public_key = make_ec(jwk.crv, jwk.x, jwk.y);
            } else if (jwk.kty == "oct") {
                key.secret = base64_url_decode(jwk.k);
            }
            if (!key.public_key && key.secret.empty()) {
                SIMPLE_HTTP_WARN_LOG("jwks: skipping unusable {} key (kid={})", jwk.kty, jwk.kid);
                continue;
            }
            out.m_keys.push_back(std::move(key));
        }
        if (out.m_keys.empty()) {
            return std::unexpected(std::string{"JWKS contains no usable keys"});
        }
        return out;
    }

    // Selects a key for a token. A non-empty kid must match exactly; an empty
    // kid is only accepted when exactly one key can speak `alg`. A key with an
    // explicit `alg` must match the token's algorithm.
    const JwkKey *find(std::string_view kid, std::string_view alg) const {
        const JwkKey *candidate = nullptr;
        for (const auto &key : m_keys) {
            if (!key.alg.empty() && key.alg != alg) {
                continue;
            }
            if (!kid.empty()) {
                if (key.kid == kid) {
                    return &key;
                }
                continue;
            }
            if (candidate) {
                return nullptr; // ambiguous: no kid and more than one candidate
            }
            candidate = &key;
        }
        return candidate;
    }

    bool empty() const { return m_keys.empty(); }
    std::size_t size() const { return m_keys.size(); }

  private:
    static std::shared_ptr<EVP_PKEY> make_rsa(std::string_view n, std::string_view e) {
        const std::string modulus = base64_url_decode(n);
        const std::string exponent = base64_url_decode(e);
        if (modulus.empty() || exponent.empty()) {
            return nullptr;
        }

        BIGNUM *bn_n = BN_bin2bn(reinterpret_cast<const unsigned char *>(modulus.data()),
                                 static_cast<int>(modulus.size()), nullptr);
        BIGNUM *bn_e = BN_bin2bn(reinterpret_cast<const unsigned char *>(exponent.data()),
                                 static_cast<int>(exponent.size()), nullptr);
        if (!bn_n || !bn_e) {
            BN_free(bn_n);
            BN_free(bn_e);
            return nullptr;
        }

        OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
        if (!builder || OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, bn_n) != 1 ||
            OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, bn_e) != 1) {
            OSSL_PARAM_BLD_free(builder);
            BN_free(bn_n);
            BN_free(bn_e);
            return nullptr;
        }
        // The builder holds pointers to the BIGNUMs; OSSL_PARAM_BLD_to_param
        // copies them into the parameter array, so they must stay alive until
        // after this call.
        OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(builder);
        OSSL_PARAM_BLD_free(builder);
        BN_free(bn_n);
        BN_free(bn_e);
        return from_params("RSA", params);
    }

    static std::shared_ptr<EVP_PKEY> make_ec(std::string_view crv, std::string_view x, std::string_view y) {
        if (crv != "P-256" && crv != "prime256v1") {
            return nullptr;
        }
        const std::string xb = base64_url_decode(x);
        const std::string yb = base64_url_decode(y);
        if (xb.size() != 32 || yb.size() != 32) {
            return nullptr;
        }
        std::string point;
        point.reserve(65);
        point.push_back('\x04'); // uncompressed point
        point += xb;
        point += yb;

        OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
        if (!builder || OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) != 1 ||
            OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) != 1) {
            OSSL_PARAM_BLD_free(builder);
            return nullptr;
        }
        OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(builder);
        OSSL_PARAM_BLD_free(builder);
        return from_params("EC", params);
    }

    static std::shared_ptr<EVP_PKEY> from_params(const char *type, OSSL_PARAM *params) {
        if (!params) {
            return nullptr;
        }
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr);
        EVP_PKEY *pkey = nullptr;
        const bool ok =
            ctx && EVP_PKEY_fromdata_init(ctx) == 1 && EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) == 1;
        EVP_PKEY_CTX_free(ctx);
        OSSL_PARAM_free(params);
        if (!ok) {
            EVP_PKEY_free(pkey);
            return nullptr;
        }
        return std::shared_ptr<EVP_PKEY>(pkey, EVP_PKEY_free);
    }

    std::vector<JwkKey> m_keys;
};

// Fetches a JWKS document. The caller decides when to refresh it and how to
// react to a fetch failure; this is intentionally not a background cache.
inline asio::awaitable<std::expected<std::string, error_code>> fetch_jwks_json(Client &client, std::string url,
                                                                               std::size_t max_bytes = 1 << 20) {
    auto response = co_await client.get(std::move(url)).send();
    if (!response) {
        co_return std::unexpected{response.error()};
    }
    auto body = co_await response->read_all(max_bytes);
    if (!body) {
        co_return std::unexpected{body.error()};
    }
    co_return std::move(*body);
}

} // namespace simple_http
