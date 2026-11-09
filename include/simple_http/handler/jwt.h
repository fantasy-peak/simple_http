#pragma once

// JWT bearer authentication (HS256 / RS256 / ES256) plus scope/role checks --
// C++17 token-only port.
//
// This is deliberately a verifier, not an authorization server: it validates a
// compact JWS, checks the standard time/issuer/audience claims, and publishes a
// Principal on the request. Token issuance, OAuth2/OIDC discovery and JWKS
// fetching are separate concerns and are left to the caller or a later module.
//
//     server.use(simple_http::middleware::jwt(simple_http::JwtConfig{
//         .algorithms = {"RS256"},          // C++20 designated init; assign
//         .public_key_pem = read_file("public.pem"), // fields individually in C++17
//         .issuer = "https://issuer.example",
//         .audience = "my-api",
//     }));
//     server.use(simple_http::middleware::require_scope("users:write"));
//
// Security properties that are intentionally enforced:
//   * the token's `alg` must be on the configured allowlist; "none" is rejected;
//   * the signature is verified with OpenSSL (HMAC / RSA-PKCS1 / ECDSA P-256);
//   * HMAC comparison is constant-time;
//   * `exp` is required by default and checked with a small leeway, as are
//     `nbf`, `iss` and `aud` when configured.
//
// The header/payload parser uses glaze, so this whole header is part of the
// glaze-gated surface (core/glaze_support.h). The middleware itself performs no
// I/O and completes synchronously.

#include "../core/glaze_support.h"

#if SIMPLE_HTTP_HAS_GLAZE

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>

#include "../core/base64.h"
#include "../core/http_date.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../proto/error_body.h"
#include "handler.h"
#include "jwks.h"
#include "session.h" // constant_time_equal

namespace simple_http {

// The authenticated identity published by the jwt() middleware.
struct Principal {
    std::string subject;
    std::string issuer;
    std::string audience;
    std::vector<std::string> scopes;
    std::vector<std::string> roles;
    std::int64_t expires_unix{0};
};

inline const Principal *principal(const Request &req) { return req.get_state<Principal>(); }

struct JwtConfig {
    // Allowed JWS algorithms. Leave the default for HS256; an unknown or
    // disallowed token algorithm is rejected even if the signature would pass.
    std::vector<std::string> algorithms{"HS256"};
    // HS256 shared secret.
    std::string secret;
    // PEM public key for RS256 / ES256.
    std::string public_key_pem;
    // A static JWKS document (RFC 7517). When non-empty it takes precedence over
    // public_key_pem and keys are selected by the token's `kid` (or the single
    // key when there is exactly one). Fetch it with async_fetch_jwks_json() and
    // pass the body here; refresh is the caller's policy.
    std::string jwks_json;
    // Claim checks. Empty issuer/audience disables that check.
    std::string issuer;
    std::string audience;
    std::chrono::seconds leeway{60};
    // Require an exp claim. Tokens without one are otherwise valid forever.
    bool require_exp{true};
    // Where the compact JWS arrives, and the scheme to strip.
    std::string header_name{"authorization"};
    std::string scheme{"Bearer"};
    // When true, a request without the header passes through anonymously;
    // require_scope/require_role can still reject it later. An invalid token
    // is always 401.
    bool optional{false};
};

namespace jwt_detail {

struct Header {
    std::string alg;
    std::string typ;
    std::string kid;
};

struct Payload {
    std::string sub;
    std::string scope; // space-separated OAuth scope string
    std::vector<std::string> roles;
    std::string iss;
    std::string aud;
    std::int64_t exp{0};
    std::int64_t nbf{0};
    std::int64_t iat{0};
};

inline std::vector<std::string> split_space(std::string_view value) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < value.size() && value[pos] != ' ' && value[pos] != '\t') {
            ++pos;
        }
        if (pos > start) {
            out.emplace_back(value.substr(start, pos - start));
        }
    }
    return out;
}

inline bool allowed_algorithm(const JwtConfig &config, std::string_view alg) {
    for (const auto &allowed : config.algorithms) {
        if (allowed == alg) {
            return true;
        }
    }
    return false;
}

inline std::string_view signing_input(std::string_view token, std::size_t second_dot) {
    return token.substr(0, second_dot);
}

inline bool verify_hmac(std::string_view secret, std::string_view input, std::string_view signature) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
              reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest.data(), &length)) {
        return false;
    }
    return constant_time_equal(std::string_view{reinterpret_cast<const char *>(digest.data()), length}, signature);
}

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using SharedKey = std::shared_ptr<EVP_PKEY>;

inline PkeyPtr load_public_key(const std::string &pem) {
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        return {nullptr, EVP_PKEY_free};
    }
    EVP_PKEY *key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return {key, EVP_PKEY_free};
}

inline bool verify_openssl(EVP_PKEY *key, std::string_view input, std::string_view signature) {
    if (!key) {
        return false;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }
    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestVerifyUpdate(ctx, input.data(), input.size()) == 1) {
        ok = EVP_DigestVerifyFinal(ctx, reinterpret_cast<const unsigned char *>(signature.data()), signature.size()) ==
             1;
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

// JWS ES256 signs the raw 32-byte R || 32-byte S pair; OpenSSL's EVP verifier
// expects a DER-encoded ECDSA_SIG. Convert before verifying.
inline std::string ecdsa_raw_to_der(std::string_view raw) {
    if (raw.size() != 64) {
        return {};
    }
    BIGNUM *r = BN_bin2bn(reinterpret_cast<const unsigned char *>(raw.data()), 32, nullptr);
    BIGNUM *s = BN_bin2bn(reinterpret_cast<const unsigned char *>(raw.data() + 32), 32, nullptr);
    if (!r || !s) {
        BN_free(r);
        BN_free(s);
        return {};
    }
    ECDSA_SIG *sig = ECDSA_SIG_new();
    if (!sig) {
        BN_free(r);
        BN_free(s);
        return {};
    }
    if (ECDSA_SIG_set0(sig, r, s) != 1) {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(sig);
        return {};
    }
    unsigned char *der = nullptr;
    const int length = i2d_ECDSA_SIG(sig, &der);
    std::string out;
    if (length > 0) {
        out.assign(reinterpret_cast<const char *>(der), static_cast<std::size_t>(length));
    }
    OPENSSL_free(der);
    ECDSA_SIG_free(sig);
    return out;
}

inline bool verify_signature(std::string_view secret, EVP_PKEY *public_key, std::string_view alg,
                             std::string_view input, std::string_view signature) {
    if (alg == "HS256") {
        return !secret.empty() && verify_hmac(secret, input, signature);
    }
    if (alg == "RS256") {
        return public_key != nullptr && verify_openssl(public_key, input, signature);
    }
    if (alg == "ES256") {
        const std::string der = ecdsa_raw_to_der(signature);
        return public_key != nullptr && !der.empty() && verify_openssl(public_key, input, der);
    }
    return false;
}

inline bool has_scope(const Principal &principal, std::string_view scope) {
    for (const auto &candidate : principal.scopes) {
        if (candidate == scope) {
            return true;
        }
    }
    return false;
}

inline bool has_role(const Principal &principal, std::string_view role) {
    for (const auto &candidate : principal.roles) {
        if (candidate == role) {
            return true;
        }
    }
    return false;
}

} // namespace jwt_detail

namespace middleware {

// Writes the standard 401/403 JSON body and completes `done`.
inline void jwt_reject(const ResponsePtr &res, int status_code, std::string_view message, HandlerCompletion done) {
    res->status(status_code);
    if (status_code == status::unauthorized) {
        res->header("www-authenticate", "Bearer error=\"invalid_token\"");
    }
    JsonError body{status_code == status::unauthorized ? "unauthorized" : "forbidden", std::string{message},
                   std::nullopt};
    async_write_error(res, status_code, std::move(body), finish_with(std::move(done)));
}

inline Middleware jwt(JwtConfig config = {}) {
    // Parse the PEM / JWKS once, at middleware construction, not per request.
    jwt_detail::SharedKey public_key;
    if (!config.public_key_pem.empty()) {
        auto loaded = jwt_detail::load_public_key(config.public_key_pem);
        public_key = jwt_detail::SharedKey(loaded.release(), EVP_PKEY_free);
    }
    const bool jwks_configured = !config.jwks_json.empty();
    std::shared_ptr<Jwks> jwks;
    if (jwks_configured) {
        auto parsed = Jwks::parse(config.jwks_json);
        if (!parsed) {
            SIMPLE_HTTP_ERROR_LOG("jwt: invalid JWKS document");
            jwks = std::make_shared<Jwks>();
        } else {
            jwks = std::make_shared<Jwks>(std::move(*parsed));
        }
    }

    return [config = std::move(config), public_key = std::move(public_key), jwks = std::move(jwks),
            jwks_configured](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
        const auto header = req->header(config.header_name);
        if (!header) {
            if (config.optional) {
                next(std::move(req), std::move(res), ssl, std::move(done));
            } else {
                jwt_reject(res, status::unauthorized, "missing bearer token", std::move(done));
            }
            return;
        }

        std::string token{*header};
        if (!config.scheme.empty()) {
            const std::string prefix = config.scheme + " ";
            if (token.size() <= prefix.size() ||
                !iequals_ci(std::string_view{token}.substr(0, prefix.size()), prefix)) {
                jwt_reject(res, status::unauthorized, "invalid authorization scheme", std::move(done));
                return;
            }
            token.erase(0, prefix.size());
        }

        const auto first = token.find('.');
        const auto second = first == std::string::npos ? std::string::npos : token.find('.', first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            token.find('.', second + 1) != std::string::npos) {
            jwt_reject(res, status::unauthorized, "malformed JWT", std::move(done));
            return;
        }

        const std::string_view token_view{token};
        const std::string_view header_b64 = token_view.substr(0, first);
        const std::string_view payload_b64 = token_view.substr(first + 1, second - first - 1);
        const std::string_view signature_b64 = token_view.substr(second + 1);
        if (header_b64.empty() || payload_b64.empty() || signature_b64.empty()) {
            jwt_reject(res, status::unauthorized, "malformed JWT", std::move(done));
            return;
        }

        jwt_detail::Header jwt_header{};
        jwt_detail::Payload payload{};
        const std::string header_json = base64_url_decode(header_b64);
        const std::string payload_json = base64_url_decode(payload_b64);
        const std::string signature = base64_url_decode(signature_b64);
        // Real tokens carry claims this verifier does not need (`jti`, `azp`,
        // `nonce`, `email`, ...) and headers such as `cty`/`x5c`. glaze defaults
        // to erroring on an unknown key, which would reject a perfectly valid
        // token; every key that matters is checked explicitly below.
        constexpr auto jwt_opts = glz::opts{.error_on_unknown_keys = false};
        if (auto ec = glz::read<jwt_opts>(jwt_header, header_json); ec) {
            jwt_reject(res, status::unauthorized, "malformed JWT header", std::move(done));
            return;
        }
        if (auto ec = glz::read<jwt_opts>(payload, payload_json); ec) {
            jwt_reject(res, status::unauthorized, "malformed JWT payload", std::move(done));
            return;
        }

        if (!jwt_detail::allowed_algorithm(config, jwt_header.alg) || jwt_header.alg == "none") {
            jwt_reject(res, status::unauthorized, "disallowed JWT algorithm", std::move(done));
            return;
        }

        std::string_view secret = config.secret;
        EVP_PKEY *signing_key = public_key.get();
        if (jwks_configured) {
            if (!jwks || jwks->empty()) {
                jwt_reject(res, status::unauthorized, "JWT key set unavailable", std::move(done));
                return;
            }
            const JwkKey *key = jwks->find(jwt_header.kid, jwt_header.alg);
            if (!key) {
                jwt_reject(res, status::unauthorized, "unknown JWT key", std::move(done));
                return;
            }
            if (!key->secret.empty()) {
                secret = key->secret;
                signing_key = nullptr;
            } else {
                secret = {};
                signing_key = key->public_key.get();
            }
        }
        if (!jwt_detail::verify_signature(secret, signing_key, jwt_header.alg, jwt_detail::signing_input(token, second),
                                          signature)) {
            jwt_reject(res, status::unauthorized, "invalid JWT signature", std::move(done));
            return;
        }

        const std::int64_t now = now_unix();
        const std::int64_t leeway = config.leeway.count();
        if (config.require_exp && payload.exp == 0) {
            jwt_reject(res, status::unauthorized, "JWT has no exp claim", std::move(done));
            return;
        }
        if (payload.exp != 0 && now > payload.exp + leeway) {
            jwt_reject(res, status::unauthorized, "JWT has expired", std::move(done));
            return;
        }
        if (payload.nbf != 0 && now + leeway < payload.nbf) {
            jwt_reject(res, status::unauthorized, "JWT is not valid yet", std::move(done));
            return;
        }
        if (!config.issuer.empty() && payload.iss != config.issuer) {
            jwt_reject(res, status::unauthorized, "JWT issuer mismatch", std::move(done));
            return;
        }
        if (!config.audience.empty() && payload.aud != config.audience) {
            jwt_reject(res, status::unauthorized, "JWT audience mismatch", std::move(done));
            return;
        }

        Principal identity{payload.sub,   payload.iss, payload.aud, jwt_detail::split_space(payload.scope),
                           payload.roles, payload.exp};
        req->set_state(std::move(identity));
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

inline Middleware require_scope(std::string scope) {
    return
        [scope = std::move(scope)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
            const Principal *identity = principal(*req);
            if (!identity) {
                jwt_reject(res, status::unauthorized, "authentication required", std::move(done));
                return;
            }
            if (!jwt_detail::has_scope(*identity, scope)) {
                jwt_reject(res, status::forbidden, "insufficient scope", std::move(done));
                return;
            }
            next(std::move(req), std::move(res), ssl, std::move(done));
        };
}

inline Middleware require_role(std::string role) {
    return [role = std::move(role)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next, HandlerCompletion done) {
        const Principal *identity = principal(*req);
        if (!identity) {
            jwt_reject(res, status::unauthorized, "authentication required", std::move(done));
            return;
        }
        if (!jwt_detail::has_role(*identity, role)) {
            jwt_reject(res, status::forbidden, "insufficient role", std::move(done));
            return;
        }
        next(std::move(req), std::move(res), ssl, std::move(done));
    };
}

} // namespace middleware

} // namespace simple_http

#endif // SIMPLE_HTTP_HAS_GLAZE
