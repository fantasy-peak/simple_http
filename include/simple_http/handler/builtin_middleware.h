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
// CORS is its own policy (router.cors / make_cors_middleware). Compression is a
// middleware here (middleware::compress): mounting it enables the compressing
// writer for that scope, so there is no separate server-level switch. A
// per-request deadline is middleware::timeout: it fences the chain with a timer
// and, on expiry, cancels the request (see Request::wait_cancelled) so a
// cooperative handler unwinds instead of running on after the client is gone.

#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/address.hpp> // real_ip's CIDR matching (ip_covered)
#include <boost/asio/steady_timer.hpp>

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

// The real client address, resolved from a forwarded header by real_ip(); the
// access log and a handler or later middleware read it:
//   if (const auto *ip = req->get_state<simple_http::middleware::ClientIp>()) ...
struct ClientIp {
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

// Access log: one structured record per request — method, path, status, wall
// time, and the request id / real IP / any LogFields context set further in —
// emitted after the chain completed, so it reports real answers including the
// router's built-in 404/405. The level follows the status (5xx error, 4xx warn),
// as the Go/Rust access-log middlewares do. Keep it outermost for shortest
// timing (tower-http TraceLayer placement); register request_id() anywhere
// inside it. The record's attrs let a JSON sink emit the fields directly; a text
// sink renders them as key=value.
inline Middleware access_log() {
    return [](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
#if SIMPLE_HTTP_ENABLE_LOG
        const auto start = std::chrono::steady_clock::now();
#endif
        // Copy, not move: the after-phase still needs req/res.
        co_await next(req, res, ssl);
        // The budget (timing, peer address, the attrs vector) is only paid when
        // logging is compiled in. The macros compile out at build time; doing
        // the same here is what keeps this middleware a no-op in that build,
        // rather than allocating a vector per request for a record nothing can
        // receive.
#if SIMPLE_HTTP_ENABLE_LOG
        const auto elapsed_us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        const int status = res->status();
        const std::string peer = req->peer_address();

        std::vector<Attr> attrs;
        attrs.reserve(12);
        attrs.emplace_back("method", req->method_token());
        attrs.emplace_back("path", req->path());
        if (!req->query().empty()) {
            attrs.emplace_back("query", req->query());
        }
        attrs.emplace_back("status", static_cast<std::int64_t>(status));
        attrs.emplace_back("duration_ms", static_cast<double>(elapsed_us) / 1000.0);
        attrs.emplace_back("peer", peer);
        attrs.emplace_back("http_version", to_string(req->version()));
        if (const auto *id = req->get_state<RequestId>()) {
            attrs.emplace_back("request_id", id->value);
        }
        if (const auto *ip = req->get_state<ClientIp>()) {
            attrs.emplace_back("client_ip", ip->value);
        }
        if (const auto *fields = req->get_state<LogFields>()) {
            for (const auto &[key, value] : fields->kv) {
                attrs.emplace_back(key, value);
            }
        }

        const LogLevel level = status >= 500 ? LogLevel::Error : (status >= 400 ? LogLevel::Warn : LogLevel::Info);
        log_with_attrs(level, "access", std::source_location::current(), "request", attrs);
#endif
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
        // A cancelled chain (request deadline, peer gone) owns no reply: the
        // layer that cancelled it answers. Answering 500 here would race that
        // reply and turn an intended 504 into a 500.
        const RequestPtr req_keep = req;
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
            if (req_keep && req_keep->cancelled()) {
                co_return;
            }
            SIMPLE_HTTP_ERROR_LOG("middleware chain threw: {}", why);
            if (fallback->started()) {
                (void)co_await fallback->close();
            } else {
                (void)co_await fallback->status(status::internal_server_error).send("");
            }
        }
    };
}

namespace basic_auth_detail {
// Compares without an early exit on the first differing byte, so a remote peer
// cannot recover the configured credentials byte by byte from response timing.
// The length is still observable; it is fixed by the configuration, not secret.
inline bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}
} // namespace basic_auth_detail

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
                ok = basic_auth_detail::constant_time_equal(decoded, expected);
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

// The direct peer has to be one of the configured proxies before anything it
// says about a forwarded address is believed. An empty list trusts nobody.
inline bool peer_is_trusted(std::string_view peer, const std::vector<std::string> &trusted) {
    for (const auto &entry : trusted) {
        if (ip_covered(entry, peer)) {
            return true;
        }
    }
    return false;
}

} // namespace real_ip_detail

// real_ip: resolves the client's real address behind a proxy. The forwarded
// fields are consulted only when the *direct peer* itself is covered by
// `trusted_proxies`; otherwise they are ignored entirely (fail closed). With a
// trusted peer, X-Real-IP wins if present (chi's precedence); otherwise
// X-Forwarded-For is walked right-to-left and the first address that is not a
// trusted proxy is taken (tower-http's SetXForwarded semantics). Entries in
// `trusted_proxies` are exact addresses or CIDR prefixes ("10.0.0.0/8",
// "2001:db8::/32"), IPv4 and IPv6.
//
// An empty list means "no proxy is trusted": the peer address remains the only
// identity, and `rate_limit`'s default key falls back to it. This is deliberate
// — only a peer we already trust may speak for someone else. The result is
// still advisory for authorization (a trusted proxy can be misconfigured), but
// it is no longer trivially spoofable by a direct client.
inline Middleware real_ip(std::vector<std::string> trusted_proxies = {}) {
    return [trusted = std::move(trusted_proxies)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                  Next next) -> asio::awaitable<void> {
        if (real_ip_detail::peer_is_trusted(req->peer_address(), trusted)) {
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
                    // The peer is trusted, so `trusted` is non-empty. Walk from
                    // the closest proxy outward; the first untrusted hop is the
                    // client, and if every hop is a trusted proxy the leftmost
                    // (original) entry is the best answer available.
                    std::string_view chosen = hops.front();
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
                    req->set_state(ClientIp{std::string{chosen}});
                }
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
        const bool is_h1_ws =
            upgrade && icontains_ci(*upgrade, "websocket") && req->header("sec-websocket-key").has_value();
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

// Per-request deadline, streaming-aware. The rule is one sentence: **the handler
// must start responding within `deadline`; once it has, it must not go silent for
// `deadline`.** So:
//   * before the response head goes out this is an absolute deadline — a hung or
//     slow handler that has produced nothing is cancelled at `deadline` (and a
//     handler reading a slow upload without responding is bounded the same way);
//   * once the head is out every write pushes the deadline back — an SSE stream,
//     a long download or a streaming proxy that keeps producing is never killed,
//     while one that stalls (a peer that stopped reading, an upstream that went
//     quiet) is.
//
// On expiry:
//   * the request's cancellation token fires first, so invoke_handler sees a
//     cancellation rather than a handler failure (no spurious 500) and any
//     coroutine the handler spawned and left running observes it;
//   * the chain is then cancelled — asio's `operator||` cancels the loser, so a
//     handler parked on a timer, a socket or wait_cancelled() unwinds at once;
//   * the reply is finished — a 504 if the head had not gone out yet, otherwise
//     the response is aborted with close(), because a truncated body must never
//     look complete.
//
//   server.use(simple_http::middleware::timeout(std::chrono::seconds(5)));
//
// Because the chain is cancelled rather than merely signalled, a handler cannot
// catch its own deadline and reply — producing the timeout reply is this
// middleware's job. The token is for work that outlives the handler body. For a
// cancellation a handler *can* observe and react to, see the engine-driven
// signals (peer stream reset / connection close): those fire Request::cancel
// without cancelling the handler, so `co_await req->wait_cancelled()` there is
// the cooperative pattern.
//
// Cancellation reaches the handler's pending await, which is as far as any
// framework can go: a handler spinning in CPU or blocked in a non-asio call is
// not interrupted. WebSocket handshakes pass through untouched — a deadline on
// a long-lived tunnel is a footgun, and this middleware is commonly mounted
// globally.
inline Middleware timeout(std::chrono::milliseconds deadline) {
    return [deadline](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
        // WebSocket (h1 Upgrade, or the h2 RFC 8441 extended CONNECT): out of
        // scope — a tunnel has no "one request" deadline.
        const auto upgrade = req->header(field::upgrade);
        const bool ws =
            (upgrade && icontains_ci(*upgrade, "websocket") && req->header("sec-websocket-key").has_value()) ||
            req->method() == Method::Connect;
        if (ws) {
            co_await next(std::move(req), std::move(res), ssl);
            co_return;
        }

        using namespace asio::experimental::awaitable_operators;
        using clock = std::chrono::steady_clock;
        // Poll granularity: a fraction of the deadline, bounded so a long deadline
        // does not wake often and a short one stays precise.
        auto tick = deadline / 4;
        if (tick < std::chrono::milliseconds(10))
            tick = std::chrono::milliseconds(10);
        if (tick > std::chrono::seconds(1))
            tick = std::chrono::seconds(1);
        if (tick > deadline && deadline > std::chrono::milliseconds(0))
            tick = deadline;

        auto timer = std::make_shared<asio::steady_timer>(co_await asio::this_coro::executor);
        auto last_change = std::make_shared<clock::time_point>(clock::now());
        auto last_seq = std::make_shared<std::uint64_t>(res->activity());
        // Poll response progress rather than observing writes directly: the
        // activity counter is a relaxed atomic, so this stays correct even if the
        // handler writes from another thread. Before the head there are no writes,
        // so the window is absolute; after it, each write advances the counter and
        // resets the window. Firing the token before returning lets invoke_handler
        // recognise the unwinding as a cancellation, not a handler failure.
        auto on_deadline = [timer, req, res, last_change, last_seq, deadline, tick]() -> asio::awaitable<void> {
            for (;;) {
                timer->expires_after(tick);
                auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
                if (ec)
                    co_return; // cancelled by operator|| (the chain finished)
                const std::uint64_t seq = res->activity();
                if (seq != *last_seq) { // progress: reset the window
                    *last_seq = seq;
                    *last_change = clock::now();
                    continue;
                }
                if (clock::now() - *last_change >= deadline) {
                    req->cancel();
                    co_return;
                }
            }
        };
        // `operator||` runs both operands on this coroutine's executor and cancels
        // the loser; `res` is kept for the after-phase — next() receives its own
        // copy of the shared_ptr.
        auto outcome = co_await (next(req, res, ssl) || on_deadline());
        if (outcome.index() == 0) {
            co_return; // the chain finished first
        }
        if (!res->started()) {
            (void)co_await res->status(status::gateway_timeout).content_type("text/plain").send("Gateway Timeout");
        } else {
            co_await res->close(); // mid-stream: abort, never truncate silently
        }
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