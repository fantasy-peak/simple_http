#pragma once

// Built-in middleware: ready-made Middleware values for the common concerns,
// matching what the Go (chi/gin/echo) and Rust (tower-http) ecosystems ship
// out of the box. Each is a plain Middleware, so it works with use() (global),
// group() (per-group) or route()'s middlewares overload (per-route):
//
//   server.use(simple_http::middleware::request_id());
//   server.use(simple_http::middleware::access_log());
//   server.group("/api", {simple_http::middleware::basic_auth("svc", "s3cret")}, ...);
//   server.route({Method::Post}, "/payments", {simple_http::middleware::recovery()}, handler);
//
// CORS is its own policy (router.cors / make_cors_middleware); compression is
// handled by the compressing writer, not by middleware. Request timeouts are
// covered by EngineLimits (idle/request caps) rather than a middleware, because
// cancelling a mid-flight coroutine chain safely needs engine-level support.

#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp> // real_ip's CIDR matching (ip_covered)

#include <array>

#include "../core/base64.h"
#include "../core/compression.h" // CompressionConfig (middleware::compress)
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/types.h"    // iequals_ci
#include "../core/url_path.h" // decode_and_normalize (clean_path)
#include "handler.h"

namespace simple_http {
namespace middleware {

// The per-request handle request_id() publishes, so a handler or a later
// middleware can read the effective id without re-deriving it:
//   if (auto *id = req->get_state<simple_http::middleware::RequestId>()) ...
struct RequestId {
    std::string value;
};

namespace detail {

// A thread-local generator seeded once; enough uniqueness for a request id.
inline std::string random_hex(std::size_t bytes) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const std::uint8_t b = static_cast<std::uint8_t>(rng() & 0xFF);
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

} // namespace detail

// Access log: one record per request — method, target, response status, wall
// time, and the request id when request_id() runs further in — after the chain
// completed, so a logging middleware reports real answers including the
// router's built-in 404/405. Keep it outermost for shortest timing (tower-http
// TraceLayer placement); register request_id() anywhere inside it.
inline Middleware access_log() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        // maybe_unused: with SIMPLE_HTTP_ENABLE_LOG=0 the log call compiles
        // away and these would otherwise trip -Wunused-variable.
        [[maybe_unused]] const auto start = std::chrono::steady_clock::now();
        // Copy, not move: the after-phase still needs req/res.
        co_await next(req, res, ssl);
        [[maybe_unused]] const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        if (const auto *id = req->get_state<RequestId>()) {
            SIMPLE_HTTP_INFO_LOG("{} [{}] \"{} {}\" {} {}us", req->peer_address(), id->value, req->method_token(),
                                 req->target(), res->status(), elapsed);
        } else {
            SIMPLE_HTTP_INFO_LOG("{} \"{} {}\" {} {}us", req->peer_address(), req->method_token(), req->target(),
                                 res->status(), elapsed);
        }
    };
}

// Request id: adopts a client-provided X-Request-Id, or generates one, echoes
// it in the response (so the client can correlate logs) and publishes it as
// RequestId on the request for the handler and later middleware. Same contract
// as chi's middleware.RequestID / tower-http's TraceLayer-with-request-id.
inline Middleware request_id() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        std::string id;
        if (const auto existing = req->header(field::x_request_id); existing && !existing->empty()) {
            id.assign(*existing);
        } else {
            id = detail::random_hex(16);
        }
        res->header(field::x_request_id, id);
        req->set_state(RequestId{std::move(id)});
        co_await next(std::move(req), std::move(res), ssl);
    };
}

// Recovery: catches an exception from the rest of the chain — a middleware
// that throws, or a handler whose 500 fell through — and answers 500 instead
// of letting the connection die mid-request. Handlers already turn their own
// exceptions into 500 (invoke_handler); this is the same guarantee for the
// chain that wraps them.
inline Middleware recovery() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        // A copy before the chain runs: req/res are moved into next(), so this
        // layer's own references — needed to answer the 500 — must be kept
        // separately (same pattern as invoke_handler's fallback).
        const ResponsePtr fallback = res;
        bool threw = false;
        // maybe_unused: consumed only by the log call below, which compiles
        // away when SIMPLE_HTTP_ENABLE_LOG=0.
        [[maybe_unused]] std::string why;
        try {
            co_await next(std::move(req), std::move(res), ssl);
        } catch (const std::exception &e) {
            threw = true;
            why = e.what();
        } catch (...) {
            threw = true;
            why = "non-std exception";
        }
        // The catch blocks only record — co_await is not permitted inside a
        // coroutine's handler (same discipline as invoke_handler).
        if (threw) {
            SIMPLE_HTTP_ERROR_LOG("middleware chain threw: {}", why);
            (void)co_await fallback->status(status::internal_server_error).send("");
        }
    };
}

// HTTP Basic authentication (RFC 7617): accepts only credentials equal to the
// configured pair, answers 401 + WWW-Authenticate (which invites a browser's
// credential prompt) and short-circuits otherwise. The compare is done on the
// decoded side so a client using an alternate base64 encoding of the same pair
// is accepted rather than rejected by spelling.
inline Middleware basic_auth(std::string username, std::string password) {
    const std::string expected = username + ":" + password;
    return [expected = std::move(expected)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                            Next next) -> asio::awaitable<void> {
        bool ok = false;
        if (const auto auth = req->header(field::authorization)) {
            constexpr std::string_view scheme = "Basic ";
            if (auth->size() > scheme.size() && iequals_ci(auth->substr(0, scheme.size()), scheme)) {
                const std::string decoded = base64_decode(std::string{auth->substr(scheme.size())});
                ok = decoded == expected;
            }
        }
        if (!ok) {
            res->header(field::www_authenticate, "Basic realm=\"restricted\"");
            co_await res->status(status::unauthorized).send("unauthorized");
            co_return; // answered here — the rest of the chain must not run
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

// The real client address, resolved from a forwarded header by real_ip(); a
// handler or a later middleware reads it:
//   if (const auto *ip = req->get_state<simple_http::middleware::ClientIp>()) ...
struct ClientIp {
    std::string value;
};

namespace real_ip_detail {

// Whether a trusted-proxy entry covers `ip`. An entry without '/' is an exact
// address (IPv4 or IPv6, case-insensitive); with '/' it is a CIDR prefix
// ("10.0.0.0/8", "2001:db8::/32"), matched by prefix bits — the tower-http
// trusted-proxy shape.
inline bool ip_covered(std::string_view entry, std::string_view ip) {
    const std::size_t slash = entry.find('/');
    if (slash == std::string_view::npos) {
        return iequals_ci(entry, ip);
    }
    const std::string address_part{entry.substr(0, slash)};
    unsigned prefix = 0;
    for (std::size_t i = slash + 1; i < entry.size(); ++i) {
        if (entry[i] < '0' || entry[i] > '9') {
            return false;
        }
        prefix = prefix * 10 + static_cast<unsigned>(entry[i] - '0');
    }
    error_code ec; // simple_http's boost::system::error_code alias
    const auto net = asio::ip::make_address(address_part, ec);
    const auto host = asio::ip::make_address(std::string{ip}, ec);
    if (ec) {
        return false;
    }
    if (net.is_v4() != host.is_v4()) {
        return false;
    }
    // One fixed-width byte array: a v4 address occupies its first 4 bytes, and
    // the prefix is capped to 32, so the trailing v6 bytes never participate.
    std::array<unsigned char, 16> bytes{};
    std::array<unsigned char, 16> want{};
    const unsigned max_bits = net.is_v4() ? 32 : 128;
    if (net.is_v4()) {
        const auto n = net.to_v4().to_bytes();
        const auto h = host.to_v4().to_bytes();
        for (std::size_t i = 0; i < 4; ++i) {
            bytes[i] = n[i];
            want[i] = h[i];
        }
    } else {
        bytes = net.to_v6().to_bytes();
        want = host.to_v6().to_bytes();
    }
    if (prefix > max_bits) {
        return false;
    }
    unsigned bits = prefix;
    for (std::size_t i = 0; i < bytes.size() && bits > 0; ++i) {
        const unsigned take = std::min(bits, 8u);
        const auto mask = static_cast<unsigned char>(0xFFu << (8 - take));
        if ((bytes[i] & mask) != (want[i] & mask)) {
            return false;
        }
        bits -= take;
    }
    return true;
}

} // namespace real_ip_detail

// real_ip: resolves the client's real address behind a proxy. X-Real-IP wins if
// present (chi's precedence); otherwise X-Forwarded-For is walked — with an
// empty `trusted_proxies` the leftmost (original) address is taken, with a
// list the first address walking right-to-left that is not a trusted proxy
// (tower-http's SetXForwarded semantics). Entries in `trusted_proxies` are
// exact addresses or CIDR prefixes ("10.0.0.0/8", "2001:db8::/32"), IPv4 and
// IPv6. The result is published as ClientIp on the request; it is never used
// as authorization — forwards can be spoofed, so treat it as advisory.
inline Middleware real_ip(std::vector<std::string> trusted_proxies = {}) {
    return [trusted = std::move(trusted_proxies)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                  Next next) -> asio::awaitable<void> {
        if (const auto x_real = req->header("x-real-ip"); x_real && !x_real->empty()) {
            req->set_state(ClientIp{std::string{*x_real}});
            co_await next(std::move(req), std::move(res), ssl);
            co_return;
        }
        if (const auto xff = req->header("x-forwarded-for")) {
            // Split into trimmed segments, left (original client) to right
            // (closest proxy).
            std::vector<std::string_view> hops;
            std::size_t pos = 0;
            while (pos <= xff->size()) {
                const std::size_t comma = xff->find(',', pos);
                std::string_view hop =
                    xff->substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
                while (!hop.empty() && hop.front() == ' ') {
                    hop.remove_prefix(1);
                }
                while (!hop.empty() && hop.back() == ' ') {
                    hop.remove_suffix(1);
                }
                if (!hop.empty()) {
                    hops.push_back(hop);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                pos = comma + 1;
            }
            if (!hops.empty()) {
                std::string_view chosen;
                if (trusted.empty()) {
                    chosen = hops.front();
                } else {
                    chosen = hops.front();
                    for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
                        bool is_trusted = false;
                        for (const auto &t : trusted) {
                            if (real_ip_detail::ip_covered(t, *it)) {
                                is_trusted = true;
                                break;
                            }
                        }
                        if (!is_trusted) {
                            chosen = *it;
                            break;
                        }
                    }
                }
                req->set_state(ClientIp{std::string{chosen}});
            }
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

// clean_path: normalizes the request target before routing — percent-decodes,
// collapses "//" and "/./" segments, and rejects ".." with a 400 (the same
// decoder static_files uses) — rewriting the target when it changed (the query
// is preserved). With this in the chain, routes are registered in canonical
// form and "/a%2Fb", "//a/b", "/a/./b" all reach them; chi's CleanPath
// middleware.
inline Middleware clean_path() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        const std::string_view raw = req->target();
        const std::size_t qmark = raw.find('?');
        const std::string_view path = raw.substr(0, qmark);
        std::string_view query = qmark == std::string_view::npos ? std::string_view{} : raw.substr(qmark);
        PathError err = PathError::None;
        auto normalized = decode_and_normalize(path, err);
        if (!normalized) {
            co_await res->status(status::bad_request).send(""); // traversal / malformed
            co_return;
        }
        if (*normalized != path) {
            req->set_target(*normalized + std::string{query});
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

// strip_prefix: strips a leading path prefix before routing — mount a
// sub-application whose routes are registered from "/" under a shared prefix
// (chi's StripPrefix, nginx's location-with-rewrite). The prefix must start
// with '/' and not end with one: strip_prefix("/api") maps "/api/users" to
// "/users" and "/api" to "/", leaving other paths untouched.
inline Middleware strip_prefix(std::string prefix) {
    return [prefix = std::move(prefix)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                        Next next) -> asio::awaitable<void> {
        std::string target;
        const std::string_view path = req->path();
        if (path == prefix) {
            target = "/";
        } else if (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/') {
            target.assign(path.substr(prefix.size()));
        } else {
            co_await next(std::move(req), std::move(res), ssl);
            co_return; // outside the prefix: pass through untouched
        }
        if (!req->query().empty()) {
            target.push_back('?');
            target.append(req->query());
        }
        req->set_target(std::move(target));
        co_await next(std::move(req), std::move(res), ssl);
    };
}

// redirect_slashes: 301s a path ending in '/' to the same path without it (the
// query is preserved, the root "/" is untouched) — chi's RedirectSlashes, for
// the API that treats "/x" and "/x/" as the same resource. Answer one form,
// redirect the other, so clients and caches converge on the canonical URL.
inline Middleware redirect_slashes() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        const std::string_view path = req->path();
        if (path.size() > 1 && path.back() == '/') {
            std::string location{path.substr(0, path.size() - 1)};
            if (!req->query().empty()) {
                location.push_back('?');
                location.append(req->query());
            }
            co_await res->redirect(std::move(location), status::moved_permanently).send("");
            co_return; // answered here — the route table never sees the slash form
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

namespace middleware_detail {

// Case-insensitive substring test (the upgrade header is `Upgrade: websocket`
// or `WebSocket`, both common).
inline bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const char a = haystack[i + j];
            const char b = needle[j];
            if (a != b && ascii_lower(a) != ascii_lower(b)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

} // namespace middleware_detail

// Rejects WebSocket upgrades whose Origin is not on the allow list — the
// Cross-Site WebSocket Hijacking defense (RFC 6455 §1.3 / OWASP). Only upgrade
// handshakes are in scope; every other request passes through untouched, so the
// middleware can be registered globally:
//
//   server.use(simple_http::middleware::ws_origin({"https://app.example"}));
//
// A browser always sends Origin; non-browser clients may omit it, which
// `allow_missing` permits. Default (false) rejects a missing Origin — a
// handshake a browser would have tagged is exactly the shape a CSWSH payload
// has. Matching is by case-insensitive equality of the whole origin
// (scheme://host[:port]); "*" allows every origin outright.
inline Middleware ws_origin(std::vector<std::string> allowed_origins, bool allow_missing = false) {
    return [allowed = std::move(allowed_origins), allow_missing](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                                 Next next) -> asio::awaitable<void> {
        const auto upgrade = req->header(field::upgrade);
        // A WebSocket handshake is either an HTTP/1.1 Upgrade (Upgrade +
        // Sec-WebSocket-Key) or an HTTP/2 RFC 8441 extended CONNECT (no Upgrade
        // header at all, so the method is the marker).
        const bool is_h1_ws = upgrade && middleware_detail::contains_ci(*upgrade, "websocket") &&
                              req->header("sec-websocket-key").has_value();
        const bool is_h2_ws = req->method() == Method::Connect;
        if (!is_h1_ws && !is_h2_ws) {
            co_await next(std::move(req), std::move(res), ssl);
            co_return; // not a WebSocket handshake: out of scope
        }
        const auto origin = req->header(field::origin);
        if (!origin) {
            if (allow_missing) {
                co_await next(std::move(req), std::move(res), ssl);
                co_return;
            }
            co_await res->status(status::forbidden).send("origin required");
            co_return; // short-circuit: the handshake never reaches the engine's 101
        }
        // Strip a trailing '/', which a hand-built client may append.
        std::string_view o = *origin;
        while (!o.empty() && o.back() == '/') {
            o.remove_suffix(1);
        }
        bool ok = false;
        for (const std::string &entry : allowed) {
            if (entry == "*" || iequals_ci(o, entry)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            co_await res->status(status::forbidden).send("origin not allowed");
            co_return;
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

// Response compression, the chi `middleware.Compress` / gin-contrib `gzip` /
// echo `middleware.Gzip` / fiber `compress` shape: *mounting this enables
// compression for the scope it is attached to* — global (use()), a group, or a
// single route's middleware list. There is no separate server-level switch;
// the middleware is the switch.
//
//   server.use(middleware::compress());                                    // everything
//   server.group("/api", {middleware::compress({.min_bytes = 512})}, reg);  // a group
//   server.route({Method::Get}, "/report", {middleware::compress()}, h);    // one route
//
// Mounting is enabling (like `e.Use(middleware.Gzip())`); to leave something
// alone, either do not mount it there, list it in `excluded_paths`, or pass a
// `skip` predicate — the same knobs Go frameworks call ExcludedPaths/Skipper.
// A request a mounted policy covers still goes through the usual per-response
// rules (client Accept-Encoding, min_bytes, content type, already-encoded,
// no-transform), exactly as the Go middlewares do.
inline Middleware compress(CompressionConfig config = {}, std::function<bool(const Request &)> skip = {}) {
    // Mounting is enabling: the middleware always turns the policy on, so the
    // caller does not have to repeat `enabled = true`.
    config.enabled = true;
    return [config = std::move(config), skip = std::move(skip)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                                Next next) -> asio::awaitable<void> {
        bool covered = true;
        const std::string_view path = req->path();
        for (const std::string &entry : config.excluded_paths) {
            if (entry.empty()) {
                continue;
            }
            if (entry.back() == '/') {
                // A trailing slash excludes a subtree.
                if (path.starts_with(std::string_view{entry}.substr(0, entry.size() - 1))) {
                    covered = false;
                    break;
                }
            } else if (path == entry) {
                covered = false;
                break;
            }
        }
        if (covered && skip && skip(*req)) {
            covered = false;
        }
        if (covered) {
            // The response writer reads this: the innermost/last mounted policy
            // wins, so a group or route can override a broader default.
            req->set_state(config);
        }
        co_await next(std::move(req), std::move(res), ssl);
    };
}

} // namespace middleware
} // namespace simple_http