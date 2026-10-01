#pragma once

// Server: the public facade.
//
// Configure the listening endpoint and (optionally) TLS, register routes on the
// built-in Router, then run(). Each accepted connection is pinned to one
// single-threaded io_context from the pool and served by the protocol detected
// in the net/connection layer. The Router's dispatch is used as the engine
// Dispatcher, so one route table serves HTTP/1.x, HTTP/2 and h2c.
//
// The accept topology follows ServerConfig::reuse_port: a single acceptor that
// round-robins the pool, or one acceptor per worker context (SO_REUSEPORT).

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/ssl.hpp>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../core/io_pool.h"
#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../handler/router.h"
#include "../transport/tcp_transport.h"
#include "../transport/tls_context.h"
#include "../transport/tls_transport.h"
#include "connection.h"

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
#include "../engine/h3/h3_engine.h"
#include "../quic/endpoint.h"
#endif

namespace simple_http {

namespace asio = boost::asio;

// An IP endpoint. Leave v6 false on an IPv6 host for dual-stack: `::` then also
// accepts IPv4-mapped connections, so one listener covers both families.
struct InetAddress {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    bool v6{false}; // IPV6_V6ONLY
};

// A UNIX-domain socket at a filesystem path.
struct UnixAddress {
    std::string path;
};

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
// A UDP endpoint for QUIC. The fields mirror InetAddress, and that is the whole
// difference: QUIC is another *transport*, not another address family. It is a
// separate field rather than another alternative in `Listen` because the two
// are not exclusive — HTTP/2 over TCP and HTTP/3 over QUIC share a port number
// by convention and are served together, exactly as nginx's `listen ... ssl`
// and `listen ... quic` are.
//
// QUIC always has TLS, so ServerConfig::tls is required alongside it; without
// it start() fails, rather than running an unencrypted QUIC, which the protocol
// does not define.
struct QuicAddress {
    std::string host{"0.0.0.0"};
    std::uint16_t port{443};
    bool v6{false};
};
#endif

// A TCP endpoint: a port, or a UNIX-domain path. The variant makes "both a port
// and a path at once" unrepresentable rather than a rule to remember.
using Listen = std::variant<InetAddress, UnixAddress>;

struct ServerConfig {
    // The TCP listener, for HTTP/1.x and HTTP/2. Absent means "do not listen on
    // TCP" — a QUIC-only server, or the reverse, is a configuration rather than
    // a special case.
    std::optional<Listen> listen{InetAddress{}};
    std::optional<TlsConfig> tls;
    // Threads serving connections. Without reuse_port the pool also carries a
    // dedicated acceptor thread, so the process runs one more than this.
    unsigned worker_threads{4};
    // Fan the listener out to one acceptor per worker context, so each worker
    // accepts on its own thread: accept capacity scales with the pool and a
    // connection is never handed across threads. Off by default — a single
    // acceptor round-robining the pool spreads load more evenly than the
    // kernel's four-tuple hash, which skews badly when few clients hold
    // long-lived connections (HTTP/2, WebSocket). Falls back to the single
    // acceptor if the platform rejects SO_REUSEPORT.
    bool reuse_port{false};
    // Disable Nagle's algorithm on every accepted connection (TCP_NODELAY).
    // On by default, matching nginx's `tcp_nodelay on`: without it small
    // responses can sit in the kernel for up to ~40ms waiting to coalesce,
    // which hurts both latency and throughput on request/response workloads.
    // Set to false to keep Nagle enabled.
    bool tcp_nodelay{true};
    // Which protocols a plaintext listener serves. A TLS listener negotiates this
    // with ALPN and ignores it. The default sniffs, which serves both — but
    // sniffing cannot tell a malformed HTTP/2 opening from an HTTP/1.x request,
    // so one of the two readings is always wrong. Declare a single protocol when
    // a peer has to be answered in that protocol's terms.
    PlaintextProtocols plaintext_protocols{PlaintextProtocols::Both};
    // All protocol-engine tunables (timeouts, size caps, HTTP/2 windows/streams).
    EngineLimits limits{};
    // Optional per-accepted-socket hook (TCP_NODELAY / keepalive / buffer sizes
    // …). Invoked right after accept, before the transport or TLS handshake
    // touches the socket. Use the error_code overloads of set_option to avoid
    // throwing.
    std::function<void(asio::ip::tcp::socket &)> socket_setup;
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // The QUIC listener, for HTTP/3. Independent of `listen`: both may be set,
    // which is the usual deployment — one port number, two transports, and the
    // client picks with ALPN. Behind the macro because the type is: a build
    // without HTTP/3 has no QUIC endpoint to configure, which is also why a
    // consumer that never defines it cannot accidentally ask for one.
    std::optional<QuicAddress> quic{};
    // QUIC tunables, used only when `quic` is set.
    quic::QuicEndpointConfig quic_options{};
#endif
};

class Server {
  public:
    explicit Server(ServerConfig config)
        : m_config(std::move(config)),
          m_pool(std::make_shared<IoCtxPool>(m_config.worker_threads == 0 ? 1 : m_config.worker_threads)),
          m_router(std::make_shared<Router>()) {
        if (m_config.tls) {
            m_tls.emplace(*m_config.tls);
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        // Alt-Svc is derived, never configured: the port a client should reach
        // HTTP/3 on is the QUIC listener's own, and with no QUIC listener there
        // is nothing to advertise. Done before anything is accepted, so every
        // engine built from these limits sees the final value.
        if (m_config.quic) {
            m_config.limits.alt_svc = m_config.limits.render_alt_svc(m_config.quic->port);
        }
#endif
        if (!m_config.reuse_port) {
            // Dedicated acceptor context, created last so it is never handed
            // out as a worker. With reuse_port the workers accept for
            // themselves and this thread would sit idle.
            m_pool->add_main_context();
        }
        m_pool->start();
    }

    ~Server() { stop(); }

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // --- route registration (fluent, forwarded to the Router) ---
    // Methods come first: a route is a (method, path) pair. Braced literal,
    // `any_methods`, or the std::vector<Method> a config file parser filled —
    // see router.h. The middlewares overload registers per-route middleware
    // (gin/echo per-route, axum route_layer): they wrap this route's handler
    // only, outermost first.
    template <typename F> Server &route(std::vector<Method> methods, std::string path, F &&handler) {
        m_router->route(std::move(methods), std::move(path), std::forward<F>(handler));
        return *this;
    }
    template <typename F>
    Server &route(std::vector<Method> methods, std::string path, std::vector<Middleware> middlewares, F &&handler) {
        m_router->route(std::move(methods), std::move(path), std::move(middlewares), std::forward<F>(handler));
        return *this;
    }
    template <typename F> Server &route_regex(std::vector<Method> methods, const std::string &pattern, F &&handler) {
        m_router->route_regex(std::move(methods), pattern, std::forward<F>(handler));
        return *this;
    }
    template <typename F>
    Server &route_regex(std::vector<Method> methods, const std::string &pattern, std::vector<Middleware> middlewares,
                        F &&handler) {
        m_router->route_regex(std::move(methods), pattern, std::move(middlewares), std::forward<F>(handler));
        return *this;
    }
    template <typename F> Server &fallback(F &&handler) {
        m_router->fallback(std::forward<F>(handler));
        return *this;
    }
    // --- middleware (Go net/http / tower style) ---
    // Appends a middleware to the dispatch chain: it runs before route matching
    // (and before any earlier-registered middleware), may short-circuit by never
    // calling next(), and runs again after the handler once next() resumes. See
    // handler.h for the Middleware shape and Router::use for the ordering.
    Server &use(Middleware middleware) {
        m_router->use(std::move(middleware));
        return *this;
    }
    // CORS — see handler/cors.h for the policy and the preflight rule. The
    // policy runs as the outermost middleware, ahead of everything registered
    // with use().
    Server &cors(CorsConfig config) {
        m_router->cors(std::move(config));
        return *this;
    }

    // --- route groups (chi/gin Group, axum nest) ---
    // A group of routes sharing a path prefix and, optionally, group
    // middleware. See Router::group for the shape — routes registered inside
    // get the prefix prepended and the group middleware wrapped around them.
    template <typename F> Server &group(std::string prefix, F &&register_routes) {
        m_router->group(std::move(prefix), std::forward<F>(register_routes));
        return *this;
    }
    template <typename F>
    Server &group(std::string prefix, std::vector<Middleware> group_middlewares, F &&register_routes) {
        m_router->group(std::move(prefix), std::move(group_middlewares), std::forward<F>(register_routes));
        return *this;
    }

    // --- WebSocket route registration ---
    Server &ws_route(std::string path, WsHandler handler) {
        m_router->ws_route(std::move(path), std::move(handler));
        return *this;
    }
    Server &ws_route_regex(const std::string &pattern, WsHandler handler) {
        m_router->ws_route_regex(pattern, std::move(handler));
        return *this;
    }

    // --- WebSocket proxy-route registration (byte-level pass-through) ---
    // An Upgrade: websocket request on `path` is spliced verbatim to the backend
    // TCP endpoint host:port. Frames, fragmentation, masking and control frames
    // all pass through untouched.
    Server &ws_proxy(std::string path, std::string host, std::uint16_t port, std::string rewrite_path = {}) {
        m_router->ws_proxy(std::move(path), WsProxyTarget{std::move(host), port, std::move(rewrite_path)});
        return *this;
    }
    Server &ws_proxy_regex(const std::string &pattern, std::string host, std::uint16_t port,
                           std::string rewrite_path = {}) {
        m_router->ws_proxy_regex(pattern, WsProxyTarget{std::move(host), port, std::move(rewrite_path)});
        return *this;
    }

    // --- HTTP reverse-proxy route registration (request-level) ---
    // A matching request on `path` is forwarded to the backend host:port with the
    // standard X-Forwarded-* headers added and hop-by-hop headers stripped, and
    // the backend's response streamed back. The frontend may be HTTP/1.x, h2c or
    // HTTP/2 — the re-framing is version-agnostic. rewrite_path rewrites the
    // request target; for the regex form it is a substitution template ($1..$9
    // capture groups). The short form proxies to a plaintext HTTP/1.1 backend.
    // The optional ClientConfig is the route's own outbound policy — TLS trust
    // and client certificates for an HTTPS backend, timeouts, pool sizing,
    // defaults suit public backends — and each route builds its own client, so
    // different backends carry different policies and pools.
    Server &http_proxy(std::string path, std::string host, std::uint16_t port, std::string rewrite_path = {}) {
        return http_proxy(std::move(path), HttpProxyTarget{std::move(host), port, std::move(rewrite_path)});
    }
    Server &http_proxy_regex(const std::string &pattern, std::string host, std::uint16_t port,
                             std::string rewrite_path = {}) {
        return http_proxy_regex(pattern, HttpProxyTarget{std::move(host), port, std::move(rewrite_path)});
    }
    // --- static file serving ---
    // Registers a site built from a document root. The site becomes a dispatch
    // stage rather than a route, so it is consulted after every real route and
    // before the fallback — see Router::static_files for why that matters.
    Server &static_files(std::shared_ptr<StaticFiles> site) {
        m_router->static_files(std::move(site));
        return *this;
    }

#ifdef SIMPLE_HTTP_ENABLE_OPENAPI
    // --- OpenAPI (typed routes → OAS document + Swagger UI) ---
    // Typed routes collect request/response schemas into the document; the
    // handler signature and dispatch are unchanged. See Router::route.
    template <typename Res, typename F, typename... Extras>
    Server &route(std::vector<Method> methods, std::string path, F &&handler, openapi::OperationInfo info = {},
                  Extras &&...extras) {
        m_router->template route<Res>(std::move(methods), std::move(path), std::forward<F>(handler), std::move(info),
                                      std::forward<Extras>(extras)...);
        return *this;
    }
    template <typename Req, typename Res, typename F, typename... Extras>
    Server &route(std::vector<Method> methods, std::string path, F &&handler, openapi::OperationInfo info = {},
                  Extras &&...extras) {
        m_router->template route<Req, Res>(std::move(methods), std::move(path), std::forward<F>(handler),
                                           std::move(info), std::forward<Extras>(extras)...);
        return *this;
    }
    // Params-typed route: Params is the path-parameter struct whose field names
    // must match the template's {name}s — see Router::route.
    template <typename Params, typename Second, typename Res, typename F, typename... Extras>
    Server &route(std::vector<Method> methods, std::string path, F &&handler, openapi::OperationInfo info = {},
                  Extras &&...extras) {
        m_router->template route<Params, Second, Res>(std::move(methods), std::move(path), std::forward<F>(handler),
                                                      std::move(info), std::forward<Extras>(extras)...);
        return *this;
    }
    // The document collected by `route<Res>` / `route<Req, Res>`. Configure its
    // info fields, then serve document and UI:
    //     server.openapi().title("petshop").version("1.0.0").server("...");
    //     server.serve_openapi("/openapi.json").serve_swagger_ui("/swagger");
    openapi::OpenApiSpec &openapi() { return m_router->openapi(); }
    Server &serve_openapi(std::string path = "/openapi.json") {
        m_router->serve_openapi(std::move(path));
        return *this;
    }
    Server &serve_swagger_ui(std::string path = "/swagger", std::string spec_url = "/openapi.json") {
        m_router->serve_swagger_ui(std::move(path), std::move(spec_url));
        return *this;
    }
#endif

    Server &http_proxy(std::string path, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        m_router->http_proxy(std::move(path), std::move(target), std::move(client_cfg));
        return *this;
    }
    Server &http_proxy_regex(const std::string &pattern, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        m_router->http_proxy_regex(pattern, std::move(target), std::move(client_cfg));
        return *this;
    }

    // Start the listener. Resolves true if the socket bound successfully.
    asio::awaitable<bool> run() { co_return start_listeners(); }

    // Synchronous convenience: start the listener and block until bound.
    // Returns true if it bound successfully.
    bool start() { return start_listeners(); }

    void stop() {
        if (m_stopped.exchange(true))
            return;

        // Close each acceptor on its own context — async_accept is running there
        // and acceptor::close is not thread-safe — and wait for those closes to
        // land before stopping the pool.
        //
        // The waiting is the load-bearing part. io_context::stop() abandons
        // queued handlers, so posting a close and then stopping immediately (or
        // simply stopping first) leaves the listening sockets bound for the
        // lifetime of this object: the port keeps listening into a backlog
        // nobody drains, a restart on it fails with EADDRINUSE, and a later
        // start() stacks a second acceptor onto the first.
        std::atomic<std::size_t> remaining{m_acceptors.size()};
        std::promise<void> all_closed;
        auto closed = all_closed.get_future();
        for (auto &listener : m_acceptors) {
            auto acceptor = listener.acceptor;
            auto mark_done = [&remaining, &all_closed] {
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    all_closed.set_value();
                }
            };
            if (listener.ctx->stopped()) {
                // That loop is already gone, so no handler can be running there:
                // closing from this thread is safe, and a posted close would
                // never execute at all.
                std::visit(
                    [](const auto &socket) {
                        error_code ec;
                        socket->close(ec);
                    },
                    acceptor);
                mark_done();
                continue;
            }
            asio::post(*listener.ctx, [acceptor, mark_done] {
                std::visit(
                    [](const auto &socket) {
                        error_code ec;
                        socket->close(ec);
                    },
                    acceptor);
                mark_done();
            });
        }
        if (!m_acceptors.empty()) {
            closed.wait();
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        // Take the QUIC connections down, but do not close the sockets yet: a
        // CONNECTION_CLOSE has to leave through the socket its connection
        // arrived on, so closing the listener first would leave the peer with a
        // port that merely stopped answering — it would wait for its own
        // timeout instead of being told. The sockets go after the drain, below.
        for (auto &quic : m_quic) {
            quic->shutdown_connections();
        }
#endif
        // The same for TCP: close what is in flight so those coroutines finish
        // and the pool drains rather than abandoning their frames.
        shutdown_connections();
        // The reverse-proxy upstream client pools idle keep-alive connections
        // on the worker executors, and nothing in the connection table ever
        // touched them — close them before the pool stops, or those sessions
        // (with their TLS streams) leak past process exit. The drain in
        // stop() below is what actually unwinds the posted closes.
        m_router->close_proxy_client();
        m_pool->stop();
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        for (auto &quic : m_quic) {
            quic->close();
        }
        m_quic.clear();
#endif
        m_acceptors.clear();
    }

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // The port the QUIC listener is on; 0 when there is none. Separate from
    // port(), because a dual-transport server has two answers and one getter
    // returning either would be a silent lie whenever the other was wanted.
    std::uint16_t quic_port() const { return m_quic.empty() ? 0 : m_quic.front()->port(); }
#endif

    // The local port a TCP listener is using (useful with port 0 / ephemeral).
    // Zero for a UNIX-domain socket, which has no port to report.
    std::uint16_t port() const {
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        if (m_acceptors.empty())
            return m_quic.empty() ? 0 : m_quic.front()->port();
#endif
        if (m_acceptors.empty())
            return 0;
        return std::visit(
            [](const auto &acceptor) {
                using Acceptor = std::remove_reference_t<decltype(*acceptor)>;
                if constexpr (std::is_same_v<Acceptor, asio::ip::tcp::acceptor>) {
                    error_code ec;
                    return acceptor->local_endpoint(ec).port();
                } else {
                    return std::uint16_t{0};
                }
            },
            m_acceptors.front().acceptor);
    }

    // How many listening sockets the accept topology ended up with: 1 for the
    // single-acceptor model, one per worker once reuse_port fanned out — and
    // back to 1 where the platform rejected SO_REUSEPORT.
    std::size_t acceptor_count() const { return m_acceptors.size(); }

    std::shared_ptr<IoCtxPool> pool() { return m_pool; }

  private:
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // The QUIC types the listener and the HTTP/3 engine are built from. Declared
    // here rather than beside the members because `make_h3_serve` names them and
    // a class's member declarations are not visible to its earlier members.
    using QuicConnectionType = quic::QuicConnection<asio::io_context::executor_type>;
    using QuicEndpointType = quic::QuicEndpoint<asio::io_context::executor_type>;
#endif

    // An acceptor of either family, and the context its accept loop runs on.
    //
    // Holding both behind one type is what keeps start and stop single: the
    // socket types are unrelated, but nothing above this line has to care. The
    // variant holds one alternative on a platform without AF_UNIX, and every
    // std::visit over it still compiles.
    using AnyAcceptor =
        std::variant<std::shared_ptr<asio::ip::tcp::acceptor>, std::shared_ptr<asio::local::stream_protocol::acceptor>>;

    struct Listener {
        AnyAcceptor acceptor;
        std::shared_ptr<asio::io_context> ctx;
    };

    Dispatcher make_dispatcher() {
        auto router = m_router;
        return [router](std::shared_ptr<Request> req, std::shared_ptr<Response> res,
                        SslHandle ssl) -> asio::awaitable<void> {
            co_await router->dispatch(std::move(req), std::move(res), ssl);
        };
    }

    WsLookup make_ws_lookup() {
        auto router = m_router;
        return [router](std::string_view path) -> std::optional<WsHandlerFn> {
            // Exact local ws routes only; the h1 engine consults this before
            // the byte-level proxy lookup (nginx order).
            if (const WsHandler *h = router->find_ws_exact(path)) {
                return *h; // copy the handler for the engine to run
            }
            return std::nullopt;
        };
    }

    WsLookup make_ws_regex_lookup() {
        auto router = m_router;
        return [router](std::string_view path) -> std::optional<WsHandlerFn> {
            if (const WsHandler *h = router->find_ws_regex(path)) {
                return *h;
            }
            return std::nullopt;
        };
    }

    WsProxyLookup make_ws_proxy_lookup() {
        auto router = m_router;
        return [router](std::string_view path) -> std::optional<WsProxyTarget> {
            return router->find_ws_proxy(path); // already expands capture groups
        };
    }

    // Where the accept loop runs. Without reuse_port the pool carries a
    // dedicated acceptor context (created last, never handed out as a worker).
    // With it, worker 0 accepts for itself — and doubles as the single acceptor
    // thread should SO_REUSEPORT turn out to be unavailable.
    std::shared_ptr<asio::io_context> acceptor_context() {
        return m_config.reuse_port ? m_pool->at(0) : m_pool->main_context();
    }

    // Bind one listening socket on `ctx`. Returns nullptr and sets `ec` on
    // failure. Does not register the acceptor.
    std::shared_ptr<asio::ip::tcp::acceptor> bind_acceptor(asio::io_context &ctx, const std::string &host,
                                                           std::uint16_t port, bool v6_only, error_code &ec) {
        auto addr = asio::ip::make_address(host, ec);
        if (ec)
            return nullptr;
        asio::ip::tcp::endpoint ep{addr, port};
        auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(ctx);
        acceptor->open(ep.protocol(), ec);
        if (ec)
            return nullptr;
        if (v6_only) {
            acceptor->set_option(asio::ip::v6_only(true), ec);
            if (ec)
                return nullptr;
        }
        acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (ec)
            return nullptr;
        if (m_config.reuse_port) {
            // Must precede bind(), and every socket in the group must set it:
            // one that doesn't cannot share the port. A failure here is how a
            // platform without SO_REUSEPORT announces itself, and the caller
            // falls back to a single acceptor — silently dropping the error
            // would leave the fan-out half-bound and half-serving.
#ifdef SO_REUSEPORT
            acceptor->set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true), ec);
            if (ec)
                return nullptr;
#else
            ec = asio::error::operation_not_supported;
            return nullptr;
#endif
        }
        acceptor->bind(ep, ec);
        if (ec)
            return nullptr;
        acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec)
            return nullptr;
        return acceptor;
    }

    // The first bind is also the probe: two things only become known once a
    // socket is bound. Whether an IPv6 host is usable here at all (containers
    // with IPv6 disabled reject `::` outright), and the concrete port when the
    // config asked for 0.
    //
    // Only a dual-stack intent falls back. `v6 = false` on an IPv6 host asks
    // for both families, so serving IPv4 alone beats refusing to serve at all.
    // A `v6 = true` host asked for IPv6 only, and quietly opening IPv4 instead
    // would hand out reachability nobody asked for — so that one fails loudly.
    std::shared_ptr<asio::ip::tcp::acceptor> probe_acceptor(const InetAddress &address, error_code &ec) {
        auto ctx = acceptor_context();
        auto acceptor = bind_acceptor(*ctx, address.host, address.port, address.v6, ec);
        if (acceptor || address.v6 || !is_ipv6_host(address.host))
            return acceptor;
        SIMPLE_HTTP_ERROR_LOG("bind({}:{}): {}; retrying on 0.0.0.0", address.host, address.port, ec.message());
        error_code ipv4_ec;
        auto fallback = bind_acceptor(*ctx, "0.0.0.0", address.port, false, ipv4_ec);
        if (!fallback) {
            ec = ipv4_ec;
        }
        return fallback;
    }

    // Give every remaining worker context its own socket on the endpoint the
    // probe resolved, so the kernel sees one SO_REUSEPORT group and each worker
    // accepts on its own thread. Returns false — undoing its own binds, and
    // leaving the probe's acceptor in place as the single acceptor — when a
    // bind fails.
    bool fan_out(const std::string &host, std::uint16_t port, bool v6_only) {
        const std::size_t bound = m_acceptors.size(); // just the probe's
        for (std::size_t i = bound; i < m_pool->size(); ++i) {
            auto &ctx = m_pool->at(i);
            error_code ec;
            auto acceptor = bind_acceptor(*ctx, host, port, v6_only, ec);
            if (!acceptor) {
                SIMPLE_HTTP_ERROR_LOG("reuse_port bind({}:{}) [{}]: {}", host, port, i, ec.message());
                while (m_acceptors.size() > bound) {
                    const auto acceptor = m_acceptors.back().acceptor;
                    std::visit(
                        [](const auto &socket) {
                            error_code ec;
                            socket->close(ec);
                        },
                        acceptor);
                    m_acceptors.pop_back();
                }
                return false;
            }
            m_acceptors.push_back(Listener{std::move(acceptor), ctx});
        }
        return true;
    }

    // One entry point for every listener. The variant decides which TCP bind
    // runs; the QUIC listener is started beside it rather than instead of it,
    // because both are usually wanted at once (nginx's `listen ... ssl` plus
    // `listen ... quic`).
    bool start_listeners() {
        bool ok = true;
        if (m_config.listen) {
            ok = std::visit([this](const auto &address) { return start_listener(address); }, *m_config.listen);
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        // A failed TCP bind does not stop the QUIC listener from being tried —
        // they are independent sockets, and one being unavailable says nothing
        // about the other.
        if (m_config.quic) {
            ok = start_listener(*m_config.quic) && ok;
        }
#endif
        return ok;
    }

    bool start_listener(const InetAddress &address) {
        error_code ec;
        auto acceptor = probe_acceptor(address, ec);
        if (!acceptor) {
            SIMPLE_HTTP_ERROR_LOG("bind({}:{}): {}", address.host, address.port, ec.message());
            return false;
        }
        // The probe resolved the real endpoint, and every further socket must
        // reuse it verbatim: re-binding the workers to a configured port 0
        // would hand each of them a *different* ephemeral port and quietly
        // split the service across several.
        const auto host = local_address(*acceptor);
        const auto port = local_port(*acceptor);
        m_acceptors.push_back(Listener{acceptor, acceptor_context()});

        bool partitioned = false;
        if (m_config.reuse_port) {
            partitioned = fan_out(host, port, address.v6 && is_ipv6_host(host));
            if (!partitioned) {
                SIMPLE_HTTP_INFO_LOG("SO_REUSEPORT unavailable; serving {}:{} with a single acceptor", host, port);
            }
        }
        spawn_accept_loops(partitioned);
        SIMPLE_HTTP_INFO_LOG("listening on {}:{} (tls={}, acceptors={})", host, port, m_tls.has_value(),
                             m_acceptors.size());
        return true;
    }

    // AF_UNIX is a platform capability rather than a switch to set: asio
    // reports it, nothing extra has to be linked, and a platform without it
    // simply cannot be asked to bind a path. Hence no macro — the one thing a
    // consumer could have got wrong is now impossible to get wrong.
    bool start_listener(const UnixAddress &address) {
#ifndef BOOST_ASIO_HAS_LOCAL_SOCKETS
        SIMPLE_HTTP_ERROR_LOG("UNIX-domain sockets are unavailable on this platform: {}", address.path);
        return false;
#else
        // A socket file left behind by a previous run makes bind() fail with
        // EADDRINUSE, and there is no way to tell a stale one from a live
        // listener by looking at it — so the path is unlinked first, which is
        // what every UNIX-socket server does. The window that opens (two
        // servers starting at once can unlink each other's) is the one they all
        // accept.
        std::error_code ignored;
        std::filesystem::remove(address.path, ignored); // best-effort

        auto ctx = acceptor_context();
        asio::local::stream_protocol::endpoint endpoint{address.path};
        auto acceptor = std::make_shared<asio::local::stream_protocol::acceptor>(*ctx);

        error_code ec;
        acceptor->open(endpoint.protocol(), ec);
        if (!ec)
            acceptor->bind(endpoint, ec);
        if (!ec)
            acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("bind(unix:{}): {}", address.path, ec.message());
            return false;
        }

        // No fan-out, and therefore no pinning: neither reason the TCP side
        // fans out applies to a path — SO_REUSEPORT means nothing there, and
        // neither does an ephemeral port.
        m_acceptors.push_back(Listener{acceptor, ctx});
        spawn_accept_loops(/*partitioned=*/false);
        SIMPLE_HTTP_INFO_LOG("listening on unix:{} (tls={})", address.path, m_tls.has_value());
        return true;
#endif
    }

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // Bind a QUIC listener. QUIC has no accept: the endpoint demuxes by
    // connection ID and hands each connection to `make_h3_serve`, which is the
    // only place this file names the HTTP/3 engine.
    bool start_listener(const QuicAddress &address) {
        if (!m_config.tls) {
            // Not a fallback to something else: QUIC has no plaintext mode, and
            // serving HTTP/3 without TLS is not a thing that exists.
            SIMPLE_HTTP_ERROR_LOG("QUIC listener on {}:{} needs ServerConfig::tls", address.host, address.port);
            return false;
        }
        try {
            m_quic_tls.emplace(*m_config.tls);
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("QUIC TLS setup: {}", e.what());
            return false;
        }

        error_code ec;
        const auto addr = asio::ip::make_address(address.host, ec);
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("QUIC bind({}:{}): {}", address.host, address.port, ec.message());
            return false;
        }
        const asio::ip::udp::endpoint endpoint{addr, address.port};

        // The same pinning rule the TCP side follows, on the *same* pool: a
        // QUIC endpoint owns its socket on one context, and without reuse_port
        // that context is the acceptor's. With reuse_port each worker gets its
        // own socket, and the kernel's four-tuple hash keeps one connection's
        // datagrams together on whichever socket they arrive at — which is what
        // makes the fan-out safe. (A connection cannot migrate *between*
        // sockets; within one it can.)
        const std::size_t sockets = m_config.reuse_port ? m_pool->size() : 1;
        for (std::size_t i = 0; i < sockets; ++i) {
            const std::shared_ptr<asio::io_context> &ctx = m_config.reuse_port ? m_pool->at(i) : acceptor_context();
            auto quic = std::make_shared<QuicEndpointType>(ctx->get_executor(), m_quic_tls->native_handle(),
                                                           m_config.quic_options, make_h3_serve());
            if (!quic->open(endpoint, m_config.reuse_port, ec)) {
                SIMPLE_HTTP_ERROR_LOG("QUIC bind({}:{}) [{}]: {}", address.host, address.port, i, ec.message());
                // Undo the sockets already bound, leaving the listener down
                // rather than half-serving on a port the caller thinks failed.
                for (auto &bound : m_quic)
                    bound->close();
                m_quic.clear();
                return false;
            }
            quic->start();
            m_quic.push_back(quic);
        }
        SIMPLE_HTTP_INFO_LOG("listening on udp {}:{} (quic, alpn=h3, sockets={})", address.host, quic_port(),
                             m_quic.size());
        return true;
    }

    typename QuicEndpointType::ServeFn make_h3_serve() {
        auto router = m_router;
        auto limits = m_config.limits;
        return [router, limits](std::shared_ptr<QuicConnectionType> conn) -> asio::awaitable<void> {
            // Shared, not automatic: the engine hands a weak_ptr to every stream
            // writer and keeps itself alive from the handlers it spawns, so it has
            // to outlive run() for the same reason the HTTP/2 engine does.
            auto engine = std::make_shared<Http3Engine<QuicConnectionType>>(conn, limits);
            co_await engine->run([router](std::shared_ptr<Request> req, std::shared_ptr<Response> res,
                                          SslHandle ssl) -> asio::awaitable<void> {
                co_await router->dispatch(std::move(req), std::move(res), ssl);
            });
            co_return;
        };
    }
#endif

    // A connection the server is still serving.
    //
    // stop() needs it so it can take connections down *before* the io_context
    // pool goes away. A connection coroutine parked on a read is unwound by its
    // own completion handler — the read completes with operation_aborted and the
    // coroutine runs to its end — which is the only destruction path that knows
    // what the frame holds. Tearing the pool down first abandons that handler
    // and destroys the frame from the io_context's destructor instead; for an
    // engine that races two child coroutines with `operator||`, that walks state
    // the operator's own teardown has already released.
    //
    // `shutdown` is posted to the connection's own executor rather than called
    // from the stopping thread: closing a socket from another thread would race
    // the read parked on it, and keeping a connection's state to one thread is
    // the model the whole server is built on.
    struct LiveConnection {
        std::shared_ptr<asio::io_context> ctx;
        std::function<void()> shutdown;
    };

    // Register a connection, keeping its entry alive for as long as the returned
    // handle is. Held by the connection's completion handler, so the entry
    // expires exactly when the connection is over.
    std::shared_ptr<LiveConnection> track_connection(const std::shared_ptr<asio::io_context> &ctx,
                                                     std::function<void()> shutdown) {
        auto live = std::make_shared<LiveConnection>();
        live->ctx = ctx;
        live->shutdown = std::move(shutdown);
        std::lock_guard lock(m_live_mutex);
        // Expired entries are dropped once they outnumber a round of
        // connections, so a long-lived server does not accumulate a weak_ptr per
        // connection it has ever served.
        if (m_live.size() > 64) {
            std::erase_if(m_live, [](const std::weak_ptr<LiveConnection> &entry) { return entry.expired(); });
        }
        m_live.push_back(live);
        return live;
    }

    // Ask every live connection to close. Returns immediately: the closes land
    // on their own executors, and the drain in IoCtxPool::stop() is what waits
    // for them to be processed.
    void shutdown_connections() {
        std::vector<std::shared_ptr<LiveConnection>> live;
        {
            std::lock_guard lock(m_live_mutex);
            for (auto &entry : m_live) {
                if (auto handle = entry.lock()) {
                    live.push_back(std::move(handle));
                }
            }
            m_live.clear();
        }
        for (auto &handle : live) {
            asio::post(*handle->ctx, [handle] { handle->shutdown(); });
        }
    }

    // Starts one accept loop per bound socket. The loop is templated on the
    // acceptor/socket pair; this is the only place that names both, and the
    // protocol hands the socket type back rather than making it a second
    // parameter to thread through by hand.
    void spawn_accept_loops(bool partitioned) {
        for (auto &listener : m_acceptors) {
            std::visit(
                [&](const auto &acceptor) {
                    using Acceptor = std::remove_reference_t<decltype(*acceptor)>;
                    using Socket = typename Acceptor::protocol_type::socket;
                    // Pinned only when the fan-out really happened: re-dispatching
                    // a connection the kernel already assigned to this socket
                    // would give back the affinity the fan-out bought.
                    asio::co_spawn(*listener.ctx,
                                   accept_loop<Acceptor, Socket>(acceptor, partitioned ? listener.ctx : nullptr),
                                   asio::detached);
                },
                listener.acceptor);
        }
    }

    // `Acceptor` is asio::ip::tcp::acceptor or its UNIX-domain counterpart, and
    // `Socket` the matching socket type. Everything from accepting a connection
    // to serving it — pinning it to a context, TLS, the peer address — is the
    // same for both; only the TCP-only extras are guarded.
    template <typename Acceptor, typename Socket>
    asio::awaitable<void> accept_loop(std::shared_ptr<Acceptor> acceptor, std::shared_ptr<asio::io_context> pinned) {
        auto dispatch = make_dispatcher();
        auto ws_lookup = make_ws_lookup();
        auto ws_proxy_lookup = make_ws_proxy_lookup();
        auto ws_regex_lookup = make_ws_regex_lookup();
        for (;;) {
            // Pinned: the kernel already picked this socket, so keep the
            // connection here. Otherwise round-robin the pool, which spreads
            // long-lived connections more evenly than the kernel's hash does.
            const std::shared_ptr<asio::io_context> &ctx_ptr = pinned ? pinned : m_pool->next_ptr();
            asio::io_context &ctx = *ctx_ptr;
            Socket socket{ctx};
            auto [ec] = co_await acceptor->async_accept(socket, asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (ec == asio::error::operation_aborted)
                    break;
                // Never retry in silence: a persistent accept error — EMFILE and
                // ENOBUFS are both reachable, since this loop opens a socket per
                // connection — would otherwise spin a core with no diagnostic at
                // all, and the port would look merely "slow" from outside.
                SIMPLE_HTTP_ERROR_LOG("accept: {}", ec.message());
                continue;
            }

            // The socket hook and the TCP linger option are typed for a TCP
            // socket, and a UNIX-domain socket has neither — nor a peer address,
            // which is why the transport's stays default-constructed.
            asio::ip::tcp::endpoint peer;
            if constexpr (std::is_same_v<Socket, asio::ip::tcp::socket>) {
                if (m_config.tcp_nodelay) {
                    error_code ne;
                    socket.set_option(asio::ip::tcp::no_delay(true), ne); // best-effort
                }
                if (m_config.socket_setup) {
                    try {
                        m_config.socket_setup(socket);
                    } catch (const std::exception &e) {
                        SIMPLE_HTTP_ERROR_LOG("socket_setup threw: {}", e.what());
                    }
                }
                error_code pe;
                peer = socket.remote_endpoint(pe);
            }

            if (m_tls) {
                auto stream = std::make_shared<asio::ssl::stream<Socket>>(std::move(socket), m_tls->context());
                auto transport = std::make_shared<TlsTransport<Socket>>(std::move(stream), peer);
                auto live = track_connection(ctx_ptr, [transport] { transport->close(); });
                // The handler is not `detached`: it holds the registry handle, so
                // the entry lives exactly as long as the connection does.
                asio::co_spawn(
                    ctx, serve_tls(transport, dispatch, ws_lookup, m_config.limits, ws_proxy_lookup, ws_regex_lookup),
                    [live](std::exception_ptr) {});
            } else {
                auto sock_ptr = std::make_shared<Socket>(std::move(socket));
                auto transport = std::make_shared<TcpTransport<Socket>>(std::move(sock_ptr), peer);
                auto live = track_connection(ctx_ptr, [transport] { transport->close(); });
                asio::co_spawn(ctx,
                               serve_plaintext(transport, dispatch, ws_lookup, m_config.limits, ws_proxy_lookup,
                                               ws_regex_lookup, m_config.plaintext_protocols),
                               [live](std::exception_ptr) {});
            }
        }
        co_return;
    }

    static std::string local_address(const asio::ip::tcp::acceptor &acceptor) {
        error_code ec;
        return acceptor.local_endpoint(ec).address().to_string();
    }

    static std::uint16_t local_port(const asio::ip::tcp::acceptor &acceptor) {
        error_code ec;
        return acceptor.local_endpoint(ec).port();
    }

    static bool is_ipv6_host(const std::string &host) {
        error_code ec;
        auto addr = asio::ip::make_address(host, ec);
        return !ec && addr.is_v6();
    }

    ServerConfig m_config;
    std::shared_ptr<IoCtxPool> m_pool;
    std::shared_ptr<Router> m_router;
    std::optional<TlsContext> m_tls;
    std::vector<Listener> m_acceptors;
    // Connections still being served, so stop() can take them down. Guarded by a
    // mutex because accept loops run on several threads and each registers its
    // own connections.
    std::mutex m_live_mutex;
    std::vector<std::weak_ptr<LiveConnection>> m_live;
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // Its own SSL_CTX: the TCP one advertises h2 and http/1.1, and a QUIC
    // listener must offer h3 and nothing else.
    std::optional<quic::QuicTlsContext> m_quic_tls;
    std::vector<std::shared_ptr<QuicEndpointType>> m_quic;
#endif
    // Atomic: stop() is the one member a signal handler or a second thread has
    // any business calling, and double-stopping must not race.
    std::atomic<bool> m_stopped{false};
};

} // namespace simple_http
