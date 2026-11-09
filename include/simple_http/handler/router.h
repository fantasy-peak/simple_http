#pragma once

// Router: matches a request to a handler and runs it (C++17 token-only port).
//
// A route is a (method, path) pair; matching order is exact path map -> path
// template trie -> regex list (first match) -> fallback. Registering the empty
// method list (`any_methods`) means "any method". Two implicit rules follow the
// modern frameworks: a route that serves GET also serves HEAD (the writer
// strips the body), and a path whose method does not match is answered 405 with
// an Allow header -- or an automatic 204 OPTIONS -- while a path nothing
// services continues to the 404.
//
// Middleware keeps the tower shape, expressed with completion handlers. Each
// middleware gets the request and a `next` continuation ending at route
// matching: it may run code before calling next, short-circuit by never calling
// it, and run code after next completes via the completion it passed in. Global
// middleware wraps the whole router; group() middleware wraps just the group's
// routes; per-route middleware wraps one route's handler.
//
// The Router's dispatch(...) matches the engine's Dispatcher type, so the same
// router serves every protocol version.
//
// Deferred to later phases (not in this first port): OpenAPI descriptors,
// static-file stages and reverse-proxy routes. The route table, middleware
// chain, template trie, host routing and WebSocket registration are complete.

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/unordered_map.hpp>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../core/glaze_support.h"
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/mime.h"
#include "../core/types.h"
#include "../engine/dispatcher.h"
#include "../openapi/openapi_doc.h"
#if SIMPLE_HTTP_HAS_GLAZE
#include "../openapi/openapi.h"
#endif
#include "cors.h"
#include "handler.h"
#include "http_proxy.h"
#include "static_files.h"

#ifdef SIMPLE_HTTP_USE_BOOST_REGEX
#include <boost/regex.hpp>
namespace simple_http_regex = boost;
#else
#include <regex>
namespace simple_http_regex = std;
#endif

namespace simple_http {

namespace asio = boost::asio;

// A reverse-proxy route: the backend recipe plus the client policy it dials
// with. Each proxied request builds a fresh Client from this config.
struct HttpProxyRoute {
    HttpProxyTarget target;
    ClientConfig client_cfg;
    std::vector<Middleware> middlewares;
};

// A registered WebSocket route: the ws handler plus its middleware chain.
struct WsRouteEntry {
    WsHandler handler;
    std::vector<Middleware> mws;
};

// A route serving every method. Pass the empty set -- or this named constant to
// say "any" out loud at the call site: `route(any_methods, "/x", h)`.
inline const std::vector<Method> any_methods{};

// A registered route: the methods it serves, folded into a bitmask, and the
// handler. GET implies HEAD; the empty method set means every method.
struct RouteEntry {
    // Every Method enumerator, Unknown included, so an any-method route matches
    // an extension-method token (`method_bit(Unknown)` is in the mask).
    static constexpr std::uint16_t all_bits = 0x03FF;
    std::uint16_t method_bits{};
    Handler handler;
    // Per-route middleware, composed once at registration around the handler.
    Next route_chain;
};

// A path-template route (`/users/{id}`): segments fixed at registration,
// std::nullopt marking a `{name}` capture.
struct TemplateRoute {
    std::vector<std::optional<std::string>> segments;
    std::vector<std::string> param_names; // one per nullopt segment
    // The final segment was written `{*name}`: captures the rest of the path.
    bool wildcard{false};
};

// A registration scope: one open group() call.
struct RegistrationScope {
    std::string prefix;
    std::vector<Middleware> mws;
};

class Router {
  public:
    using StateRegistry = AppStateView::Registry;

    Router() : m_states(std::make_shared<StateRegistry>()) {}

    // --- registration (fluent) ---

    // The optional trailing argument is an OpenAPI operation descriptor
    // (openapi::doc()...). A route that passes one is documented, a route that
    // does not is not, and dispatch is identical either way. The descriptor
    // reduces its schemas to strings at the call site, so the router only ever
    // sees one plain object. Without glaze (a plain C++17 build) there is no
    // descriptor type; the argument is accepted and ignored.
    template <typename F, typename Doc = openapi::NoDoc>
    Router &route(std::vector<Method> methods, std::string path, F &&handler, Doc doc = {}) {
        return route(std::move(methods), std::move(path), {}, std::forward<F>(handler), std::move(doc));
    }
    template <typename F, typename Doc = openapi::NoDoc>
    Router &route(std::vector<Method> methods, std::string path, std::vector<Middleware> middlewares, F &&handler,
                  Doc doc = {}) {
        auto scoped_registration = scoped(std::move(path), std::move(middlewares));
#if SIMPLE_HTTP_HAS_GLAZE
        // The descriptor's path-parameter binding is checked before anything is
        // registered: a mismatch is a loud skip, not a runtime 404 or a silent
        // divergence between the document and the route table.
        if constexpr (!std::is_same_v<std::decay_t<Doc>, openapi::NoDoc>) {
            if (!doc.path_params_match(scoped_registration.first)) {
                SIMPLE_HTTP_ERROR_LOG("route [{}]: template {{name}}s do not match the Params fields; "
                                      "registration skipped",
                                      scoped_registration.first);
                return *this;
            }
        }
#else
        (void)doc;
#endif
        auto entry =
            make_route_entry(methods, make_handler(std::forward<F>(handler)), std::move(scoped_registration.second));
        Router &result = insert_route(methods, scoped_registration.first, std::move(entry));
#if SIMPLE_HTTP_HAS_GLAZE
        if constexpr (!std::is_same_v<std::decay_t<Doc>, openapi::NoDoc>) {
            openapi::record_operation(*m_openapi, doc, methods, scoped_registration.first);
        }
#endif
        return result;
    }

    template <typename F> Router &route_regex(std::vector<Method> methods, const std::string &pattern, F &&handler) {
        return route_regex(std::move(methods), pattern, {}, std::forward<F>(handler));
    }
    template <typename F>
    Router &route_regex(std::vector<Method> methods, const std::string &pattern, std::vector<Middleware> middlewares,
                        F &&handler) {
        auto scoped_registration = scoped(pattern, std::move(middlewares));
        auto entry =
            make_route_entry(methods, make_handler(std::forward<F>(handler)), std::move(scoped_registration.second));
        if (!entry) {
            return *this; // Method::Unknown rejected -- see make_route_entry
        }
        const std::string full_pattern = m_scopes.empty() ? pattern : scope_regex(pattern);
        try {
            m_regex.emplace_back(RegexRoute{simple_http_regex::regex{full_pattern}, literal_prefix(full_pattern)},
                                 std::move(*entry));
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("invalid route regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // --- OpenAPI document ---

    // The document being collected, for its info fields:
    //     server.openapi().title("petshop").version("1.0.0");
    openapi::OpenApiSpec &openapi() { return *m_openapi; }

    // Cross-checks the document against the route tables and logs every
    // operation the doc claims but the router does not serve -- a registration
    // that was skipped while its doc entry went through is exactly the
    // divergence that finds.
    void verify_openapi() const {
        for (const auto &[path, methods] : m_openapi->operations()) {
            for (Method m : methods) {
                if (!serves_route(m, path)) {
                    SIMPLE_HTTP_WARN_LOG("openapi: documented {} {} is not served by the router", to_string(m), path);
                }
            }
        }
    }

    // Serves the collected document at `path`. Render runs per request, so
    // routes added after this call still appear in the document.
    Router &serve_openapi(std::string path = "/openapi.json") {
        verify_openapi();
        auto spec = m_openapi;
        route(any_methods, std::move(path), [spec](RequestPtr, ResponsePtr res, HandlerCompletion done) {
            res->status(status::ok).content_type(mime::app_json);
            res->async_send(spec->render_json(), finish_with(std::move(done)));
        });
        return *this;
    }

    // Serves a CDN-backed Swagger UI page that loads the document from
    // `spec_url` (usually the path passed to serve_openapi).
    Router &serve_swagger_ui(std::string path = "/swagger", std::string spec_url = "/openapi.json") {
        route(any_methods, std::move(path),
              [spec_url = std::move(spec_url)](RequestPtr, ResponsePtr res, HandlerCompletion done) {
                  res->status(status::ok).content_type("text/html; charset=utf-8");
                  res->async_send(openapi::swagger_ui_html(spec_url), finish_with(std::move(done)));
              });
        return *this;
    }

    // --- WebSocket proxy-route registration (byte-level pass-through) ---
    Router &ws_proxy(std::string path, WsProxyTarget target) {
        auto scoped_registration = scoped(std::move(path), {});
        // Byte-level ws proxying bypasses the HTTP middleware chain (the engine
        // tunnels bytes before a Response handles the upgrade), so a group's
        // middleware cannot be applied here. Registering anyway would silently
        // drop auth; refuse instead, so the misconfiguration is fail-closed.
        if (!scoped_registration.second.empty()) {
            SIMPLE_HTTP_ERROR_LOG("ws_proxy [{}] registered inside a group with middleware; refusing (unsupported)",
                                  scoped_registration.first);
            return *this;
        }
        m_ws_proxy_exact.emplace(std::move(scoped_registration.first), std::move(target));
        return *this;
    }

    Router &ws_proxy_regex(const std::string &pattern, WsProxyTarget target) {
        if (!m_scopes.empty() && !m_scopes.back().mws.empty()) {
            SIMPLE_HTTP_ERROR_LOG("ws_proxy_regex [{}] registered inside a group with middleware; refusing "
                                  "(unsupported)",
                                  pattern);
            return *this;
        }
        const std::string full_pattern = m_scopes.empty() ? pattern : scope_regex(pattern);
        try {
            m_ws_proxy_regex.emplace_back(
                RegexRoute{simple_http_regex::regex{full_pattern}, literal_prefix(full_pattern)}, std::move(target));
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("invalid ws proxy regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // --- HTTP reverse-proxy routes ---
    Router &http_proxy(std::string path, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        auto scoped_registration = scoped(std::move(path), {});
        m_http_proxy_exact.emplace(
            std::move(scoped_registration.first),
            HttpProxyRoute{std::move(target), std::move(client_cfg), std::move(scoped_registration.second)});
        return *this;
    }

    Router &http_proxy_regex(const std::string &pattern, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        auto scoped_registration = scoped(pattern, {});
        const std::string full_pattern = m_scopes.empty() ? pattern : scope_regex(pattern);
        try {
            m_http_proxy_regex.emplace_back(
                RegexRoute{simple_http_regex::regex{full_pattern}, literal_prefix(full_pattern)},
                HttpProxyRoute{std::move(target), std::move(client_cfg), std::move(scoped_registration.second)});
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("invalid http proxy regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // Registers a static file site. It becomes a dispatch stage (after routes,
    // before the fallback), so a real route always wins over a file.
    Router &static_files(std::shared_ptr<StaticFiles> site) {
        if (!site || !site->enabled()) {
            SIMPLE_HTTP_INFO_LOG("static files: ignoring a disabled site");
            return *this;
        }
        if (!m_scopes.empty()) {
            SIMPLE_HTTP_ERROR_LOG("static files: cannot register inside group('{}'); register the site outside "
                                  "the group and protect its prefix with middleware",
                                  m_scopes.back().prefix);
            return *this;
        }
        m_static.push_back(std::move(site));
        return *this;
    }

    template <typename F> Router &fallback(F &&handler) {
        m_fallback = make_handler(std::forward<F>(handler));
        return *this;
    }

    // --- application state (typed, shared across connections) ---
    template <typename T> Router &state(std::shared_ptr<T> value) {
        (*m_states)[std::type_index(typeid(T))] = std::move(value);
        return *this;
    }
    template <typename T> std::shared_ptr<T> state() const {
        const auto it = m_states->find(std::type_index(typeid(T)));
        if (it == m_states->end() || !it->second) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(it->second);
    }

    // --- host routing ---
    template <typename F> Router &host(std::string pattern, F &&register_routes) {
        return host(std::move(pattern), {}, std::forward<F>(register_routes));
    }
    template <typename F> Router &host(std::string pattern, std::vector<Middleware> middlewares, F &&register_routes) {
        auto child = std::shared_ptr<Router>(new Router(m_states));
        for (auto &middleware : middlewares) {
            child->use(std::move(middleware));
        }
        std::forward<F>(register_routes)(*child);
        register_host(std::move(pattern), std::move(child));
        return *this;
    }

    // --- middleware ---
    // Appends a middleware to the dispatch chain. Middleware run in registration
    // order, outermost first. A middleware short-circuits by never calling next.
    // The chain is frozen at the first dispatch -- register before start().
    Router &use(Middleware middleware) {
        if (m_scopes.empty()) {
            m_middleware.push_back(std::move(middleware));
            m_chain_built.store(false, std::memory_order_relaxed);
        } else {
            m_scopes.back().mws.push_back(std::move(middleware));
        }
        return *this;
    }

    // Accepts the callback or (C++20) coroutine middleware shape directly.
    // A plain Middleware still binds to the non-template overload above.
    template <typename F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, Middleware>, int> = 0>
    Router &use(F &&middleware) {
        return use(make_middleware(std::forward<F>(middleware)));
    }

    // --- CORS ---
    // A policy in; the middleware it compiles to is always the outermost
    // element of the dispatch chain (a preflight is answered here and never
    // reaches another middleware or a route).
    Router &cors(CorsConfig config) {
        m_cors = make_cors_middleware(std::move(config));
        m_chain_built.store(false, std::memory_order_relaxed);
        return *this;
    }

    // --- route groups ---
    template <typename F> Router &group(std::string prefix, F &&register_routes) {
        return group(std::move(prefix), {}, std::forward<F>(register_routes));
    }
    template <typename F>
    Router &group(std::string prefix, std::vector<Middleware> group_middlewares, F &&register_routes) {
        RegistrationScope scope = m_scopes.empty() ? RegistrationScope{} : m_scopes.back();
        scope.prefix += prefix;
        scope.mws.insert(scope.mws.end(), std::make_move_iterator(group_middlewares.begin()),
                         std::make_move_iterator(group_middlewares.end()));
        m_scopes.push_back(std::move(scope));
        std::forward<F>(register_routes)(*this);
        m_scopes.pop_back();
        return *this;
    }

    // --- WebSocket route registration ---
    Router &ws_route(std::string path, WsHandler handler) { return ws_route(std::move(path), {}, std::move(handler)); }
    Router &ws_route(std::string path, std::vector<Middleware> middlewares, WsHandler handler) {
        auto scoped_registration = scoped(std::move(path), std::move(middlewares));
        m_ws_exact.emplace(std::move(scoped_registration.first),
                           WsRouteEntry{std::move(handler), std::move(scoped_registration.second)});
        return *this;
    }

    // Same shape normalization as route(): a callback or (C++20) coroutine
    // WebSocket handler can be registered directly.
    template <typename F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, WsHandler>, int> = 0>
    Router &ws_route(std::string path, F &&handler) {
        return ws_route(std::move(path), make_ws_handler(std::forward<F>(handler)));
    }
    template <typename F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, WsHandler>, int> = 0>
    Router &ws_route(std::string path, std::vector<Middleware> middlewares, F &&handler) {
        return ws_route(std::move(path), std::move(middlewares), make_ws_handler(std::forward<F>(handler)));
    }

    Router &ws_route_regex(const std::string &pattern, WsHandler handler) {
        return ws_route_regex(pattern, {}, std::move(handler));
    }
    Router &ws_route_regex(const std::string &pattern, std::vector<Middleware> middlewares, WsHandler handler) {
        auto scoped_registration = scoped(pattern, std::move(middlewares));
        WsRouteEntry entry{std::move(handler), std::move(scoped_registration.second)};
        const std::string full_pattern = m_scopes.empty() ? pattern : scope_regex(pattern);
        try {
            m_ws_regex.emplace_back(RegexRoute{simple_http_regex::regex{full_pattern}, literal_prefix(full_pattern)},
                                    std::move(entry));
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("invalid ws route regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    template <typename F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, WsHandler>, int> = 0>
    Router &ws_route_regex(const std::string &pattern, F &&handler) {
        return ws_route_regex(pattern, make_ws_handler(std::forward<F>(handler)));
    }
    template <typename F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, WsHandler>, int> = 0>
    Router &ws_route_regex(const std::string &pattern, std::vector<Middleware> middlewares, F &&handler) {
        return ws_route_regex(pattern, std::move(middlewares), make_ws_handler(std::forward<F>(handler)));
    }

    const WsRouteEntry *find_ws_exact(std::string_view path) const {
        if (auto it = m_ws_exact.find(path); it != m_ws_exact.end()) {
            return &it->second;
        }
        return nullptr;
    }

    const WsRouteEntry *find_ws_regex(std::string_view path) const {
        if (m_ws_regex.empty()) {
            return nullptr;
        }
        for (const auto &[route, entry] : m_ws_regex) {
            if (!route.literal_prefix.empty() && !starts_with(path, route.literal_prefix))
                continue;
            const std::string p{path};
            if (simple_http_regex::regex_match(p, route.pattern)) {
                return &entry;
            }
        }
        return nullptr;
    }

    const WsRouteEntry *find_ws(std::string_view path) const {
        if (const WsRouteEntry *h = find_ws_exact(path))
            return h;
        return find_ws_regex(path);
    }

    std::optional<HttpProxyRoute> find_http_proxy(std::string_view path) const {
        if (auto it = m_http_proxy_exact.find(path); it != m_http_proxy_exact.end()) {
            return it->second;
        }
        if (m_http_proxy_regex.empty()) {
            return std::nullopt;
        }
        for (const auto &[route, proxy] : m_http_proxy_regex) {
            if (!route.literal_prefix.empty() && !starts_with(path, route.literal_prefix))
                continue;
            const std::string p{path};
            simple_http_regex::smatch match;
            if (simple_http_regex::regex_match(p, match, route.pattern)) {
                HttpProxyRoute out = proxy;
                if (!out.target.rewrite_path.empty())
                    out.target.rewrite_path = expand_rewrite(proxy.target.rewrite_path, match);
                return out;
            }
        }
        return std::nullopt;
    }
    // Looks up a WebSocket proxy backend for a path (exact then regex). Returns
    // std::nullopt if the path is not a proxy route. Consulted by the engine
    // between the local exact and the local regex ws handler.
    //
    // For a regex route whose rewrite_path is a substitution template, capture
    // groups from the match are expanded here, so the returned target's
    // rewrite_path is the final path to send to the backend.
    std::optional<WsProxyTarget> find_ws_proxy(std::string_view path) const {
        if (auto it = m_ws_proxy_exact.find(std::string{path}); it != m_ws_proxy_exact.end()) {
            return it->second; // exact route: rewrite_path used verbatim
        }
        if (m_ws_proxy_regex.empty()) {
            return std::nullopt;
        }
        for (const auto &[route, target] : m_ws_proxy_regex) {
            if (!route.literal_prefix.empty() && !starts_with(path, route.literal_prefix))
                continue;
            const std::string p{path};
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

    // Builds the WsHandlerFn the engine runs once a ws upgrade is matched: the
    // global middleware chain, then the route's group + per-route middleware,
    // ending at the engine's upgrade terminal.
    std::optional<WsHandlerFn> make_ws_handler_fn(const WsRouteEntry &route) const {
        const std::vector<Middleware> global = m_middleware;
        return [route, global](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res, SslHandle ssl,
                               WsUpgrade up, WsUpgradeCompletion done) {
            auto upgraded = std::make_shared<bool>(false);
            WsHandler handler = route.handler;
            Next terminal = [upgraded, up = std::move(up), handler = std::move(handler)](
                                RequestPtr r, ResponsePtr rw, SslHandle, HandlerCompletion next_done) mutable {
                *upgraded = true;
                up(std::move(r), std::move(rw), to_callback_ws_handler(std::move(handler)),
                   [next_done = std::move(next_done)](error_code, bool) mutable { std::move(next_done)(); });
            };
            std::vector<Middleware> middlewares = global;
            middlewares.insert(middlewares.end(), route.mws.begin(), route.mws.end());
            Next chain = compose_middleware(std::move(middlewares), std::move(terminal));
            chain(std::move(req), std::move(res), ssl,
                  [upgraded, done = std::move(done)]() mutable { std::move(done)(error_code{}, *upgraded); });
        };
    }

    // --- dispatch (satisfies the engine Dispatcher) ---
    void dispatch(RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion done) const {
        req->set_state(AppStateView{m_states.get()});
        chain()(std::move(req), std::move(res), ssl, std::move(done));
    }

  private:
    explicit Router(std::shared_ptr<StateRegistry> states) : m_states(std::move(states)) {}

    struct HostWildcard {
        std::string pattern;
        std::string suffix; // pattern without the leading '*'
        std::shared_ptr<Router> router;
    };

    static std::string normalize_host_pattern(std::string pattern) {
        pattern = ascii_lower_copy(pattern);
        while (!pattern.empty() && pattern.back() == '.') {
            pattern.pop_back();
        }
        return pattern;
    }

    static std::string normalize_host_value(std::string_view host) {
        std::string out = ascii_lower_copy(host);
        while (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
        return out;
    }

    static std::string strip_port(std::string_view host) {
        if (!host.empty() && host.front() == '[') {
            const auto close = host.find(']');
            return close == std::string_view::npos ? std::string{host} : std::string{host.substr(0, close + 1)};
        }
        const auto first = host.find(':');
        if (first != std::string_view::npos && host.find(':', first + 1) == std::string_view::npos) {
            return std::string{host.substr(0, first)};
        }
        return std::string{host};
    }

    void register_host(std::string pattern, std::shared_ptr<Router> router) {
        pattern = normalize_host_pattern(std::move(pattern));
        if (pattern.empty() || pattern == "*") {
            SIMPLE_HTTP_ERROR_LOG("host route [{}]: invalid pattern; registration skipped", pattern);
            return;
        }
        if (starts_with(pattern, "*.")) {
            for (const auto &entry : m_host_wildcards) {
                if (entry.pattern == pattern) {
                    SIMPLE_HTTP_INFO_LOG("host wildcard [{}] already registered; the first router stays", pattern);
                    return;
                }
            }
            m_host_wildcards.push_back(HostWildcard{pattern, pattern.substr(1), std::move(router)});
            std::sort(m_host_wildcards.begin(), m_host_wildcards.end(),
                      [](const HostWildcard &a, const HostWildcard &b) { return a.suffix.size() > b.suffix.size(); });
            return;
        }
        if (!m_host_exact.emplace(std::move(pattern), std::move(router)).second) {
            SIMPLE_HTTP_INFO_LOG("host [{}] already registered; the first router stays", pattern);
        }
    }

    std::shared_ptr<Router> find_host_router(const Request &req) const {
        if (m_host_exact.empty() && m_host_wildcards.empty()) {
            return nullptr;
        }
        const auto raw = req.header("host");
        if (!raw) {
            return nullptr;
        }
        const std::string host = normalize_host_value(*raw);
        const std::string host_without_port = strip_port(host);
        if (auto it = m_host_exact.find(host); it != m_host_exact.end()) {
            return it->second;
        }
        if (auto it = m_host_exact.find(host_without_port); it != m_host_exact.end()) {
            return it->second;
        }
        for (const auto &entry : m_host_wildcards) {
            const bool pattern_has_port = entry.pattern.find(':') != std::string::npos;
            const std::string &candidate = pattern_has_port ? host : host_without_port;
            // suffix includes the leading dot, so "*.example.com" cannot match
            // "example.com" or "evil-example.com".
            if (candidate.size() > entry.suffix.size() && ends_with(candidate, entry.suffix)) {
                return entry.router;
            }
        }
        return nullptr;
    }

    // The composed chain, built lazily on first dispatch and frozen thereafter.
    const Next &chain() const {
        if (m_chain_built.load(std::memory_order_acquire)) {
            return m_chain;
        }
        std::lock_guard<std::mutex> lock(m_chain_mutex);
        if (m_chain_built.load(std::memory_order_relaxed)) {
            return m_chain;
        }
        std::vector<Middleware> middlewares;
        if (m_cors) {
            middlewares.push_back(m_cors);
        }
        for (const Middleware &middleware : m_middleware) {
            middlewares.push_back(middleware);
        }
        Next terminal = [this](RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion done) {
            dispatch_impl(std::move(req), std::move(res), ssl, std::move(done));
        };
        m_chain = compose_middleware(std::move(middlewares), std::move(terminal));
        m_chain_built.store(true, std::memory_order_release);
        return m_chain;
    }

    // Runs a matched route's handler: through its per-route middleware chain
    // when the route has one, directly otherwise.
    static void run_handler(const RouteEntry &hit, RequestPtr req, ResponsePtr res, SslHandle ssl,
                            HandlerCompletion done) {
        if (hit.route_chain) {
            hit.route_chain(std::move(req), std::move(res), ssl, std::move(done));
            return;
        }
        invoke_handler(hit.handler, std::move(req), std::move(res), ssl, std::move(done));
    }

    // Route matching itself -- the terminal stage of the middleware chain.
    void dispatch_impl(RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion done) const {
        // Host-specific routes take over the request before path matching.
        if (auto host_router = find_host_router(*req)) {
            host_router->dispatch(std::move(req), std::move(res), ssl, std::move(done));
            return;
        }

        const std::string_view path = req->path();
        const Method method = req->method();

        // Local exact routes win outright.
        if (auto it = m_exact.find(path); it != m_exact.end()) {
            const MethodTable &tbl = it->second;
            if (const RouteEntry *hit = lookup(tbl, method)) {
                run_handler(*hit, std::move(req), std::move(res), ssl, std::move(done));
                return;
            }
            const std::uint16_t bits = method_allow_bits(tbl);
            if (method == Method::Options) {
                reply_auto_options(std::move(res), bits, std::move(done));
            } else {
                reply_method_not_allowed(std::move(res), bits, std::move(done));
            }
            return;
        }

        // Path-template routes (`/users/{id}`).
        const TrieTerminal *term = nullptr;
        std::vector<std::string_view> values;
        if (match_template(m_trie, path, term, values)) {
            if (const RouteEntry *hit = lookup(term->tbl, method)) {
                for (std::size_t i = 0; i < values.size(); ++i) {
                    req->set_param(term->param_names[i], values[i]);
                }
                run_handler(*hit, std::move(req), std::move(res), ssl, std::move(done));
                return;
            }
            const std::uint16_t bits = method_allow_bits(term->tbl);
            if (method == Method::Options) {
                reply_auto_options(std::move(res), bits, std::move(done));
            } else {
                reply_method_not_allowed(std::move(res), bits, std::move(done));
            }
            return;
        }

        if (!m_regex.empty()) {
            const std::string owned_path{path};
            bool method_mismatch = false;
            std::uint16_t mismatch_bits = 0;
            for (const auto &[route, entry] : m_regex) {
                if (!route.literal_prefix.empty() && !starts_with(path, route.literal_prefix))
                    continue;
                if (!simple_http_regex::regex_match(owned_path, route.pattern))
                    continue;
                if (method_allowed(entry, method)) {
                    run_handler(entry, std::move(req), std::move(res), ssl, std::move(done));
                    return;
                }
                method_mismatch = true;
                mismatch_bits |= entry.method_bits;
            }
            if (method_mismatch) {
                if (method == Method::Options) {
                    reply_auto_options(std::move(res), mismatch_bits, std::move(done));
                } else {
                    reply_method_not_allowed(std::move(res), mismatch_bits, std::move(done));
                }
                return;
            }
        }

        if (auto proxy = find_http_proxy(path)) {
            const bool client_is_tls = ssl.has_value() && *ssl != nullptr;
            auto target = std::move(proxy->target);
            auto client_cfg = std::move(proxy->client_cfg);
            Next terminal = [client_is_tls, target = std::move(target), client_cfg = std::move(client_cfg)](
                                RequestPtr r, ResponsePtr w, SslHandle, HandlerCompletion finish) mutable {
                run_http_proxy(std::move(r), std::move(w), client_is_tls, std::move(target), std::move(client_cfg),
                               std::move(finish));
            };
            if (proxy->middlewares.empty()) {
                terminal(std::move(req), std::move(res), ssl, std::move(done));
            } else {
                Next chain = compose_middleware(std::move(proxy->middlewares), std::move(terminal));
                chain(std::move(req), std::move(res), ssl, std::move(done));
            }
            return;
        }

        run_static_stage(0, std::move(req), std::move(res), ssl, std::move(done));
    }



    // $0 = whole match, $1..$9 = capture groups, $$ = a literal '$'.
    static std::string expand_rewrite(const std::string &tmpl, const simple_http_regex::smatch &match) {
        std::string out;
        out.reserve(tmpl.size());
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] != '$') {
                out.push_back(tmpl[i]);
                continue;
            }
            if (i + 1 >= tmpl.size()) {
                out.push_back('$');
                break;
            }
            const char c = tmpl[i + 1];
            if (c == '$') {
                out.push_back('$');
                ++i;
            } else if (c >= '0' && c <= '9') {
                const std::size_t idx = static_cast<std::size_t>(c - '0');
                if (idx < match.size() && match[idx].matched)
                    out.append(match[idx].str());
                ++i;
            } else {
                out.push_back('$');
            }
        }
        return out;
    }

    // The static stage: each site may answer or decline; the first one that
    // answers ends the request. Declines fall through to the fallback/404.
    void run_static_stage(std::size_t index, RequestPtr req, ResponsePtr res, SslHandle ssl,
                          HandlerCompletion done) const {
        if (index < m_static.size()) {
            auto self = this;
            m_static[index]->try_serve(
                req, res, [self, index, req, res, ssl, done = std::move(done)](bool served) mutable {
                    if (served) {
                        std::move(done)();
                        return;
                    }
                    self->run_static_stage(index + 1, std::move(req), std::move(res), ssl, std::move(done));
                });
            return;
        }
        if (m_fallback) {
            invoke_handler(*m_fallback, std::move(req), std::move(res), ssl, std::move(done));
            return;
        }
        // Built-in 404.
        res->status(status::not_found);
        res->async_send("", finish_with(std::move(done)));
    }

    // Splits a path into TemplateRoute segments; a `{name}` segment becomes a
    // capture. Returns nullopt for a malformed template.
    static std::optional<TemplateRoute> parse_template(std::string_view path) {
        TemplateRoute out;
        std::vector<std::string> seen;
        std::size_t pos = 0;
        for (;;) {
            const std::size_t end = path.find('/', pos);
            const std::string_view seg =
                path.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
            if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
                std::string name{seg.substr(1, seg.size() - 2)};
                bool wildcard = false;
                if (!name.empty() && name.front() == '*') {
                    wildcard = true;
                    name.erase(0, 1);
                    if (end != std::string_view::npos) {
                        return std::nullopt;
                    }
                }
                if (name.empty()) {
                    return std::nullopt;
                }
                for (const auto &prior : seen) {
                    if (prior == name) {
                        return std::nullopt;
                    }
                }
                seen.push_back(name);
                out.segments.push_back(std::nullopt);
                out.param_names.push_back(std::move(name));
                if (wildcard) {
                    out.wildcard = true;
                }
            } else {
                if (seg.find('{') != std::string_view::npos || seg.find('}') != std::string_view::npos) {
                    return std::nullopt;
                }
                out.segments.push_back(std::string{seg});
            }
            if (end == std::string_view::npos) {
                break;
            }
            pos = end + 1;
        }
        if (out.segments.empty()) {
            return std::nullopt;
        }
        return out;
    }

    struct MethodTable {
        static constexpr std::size_t kSlots = 10; // Method::Get .. Method::Unknown
        std::array<std::optional<RouteEntry>, kSlots> by_method;
        std::optional<RouteEntry> any;
    };

    struct TrieTerminal {
        std::vector<std::string> param_names;
        MethodTable tbl;
    };

    struct TrieNode {
        static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
        // std::less<> (transparent) lets lookup take a string_view segment
        // without building a std::string per segment per request.
        std::map<std::string, std::size_t, std::less<>> literals;
        std::size_t param_child{npos};
        std::size_t wildcard_child{npos};
        TrieTerminal terminal;
    };

    static void insert_entry(MethodTable &tbl, const RouteEntry &entry, const std::vector<Method> &methods) {
        if (methods.empty()) {
            if (tbl.any) {
                SIMPLE_HTTP_INFO_LOG("any-method route already registered; the first handler stays");
            } else {
                tbl.any = entry;
            }
            return;
        }
        for (Method m : methods) {
            if (m == Method::Unknown) {
                continue;
            }
            auto &slot = tbl.by_method[static_cast<std::size_t>(m)];
            if (slot) {
                SIMPLE_HTTP_INFO_LOG("route already registered for this method; the first handler stays");
            } else {
                slot = entry;
            }
        }
        if ((entry.method_bits & method_bit(Method::Get)) != 0) {
            auto &head = tbl.by_method[static_cast<std::size_t>(Method::Head)];
            if (!head) {
                head = entry; // GET implies HEAD, unless HEAD is registered explicitly
            }
        }
    }

    static std::uint16_t method_allow_bits(const MethodTable &tbl) {
        std::uint16_t bits = 0;
        for (const auto &slot : tbl.by_method) {
            if (slot) {
                bits |= slot->method_bits;
            }
        }
        return bits;
    }

    static const RouteEntry *lookup(const MethodTable &tbl, Method m) {
        const std::size_t idx = static_cast<std::size_t>(m);
        if (idx < MethodTable::kSlots && tbl.by_method[idx]) {
            return &*tbl.by_method[idx];
        }
        return tbl.any ? &*tbl.any : nullptr;
    }

    // Whether this router actually serves (method, path). Used by
    // verify_openapi's startup reconciliation.
    bool serves_route(Method m, std::string_view path) const {
        if (auto it = m_exact.find(std::string{path}); it != m_exact.end()) {
            return lookup(it->second, m) != nullptr;
        }
        const TrieTerminal *term = nullptr;
        std::vector<std::string_view> values;
        if (match_template(m_trie, path, term, values)) {
            return lookup(term->tbl, m) != nullptr;
        }
        return false;
    }

    Router &insert_route(std::vector<Method> methods, std::string path, std::optional<RouteEntry> entry) {
        if (!entry) {
            return *this;
        }
        if (path.find('{') != std::string::npos) {
            auto tmpl = parse_template(path);
            if (!tmpl) {
                SIMPLE_HTTP_ERROR_LOG("route [{}]: invalid path template; registration skipped", path);
                return *this;
            }
            insert_template(std::move(*tmpl), std::move(methods), std::move(*entry));
            return *this;
        }
        insert_entry(m_exact[std::move(path)], *entry, std::move(methods));
        return *this;
    }

    std::pair<std::string, std::vector<Middleware>> scoped(std::string path, std::vector<Middleware> route_mws) const {
        std::pair<std::string, std::vector<Middleware>> out;
        if (!m_scopes.empty()) {
            const RegistrationScope &scope = m_scopes.back();
            out.first = scope.prefix + std::move(path);
            out.second = scope.mws;
        } else {
            out.first = std::move(path);
        }
        out.second.insert(out.second.end(), std::make_move_iterator(route_mws.begin()),
                          std::make_move_iterator(route_mws.end()));
        return out;
    }

    std::string scope_regex(const std::string &pattern) const {
        std::string body = pattern;
        if (!body.empty() && body.front() == '^') {
            body.erase(0, 1);
        }
        return escape_literal(m_scopes.back().prefix) + body;
    }

    static std::string escape_literal(std::string_view s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (char c : s) {
            switch (c) {
            case '\\':
            case '^':
            case '$':
            case '.':
            case '[':
            case ']':
            case '(':
            case ')':
            case '{':
            case '}':
            case '*':
            case '+':
            case '?':
            case '|':
            case '-':
                out.push_back('\\');
                out.push_back(c);
                break;
            default:
                out.push_back(c);
            }
        }
        return out;
    }

    void insert_template(TemplateRoute tmpl, std::vector<Method> methods, RouteEntry entry) {
        if (m_trie.empty()) {
            m_trie.push_back(TrieNode{});
        }
        std::size_t node = 0;
        for (std::size_t i = 0; i < tmpl.segments.size(); ++i) {
            const auto &seg = tmpl.segments[i];
            const bool is_wildcard = tmpl.wildcard && i + 1 == tmpl.segments.size();
            if (is_wildcard) {
                if (m_trie[node].wildcard_child == TrieNode::npos) {
                    m_trie[node].wildcard_child = m_trie.size();
                    m_trie.push_back(TrieNode{});
                }
                node = m_trie[node].wildcard_child;
            } else if (seg.has_value()) {
                auto [it, fresh] = m_trie[node].literals.emplace(*seg, TrieNode::npos);
                if (fresh) {
                    it->second = m_trie.size();
                    m_trie.push_back(TrieNode{});
                }
                node = it->second;
            } else {
                if (m_trie[node].param_child == TrieNode::npos) {
                    m_trie[node].param_child = m_trie.size();
                    m_trie.push_back(TrieNode{});
                }
                node = m_trie[node].param_child;
            }
        }
        auto &term = m_trie[node].terminal;
        if (term.param_names.empty()) {
            term.param_names = std::move(tmpl.param_names);
        } else if (term.param_names != tmpl.param_names) {
            SIMPLE_HTTP_ERROR_LOG("route template conflicts on the {{name}} names at the same shape; "
                                  "registration skipped");
            return;
        }
        insert_entry(term.tbl, entry, methods);
    }

    bool match_template(const std::vector<TrieNode> &trie, std::string_view path, const TrieTerminal *&term,
                        std::vector<std::string_view> &values) const {
        if (trie.empty()) {
            return false;
        }
        std::vector<std::string_view> found;
        term = nullptr;
        if (!walk_template(trie, 0, path, 0, found, term))
            return false;
        values = std::move(found);
        return true;
    }

    // One DFS step: literal edge first, then the anonymous {param} edge, then
    // the catch-all. A literal subtree that cannot finish the path falls back to
    // the param sibling rather than shadowing it.
    bool walk_template(const std::vector<TrieNode> &trie, std::size_t node, std::string_view path, std::size_t pos,
                       std::vector<std::string_view> &found, const TrieTerminal *&term) const {
        if (node >= trie.size())
            return false;
        const TrieNode &n = trie[node];
        const std::size_t end = path.find('/', pos);
        const std::string_view part =
            path.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        const bool last = end == std::string_view::npos;

        if (const auto lit = n.literals.find(part); lit != n.literals.end()) {
            if (last) {
                if (terminal_ready(trie, lit->second, term))
                    return true;
            } else if (walk_template(trie, lit->second, path, end + 1, found, term)) {
                return true;
            }
        }

        if (n.param_child != TrieNode::npos && !part.empty()) {
            found.push_back(part);
            const bool hit = last ? terminal_ready(trie, n.param_child, term)
                                  : walk_template(trie, n.param_child, path, end + 1, found, term);
            if (hit)
                return true;
            found.pop_back();
        }

        if (n.wildcard_child != TrieNode::npos) {
            if (terminal_ready(trie, n.wildcard_child, term)) {
                found.push_back(path.substr(pos));
                return true;
            }
        }
        return false;
    }

    static bool terminal_ready(const std::vector<TrieNode> &trie, std::size_t node, const TrieTerminal *&term) {
        if (node >= trie.size())
            return false;
        const TrieTerminal &candidate = trie[node].terminal;
        if (!candidate.tbl.any) {
            bool has_handler = false;
            for (const auto &slot : candidate.tbl.by_method) {
                if (slot) {
                    has_handler = true;
                    break;
                }
            }
            if (!has_handler)
                return false;
        }
        term = &candidate;
        return true;
    }

    static std::optional<RouteEntry> make_route_entry(const std::vector<Method> &methods, Handler handler,
                                                      std::vector<Middleware> middlewares) {
        std::uint16_t bits = 0;
        for (Method m : methods) {
            if (m == Method::Unknown) {
                SIMPLE_HTTP_ERROR_LOG("route: Method::Unknown is not routable; registration skipped");
                return std::nullopt;
            }
            bits |= method_bit(m);
        }
        if (bits == 0) {
            bits = RouteEntry::all_bits; // any_methods -- every method, Unknown included
        } else if ((bits & method_bit(Method::Get)) != 0) {
            bits |= method_bit(Method::Head);
        }
        RouteEntry entry{bits, std::move(handler), {}};
        if (!middlewares.empty()) {
            Handler local = entry.handler; // a copy for the chain to own
            Next terminal = [local = std::move(local)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                       HandlerCompletion done) mutable {
                invoke_handler(local, std::move(req), std::move(res), ssl, std::move(done));
            };
            entry.route_chain = compose_middleware(std::move(middlewares), std::move(terminal));
        }
        return entry;
    }

    static bool method_allowed(const RouteEntry &entry, Method m) noexcept {
        return (entry.method_bits & method_bit(m)) != 0;
    }

    static std::string allow_value(std::uint16_t bits) {
        bits |= method_bit(Method::Options);
        std::string out;
        for (Method m : {Method::Get, Method::Head, Method::Post, Method::Put, Method::Delete, Method::Options,
                         Method::Patch, Method::Connect, Method::Trace}) {
            if ((bits & method_bit(m)) == 0)
                continue;
            if (!out.empty())
                out += ", ";
            out += to_string(m);
        }
        return out;
    }

    static void reply_method_not_allowed(ResponsePtr res, std::uint16_t bits, HandlerCompletion done) {
        res->status(status::method_not_allowed);
        res->header(field::allow, allow_value(bits));
        res->content_type("text/plain; charset=utf-8");
        res->async_send("405 Method Not Allowed", finish_with(std::move(done)));
    }

    static void reply_auto_options(ResponsePtr res, std::uint16_t bits, HandlerCompletion done) {
        res->status(status::no_content);
        res->header(field::allow, allow_value(bits));
        res->async_send("", finish_with(std::move(done)));
    }

    struct RegexRoute {
        simple_http_regex::regex pattern;
        std::string literal_prefix; // leading literal bytes; empty = starts with a metachar
    };

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
                return out;
            default:
                out.push_back(c);
            }
        }
        return out;
    }

    // Transparent hashing/equality would be enough on C++20; boost::unordered_map
    // provides the heterogeneous find under C++17.
    boost::unordered_map<std::string, MethodTable, StringHash, std::equal_to<>> m_exact;
    std::vector<TrieNode> m_trie;
    // The collected OpenAPI document. Always present so serve_openapi() works
    // even when no route carries a descriptor; routes record into it only when
    // glaze is enabled (openapi::doc() exists only in that build).
    std::shared_ptr<openapi::OpenApiSpec> m_openapi{std::make_shared<openapi::OpenApiSpec>()};
    std::vector<std::pair<RegexRoute, RouteEntry>> m_regex;
    std::optional<Handler> m_fallback;
    std::vector<std::shared_ptr<StaticFiles>> m_static;
    std::vector<Middleware> m_middleware;
    Middleware m_cors;
    std::vector<RegistrationScope> m_scopes;
    mutable Next m_chain;
    mutable std::atomic<bool> m_chain_built{false};
    mutable std::mutex m_chain_mutex;

    std::shared_ptr<StateRegistry> m_states;

    boost::unordered_map<std::string, std::shared_ptr<Router>, StringHash, std::equal_to<>> m_host_exact;
    std::vector<HostWildcard> m_host_wildcards;

    boost::unordered_map<std::string, WsRouteEntry, StringHash, std::equal_to<>> m_ws_exact;
    std::vector<std::pair<RegexRoute, WsRouteEntry>> m_ws_regex;
    std::unordered_map<std::string, WsProxyTarget, StringHash, std::equal_to<>> m_ws_proxy_exact;
    std::vector<std::pair<RegexRoute, WsProxyTarget>> m_ws_proxy_regex;

    boost::unordered_map<std::string, HttpProxyRoute, StringHash, std::equal_to<>> m_http_proxy_exact;
    std::vector<std::pair<RegexRoute, HttpProxyRoute>> m_http_proxy_regex;
};

} // namespace simple_http
