#pragma once

// Built-in CORS: a policy in, a Filter out.
//
// The problem this solves is the preflight. Nothing in the engines
// special-cases OPTIONS, so an OPTIONS carrying Access-Control-Request-Method
// reaches the route table like any other request and comes back 404 — which a
// browser reads as a failed preflight, whatever the actual route offers. The
// filter built here answers that OPTIONS itself (204, no body, no framing) and
// returns false, so a preflight never reaches a route at all.
//
// Router::cors takes a policy and stores the filter it compiles to, so
// dispatch() needs no branch of its own: a filter returning false was already
// its short-circuit signal. A policy this config cannot express belongs in
// before() instead — which can start from make_cors_filter() when it only wants
// to narrow the behaviour below.
//
// Two boundaries worth knowing:
//   * Only requests carrying an Origin are filtered (that gate lives in
//     Router::dispatch). Browsers send Origin on both the preflight and the
//     actual cross-origin request, so this is invisible in practice.
//   * WebSocket upgrades do not go through dispatch — they are looked up ahead
//   of
//     it — so ws_route/ws_proxy connections get no CORS handling from here.

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/content_encoding.h" // trim_ascii
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/types.h" // iequals_ci, is_http_token
#include "../proto/request.h"
#include "../proto/response.h"
#include "handler.h" // Filter

namespace simple_http {

// A CORS policy. Every member is a plain aggregate field — no user-declared
// constructor, snake_case, default-initialized — so a policy reads as
// designated initializers, like StaticFilesConfig and HttpProxyTarget:
//
//   server.cors(CorsConfig{.allow_origins = {"https://app.example"}});
//
// Installing one is what turns CORS on: there is no `enabled` flag, and an
// empty config is not a no-op (it allows any origin).
struct CorsConfig {
    // Exact serialized origins: "https://app.example" — scheme, host, and port
    // when non-default, with no trailing slash (an Origin never carries one, so a
    // slash here would never match). Empty means any origin, and a literal "*"
    // entry means the same. Matching is case-insensitive.
    std::vector<std::string> allow_origins{};

    // Send Access-Control-Allow-Credentials: true. "*" is illegal alongside it,
    // so with this on the request's own origin is mirrored even under an
    // any-origin policy.
    bool allow_credentials{false};

    // Methods advertised on a preflight. Empty echoes the preflight's own
    // Access-Control-Request-Method — the permissive default matching an empty
    // allow_origins, and the only default that does not silently break a PUT or
    // DELETE API.
    std::vector<std::string> allow_methods{};

    // Request header names a browser may send. Empty echoes the preflight's
    // Access-Control-Request-Headers, up to max_echoed_request_headers bytes.
    // Non-empty is enforced: the requested list is echoed only when every name on
    // it is on this one.
    std::vector<std::string> allow_headers{};

    // Response headers a browser may read from an actual response. Empty exposes
    // none beyond the CORS-safelisted ones.
    std::vector<std::string> expose_headers{};

    // Access-Control-Max-Age on a preflight. Zero omits the field and leaves the
    // browser its own (very short) default — the conservative choice, since a
    // long max-age decides how stale the policy is allowed to become.
    std::chrono::seconds max_age{0};

    // Cap, in bytes, on an echoed Access-Control-Request-Headers value. A real
    // preflight lists a handful of short names; the engine accepts request heads
    // up to 64 KiB, so without a cap one request could dictate a 64 KiB response
    // head. Past the cap the field is omitted, which fails the preflight exactly
    // as an unlisted header name would.
    std::size_t max_echoed_request_headers{1024};
};

namespace cors_detail {

// Case-insensitive membership over a configured list. The codebase has no
// generic helper for this; StaticFiles::reserved is the precedent for a plain
// scan.
inline bool list_contains(const std::vector<std::string> &list, std::string_view value) {
    for (const auto &item : list) {
        if (iequals_ci(item, value)) {
            return true;
        }
    }
    return false;
}

inline std::string join(const std::vector<std::string> &list, std::string_view sep) {
    std::string out;
    for (const auto &item : list) {
        if (!out.empty()) {
            out.append(sep);
        }
        out.append(item);
    }
    return out;
}

// An empty list allows any origin, and so does a literal "*" entry. Origins are
// compared whole and case-insensitively: "https://a.example/" would never
// match, because a serialized Origin has no trailing slash.
inline bool origin_allowed(const std::vector<std::string> &allowed, std::string_view origin) {
    if (allowed.empty()) {
        return true;
    }
    for (const auto &entry : allowed) {
        if (entry == "*" || iequals_ci(entry, origin)) {
            return true;
        }
    }
    return false;
}

// Every name in the request's comma-separated list must be on the configured
// list: a preflight is all-or-nothing, so one unknown name fails it without
// forbidding the rest. Same split-and-trim shape as header_has_token, opposite
// direction.
inline bool all_headers_allowed(const std::vector<std::string> &allowed, std::string_view requested) {
    std::size_t pos = 0;
    for (;;) {
        const auto comma = requested.find(',', pos);
        const auto len = comma == std::string_view::npos ? std::string_view::npos : comma - pos;
        const std::string_view name = trim_ascii(requested.substr(pos, len));
        if (!name.empty() && !list_contains(allowed, name)) {
            return false;
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        pos = comma + 1;
    }
}

// A method echoed into Access-Control-Allow-Methods has to be valid by grammar
// and short enough that the response head stays the server's to size.
inline constexpr std::size_t kMaxEchoedMethodBytes = 32;

// Fills in the preflight-only fields: the advertised methods and headers, and
// max-age. Allow-Origin and Vary are the caller's, since they depend on the
// origin check the caller already made.
inline void apply_preflight(const CorsConfig &cfg, const RequestPtr &req, const ResponsePtr &res) {
    // A configured list wins — "whatever the routes support" is not knowable from
    // here. Empty echoes the requested method, so a PUT/DELETE API is not broken
    // by the permissive default. A value that is not a token, or is oversized, is
    // omitted: that fails the preflight, the same answer as a method the policy
    // does not allow, and keeps a malformed value out of the response head.
    if (!cfg.allow_methods.empty()) {
        res->header(field::access_control_allow_methods, join(cfg.allow_methods, ", "));
    } else if (auto requested = req->header(field::access_control_request_method)) {
        const auto method = trim_ascii(*requested);
        if (is_http_token(method) && method.size() <= kMaxEchoedMethodBytes) {
            res->header(field::access_control_allow_methods, std::string{method});
        }
    }

    // Configured means enforced; unconfigured means echo, bounded.
    if (auto requested = req->header(field::access_control_request_headers)) {
        const auto value = trim_ascii(*requested);
        if (value.empty()) {
            if (!cfg.allow_headers.empty()) {
                res->header(field::access_control_allow_headers, join(cfg.allow_headers, ", "));
            }
        } else if (!cfg.allow_headers.empty()) {
            if (all_headers_allowed(cfg.allow_headers, value)) {
                res->header(field::access_control_allow_headers, std::string{value});
            }
        } else if (value.size() <= cfg.max_echoed_request_headers) {
            res->header(field::access_control_allow_headers, std::string{value});
        } else {
            SIMPLE_HTTP_WARN_LOG("cors: not echoing a {}-byte Access-Control-Request-Headers", value.size());
        }
    } else if (!cfg.allow_headers.empty()) {
        // The client asked for nothing; still tell it what it may ask for.
        res->header(field::access_control_allow_headers, join(cfg.allow_headers, ", "));
    }

    if (cfg.max_age.count() > 0) {
        res->header(field::access_control_max_age, std::to_string(cfg.max_age.count()));
    }
}

} // namespace cors_detail

// Builds the filter that implements `cfg`. The config is captured by value, so
// the returned filter owns its policy and can outlive the caller's.
//
// The filter is also usable outside Router — pass it to before(), or hand it to
// anything taking a Filter. One caveat when doing so: it appends `Vary: Origin`
// rather than merging, which is correct only because Router::dispatch runs it
// before any handler or static site writes a header. Called later, merge
// instead.
inline Filter make_cors_filter(CorsConfig cfg) {
    return [cfg = std::move(cfg)](RequestPtr req, ResponsePtr res) -> asio::awaitable<bool> {
        // Router::dispatch only calls this for a request carrying an Origin, but
        // the filter is reachable other ways too, so a missing or empty Origin is a
        // pass-through rather than a dereference.
        const auto origin = req->header(field::origin);
        if (!origin || origin->empty()) {
            co_return true;
        }

        // A preflight is exactly "OPTIONS + Access-Control-Request-Method". A plain
        // OPTIONS is an ordinary cross-origin request: headers are added and the
        // route still runs.
        const bool preflight =
            req->method() == Method::Options && req->header(field::access_control_request_method).has_value();

        if (cors_detail::origin_allowed(cfg.allow_origins, *origin)) {
            // "*" is illegal next to credentials, and wrong under an explicit
            // allowlist — the answer would not be origin-independent after all — so
            // those cases mirror the request's own origin instead.
            const bool any_origin = cfg.allow_origins.empty() || cors_detail::list_contains(cfg.allow_origins, "*");
            const bool mirror = cfg.allow_credentials || !any_origin;

            res->header(field::access_control_allow_origin, mirror ? std::string{*origin} : std::string{"*"});
            if (cfg.allow_credentials) {
                res->header(field::access_control_allow_credentials, "true");
            }
            if (mirror) {
                // The reply depends on Origin exactly when the origin is mirrored,
                // so a cache keyed on the URL alone must not hand one origin's
                // answer to another. Appending is right from this position: the
                // filter runs before any handler or static site writes, so this is
                // the first Vary, and compressing_writer's merge_vary folds
                // Accept-Encoding into it afterwards.
                res->header(field::vary, "Origin");
            }

            if (preflight) {
                cors_detail::apply_preflight(cfg, req, res);
                // Bodyless: no body, no Content-Length, no chunked terminator. A
                // framed empty body on a keep-alive connection desynchronizes the
                // peer.
                res->status(status::no_content);
                (void)co_await res->send_bodyless();
                co_return false; // answered here — a preflight never reaches a route
            }

            if (!cfg.expose_headers.empty()) {
                res->header(field::access_control_expose_headers, cors_detail::join(cfg.expose_headers, ", "));
            }
            // CORS is a browser gate, not authorization: the route still runs.
            co_return true;
        }

        // An origin the policy does not allow: not one CORS header, so the browser
        // rejects the reply. A preflight is still answered here rather than handed
        // to the routes — a 404/405 for an OPTIONS the browser made on purpose
        // reads as a broken server, and a route that happens to handle OPTIONS
        // would run its handler for a request that was never going to be followed
        // by the real one. Answering every path alike also leaks nothing about
        // which paths exist.
        if (preflight) {
            res->status(status::no_content);
            (void)co_await res->send_bodyless();
            co_return false;
        }
        co_return true;
    };
}

} // namespace simple_http
