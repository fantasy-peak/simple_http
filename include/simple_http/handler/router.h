#pragma once

// Router: matches a request to a handler and runs it.
//
// A route is a (method, path) pair; matching order is exact path map -> regex
// list (first match) -> fallback. The methods are explicit at registration;
// registering the empty list (`any_methods`) means "any method". Two implicit
// rules follow the modern frameworks (Flask / axum / Go 1.22): a route that
// serves GET also serves HEAD (the writer strips the body), and a path whose
// method does not match is answered 405 with an Allow header — or an automatic
// 204 OPTIONS — while a path nothing services continues to the 404. Optional
// `before` and `cors` filters run first and may short-circuit. The Router's
// dispatch(shared_ptr<Request>, shared_ptr<Response>, SslHandle) matches the engine's
// Dispatcher type, so the same router serves every protocol version.

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include "../client/http_client.h"  // HttpClient (reverse-proxy upstreams)
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../engine/dispatcher.h"  // WsProxyTarget, HttpProxyTarget
#include "cors.h"  // CorsConfig (built-in CORS -> a Filter)
#include "handler.h"
#include "http_proxy.h"
#include "static_files.h"  // the static stage (m_static)

#ifdef SIMPLE_HTTP_USE_BOOST_REGEX
#include <boost/regex.hpp>
namespace simple_http_regex = boost;
#else
#include <regex>
namespace simple_http_regex = std;
#endif

namespace simple_http {

namespace asio = boost::asio;

// A reverse-proxy route: the backend recipe (`HttpProxyTarget`) plus the
// HttpClient that reaches it. Every proxy route builds its own client, so each
// backend carries its own TLS trust policy and connection pool — an internal
// mTLS backend and a public one can never share credentials or sockets.
struct HttpProxyRoute {
    HttpProxyTarget target;
    std::shared_ptr<HttpClient> client;
};

// A route serving every method. Pass the empty set — or this named constant to
// say "any" out loud at the call site: `route(any_methods, "/x", h)`.
inline const std::vector<Method> any_methods{};

// A registered route: the methods it serves, folded into a bitmask, and the
// handler. Implied rules are applied at registration: the empty set (see
// `any_methods`) becomes every method, and GET implies HEAD.
struct RouteEntry {
    // Every Method enumerator, Unknown included, so an any-method route matches
    // an extension-method token (`method_bit(Unknown)` is in the mask).
    static constexpr std::uint16_t all_bits = 0x03FF;
    std::uint16_t method_bits{};
    Handler handler;
};

class Router {
  public:
    // The reverse-proxy routes each build their own HttpClient (per-route TLS
    // policy and pool) at registration — see http_proxy.
    Router() = default;

    // Closes every reverse-proxy route's pooled idle sessions. The server calls
    // this just before stopping its io pool: the pooled keep-alive connections
    // are bound to worker executors, and if they are left open they outlive the
    // process (their TLS streams leak at exit — ASan finds them as ssl::streams
    // never freed). The pool's drain is what unwinds the posted closes. Idle
    // only: a session in the middle of a request is not in the pool and is left
    // alone.
    void close_proxy_client() {
        for (const auto& [_, route] : m_http_proxy_exact) {
            route.client->close_idle();
        }
        for (const auto& [_, route] : m_http_proxy_regex) {
            route.client->close_idle();
        }
    }

    // --- registration (fluent) ---
    // Methods come first: a route is a (method, path) pair. The list may be a
    // braced literal (`{Method::Get, Method::Post}`), `any_methods`, or — the
    // config-file case — the std::vector<Method> a parser filled, passed
    // straight through: `route(parsed_methods, "/x", h)`.
    template <typename F>
    Router& route(std::vector<Method> methods, std::string path, F&& handler) {
        auto entry = make_route_entry(methods, make_handler(std::forward<F>(handler)));
        if (!entry) {
            return *this;  // Method::Unknown rejected — see make_route_entry
        }
        // emplace, not insert_or_assign: the first registration wins, and that is
        // deliberate — test_router.cpp pins it by name. What it should not do is
        // keep the second one *silently*, since re-registering a path is a likely
        // result of moving a route around, so the duplicate says so.
        auto [it, inserted] = m_exact.emplace(std::move(path), std::move(*entry));
        if (!inserted) {
            SIMPLE_HTTP_INFO_LOG("route [{}] already registered; the first handler stays", it->first);
        }
        return *this;
    }

    template <typename F>
    Router& route_regex(std::vector<Method> methods, const std::string& pattern, F&& handler) {
        auto entry = make_route_entry(methods, make_handler(std::forward<F>(handler)));
        if (!entry) {
            return *this;  // Method::Unknown rejected — see make_route_entry
        }
        try {
            m_regex.emplace_back(RegexRoute{simple_http_regex::regex{pattern}, literal_prefix(pattern)},
                                 std::move(*entry));
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("invalid route regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // Registers a static file site. It becomes a *stage* of dispatch rather than
    // a route, which is what makes the ordering guarantee structural: a real
    // route always wins over a file of the same name, and — because the built-in
    // 404 sits behind the stage — a site that declines every path still cannot
    // leave a request unanswered.
    //
    // A disabled site (empty root) is ignored rather than registered, with the
    // reason logged: "why is my site serving 404s" should not be a silent
    // configuration mistake.
    Router& static_files(std::shared_ptr<StaticFiles> site) {
        if (!site || !site->enabled()) {
            SIMPLE_HTTP_INFO_LOG("static files: ignoring a disabled site");
            return *this;
        }
        m_static.push_back(std::move(site));
        return *this;
    }

    template <typename F>
    Router& fallback(F&& handler) {
        m_fallback = make_handler(std::forward<F>(handler));
        return *this;
    }

    Router& before(Filter filter) {
        m_before = std::move(filter);
        return *this;
    }
    // CORS: a policy in, and the filter it compiles to is what dispatch runs. See
    // handler/cors.h — including why a preflight never reaches a route. A policy the
    // config cannot express goes in a before() filter instead, which can start from
    // make_cors_filter() if it only wants to narrow the built-in behaviour.
    Router& cors(CorsConfig config) {
        m_cors = make_cors_filter(std::move(config));
        return *this;
    }

    // --- WebSocket route registration ---
    Router& ws_route(std::string path, WsHandler handler) {
        m_ws_exact.emplace(std::move(path), std::move(handler));
        return *this;
    }

    Router& ws_route_regex(const std::string& pattern, WsHandler handler) {
        try {
            m_ws_regex.emplace_back(RegexRoute{simple_http_regex::regex{pattern}, literal_prefix(pattern)}, std::move(handler));
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("invalid ws route regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // --- WebSocket proxy-route registration (byte-level pass-through) ---
    Router& ws_proxy(std::string path, WsProxyTarget target) {
        m_ws_proxy_exact.emplace(std::move(path), std::move(target));
        return *this;
    }

    Router& ws_proxy_regex(const std::string& pattern, WsProxyTarget target) {
        try {
            m_ws_proxy_regex.emplace_back(RegexRoute{simple_http_regex::regex{pattern}, literal_prefix(pattern)}, std::move(target));
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("invalid ws proxy regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // --- HTTP reverse-proxy route registration (request-level) ---
    Router& http_proxy(std::string path, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        m_http_proxy_exact.emplace(std::move(path),
                                   HttpProxyRoute{std::move(target),
                                                  std::make_shared<HttpClient>(std::move(client_cfg))});
        return *this;
    }

    Router& http_proxy_regex(const std::string& pattern, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        try {
            m_http_proxy_regex.emplace_back(
                RegexRoute{simple_http_regex::regex{pattern}, literal_prefix(pattern)},
                HttpProxyRoute{std::move(target), std::make_shared<HttpClient>(std::move(client_cfg))});
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("invalid http proxy regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // Local WebSocket handler for an exact path (nginx `location = /path`):
    // the h1 engine consults this before a proxy route for the same path.
    const WsHandler* find_ws_exact(std::string_view path) const {
        if (auto it = m_ws_exact.find(path); it != m_ws_exact.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Local WebSocket handler via the regex routes, consulted after the proxy
    // lookup. Same literal-prefix fast path as the other regex walks.
    const WsHandler* find_ws_regex(std::string_view path) const {
        if (m_ws_regex.empty()) {
            return nullptr;  // nothing to match, and no string to build for it
        }
        for (const auto& [route, handler] : m_ws_regex) {
            if (!route.literal_prefix.empty() && !path.starts_with(route.literal_prefix)) continue;
            const std::string p{path};  // regex_match needs std::string; built only when a prefix may match
            if (simple_http_regex::regex_match(p, route.pattern)) {
                return &handler;
            }
        }
        return nullptr;
    }

    // Combined exact-then-regex lookup. The engine uses find_ws_exact and
    // find_ws_regex separately (in nginx order a proxy route sits between
    // them); this form is kept for callers that only ask "is there a local ws
    // handler at all".
    const WsHandler* find_ws(std::string_view path) const {
        if (const WsHandler* h = find_ws_exact(path)) return h;
        return find_ws_regex(path);
    }

    // Looks up a WebSocket proxy backend for a path (exact then regex). Returns
    // std::nullopt if the path is not a proxy route. Consulted by the engine
    // between the local exact and the local regex ws handler.
    //
    // For a regex route whose rewrite_path is a substitution template, capture
    // groups from the match are expanded here, so the returned target's
    // rewrite_path is the final path to send to the backend.
    std::optional<WsProxyTarget> find_ws_proxy(std::string_view path) const {
        if (auto it = m_ws_proxy_exact.find(path); it != m_ws_proxy_exact.end()) {
            return it->second;  // exact route: rewrite_path used verbatim
        }
        if (m_ws_proxy_regex.empty()) {
            return std::nullopt;
        }
        for (const auto& [route, target] : m_ws_proxy_regex) {
            if (!route.literal_prefix.empty() && !path.starts_with(route.literal_prefix)) continue;
            const std::string p{path};  // regex_match needs std::string; built only when a prefix may match
            simple_http_regex::smatch m;
            if (simple_http_regex::regex_match(p, m, route.pattern)) {
                WsProxyTarget out = target;
                if (!out.rewrite_path.empty()) {
                    out.rewrite_path = expand_rewrite(target.rewrite_path, m);
                }
                return out;
            }
        }
        return std::nullopt;
    }

    // Looks up an HTTP reverse-proxy route for a path (exact then regex):
    // the backend recipe and its own client. Returns std::nullopt if the path
    // is not a proxy route. For a regex route whose rewrite_path is a
    // substitution template, capture groups are expanded here, so the returned
    // target's rewrite_path is the final target to send.
    std::optional<HttpProxyRoute> find_http_proxy(std::string_view path) const {
        if (auto it = m_http_proxy_exact.find(path); it != m_http_proxy_exact.end()) {
            return it->second;
        }
        if (m_http_proxy_regex.empty()) {
            return std::nullopt;
        }
        for (const auto& [route, proxy] : m_http_proxy_regex) {
            if (!route.literal_prefix.empty() && !path.starts_with(route.literal_prefix)) continue;
            const std::string p{path};  // regex_match needs std::string; built only when a prefix may match
            simple_http_regex::smatch m;
            if (simple_http_regex::regex_match(p, m, route.pattern)) {
                HttpProxyRoute out = proxy;
                if (!out.target.rewrite_path.empty()) {
                    out.target.rewrite_path = expand_rewrite(proxy.target.rewrite_path, m);
                }
                return out;
            }
        }
        return std::nullopt;
    }

    // --- dispatch (satisfies the engine Dispatcher) ---
    asio::awaitable<void> dispatch(RequestPtr req, ResponsePtr res, SslHandle ssl) const {
        // CORS runs only for cross-origin requests. The filter answers an OPTIONS
        // preflight here and returns false, so a preflight never reaches a route.
        if (m_cors && req->header("origin").has_value()) {
            if (!co_await m_cors(req, res)) {
                co_return;
            }
        }
        if (m_before) {
            if (!co_await m_before(req, res)) {
                co_return;
            }
        }

        // A view, not a copy: the exact maps have transparent hashers, so they can
        // be probed with the request's own path, and the proxy lookups take a
        // string_view too. The copy is only needed for the regex walk — and only
        // when there are regex routes, which is the uncommon case. `req->path()`
        // stays valid until req is moved into a handler below, and it is not
        // touched after that.
        const std::string_view path = req->path();
        const Method method = req->method();

        // Local exact routes win outright — nginx's `location = /path` beats
        // everything, a proxy registered for the same path included. An exact
        // endpoint therefore never pays the reverse-proxy lookup at all. A
        // method this path does not serve is answered here (resolved first,
        // rejected second): 405 + Allow, or the automatic OPTIONS reply — a
        // path that nothing services continues below and ends in the 404.
        if (auto it = m_exact.find(path); it != m_exact.end()) {
            const RouteEntry& entry = it->second;
            if (method_allowed(entry, method)) {
                co_await invoke_handler(entry.handler, std::move(req), std::move(res), ssl);
                co_return;
            }
            if (method == Method::Options) {
                co_await reply_auto_options(res, entry.method_bits);
            } else {
                co_await reply_method_not_allowed(res, entry.method_bits);
            }
            co_return;
        }

        // HTTP reverse-proxy routes: consulted after the exact local match but
        // before local regex/static handlers (exact before regex in both
        // tables). A dead backend still surfaces as a 502 for proxy-only paths.
        if (auto proxy = find_http_proxy(path)) {
            bool client_is_tls = ssl.has_value() && *ssl != nullptr;
            co_await run_http_proxy(std::move(req), std::move(res), client_is_tls, std::move(proxy->target),
                                    *proxy->client);
            co_return;
        }

        if (!m_regex.empty()) {
            const std::string owned_path{path};  // regex_match needs a string
            bool method_mismatch = false;
            std::uint16_t mismatch_bits = 0;
            for (const auto& [route, entry] : m_regex) {
                if (!route.literal_prefix.empty() && !path.starts_with(route.literal_prefix)) continue;
                if (!simple_http_regex::regex_match(owned_path, route.pattern)) continue;
                if (method_allowed(entry, method)) {
                    co_await invoke_handler(entry.handler, std::move(req), std::move(res), ssl);
                    co_return;
                }
                // The path resolved to this pattern but the method was not
                // allowed. Keep walking: a later pattern may resolve the same
                // path with an allowed method. Method bits accumulate, so the
                // 405's Allow lists every method any matching pattern serves.
                method_mismatch = true;
                mismatch_bits |= entry.method_bits;
            }
            if (method_mismatch) {
                if (method == Method::Options) {
                    co_await reply_auto_options(res, mismatch_bits);
                } else {
                    co_await reply_method_not_allowed(res, mismatch_bits);
                }
                co_return;
            }
        }
        // The static stage. Running it here — after the routes, before the
        // fallback — is what keeps a placeholder file from shadowing a real
        // endpoint, and it is also why a site's try_serve may return "not mine"
        // at any point without the request being dropped: whatever comes next in
        // this function still runs, and the built-in 404 is the last of them.
        for (const auto& site : m_static) {
            if (co_await site->try_serve(req, res)) co_return;
        }

        if (m_fallback) {
            co_await invoke_handler(*m_fallback, std::move(req), std::move(res), ssl);
            co_return;
        }
        // Built-in 404.
        co_await res->status(status::not_found).send("");
        co_return;
    }

  private:
    // The empty method set (see `any_methods`) means "any method", and GET implies
    // HEAD — the two implicit rules shared with Flask / axum / Go 1.22.
    static std::optional<RouteEntry> make_route_entry(const std::vector<Method>& methods, Handler handler) {
        std::uint16_t bits = 0;
        for (Method m : methods) {
            if (m == Method::Unknown) {
                SIMPLE_HTTP_ERROR_LOG("route: Method::Unknown is not routable; registration skipped");
                return std::nullopt;
            }
            bits |= method_bit(m);
        }
        if (bits == 0) {
            bits = RouteEntry::all_bits;  // any_methods — every method, Unknown included
        } else if ((bits & method_bit(Method::Get)) != 0) {
            // A GET route also serves HEAD; the response writer strips the body
            // (regression-tested: "HEAD and 204 carry no body").
            bits |= method_bit(Method::Head);
        }
        return RouteEntry{bits, std::move(handler)};
    }

    static bool method_allowed(const RouteEntry& entry, Method m) noexcept {
        return (entry.method_bits & method_bit(m)) != 0;
    }

    // The Allow header (RFC 9110 §10.2.1) for a method bitmask, in canonical
    // method order. HEAD appears with GET (folded in at registration) and
    // OPTIONS always appears: the router answers OPTIONS automatically for a
    // path it resolved, so it is supported even when never registered.
    static std::string allow_value(std::uint16_t bits) {
        bits |= method_bit(Method::Options);
        std::string out;
        for (Method m : {Method::Get, Method::Head, Method::Post, Method::Put, Method::Delete,
                         Method::Options, Method::Patch, Method::Connect, Method::Trace}) {
            if ((bits & method_bit(m)) == 0) continue;
            if (!out.empty()) out += ", ";
            out += to_string(m);
        }
        return out;
    }

    // Resolved first, rejected second: called only once a route's path matched
    // but its method did not. The reply shape mirrors the static stage's 405
    // (static_files.h), including the close that keeps the engine from draining
    // a request body that was never going to be used.
    asio::awaitable<void> reply_method_not_allowed(ResponsePtr res, std::uint16_t bits) const {
        res->status(status::method_not_allowed);
        res->header(field::allow, allow_value(bits));
        res->content_type("text/plain; charset=utf-8");
        (void)co_await res->send("405 Method Not Allowed");
        (void)co_await res->close();
    }

    asio::awaitable<void> reply_auto_options(ResponsePtr res, std::uint16_t bits) const {
        res->status(status::no_content);
        res->header(field::allow, allow_value(bits));
        (void)co_await res->send("");
        (void)co_await res->close();
    }

    // Expands a rewrite template against a regex match: $0 = whole match,
    // $1..$9 = capture groups (empty if the group did not participate), $$ = a
    // literal '$'. A lone '$' or '$' before a non-digit/non-'$' is kept verbatim.
    static std::string expand_rewrite(const std::string& tmpl, const simple_http_regex::smatch& m) {
        std::string out;
        out.reserve(tmpl.size());
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] != '$') {
                out.push_back(tmpl[i]);
                continue;
            }
            if (i + 1 >= tmpl.size()) {
                out.push_back('$');  // trailing '$'
                break;
            }
            char c = tmpl[i + 1];
            if (c == '$') {
                out.push_back('$');
                ++i;
            } else if (c >= '0' && c <= '9') {
                std::size_t idx = static_cast<std::size_t>(c - '0');
                if (idx < m.size() && m[idx].matched) {
                    out.append(m[idx].str());
                }
                ++i;
            } else {
                out.push_back('$');  // not a placeholder; keep the '$'
            }
        }
        return out;
    }

    struct RegexRoute {
        simple_http_regex::regex pattern;
        std::string literal_prefix;  // leading literal bytes; empty = pattern starts with a metachar
    };

    // Leading literal characters of a regex source — everything before the first
    // metacharacter ('\ ^ $ . [ ] * + ? ( ) { } |'). Both regex engines are
    // configured case-sensitive and regex_match spans the whole input, so any
    // path that could match MUST begin with these exact bytes. Checking the
    // prefix first spares every non-matching request a full backtracking match
    // (Boost.Regex especially); when the prefix is empty nothing is skipped and
    // semantics are byte-for-byte unchanged.
    static std::string literal_prefix(std::string_view pattern) {
        std::string out;
        out.reserve(pattern.size());
        for (char c : pattern) {
            switch (c) {
                case '\\':
                case '^':
                case '$':
                case '.':
                case '[':
                case ']':
                case '*':
                case '+':
                case '?':
                case '(':
                case ')':
                case '{':
                case '}':
                case '|':
                    return out;  // metacharacter ends the literal run
                default:
                    out.push_back(c);
            }
        }
        return out;
    }

    struct string_hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };

    std::unordered_map<std::string, RouteEntry, string_hash, std::equal_to<>> m_exact;
    std::vector<std::pair<RegexRoute, RouteEntry>> m_regex;
    // Static file sites, consulted in registration order after the regex routes
    // and before the fallback. A vector rather than one slot so mounting several
    // roots is a later addition rather than a change of shape.
    std::vector<std::shared_ptr<StaticFiles>> m_static;
    std::optional<Handler> m_fallback;
    Filter m_before;
    Filter m_cors;

    std::unordered_map<std::string, WsHandler, string_hash, std::equal_to<>> m_ws_exact;
    std::vector<std::pair<RegexRoute, WsHandler>> m_ws_regex;

    std::unordered_map<std::string, WsProxyTarget, string_hash, std::equal_to<>> m_ws_proxy_exact;
    std::vector<std::pair<RegexRoute, WsProxyTarget>> m_ws_proxy_regex;

    std::unordered_map<std::string, HttpProxyRoute, string_hash, std::equal_to<>> m_http_proxy_exact;
    std::vector<std::pair<RegexRoute, HttpProxyRoute>> m_http_proxy_regex;
};

}  // namespace simple_http
