#pragma once

// Security response headers, written before the chain runs so the handler's
// send() carries them (a one-shot send moves the headers away; writing after
// next() would be too late). Equivalent to tower-http SetResponseHeader / Go
// `unrolled/secure` / Spring Security Headers:
//
//   server.use(simple_http::middleware::secure_headers());
//
// A handler that needs a different value overrides with replace_header():
//   res->replace_header(field::x_frame_options, "SAMEORIGIN");
//
// Defaults follow the defensive baseline (Spring's): X-Frame-Options DENY,
// X-Content-Type-Options nosniff, Referrer-Policy strict-origin-when-cross-
// origin. Content-Security-Policy and Permissions-Policy are off until
// configured — a guessed CSP breaks the site, so it must be a deliberate
// policy. Strict-Transport-Security is off until a max-age is set, and is only
// ever written over TLS.

#include <chrono>
#include <string>
#include <utility>

#include "../core/http_field.h"
#include "handler.h"

namespace simple_http {
namespace middleware {

// The policy. Empty string = "do not emit this header at all" for the
// string fields; no_sniff turns the nosniff header off.
struct SecureHeadersConfig {
    // Content-Security-Policy value ("default-src 'self'; ..."). Empty = off.
    // CSP is a per-site policy, not a baseline — it must be configured, not
    // guessed.
    std::string content_security_policy{};
    // X-Frame-Options: "DENY" / "SAMEORIGIN". Empty = off.
    std::string frame_options{"DENY"};
    // X-Content-Type-Options: nosniff.
    bool no_sniff{true};
    // Referrer-Policy.
    std::string referrer_policy{"strict-origin-when-cross-origin"};
    // X-XSS-Protection. Empty = off (the header is deprecated; browsers have
    // replaced it with real XSS protections).
    std::string x_xss_protection{};
    // Permissions-Policy. Empty = off.
    std::string permissions_policy{};
    // Strict-Transport-Security. max_age == 0 = off. Only written when the
    // connection is TLS: HSTS told to a plaintext client would be ignored by
    // the browser after an initial insecure load, so no plaintext hop gets it.
    std::chrono::seconds hsts_max_age{0};
    bool hsts_include_subdomains{false};
    bool hsts_preload{false};
};

inline Middleware secure_headers(SecureHeadersConfig cfg = {}) {
    return [cfg = std::move(cfg)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        // Before the chain: the headers ride along with whatever send() the
        // handler (or the router's own 404/405/OPTIONS) emits next.
        if (!cfg.content_security_policy.empty()) {
            res->header(field::content_security_policy, cfg.content_security_policy);
        }
        if (!cfg.frame_options.empty()) {
            res->header(field::x_frame_options, cfg.frame_options);
        }
        if (cfg.no_sniff) {
            res->header(field::x_content_type_options, "nosniff");
        }
        if (!cfg.referrer_policy.empty()) {
            res->header(field::referrer_policy, cfg.referrer_policy);
        }
        if (!cfg.x_xss_protection.empty()) {
            res->header(field::x_xss_protection, cfg.x_xss_protection);
        }
        if (!cfg.permissions_policy.empty()) {
            res->header(field::permissions_policy, cfg.permissions_policy);
        }
        if (cfg.hsts_max_age.count() > 0 && ssl && *ssl != nullptr) {
            std::string hsts = "max-age=" + std::to_string(cfg.hsts_max_age.count());
            if (cfg.hsts_include_subdomains) {
                hsts += "; includeSubDomains";
            }
            if (cfg.hsts_preload) {
                hsts += "; preload";
            }
            res->header(field::strict_transport_security, std::move(hsts));
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

} // namespace middleware
} // namespace simple_http