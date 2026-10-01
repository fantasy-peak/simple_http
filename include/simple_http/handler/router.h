#pragma once

// Router: matches a request to a handler and runs it.
//
// A route is a (method, path) pair; matching order is exact path map -> regex
// list (first match) -> fallback. The methods are explicit at registration;
// registering the empty list (`any_methods`) means "any method". Two implicit
// rules follow the modern frameworks (Flask / axum / Go 1.22): a route that
// serves GET also serves HEAD (the writer strips the body), and a path whose
// method does not match is answered 405 with an Allow header — or an automatic
// 204 OPTIONS — while a path nothing services continues to the 404.
//
// Middleware (Go net/http / tower style) runs before any of that, as a chain:
// each middleware gets the request and a `next` continuation that runs the rest
// of the chain, ending at route matching itself — so a middleware can run
// before the handler, short-circuit it by never calling next(), and run again
// after it once next() resumes (handler.h, Router::use). Global middleware
// wraps the whole router; group() middlewares wrap just the group's routes;
// per-route middlewares wrap just one route's handler (Router::route's
// middlewares overload). The CORS policy (Router::cors) is the outermost
// middleware, always positioned ahead of the user chain.
//
// The Router's dispatch(shared_ptr<Request>, shared_ptr<Response>, SslHandle)
// matches the engine's Dispatcher type, so the same router serves every
// protocol version.

#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../client/http_client.h" // HttpClient (reverse-proxy upstreams)
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../engine/dispatcher.h" // WsProxyTarget, HttpProxyTarget
#include "cors.h"                 // CorsConfig (built-in CORS -> a Middleware)
#include "handler.h"
#include "http_proxy.h"
#include "static_files.h" // the static stage (m_static)

#ifdef SIMPLE_HTTP_ENABLE_OPENAPI
#include "../openapi/openapi.h" // glaze-backed JSON Schemas for route<Req, Res>
#endif

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
    // Per-route middleware (see Router::route's middlewares overload), composed
    // once at registration around the handler. Empty for routes registered
    // without any — dispatch then calls the handler directly and skips the
    // type-erased hop.
    Next route_chain;
};

// A path-template route (`/users/{id}`): segments fixed at registration,
// `std::nullopt` marking a `{name}` capture whose name sits in `param_names`.
// Dispatching matches segment-by-segment and publishes captures onto the
// Request (req->param("id")), so a template is an exact route with wildcards —
// same tier, after the literal map.
struct TemplateRoute {
    std::vector<std::optional<std::string>> segments;
    std::vector<std::string> param_names; // one per nullopt segment
};

// A registration scope: one open group() call. The prefix accumulates across
// nested groups ("/api" then "/admin" -> "/api/admin"); the middlewares are the
// group chain, outermost first, applied to every route registered while the
// scope is open (between the global chain and a route's own per-route chain).
// The root scope (empty stack) is global: prefix "" and no middleware.
struct RegistrationScope {
    std::string prefix;
    std::vector<Middleware> mws;
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
        for (const auto &[_, route] : m_http_proxy_exact) {
            route.client->close_idle();
        }
        for (const auto &[_, route] : m_http_proxy_regex) {
            route.client->close_idle();
        }
    }

    // --- registration (fluent) ---
    // Methods come first: a route is a (method, path) pair. The list may be a
    // braced literal (`{Method::Get, Method::Post}`), `any_methods`, or — the
    // config-file case — the std::vector<Method> a parser filled, passed
    // straight through: `route(parsed_methods, "/x", h)`.
    //
    // The middlewares overload registers per-route (local) middleware — Go
    // gin/echo per-route middleware, axum's route_layer. The middlewares wrap
    // this route's handler only: they run in order (first outermost) when the
    // handler is chosen, and a route whose method does not match (405 /
    // automatic OPTIONS) or whose path nothing services never runs them. Global
    // middleware (use) and CORS still run for every request, ahead of them.
    // Registered inside a group, the path and the group middleware are applied
    // automatically (see group()).
    template <typename F> Router &route(std::vector<Method> methods, std::string path, F &&handler) {
        return route(std::move(methods), std::move(path), {}, std::forward<F>(handler));
    }
    template <typename F>
    Router &route(std::vector<Method> methods, std::string path, std::vector<Middleware> middlewares, F &&handler) {
        auto scoped_registration = scoped(std::move(path), std::move(middlewares));
        return insert_route(
            std::move(methods), std::move(scoped_registration.first),
            make_route_entry(methods, make_handler(std::forward<F>(handler)), std::move(scoped_registration.second)));
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
            return *this; // Method::Unknown rejected — see make_route_entry
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

#ifdef SIMPLE_HTTP_ENABLE_OPENAPI
    // Typed route: the (method, path, handler) registration is identical to
    // route(), and <Req, Res> name the request/response body types, whose JSON
    // Schemas (glaze, see openapi.h) are recorded into the OpenAPI document.
    // The handler signature is unchanged — the types are a *declaration* for
    // the document; parsing the body and composing the response stays the
    // handler's job. <Res> alone means no request body (GET, DELETE, HEAD).
    // Optional trailing parameters: OperationInfo for the free text and success
    // status, then any number of openapi::resp<T>(status, description) for the
    // error responses the handler may answer.
    template <typename Res, typename F, typename... Extras>
    Router &route(std::vector<Method> methods, std::string path, F &&handler, openapi::OperationInfo info = {},
                  Extras &&...extras) {
        route(methods, path, std::forward<F>(handler)); // the normal registration (applies the group prefix)
        std::vector<openapi::Param> params;
        std::vector<openapi::Response> responses;
        openapi::collect_annotation(params, responses, std::forward<Extras>(extras)...);
        openapi::merge_path_params(params, path);
        m_openapi->add_operation(methods, scoped_path(path), std::move(info), {}, openapi::schema_json<Res>(),
                                 std::move(params), std::move(responses));
        return *this;
    }
    template <typename Req, typename Res, typename F, typename... Extras>
    Router &route(std::vector<Method> methods, std::string path, F &&handler, openapi::OperationInfo info = {},
                  Extras &&...extras) {
        route(methods, path, std::forward<F>(handler)); // the normal registration (applies the group prefix)
        std::vector<openapi::Param> params;
        std::vector<openapi::Response> responses;
        openapi::collect_annotation(params, responses, std::forward<Extras>(extras)...);
        openapi::merge_path_params(params, path);
        m_openapi->add_operation(methods, scoped_path(path), std::move(info), openapi::schema_json<Req>(),
                                 openapi::schema_json<Res>(), std::move(params), std::move(responses));
        return *this;
    }

    // Params-typed route: `Params` is the path-parameter struct whose field
    // names must equal the route template's `{name}`s — the axum `Path<Params>`
    // contract. Registration validates that equality (a mismatch is a loud
    // skip, not a runtime 404), the document derives its path parameters from
    // the struct's fields (name + type schema), and the handler reads the
    // parsed values with openapi::path_params<Params>(req). `Second` is the
    // request-body type or openapi::NoBody.
    template <typename Params, typename Second, typename Res, typename F, typename... Extras>
    Router &route(std::vector<Method> methods, std::string path, F &&handler, openapi::OperationInfo info = {},
                  Extras &&...extras) {
        if (!openapi::params_match_template<Params>(openapi::template_param_names(path))) {
            SIMPLE_HTTP_ERROR_LOG("route [{}]: template {{name}}s do not match the Params fields; "
                                  "registration skipped",
                                  path);
            return *this;
        }
        route(methods, path, std::forward<F>(handler)); // the normal registration (applies the group prefix)
        std::vector<openapi::Param> params;
        std::vector<openapi::Response> responses;
        openapi::collect_annotation(params, responses, std::forward<Extras>(extras)...);
        // The path parameters come from the Params struct; a user-declared
        // annotation for the same name only refines it.
        for (auto &p : openapi::path_params_schema<Params>()) {
            bool declared = false;
            for (const auto &q : params) {
                if (q.name == p.name && q.in == p.in) {
                    declared = true;
                    break;
                }
            }
            if (!declared) {
                params.push_back(std::move(p));
            }
        }
        const std::string request_schema = [] {
            if constexpr (std::is_same_v<Second, openapi::NoBody>) {
                return std::string{};
            } else {
                return openapi::schema_json<Second>();
            }
        }();
        m_openapi->add_operation(methods, scoped_path(path), std::move(info), request_schema,
                                 openapi::schema_json<Res>(), std::move(params), std::move(responses));
        return *this;
    }

    // The document being collected, for its info fields: server.openapi()
    //     .title("petshop").version("1.0.0").server("https://api.example");
    openapi::OpenApiSpec &openapi() { return *m_openapi; }

    // Cross-checks the document against the route tables and logs every
    // operation the doc claims but the router does not serve — a registration
    // that was skipped (invalid template, Params mismatch, method conflict)
    // while its doc entry went through is exactly the divergence that finds.
    void verify_openapi() const {
        for (const auto &[path, methods] : m_openapi->operations()) {
            for (Method m : methods) {
                if (!serves_route(m, path)) {
                    SIMPLE_HTTP_WARN_LOG("openapi: documented {} {} is not served by the router", to_string(m), path);
                }
            }
        }
    }

    // Serves the collected document at `path` (register under /openapi.json or
    // wherever fits). Render runs per request, so routes added after this call
    // still appear in the document. Also runs the startup reconciliation, so a
    // documented-but-unserved operation is a warning, not a silent lie.
    Router &serve_openapi(std::string path = "/openapi.json") {
        verify_openapi();
        auto spec = m_openapi;
        route(any_methods, std::move(path), [spec](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::ok).content_type(mime::app_json).send(spec->render_json());
        });
        return *this;
    }
    // Serves a CDN-backed Swagger UI page that loads the document from
    // `spec_url` (usually the path passed to serve_openapi). See
    // openapi::swagger_ui_html for the offline alternative.
    Router &serve_swagger_ui(std::string path = "/swagger", std::string spec_url = "/openapi.json") {
        route(any_methods, std::move(path),
              [spec_url = std::move(spec_url)](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                  co_await res->status(status::ok)
                      .content_type("text/html; charset=utf-8")
                      .send(openapi::swagger_ui_html(spec_url));
              });
        return *this;
    }
#endif

    // Registers a static file site. It becomes a *stage* of dispatch rather than
    // a route, which is what makes the ordering guarantee structural: a real
    // route always wins over a file of the same name, and — because the built-in
    // 404 sits behind the stage — a site that declines every path still cannot
    // leave a request unanswered.
    //
    // A disabled site (empty root) is ignored rather than registered, with the
    // reason logged: "why is my site serving 404s" should not be a silent
    // configuration mistake.
    Router &static_files(std::shared_ptr<StaticFiles> site) {
        if (!site || !site->enabled()) {
            SIMPLE_HTTP_INFO_LOG("static files: ignoring a disabled site");
            return *this;
        }
        m_static.push_back(std::move(site));
        return *this;
    }

    template <typename F> Router &fallback(F &&handler) {
        m_fallback = make_handler(std::forward<F>(handler));
        return *this;
    }

    // --- middleware (Go net/http / tower style) ---
    // Appends a middleware to the dispatch chain. Middleware run in
    // registration order, outermost first: the first `use`d sees the request
    // before any other middleware and its after-phase (code after
    // `co_await next(...)`) runs last. A middleware short-circuits the request
    // by never calling next(); CORS (see cors()) is always outermost, ahead of
    // everything registered here. The chain is frozen at the first dispatch —
    // register middleware before start().
    //
    // Inside a group (see group()), use() scopes to that group: the middleware
    // then wraps only the routes registered in the group, like chi/gin's
    // `r.Group(...)` + `r.Use(...)`.
    Router &use(Middleware middleware) {
        if (m_scopes.empty()) {
            m_middleware.push_back(std::move(middleware));
            m_chain_built.store(false, std::memory_order_relaxed);
        } else {
            // Group scope: a group middleware is applied per route at
            // registration, so the dispatch chain is untouched.
            m_scopes.back().mws.push_back(std::move(middleware));
        }
        return *this;
    }
    // CORS: a policy in, and the middleware it compiles to is what dispatch
    // runs, always at the head of the chain (a preflight is answered here and
    // never reaches any other middleware or a route). See handler/cors.h — a
    // policy the config cannot express goes in a use() middleware instead,
    // which can start from make_cors_middleware() if it only wants to narrow
    // the built-in behaviour.
    Router &cors(CorsConfig config) {
        m_cors = make_cors_middleware(std::move(config));
        m_chain_built.store(false, std::memory_order_relaxed);
        return *this;
    }

    // --- route groups (chi/gin Group, axum nest) ---
    // Registers a group of routes that share a path prefix and (optionally)
    // group middleware. The group middleware wraps every route registered
    // inside — before the route's own per-route middleware — and the prefix is
    // prepended to every path and regex registered inside, so a group reads as
    // a sub-application:
    //
    //   server.group("/api", {auth_mw}, [](simple_http::Router &api) {
    //       api.route({Method::Get}, "/users", users_handler);      // /api/users
    //       api.route({Method::Post}, "/users", {rate_limit}, create); // + per-route mw
    //       api.group("/admin", {admin_mw}, [](simple_http::Router &adm) {
    //           adm.route({any_methods}, "/kick", kick_handler);    // /api/admin/kick
    //       });
    //   });
    //
    // The lambda receives the *same* Router (registration is flat — a group is
    // a registration scope, not a separate table), so any route form works
    // inside: route, route_regex, ws_route, http_proxy, static_files, and
    // nested groups. Global middleware (use() outside a group) and CORS still
    // run for every request, ahead of the group chain.
    template <typename F> Router &group(std::string prefix, F &&register_routes) {
        return group(std::move(prefix), {}, std::forward<F>(register_routes));
    }
    template <typename F>
    Router &group(std::string prefix, std::vector<Middleware> group_middlewares, F &&register_routes) {
        // Accumulate: the group's prefix appends to any outer group's, and its
        // middlewares join the outer chain (outermost first). The scope stack is
        // captured by the lambda that closes it, so nested groups compose.
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
    Router &ws_route(std::string path, WsHandler handler) {
        m_ws_exact.emplace(std::move(path), std::move(handler));
        return *this;
    }

    Router &ws_route_regex(const std::string &pattern, WsHandler handler) {
        try {
            m_ws_regex.emplace_back(RegexRoute{simple_http_regex::regex{pattern}, literal_prefix(pattern)},
                                    std::move(handler));
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("invalid ws route regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // --- WebSocket proxy-route registration (byte-level pass-through) ---
    Router &ws_proxy(std::string path, WsProxyTarget target) {
        m_ws_proxy_exact.emplace(std::move(path), std::move(target));
        return *this;
    }

    Router &ws_proxy_regex(const std::string &pattern, WsProxyTarget target) {
        try {
            m_ws_proxy_regex.emplace_back(RegexRoute{simple_http_regex::regex{pattern}, literal_prefix(pattern)},
                                          std::move(target));
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("invalid ws proxy regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // --- HTTP reverse-proxy route registration (request-level) ---
    Router &http_proxy(std::string path, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        m_http_proxy_exact.emplace(
            std::move(path), HttpProxyRoute{std::move(target), std::make_shared<HttpClient>(std::move(client_cfg))});
        return *this;
    }

    Router &http_proxy_regex(const std::string &pattern, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        try {
            m_http_proxy_regex.emplace_back(
                RegexRoute{simple_http_regex::regex{pattern}, literal_prefix(pattern)},
                HttpProxyRoute{std::move(target), std::make_shared<HttpClient>(std::move(client_cfg))});
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("invalid http proxy regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // Local WebSocket handler for an exact path (nginx `location = /path`):
    // the h1 engine consults this before a proxy route for the same path.
    const WsHandler *find_ws_exact(std::string_view path) const {
        if (auto it = m_ws_exact.find(path); it != m_ws_exact.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Local WebSocket handler via the regex routes, consulted after the proxy
    // lookup. Same literal-prefix fast path as the other regex walks.
    const WsHandler *find_ws_regex(std::string_view path) const {
        if (m_ws_regex.empty()) {
            return nullptr; // nothing to match, and no string to build for it
        }
        for (const auto &[route, handler] : m_ws_regex) {
            if (!route.literal_prefix.empty() && !path.starts_with(route.literal_prefix))
                continue;
            const std::string p{path}; // regex_match needs std::string; built only
                                       // when a prefix may match
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
    const WsHandler *find_ws(std::string_view path) const {
        if (const WsHandler *h = find_ws_exact(path))
            return h;
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
            return it->second; // exact route: rewrite_path used verbatim
        }
        if (m_ws_proxy_regex.empty()) {
            return std::nullopt;
        }
        for (const auto &[route, target] : m_ws_proxy_regex) {
            if (!route.literal_prefix.empty() && !path.starts_with(route.literal_prefix))
                continue;
            const std::string p{path}; // regex_match needs std::string; built only
                                       // when a prefix may match
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
        for (const auto &[route, proxy] : m_http_proxy_regex) {
            if (!route.literal_prefix.empty() && !path.starts_with(route.literal_prefix))
                continue;
            const std::string p{path}; // regex_match needs std::string; built only
                                       // when a prefix may match
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
    // Runs the composed middleware chain — CORS first, then the user
    // middlewares in registration order — ending at route matching
    // (dispatch_impl). The chain is built once, on first use, and frozen
    // thereafter: registration (use/cors) must finish before start().
    asio::awaitable<void> dispatch(RequestPtr req, ResponsePtr res, SslHandle ssl) const {
        co_await chain()(std::move(req), std::move(res), ssl);
    }

  private:
    // The composed chain, built lazily on first dispatch so a Router that is
    // configured but never dispatched pays nothing. Built under a mutex because
    // dispatch may be entered concurrently (one worker thread per connection);
    // after the first build the chain is read-only for the server's lifetime.
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
        Next terminal = [this](RequestPtr req, ResponsePtr res, SslHandle ssl) -> asio::awaitable<void> {
            co_await dispatch_impl(std::move(req), std::move(res), ssl);
        };
        m_chain = compose_middleware(std::move(middlewares), std::move(terminal));
        m_chain_built.store(true, std::memory_order_release);
        return m_chain;
    }

    // Runs a matched route's handler: through its per-route middleware chain
    // (composed at registration) when the route has one, directly otherwise.
    // The chain runs before the handler, may short-circuit it, and wraps its
    // after-phase — same semantics as the global chain, scoped to one route.
    static asio::awaitable<void> run_handler(const RouteEntry &hit, RequestPtr req, ResponsePtr res, SslHandle ssl) {
        if (hit.route_chain) {
            co_await hit.route_chain(std::move(req), std::move(res), ssl);
        } else {
            co_await invoke_handler(hit.handler, std::move(req), std::move(res), ssl);
        }
    }

    // Route matching itself — the terminal stage of the middleware chain, what
    // a middleware's next() ultimately reaches: exact routes, path templates,
    // the reverse-proxy stage, regex, the static stage, fallback, built-in 404.
    asio::awaitable<void> dispatch_impl(RequestPtr req, ResponsePtr res, SslHandle ssl) const {
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
            const MethodTable &tbl = it->second;
            if (const RouteEntry *hit = lookup(tbl, method)) {
                co_await run_handler(*hit, std::move(req), std::move(res), ssl);
                co_return;
            }
            const std::uint16_t bits = method_allow_bits(tbl);
            if (method == Method::Options) {
                co_await reply_auto_options(res, bits);
            } else {
                co_await reply_method_not_allowed(res, bits);
            }
            co_return;
        }

        // Path-template routes (`/users/{id}`): a segment trie walked once per
        // request; literal edges beat a {param} edge at each level (the more
        // specific match wins, like Go's httprouter). A hit publishes its
        // captures onto the Request before the handler runs; method miss is a
        // 405, the same as an exact route.
        const TrieTerminal *term = nullptr;
        std::vector<std::string_view> values;
        if (match_template(m_trie, path, term, values)) {
            if (const RouteEntry *hit = lookup(term->tbl, method)) {
                for (std::size_t i = 0; i < values.size(); ++i) {
                    req->set_param(term->param_names[i], values[i]);
                }
                co_await run_handler(*hit, std::move(req), std::move(res), ssl);
                co_return;
            }
            const std::uint16_t bits = method_allow_bits(term->tbl);
            if (method == Method::Options) {
                co_await reply_auto_options(res, bits);
            } else {
                co_await reply_method_not_allowed(res, bits);
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
            const std::string owned_path{path}; // regex_match needs a string
            bool method_mismatch = false;
            std::uint16_t mismatch_bits = 0;
            for (const auto &[route, entry] : m_regex) {
                if (!route.literal_prefix.empty() && !path.starts_with(route.literal_prefix))
                    continue;
                if (!simple_http_regex::regex_match(owned_path, route.pattern))
                    continue;
                if (method_allowed(entry, method)) {
                    co_await run_handler(entry, std::move(req), std::move(res), ssl);
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
        for (const auto &site : m_static) {
            if (co_await site->try_serve(req, res))
                co_return;
        }

        if (m_fallback) {
            co_await invoke_handler(*m_fallback, std::move(req), std::move(res), ssl);
            co_return;
        }
        // Built-in 404.
        co_await res->status(status::not_found).send("");
        co_return;
    }

    // Splits a path into TemplateRoute segments; a `{name}` segment becomes a
    // capture. Returns nullopt for a malformed template: an unmatched brace, an
    // empty or duplicate parameter name, a brace inside a literal.
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

    // A path's handlers, keyed by HTTP method: one slot per Method (indexed by
    // the
    // enum), plus an any-method slot for a route registered with `any_methods`.
    // GET's implied HEAD is expanded into the HEAD slot at registration, so an
    // explicit HEAD registration simply overrides it. This is the "store by
    // (method, path)" shape: dispatch reads the exact slot, no scanning.
    struct MethodTable {
        static constexpr std::size_t kSlots = 10; // Method::Get .. Method::Unknown
        std::array<std::optional<RouteEntry>, kSlots> by_method;
        std::optional<RouteEntry> any;
    };

    // The handlers shared by every template that ends at a trie node, plus the
    // path-order of their {name}s. The param edges themselves are anonymous —
    // they are shared by every route that has a capture at that position, so
    // two templates like /owners/{owner}/pets/{pet_id} and
    // /owners/{owner_id}/pets coexist — and the names are applied at this
    // terminal, where every same-shape route must agree on them (regex-less
    // httprouter keeps the same discipline).
    struct TrieTerminal {
        std::vector<std::string> param_names;
        MethodTable tbl;
    };

    // A node of the path-template trie: literal child edges keyed by segment,
    // at most one (anonymous) `{name}` param edge, and the terminal routes
    // ending here.
    struct TrieNode {
        static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
        std::map<std::string, std::size_t> literals;
        std::size_t param_child{npos};
        TrieTerminal terminal;
    };

    // Fills a path's per-method table from one registration: each explicit
    // method becomes its own slot (same handler, copied), an empty list fills
    // the any-method slot, and a GET registration also fills the implied HEAD
    // slot unless HEAD was registered explicitly. A slot already holding a
    // handler keeps it — first registration wins, test_router.cpp pins that.
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
                SIMPLE_HTTP_INFO_LOG("route already registered for this method; the first handler "
                                     "stays");
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

    // The union of the methods a path actually serves, for the Allow header.
    // (An `any` slot means every method is served, so this is only consulted on
    // the 405/OPTIONS path, where `any` cannot be present.)
    static std::uint16_t method_allow_bits(const MethodTable &tbl) {
        std::uint16_t bits = 0;
        for (const auto &slot : tbl.by_method) {
            if (slot) {
                bits |= slot->method_bits;
            }
        }
        return bits;
    }

    // The handler for `method` on a path's method-keyed table: the exact slot,
    // or the any-method slot, or null. Every registration expands into slots,
    // so this is a plain indexed read — no scanning.
    static const RouteEntry *lookup(const MethodTable &tbl, Method m) {
        const std::size_t idx = static_cast<std::size_t>(m);
        if (idx < MethodTable::kSlots && tbl.by_method[idx]) {
            return &*tbl.by_method[idx];
        }
        return tbl.any ? &*tbl.any : nullptr;
    }

    // Whether the router serves `m` on `path`: an exact-route slot, a template
    // terminal slot, or neither. Used by verify_openapi.
    bool serves_route(Method m, std::string_view path) const {
        if (auto it = m_exact.find(path); it != m_exact.end()) {
            return lookup(it->second, m) != nullptr;
        }
        const TrieTerminal *term = nullptr;
        std::vector<std::string_view> values;
        if (match_template(m_trie, path, term, values)) {
            return lookup(term->tbl, m) != nullptr;
        }
        return false;
    }

    // Shared body of the route() overloads: honors `{name}` template paths
    // (segment trie, captures published onto the Request) and literal paths
    // (per-(path, method) table). Rejects Method::Unknown and bad templates
    // loudly, like the pre-refactor registration.
    Router &insert_route(std::vector<Method> methods, std::string path, std::optional<RouteEntry> entry) {
        if (!entry) {
            return *this; // Method::Unknown rejected — see make_route_entry
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
        // Per-(path, method) storage: one slot per method (see MethodTable),
        // so GET /pets and POST /pets coexist under the same path and never
        // substitute for each other.
        insert_entry(m_exact[std::move(path)], *entry, std::move(methods));
        return *this;
    }

    // --- registration scope helpers (group()) ---

    // The path as the current group scope sees it: group prefixes prepended.
    std::string scoped_path(const std::string &path) const {
        return m_scopes.empty() ? path : m_scopes.back().prefix + path;
    }

    // Applies the current scope to a route registration: the group prefix in
    // front of the path, and the group middlewares (outermost first) ahead of
    // the route's own per-route middlewares.
    std::pair<std::string, std::vector<Middleware>> scoped(std::string path, std::vector<Middleware> route_mws) const {
        std::pair<std::string, std::vector<Middleware>> out;
        if (!m_scopes.empty()) {
            const RegistrationScope &scope = m_scopes.back();
            out.first = scope.prefix + std::move(path);
            out.second = scope.mws; // copy: group middleware, shared by every route in the group
        } else {
            out.first = std::move(path);
        }
        out.second.insert(out.second.end(), std::make_move_iterator(route_mws.begin()),
                          std::make_move_iterator(route_mws.end()));
        return out;
    }

    // A regex registered inside a group is scoped to the prefix: the prefix is
    // inserted as literal text (metacharacters escaped) before the pattern, with
    // a leading '^' anchor staying at the head — `^/admin/(.*)$` under `/api`
    // matches `/api/admin/x`.
    std::string scope_regex(const std::string &pattern) const {
        std::string body = pattern;
        if (!body.empty() && body.front() == '^') {
            body.erase(0, 1); // regex_match spans the whole input anyway; the anchor is redundant
        }
        return escape_literal(m_scopes.back().prefix) + body;
    }

    // Escapes regex metacharacters so a group prefix participates literally in a
    // pattern instead of being parsed as one.
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

    // Inserts a template route into the trie. A literal segment becomes a
    // named edge; a `{name}` segment follows (or creates) the single, anonymous
    // param edge. At the terminal the route's param names are recorded (first
    // one wins; a later same-shape route must agree) and its per-method table
    // is filled.
    void insert_template(TemplateRoute tmpl, std::vector<Method> methods, RouteEntry entry) {
        if (m_trie.empty()) {
            m_trie.push_back(TrieNode{});
        }
        std::size_t node = 0;
        for (const auto &seg : tmpl.segments) {
            if (seg.has_value()) {
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

    // Walks the trie against the request path, segment by segment. A literal
    // edge beats a param edge at each level (the more specific route wins).
    // `term` points at the terminal and `values` holds the captures in path
    // order (views into the request path, valid for the request's lifetime);
    // the terminal's param_names pair them up. Returns false when no template
    // matches or the shape differs.
    bool match_template(const std::vector<TrieNode> &trie, std::string_view path, const TrieTerminal *&term,
                        std::vector<std::string_view> &values) const {
        if (trie.empty()) {
            return false;
        }
        std::vector<std::string_view> found;
        std::size_t node = 0;
        std::size_t pos = 0;
        for (;;) {
            const TrieNode &n = trie[node];
            const std::size_t end = path.find('/', pos);
            const std::string_view part =
                path.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
            const auto lit = n.literals.find(std::string{part});
            if (lit != n.literals.end()) {
                node = lit->second;
            } else if (n.param_child != TrieNode::npos && !part.empty()) {
                found.push_back(part); // anonymous edge: the value only; the
                node = n.param_child;  // terminal remaps it to this route's names
            } else {
                return false;
            }
            if (end == std::string_view::npos) {
                break;
            }
            pos = end + 1;
        }
        // Only a *terminal* node (the walk's last segment lands where a route
        // actually ends) is a hit; an empty table means the path is a strict
        // prefix of a longer template and must fall through to 404. The table
        // of a true terminal is never empty (insert_template fills it).
        term = &trie[node].terminal;
        if (!term->tbl.any) {
            bool has_handler = false;
            for (const auto &slot : term->tbl.by_method) {
                if (slot) {
                    has_handler = true;
                    break;
                }
            }
            if (!has_handler) {
                term = nullptr;
                return false;
            }
        }
        values = std::move(found);
        return true;
    }

    // The empty method set (see `any_methods`) means "any method", and GET
    // implies HEAD — the two implicit rules shared with Flask / axum / Go 1.22.
    // `middlewares` are the route's local middleware: composed once, here, into
    // route_chain (which captures a *copy* of the handler — the entry is copied
    // into per-method slots afterwards, so the chain must not reference the
    // entry itself).
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
            bits = RouteEntry::all_bits; // any_methods — every method, Unknown included
        } else if ((bits & method_bit(Method::Get)) != 0) {
            // A GET route also serves HEAD; the response writer strips the body
            // (regression-tested: "HEAD and 204 carry no body").
            bits |= method_bit(Method::Head);
        }
        RouteEntry entry{bits, std::move(handler)};
        if (!middlewares.empty()) {
            Handler local = entry.handler; // a copy for the chain to own
            Next terminal = [local = std::move(local)](RequestPtr req, ResponsePtr res,
                                                       SslHandle ssl) -> asio::awaitable<void> {
                co_await invoke_handler(local, std::move(req), std::move(res), ssl);
            };
            entry.route_chain = compose_middleware(std::move(middlewares), std::move(terminal));
        }
        return entry;
    }

    static bool method_allowed(const RouteEntry &entry, Method m) noexcept {
        return (entry.method_bits & method_bit(m)) != 0;
    }

    // The Allow header (RFC 9110 §10.2.1) for a method bitmask, in canonical
    // method order. HEAD appears with GET (folded in at registration) and
    // OPTIONS always appears: the router answers OPTIONS automatically for a
    // path it resolved, so it is supported even when never registered.
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

    // Resolved first, rejected second: called only once a route's path matched
    // but its method did not. The reply is a normal response — the engine
    // drains any unread request body and keeps the connection alive, exactly
    // like any handler that did not read the body. This matches the ecosystem
    // (Go's ServeMux, axum, Spring: 405 is a keep-alive response, not a
    // terminal close).
    asio::awaitable<void> reply_method_not_allowed(ResponsePtr res, std::uint16_t bits) const {
        res->status(status::method_not_allowed);
        res->header(field::allow, allow_value(bits));
        res->content_type("text/plain; charset=utf-8");
        (void)co_await res->send("405 Method Not Allowed");
    }

    asio::awaitable<void> reply_auto_options(ResponsePtr res, std::uint16_t bits) const {
        res->status(status::no_content);
        res->header(field::allow, allow_value(bits));
        (void)co_await res->send("");
    }

    // Expands a rewrite template against a regex match: $0 = whole match,
    // $1..$9 = capture groups (empty if the group did not participate), $$ = a
    // literal '$'. A lone '$' or '$' before a non-digit/non-'$' is kept verbatim.
    static std::string expand_rewrite(const std::string &tmpl, const simple_http_regex::smatch &m) {
        std::string out;
        out.reserve(tmpl.size());
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] != '$') {
                out.push_back(tmpl[i]);
                continue;
            }
            if (i + 1 >= tmpl.size()) {
                out.push_back('$'); // trailing '$'
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
                out.push_back('$'); // not a placeholder; keep the '$'
            }
        }
        return out;
    }

    struct RegexRoute {
        simple_http_regex::regex pattern;
        std::string literal_prefix; // leading literal bytes; empty = pattern
                                    // starts with a metachar
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
                return out; // metacharacter ends the literal run
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

    // A path's handlers, keyed by HTTP method: one slot per Method (indexed by
    // the enum value), plus an any-method slot for `any_methods`. GET's implied
    // HEAD is expanded into the HEAD slot here, so an explicit HEAD registration
    // simply overrides it. This is the "store by (method, path)" shape.
    std::unordered_map<std::string, MethodTable, string_hash, std::equal_to<>> m_exact;
    // Path-template routes, indexed in a segment trie (see TrieNode); each
    // terminal holds the same per-method table. Literal templates stay in
    // `m_exact`; only paths containing `{name}` land here.
    std::vector<TrieNode> m_trie;
    std::vector<std::pair<RegexRoute, RouteEntry>> m_regex;
#ifdef SIMPLE_HTTP_ENABLE_OPENAPI
    // The OpenAPI document collected by the typed route overloads. Created
    // eagerly; a build that never calls openapi() / serve_openapi just carries
    // an empty spec.
    std::shared_ptr<openapi::OpenApiSpec> m_openapi{std::make_shared<openapi::OpenApiSpec>()};
#endif
    // Static file sites, consulted in registration order after the regex routes
    // and before the fallback. A vector rather than one slot so mounting several
    // roots is a later addition rather than a change of shape.
    std::vector<std::shared_ptr<StaticFiles>> m_static;
    std::optional<Handler> m_fallback;
    // The global middleware chain, in registration order (Router::use outside a
    // group). Read by chain() when the dispatch chain is composed.
    std::vector<Middleware> m_middleware;
    // The CORS policy as a middleware, always the outermost element of the
    // chain (Router::cors). Empty until a policy is installed.
    Middleware m_cors;
    // The open group() registration scopes. Empty at the root: routes register
    // globally and use() lands in m_middleware. Non-empty inside group(): the
    // back() scope's prefix and middlewares apply to every route registered
    // then, and use() appends to its middlewares.
    std::vector<RegistrationScope> m_scopes;
    // The composed dispatch chain, built once on first use (chain()). After
    // that it is read-only for the server's lifetime; registration clears
    // m_chain_built on the configuring thread, before start().
    mutable Next m_chain;
    mutable std::atomic<bool> m_chain_built{false};
    mutable std::mutex m_chain_mutex;

    std::unordered_map<std::string, WsHandler, string_hash, std::equal_to<>> m_ws_exact;
    std::vector<std::pair<RegexRoute, WsHandler>> m_ws_regex;

    std::unordered_map<std::string, WsProxyTarget, string_hash, std::equal_to<>> m_ws_proxy_exact;
    std::vector<std::pair<RegexRoute, WsProxyTarget>> m_ws_proxy_regex;

    std::unordered_map<std::string, HttpProxyRoute, string_hash, std::equal_to<>> m_http_proxy_exact;
    std::vector<std::pair<RegexRoute, HttpProxyRoute>> m_http_proxy_regex;
};

} // namespace simple_http
