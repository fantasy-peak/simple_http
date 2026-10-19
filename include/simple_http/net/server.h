#pragma once

// Server: the public facade.
//
// Configure the listening endpoint and (optionally) TLS, register routes on the
// built-in Router, then run(). Each accepted connection is pinned to one
// single-threaded io_context from the pool and served by the protocol detected
// in the net/connection layer. The Router's dispatch is used as the engine
// Dispatcher, so one route table serves HTTP/1.x, HTTP/2 and h2c.
//
// The accept topology follows ServerConfig::reuse_port (default true): one
// acceptor per worker context (SO_REUSEPORT), falling back to a single
// acceptor that round-robins the pool when the platform rejects it.

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
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

// --- per-listener protocol policy -------------------------------------------
//
// Plaintext cannot rely on ALPN, so the listener has to say what it is willing
// to serve; `Http1AndHttp2` is the historical sniffing default, the others
// remove the ambiguity a malformed opening would otherwise create.
enum class PlaintextHttp {
    Http1Only,               // h1 only: h2 preface is a protocol error
    Http1WithH2cUpgrade,     // h1 plus an Upgrade: h2c handshake
    Http2PriorKnowledgeOnly, // h2 preface only; h1 is never parsed
    Http1AndHttp2,           // sniff the preface, else h1; Upgrade allowed
};

// TLS listeners negotiate with ALPN; this is the offered protocol list.
enum class TlsHttp {
    Http1Only,     // offer http/1.1 only
    Http2Only,     // offer h2 only
    Http1AndHttp2, // offer h2, http/1.1 (default)
};

// One TCP listener. Plaintext and TLS share the type: `tls` decides which path
// the accepted socket takes, and the other fields are read accordingly.
struct TcpListenerConfig {
    std::string name; // logs/reports; default derived from address
    Listen address;
    std::optional<TlsConfig> tls; // nullopt = plaintext

    PlaintextHttp plaintext{PlaintextHttp::Http1AndHttp2}; // tls empty
    TlsHttp alpn{TlsHttp::Http1AndHttp2};                  // tls set

    // Accept topology. true = one acceptor per worker context (SO_REUSEPORT);
    // false = one acceptor that round-robins the pool. `acceptors` caps the
    // fan-out (0 = one per worker).
    bool reuse_port{true};
    std::size_t acceptors{0};

    std::size_t max_connections{0};     // 0 = ServerConfig::max_connections
    std::optional<EngineLimits> limits; // override the global limits

    std::function<void(asio::ip::tcp::socket &)> socket_setup;
    std::function<void(asio::ip::tcp::acceptor &)> listener_setup;
};

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
// One QUIC listener. TLS is not optional: QUIC has no plaintext mode.
struct QuicListenerConfig {
    std::string name;
    QuicAddress address;
    TlsConfig tls;
    std::size_t sockets{0}; // 0 = one per worker
    bool reuse_port{true};
    std::size_t max_connections{0};
    std::optional<EngineLimits> limits;
    quic::QuicEndpointConfig options;
    bool advertise_alt_svc{true};
};

using ListenerConfig = std::variant<TcpListenerConfig, QuicListenerConfig>;
#else
using ListenerConfig = TcpListenerConfig;
#endif

// --- startup results ---------------------------------------------------------
//
// Starting is fallible per listener, and a caller needs to know which one
// failed. AllOrNothing rolls the whole server back; BestEffort keeps what
// bound and reports the rest.
enum class StartPolicy { AllOrNothing, BestEffort };

enum class server_errc { bind_failed = 1, tls_setup_failed, invalid_config };

inline const boost::system::error_category &server_category() {
    static const struct : boost::system::error_category {
        const char *name() const noexcept override { return "simple_http.server"; }
        std::string message(int ev) const override {
            switch (static_cast<server_errc>(ev)) {
            case server_errc::bind_failed:
                return "listener bind failed";
            case server_errc::tls_setup_failed:
                return "listener TLS setup failed";
            case server_errc::invalid_config:
            default:
                return "invalid listener configuration";
            }
        }
    } category;
    return category;
}

inline error_code make_error_code(server_errc e) { return {static_cast<int>(e), server_category()}; }

struct BoundEndpoint {
    std::string name;
    std::string scheme; // http / https / h2c / h3 / unix
    std::string host;
    std::uint16_t port{0};
    bool tls{false};
    std::size_t acceptors{1};
    bool reuse_port{false};
};

struct ListenerFailure {
    std::string name;
    error_code error;
};

struct StartupError {
    std::vector<ListenerFailure> failures;
};

struct StartReport {
    std::vector<BoundEndpoint> bound;
    std::vector<ListenerFailure> failures; // BestEffort only
};

struct ServerConfig {
    // Explicit listener list. When non-empty, the single-listener fields below
    // are ignored: the vector is the whole configuration.
    std::vector<ListenerConfig> listeners;

    // The TCP listener, for HTTP/1.x and HTTP/2. Absent means "do not listen on
    // TCP" — a QUIC-only server, or the reverse, is a configuration rather than
    // a special case.
    std::optional<Listen> listen{InetAddress{}};
    std::optional<TlsConfig> tls;
    // Threads serving connections. Without reuse_port the pool also carries a
    // dedicated acceptor thread, so the process runs one more than this.
    unsigned worker_threads{4};
    // Upper bound on simultaneously served connections; 0 (the default) means
    // unlimited. Modelled on Go's netutil.LimitListener: when the limit is
    // reached the acceptor stops calling async_accept, so the kernel's listen
    // backlog holds new connections (bounded by the listen queue) instead of the
    // server accepting and immediately closing them. A connection is counted
    // from just before accept until its connection coroutine finishes. Reaching
    // the limit is backpressure, not rejection — a client simply waits for the
    // next free slot, exactly as it would for a busy worker.
    std::size_t max_connections{0};
    // Fan the listener out to one acceptor per worker context, so each worker
    // accepts on its own thread: accept capacity scales with the pool and a
    // connection is never handed across threads. Applies to every TCP listener
    // — plaintext and TLS share this path — and to the QUIC listener (where it
    // is effectively required: without it one io_context thread runs every QUIC
    // connection).
    //
    // On by default. With SO_REUSEPORT, a second process can bind the same port
    // and the kernel splits traffic between them: that is what enables zero-
    // downtime restarts, but it also means `EADDRINUSE` no longer signals "an
    // instance is already running". Set it to false when you want an exclusive
    // port (single-process dev, tests, or a workload where one round-robin
    // acceptor distributes long-lived connections more evenly than the kernel's
    // four-tuple hash). Falls back to the single acceptor if the platform
    // rejects SO_REUSEPORT.
    bool reuse_port{true};
    // Cap on the TCP fan-out when reuse_port is on (0 = one per worker).
    std::size_t acceptors{0};
    // Derive Alt-Svc for same-port TLS listeners from a QUIC listener.
    bool auto_alt_svc{true};
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
    // Optional per-listening-socket hook — the acceptor-side counterpart of
    // socket_setup, for the options that only exist on a listener. TCP_FASTOPEN
    // is the motivating one (set the queue length here, e.g.
    // `listener.set_option(asio::detail::socket_option::integer<IPPROTO_TCP,
    // TCP_FASTOPEN>{1024})`, and the accepted sockets' early data arrives through
    // the normal read path); SO_RCVBUF/SO_SNDBUF on the listen queue also belong
    // here. Invoked once per TCP acceptor after bind() and before listen(), which
    // is where nginx sets TCP_FASTOPEN, and before any accept loop starts — so it
    // also runs for every SO_REUSEPORT fan-out socket. Not called for a
    // UNIX-domain listener, which has no TCP options. Exceptions propagate out of
    // Server::start(); use the error_code overloads of set_option to avoid them.
    std::function<void(asio::ip::tcp::acceptor &)> listener_setup;
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
    explicit Server(ServerConfig config, std::shared_ptr<Router> router = std::make_shared<Router>())
        : m_config(std::move(config)),
          m_pool(std::make_shared<IoCtxPool>(m_config.worker_threads == 0 ? 1 : m_config.worker_threads)),
          m_router(std::move(router)) {
        normalize_configs();
        const bool need_main = std::any_of(m_tcp_configs.begin(), m_tcp_configs.end(),
                                           [](const TcpListenerConfig &cfg) {
                                               if (!cfg.reuse_port)
                                                   return true;
                                               return std::holds_alternative<UnixAddress>(cfg.address);
                                           })
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
                               || std::any_of(m_quic_configs.begin(), m_quic_configs.end(),
                                              [](const QuicListenerConfig &cfg) { return !cfg.reuse_port; })
#endif
            ;
        if (need_main) {
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
    //
    // The optional trailing argument is an OpenAPI operation descriptor
    // (openapi::doc()...); a route that passes one is documented, one that does
    // not is not. Dispatch is identical either way — the descriptor is
    // registration-time metadata (see openapi/openapi.h).
    template <typename F, typename Doc = openapi::NoDoc>
        requires(!std::is_same_v<std::remove_cvref_t<F>, std::vector<Middleware>>)
    Server &route(std::vector<Method> methods, std::string path, F &&handler, Doc doc = {}) {
        m_router->route(std::move(methods), std::move(path), std::forward<F>(handler), std::move(doc));
        return *this;
    }
    template <typename F, typename Doc = openapi::NoDoc>
    Server &route(std::vector<Method> methods, std::string path, std::vector<Middleware> middlewares, F &&handler,
                  Doc doc = {}) {
        m_router->route(std::move(methods), std::move(path), std::move(middlewares), std::forward<F>(handler),
                        std::move(doc));
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
    // The middlewares overload registers per-route middleware wrapping only this
    // ws route's upgrade and handler (auth, an Origin gate, ...) — the same
    // shape as route()'s middlewares overload.
    Server &ws_route(std::string path, std::vector<Middleware> middlewares, WsHandler handler) {
        m_router->ws_route(std::move(path), std::move(middlewares), std::move(handler));
        return *this;
    }
    Server &ws_route_regex(const std::string &pattern, WsHandler handler) {
        m_router->ws_route_regex(pattern, std::move(handler));
        return *this;
    }
    Server &ws_route_regex(const std::string &pattern, std::vector<Middleware> middlewares, WsHandler handler) {
        m_router->ws_route_regex(pattern, std::move(middlewares), std::move(handler));
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

    // --- OpenAPI (per-route descriptors → OAS document + Swagger UI) ---
    // Routes carrying an openapi::doc()... descriptor collect request/response
    // schemas into the document; the handler signature and dispatch are
    // unchanged — this is registration-time metadata, not a separate route
    // form. See Router::route.
    // Configure the document's info fields, then serve document and UI:
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

    Server &http_proxy(std::string path, HttpProxyTarget target, simple_http::ClientConfig client_cfg = {}) {
        m_router->http_proxy(std::move(path), std::move(target), std::move(client_cfg));
        return *this;
    }
    Server &http_proxy_regex(const std::string &pattern, HttpProxyTarget target,
                             simple_http::ClientConfig client_cfg = {}) {
        m_router->http_proxy_regex(pattern, std::move(target), std::move(client_cfg));
        return *this;
    }

    // Start every configured listener. AllOrNothing (default) rolls the whole
    // server back on the first failure; BestEffort keeps what bound and reports
    // the rest in StartReport::failures.
    std::expected<StartReport, StartupError> start(StartPolicy policy = StartPolicy::AllOrNothing) {
        return start_listeners(policy);
    }
    asio::awaitable<std::expected<StartReport, StartupError>> run(StartPolicy policy = StartPolicy::AllOrNothing) {
        co_return start_listeners(policy);
    }

    // The route table, exposed so several Servers can share one Router.
    std::shared_ptr<Router> router() { return m_router; }

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
            // Bound the wait the same way IoCtxPool::stop() bounds its drain: a
            // close posted onto a running context normally lands in
            // microseconds, so a timeout here only ever fires for an acceptor
            // loop that is stuck — a refusal to stop is a delayed shutdown, not
            // a hung process.
            if (closed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                SIMPLE_HTTP_ERROR_LOG("server: acceptor close did not land within 2s; stopping anyway");
            }
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        // Take the QUIC connections down, but do not close the sockets yet: a
        // CONNECTION_CLOSE has to leave through the socket its connection
        // arrived on, so closing the listener first would leave the peer with a
        // port that merely stopped answering — it would wait for its own
        // timeout instead of being told. The sockets go after the drain, below.
        for (auto &rt : m_quic_runtimes) {
            for (auto &endpoint : rt->endpoints)
                endpoint->shutdown_connections();
        }
#endif
        // The same for TCP: close what is in flight so those coroutines finish
        // and the pool drains rather than abandoning their frames.
        shutdown_connections();
        m_pool->stop();
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        for (auto &rt : m_quic_runtimes) {
            for (auto &endpoint : rt->endpoints)
                endpoint->close();
        }
        m_quic_runtimes.clear();
#endif
        m_acceptors.clear();
    }

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // The port the QUIC listener is on; 0 when there is none. Separate from
    // port(), because a dual-transport server has two answers and one getter
    // returning either would be a silent lie whenever the other was wanted.
    std::uint16_t quic_port() const {
        return m_quic_runtimes.empty() || m_quic_runtimes.front()->endpoints.empty()
                   ? 0
                   : m_quic_runtimes.front()->endpoints.front()->port();
    }
#endif

    // The local port a TCP listener is using (useful with port 0 / ephemeral).
    // Zero for a UNIX-domain socket, which has no port to report.
    std::uint16_t port() const {
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        if (m_acceptors.empty())
            return quic_port();
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

    // Connections currently being served, against ServerConfig::max_connections.
    // Readable from any thread; useful for a readiness/health endpoint or for
    // checking that the limit is in effect.
    std::size_t active_connections() const {
        std::size_t total = 0;
        for (const auto &meta : m_metas)
            total += meta->active.load(std::memory_order_relaxed);
        return total;
    }

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

    // Everything an accept loop needs that is not the socket itself: the
    // resolved TLS context (null = plaintext), the protocol policy, the limits,
    // the connection cap, and the optional hooks. One per TCP listener.
    struct ListenerMeta {
        std::string name;
        TcpListenerConfig cfg;
        EngineLimits limits;
        std::size_t max_connections{0};
        std::shared_ptr<TlsContext> tls; // null = plaintext
        std::atomic<std::size_t> active{0};
    };

    struct Listener {
        AnyAcceptor acceptor;
        std::shared_ptr<asio::io_context> ctx;
        std::shared_ptr<ListenerMeta> meta;
    };

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    struct QuicRuntime {
        std::string name;
        QuicListenerConfig cfg;
        EngineLimits limits;
        std::shared_ptr<quic::QuicTlsContext> tls;
        std::vector<std::shared_ptr<QuicEndpointType>> endpoints;
    };
#endif

    static PlaintextHttp to_plaintext_http(PlaintextProtocols protocols) {
        switch (protocols) {
        case PlaintextProtocols::Http1:
            return PlaintextHttp::Http1WithH2cUpgrade;
        case PlaintextProtocols::Http2:
            return PlaintextHttp::Http2PriorKnowledgeOnly;
        case PlaintextProtocols::Both:
        default:
            return PlaintextHttp::Http1AndHttp2;
        }
    }

    static PlaintextProtocols to_plaintext_protocols(PlaintextHttp protocols) {
        switch (protocols) {
        case PlaintextHttp::Http1Only:
        case PlaintextHttp::Http1WithH2cUpgrade:
            return PlaintextProtocols::Http1;
        case PlaintextHttp::Http2PriorKnowledgeOnly:
            return PlaintextProtocols::Http2;
        case PlaintextHttp::Http1AndHttp2:
        default:
            return PlaintextProtocols::Both;
        }
    }

    static std::string default_listener_name(const Listen &address) {
        return std::visit(
            [](const auto &a) {
                using T = std::remove_cvref_t<decltype(a)>;
                if constexpr (std::is_same_v<T, InetAddress>) {
                    return std::string{"tcp:"} + a.host + ":" + std::to_string(a.port);
                } else {
                    return std::string{"unix:"} + a.path;
                }
            },
            address);
    }

    // Turns the two public config shapes (explicit `listeners`, or the
    // single-listener sugar) into the per-listener plans the start path uses.
    void normalize_configs() {
        if (!m_config.listeners.empty()) {
            for (auto &entry : m_config.listeners) {
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
                if (auto *tcp = std::get_if<TcpListenerConfig>(&entry)) {
                    m_tcp_configs.push_back(*tcp);
                } else if (auto *quic = std::get_if<QuicListenerConfig>(&entry)) {
                    m_quic_configs.push_back(*quic);
                }
#else
                m_tcp_configs.push_back(entry);
#endif
            }
        } else if (m_config.listen) {
            TcpListenerConfig tcp;
            tcp.address = *m_config.listen;
            tcp.tls = m_config.tls;
            tcp.plaintext = to_plaintext_http(m_config.plaintext_protocols);
            tcp.reuse_port = m_config.reuse_port;
            tcp.acceptors = m_config.acceptors;
            tcp.max_connections = m_config.max_connections;
            tcp.limits = m_config.limits;
            tcp.socket_setup = m_config.socket_setup;
            tcp.listener_setup = m_config.listener_setup;
            m_tcp_configs.push_back(std::move(tcp));
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        if (m_config.listeners.empty() && m_config.quic) {
            QuicListenerConfig quic;
            quic.address = *m_config.quic;
            if (m_config.tls) {
                quic.tls = *m_config.tls;
            }
            quic.reuse_port = m_config.reuse_port;
            quic.max_connections = m_config.max_connections;
            quic.limits = m_config.limits;
            quic.options = m_config.quic_options;
            m_quic_configs.push_back(std::move(quic));
        }
#endif
        for (auto &tcp : m_tcp_configs) {
            if (tcp.name.empty())
                tcp.name = default_listener_name(tcp.address);
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        for (auto &quic : m_quic_configs) {
            if (quic.name.empty())
                quic.name = "quic:" + quic.address.host + ":" + std::to_string(quic.address.port);
        }
#endif
    }

    Dispatcher make_dispatcher() {
        auto router = m_router;
        return [router](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                        SslHandle ssl) -> asio::awaitable<void> {
            co_await router->dispatch(std::move(req), std::move(res), ssl);
        };
    }

    WsLookup make_ws_lookup() {
        auto router = m_router;
        return [router](std::string_view path) -> std::optional<WsHandlerFn> {
            // Exact local ws routes only; the h1 engine consults this before
            // the byte-level proxy lookup (nginx order).
            if (const WsRouteEntry *h = router->find_ws_exact(path)) {
                return router->make_ws_handler_fn(*h);
            }
            return std::nullopt;
        };
    }

    WsLookup make_ws_regex_lookup() {
        auto router = m_router;
        return [router](std::string_view path) -> std::optional<WsHandlerFn> {
            if (const WsRouteEntry *h = router->find_ws_regex(path)) {
                return router->make_ws_handler_fn(*h);
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

    // Resolve one TCP listener into the metadata its accept loop reads.
    std::shared_ptr<ListenerMeta> make_tcp_meta(TcpListenerConfig cfg) {
        auto meta = std::make_shared<ListenerMeta>();
        meta->name = cfg.name;
        meta->cfg = std::move(cfg);
        meta->limits = meta->cfg.limits.value_or(m_config.limits);
        meta->max_connections = meta->cfg.max_connections != 0 ? meta->cfg.max_connections : m_config.max_connections;
        if (meta->cfg.tls) {
            meta->tls = std::make_shared<TlsContext>(*meta->cfg.tls);
        }
        m_metas.push_back(meta);
        return meta;
    }

    // Bind one listening socket on `ctx`, honoring the listener's own
    // reuse_port and listener_setup. Returns nullptr and sets `ec` on failure.
    std::shared_ptr<asio::ip::tcp::acceptor> bind_acceptor(asio::io_context &ctx, const TcpListenerConfig &cfg,
                                                           const std::string &host, std::uint16_t port, bool v6_only,
                                                           error_code &ec) {
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
        if (cfg.reuse_port) {
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
        if (cfg.listener_setup)
            cfg.listener_setup(*acceptor);
        acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec)
            return nullptr;
        return acceptor;
    }

    // The first bind is also the probe: it resolves an IPv6 fallback and the
    // concrete port when the config asked for 0.
    std::shared_ptr<asio::ip::tcp::acceptor> probe_acceptor(const TcpListenerConfig &cfg, error_code &ec) {
        const auto &address = std::get<InetAddress>(cfg.address);
        auto &ctx = cfg.reuse_port ? *m_pool->at(0) : *m_pool->main_context();
        auto acceptor = bind_acceptor(ctx, cfg, address.host, address.port, address.v6, ec);
        if (acceptor || address.v6 || !is_ipv6_host(address.host))
            return acceptor;
        SIMPLE_HTTP_ERROR_LOG("bind({}:{}): {}; retrying on 0.0.0.0", address.host, address.port, ec.message());
        error_code ipv4_ec;
        auto fallback = bind_acceptor(ctx, cfg, "0.0.0.0", address.port, false, ipv4_ec);
        if (!fallback) {
            ec = ipv4_ec;
        }
        return fallback;
    }

    // Adds the remaining SO_REUSEPORT sockets for one listener, on the endpoint
    // the probe resolved. `first_new` is the index of the probe's entry.
    bool fan_out(const TcpListenerConfig &cfg, const std::shared_ptr<ListenerMeta> &meta, const std::string &host,
                 std::uint16_t port, bool v6_only, std::size_t first_new) {
        const std::size_t wanted = cfg.acceptors == 0 ? m_pool->size() : std::min(cfg.acceptors, m_pool->size());
        for (std::size_t i = 1; i < wanted; ++i) {
            auto &ctx = m_pool->at(i);
            error_code ec;
            auto acceptor = bind_acceptor(*ctx, cfg, host, port, v6_only, ec);
            if (!acceptor) {
                SIMPLE_HTTP_ERROR_LOG("reuse_port bind({}:{}) [{}]: {}", host, port, i, ec.message());
                while (m_acceptors.size() > first_new) {
                    const auto extra = m_acceptors.back().acceptor;
                    std::visit(
                        [](const auto &socket) {
                            error_code ignored;
                            socket->close(ignored);
                        },
                        extra);
                    m_acceptors.pop_back();
                }
                return false;
            }
            m_acceptors.push_back(Listener{std::move(acceptor), ctx, meta});
        }
        return true;
    }

    // Closes everything a failed start bound, without touching the pool: a
    // caller may fix the config and start() again. stop() is the full teardown.
    void close_listeners() {
        for (auto &listener : m_acceptors) {
            if (!listener.ctx->stopped()) {
                auto acceptor = listener.acceptor;
                asio::post(*listener.ctx, [acceptor] {
                    std::visit(
                        [](const auto &socket) {
                            error_code ec;
                            socket->close(ec);
                        },
                        acceptor);
                });
            } else {
                std::visit(
                    [](const auto &socket) {
                        error_code ec;
                        socket->close(ec);
                    },
                    listener.acceptor);
            }
        }
        m_acceptors.clear();
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        for (auto &rt : m_quic_runtimes) {
            for (auto &endpoint : rt->endpoints)
                endpoint->close();
        }
        m_quic_runtimes.clear();
#endif
        m_metas.clear();
    }

    BoundEndpoint make_bound(std::size_t first, std::size_t count) const {
        BoundEndpoint out;
        if (first >= m_acceptors.size())
            return out;
        const auto &listener = m_acceptors[first];
        out.name = listener.meta->name;
        out.tls = static_cast<bool>(listener.meta->tls);
        out.acceptors = count;
        out.reuse_port = listener.meta->cfg.reuse_port;
        if (const auto *inet = std::get_if<InetAddress>(&listener.meta->cfg.address)) {
            (void)inet;
            std::visit(
                [&](const auto &acceptor) {
                    using Acceptor = std::remove_reference_t<decltype(*acceptor)>;
                    if constexpr (std::is_same_v<Acceptor, asio::ip::tcp::acceptor>) {
                        error_code ec;
                        const auto ep = acceptor->local_endpoint(ec);
                        out.host = ep.address().to_string();
                        out.port = ep.port();
                        out.scheme = out.tls ? "https" : "http";
                    } else {
                        out.scheme = "unix";
                    }
                },
                listener.acceptor);
        } else {
            out.scheme = "unix";
            out.host = std::get<UnixAddress>(listener.meta->cfg.address).path;
        }
        return out;
    }

    std::expected<StartReport, StartupError> start_listeners(StartPolicy policy) {
        StartReport report;
        std::vector<ListenerFailure> failures;
        for (auto &cfg : m_tcp_configs) {
            std::string name = cfg.name;
            const std::size_t before = m_acceptors.size();
            if (!start_tcp_listener(std::move(cfg))) {
                failures.push_back(ListenerFailure{name, make_error_code(server_errc::bind_failed)});
                if (policy == StartPolicy::AllOrNothing) {
                    close_listeners();
                    return std::unexpected{StartupError{failures}};
                }
                continue;
            }
            report.bound.push_back(make_bound(before, m_acceptors.size() - before));
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        for (auto &cfg : m_quic_configs) {
            std::string name = cfg.name;
            const std::size_t before = m_quic_runtimes.size();
            if (!start_quic_listener(std::move(cfg))) {
                failures.push_back(ListenerFailure{name, make_error_code(server_errc::bind_failed)});
                if (policy == StartPolicy::AllOrNothing) {
                    close_listeners();
                    return std::unexpected{StartupError{failures}};
                }
                continue;
            }
            const auto &rt = m_quic_runtimes[before];
            BoundEndpoint bound;
            bound.name = rt->name;
            bound.scheme = "h3";
            bound.host = rt->cfg.address.host;
            bound.port = rt->endpoints.empty() ? rt->cfg.address.port : rt->endpoints.front()->port();
            bound.tls = true;
            bound.acceptors = rt->endpoints.size();
            bound.reuse_port = rt->cfg.reuse_port;
            report.bound.push_back(std::move(bound));
        }
        apply_alt_svc();
#endif
        report.failures = std::move(failures);
        return report;
    }

    bool start_tcp_listener(TcpListenerConfig cfg) {
        std::shared_ptr<ListenerMeta> meta;
        try {
            meta = make_tcp_meta(std::move(cfg));
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("TLS setup failed: {}", e.what());
            return false;
        }
        const std::size_t first_new = m_acceptors.size();

        return std::visit(
            [&](const auto &address) -> bool {
                using T = std::remove_cvref_t<decltype(address)>;
                if constexpr (std::is_same_v<T, InetAddress>) {
                    error_code ec;
                    auto acceptor = probe_acceptor(meta->cfg, ec);
                    if (!acceptor) {
                        SIMPLE_HTTP_ERROR_LOG("bind({}:{}): {}", address.host, address.port, ec.message());
                        return false;
                    }
                    const auto host = local_address(*acceptor);
                    const auto port = local_port(*acceptor);
                    auto ctx = meta->cfg.reuse_port ? m_pool->at(0) : m_pool->main_context();
                    m_acceptors.push_back(Listener{acceptor, ctx, meta});
                    bool partitioned = false;
                    if (meta->cfg.reuse_port) {
                        partitioned = fan_out(meta->cfg, meta, host, port, address.v6 && is_ipv6_host(host), first_new);
                        if (!partitioned) {
                            SIMPLE_HTTP_INFO_LOG("SO_REUSEPORT unavailable; serving {}:{} with a single acceptor", host,
                                                 port);
                        }
                    }
                    spawn_accept_loops(first_new, partitioned);
                    SIMPLE_HTTP_INFO_LOG("listening on {}:{} ({})", host, port, meta->name);
                    return true;
                } else {
#ifndef BOOST_ASIO_HAS_LOCAL_SOCKETS
                    SIMPLE_HTTP_ERROR_LOG("UNIX-domain sockets are unavailable on this platform: {}", address.path);
                    return false;
#else
                    std::error_code ignored;
                    std::filesystem::remove(address.path, ignored);
                    auto ctx = m_pool->main_context();
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
                    m_acceptors.push_back(Listener{acceptor, ctx, meta});
                    spawn_accept_loops(first_new, /*partitioned=*/false);
                    SIMPLE_HTTP_INFO_LOG("listening on unix:{} ({})", address.path, meta->name);
                    return true;
#endif
                }
            },
            meta->cfg.address);
    }

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    bool start_quic_listener(QuicListenerConfig cfg) {
        auto rt = std::make_shared<QuicRuntime>();
        rt->name = cfg.name;
        rt->cfg = std::move(cfg);
        rt->limits = rt->cfg.limits.value_or(m_config.limits);
        try {
            rt->tls = std::make_shared<quic::QuicTlsContext>(rt->cfg.tls);
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("QUIC TLS setup: {}", e.what());
            return false;
        }
        error_code ec;
        const auto addr = asio::ip::make_address(rt->cfg.address.host, ec);
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("QUIC bind({}:{}): {}", rt->cfg.address.host, rt->cfg.address.port, ec.message());
            return false;
        }
        const asio::ip::udp::endpoint endpoint{addr, rt->cfg.address.port};
        const std::size_t sockets =
            rt->cfg.reuse_port ? (rt->cfg.sockets == 0 ? m_pool->size() : std::min(rt->cfg.sockets, m_pool->size()))
                               : 1;
        for (std::size_t i = 0; i < sockets; ++i) {
            const auto &ctx = rt->cfg.reuse_port ? m_pool->at(i) : m_pool->main_context();
            auto quic = std::make_shared<QuicEndpointType>(ctx->get_executor(), rt->tls->native_handle(),
                                                           rt->cfg.options, make_h3_serve(rt->limits));
            if (!quic->open(endpoint, rt->cfg.reuse_port, ec)) {
                SIMPLE_HTTP_ERROR_LOG("QUIC bind({}:{}) [{}]: {}", rt->cfg.address.host, rt->cfg.address.port, i,
                                      ec.message());
                for (auto &bound : rt->endpoints)
                    bound->close();
                return false;
            }
            quic->start();
            rt->endpoints.push_back(std::move(quic));
        }
        SIMPLE_HTTP_INFO_LOG("listening on udp {}:{} (quic, alpn=h3, sockets={})", rt->cfg.address.host,
                             rt->cfg.address.port, rt->endpoints.size());
        m_quic_runtimes.push_back(std::move(rt));
        return true;
    }

    // A TLS TCP listener advertises a same-port QUIC listener (nginx's
    // `listen ... quic` + Alt-Svc), unless the listener or the server opted out.
    void apply_alt_svc() {
        if (!m_config.auto_alt_svc)
            return;
        for (const auto &meta : m_metas) {
            if (!meta->tls)
                continue;
            if (const auto *inet = std::get_if<InetAddress>(&meta->cfg.address); inet && inet->port != 0) {
                for (const auto &rt : m_quic_runtimes) {
                    if (rt->cfg.address.port == inet->port && !rt->endpoints.empty()) {
                        const auto quic_port = rt->endpoints.front()->port();
                        meta->limits.alt_svc = meta->limits.render_alt_svc(quic_port);
                        break;
                    }
                }
            }
        }
    }

    typename QuicEndpointType::ServeFn make_h3_serve(EngineLimits limits) {
        auto router = m_router;
        return [router, limits](std::shared_ptr<QuicConnectionType> conn) -> asio::awaitable<void> {
            auto engine = std::make_shared<Http3Engine<QuicConnectionType>>(conn, limits);
            co_await engine->run([router](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
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
    void spawn_accept_loops(std::size_t from, bool partitioned) {
        for (std::size_t i = from; i < m_acceptors.size(); ++i) {
            auto &listener = m_acceptors[i];
            std::visit(
                [&](const auto &acceptor) {
                    using Acceptor = std::remove_reference_t<decltype(*acceptor)>;
                    using Socket = typename Acceptor::protocol_type::socket;
                    // Pinned only when the fan-out really happened: re-dispatching
                    // a connection the kernel already assigned to this socket
                    // would give back the affinity the fan-out bought.
                    asio::co_spawn(
                        *listener.ctx,
                        accept_loop<Acceptor, Socket>(acceptor, partitioned ? listener.ctx : nullptr, listener.meta),
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
    asio::awaitable<void> accept_loop(std::shared_ptr<Acceptor> acceptor, std::shared_ptr<asio::io_context> pinned,
                                      std::shared_ptr<ListenerMeta> meta) {
        auto dispatch = make_dispatcher();
        auto ws_lookup = make_ws_lookup();
        auto ws_proxy_lookup = make_ws_proxy_lookup();
        auto ws_regex_lookup = make_ws_regex_lookup();
        // Exponential backoff for a retryable accept error (a resource-exhaustion
        // error like EMFILE). Reset on every successful accept; see below.
        std::chrono::milliseconds accept_backoff{0};
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
                // Never retry in silence, and never retry hot: a persistent
                // accept error — EMFILE and ENOBUFS are both reachable, since
                // this loop opens a socket per connection — would otherwise spin
                // a core with no diagnostic, and the port would look merely
                // "slow" from outside. Go's http.Server backs off exponentially
                // for the same reason.
                if (is_retryable_accept_error(ec)) {
                    accept_backoff = accept_backoff == std::chrono::milliseconds{0}
                                         ? kAcceptBackoffMin
                                         : std::min(accept_backoff * 2, kAcceptBackoffMax);
                    SIMPLE_HTTP_ERROR_LOG("accept: {}; retrying in {}ms", ec.message(), accept_backoff.count());
                    asio::steady_timer timer{ctx};
                    timer.expires_after(accept_backoff);
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                } else {
                    SIMPLE_HTTP_ERROR_LOG("accept: {}", ec.message());
                }
                continue;
            }
            accept_backoff = std::chrono::milliseconds{0};

            // A connection slot, now that the kernel has handed us a socket.
            // Acquiring before async_accept would starve connections that hash
            // to another SO_REUSEPORT listener: that socket's loop would hold
            // the permit while blocked in accept on a queue with no pending
            // connection, and the socket that does have one would never be
            // polled. Accepting first keeps every listener's accept posted;
            // each loop parks at most one accepted socket here while it waits
            // for a permit, so the over-limit backlog stays in the kernel.
            //
            // The count is global, shared by every accept loop; a freed slot
            // lands on an arbitrary thread, so a cheap re-check timer is used
            // instead of a cross-thread wake channel — it only runs while
            // saturated.
            if (meta->max_connections != 0) {
                for (;;) {
                    if (m_stopped.load(std::memory_order_acquire)) {
                        error_code ignored;
                        socket.close(ignored);
                        co_return;
                    }
                    std::size_t current = meta->active.load(std::memory_order_acquire);
                    if (current < meta->max_connections &&
                        meta->active.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel))
                        break;
                    asio::steady_timer timer{ctx};
                    timer.expires_after(std::chrono::milliseconds(25));
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                }
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
                if (meta->cfg.socket_setup) {
                    try {
                        meta->cfg.socket_setup(socket);
                    } catch (const std::exception &e) {
                        SIMPLE_HTTP_ERROR_LOG("socket_setup threw: {}", e.what());
                    }
                }
                error_code pe;
                peer = socket.remote_endpoint(pe);
            }

            // The release is the mirror of the acquire above, run once the
            // connection's coroutine has finished (not when the handler returns —
            // a streaming handler keeps the connection alive long after). It also
            // carries the registry handle (`live`), which is what keeps the
            // connection's `LiveConnection` entry alive for exactly its lifetime.
            if (meta->tls) {
                auto stream = std::make_shared<asio::ssl::stream<Socket>>(std::move(socket), meta->tls->context());
                auto transport = std::make_shared<TlsTransport<Socket>>(std::move(stream), peer);
                auto live = track_connection(ctx_ptr, [transport] { transport->close(); });
                // The handler is not `detached`: it holds the registry handle, so
                // the entry lives exactly as long as the connection does.
                asio::co_spawn(
                    ctx, serve_tls(transport, dispatch, ws_lookup, meta->limits, ws_proxy_lookup, ws_regex_lookup),
                    [this, live, meta](std::exception_ptr) {
                        if (meta->max_connections != 0)
                            meta->active.fetch_sub(1, std::memory_order_acq_rel);
                    });
            } else {
                auto sock_ptr = std::make_shared<Socket>(std::move(socket));
                auto transport = std::make_shared<TcpTransport<Socket>>(std::move(sock_ptr), peer);
                auto live = track_connection(ctx_ptr, [transport] { transport->close(); });
                asio::co_spawn(ctx,
                               serve_plaintext(transport, dispatch, ws_lookup, meta->limits, ws_proxy_lookup,
                                               ws_regex_lookup, to_plaintext_protocols(meta->cfg.plaintext)),
                               [this, live, meta](std::exception_ptr) {
                                   if (meta->max_connections != 0)
                                       meta->active.fetch_sub(1, std::memory_order_acq_rel);
                               });
            }
        }
        co_return;
    }

    // Accept errors that mean "the process is out of a resource right now", not
    // "this listener is broken": retrying immediately would spin, so the loop
    // backs off instead. EMFILE (no_descriptors) is the one that matters — this
    // loop opens a socket per connection — with ENOBUFS/ENOMEM beside it.
    static bool is_retryable_accept_error(const error_code &ec) {
        return ec == asio::error::no_descriptors || ec == asio::error::no_buffer_space || ec == asio::error::no_memory;
    }

    static constexpr auto kAcceptBackoffMin = std::chrono::milliseconds(5);
    static constexpr auto kAcceptBackoffMax = std::chrono::milliseconds(1000);

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
    std::vector<TcpListenerConfig> m_tcp_configs;
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    std::vector<QuicListenerConfig> m_quic_configs;
    std::vector<std::shared_ptr<QuicRuntime>> m_quic_runtimes;
#endif
    std::vector<std::shared_ptr<ListenerMeta>> m_metas;
    std::vector<Listener> m_acceptors;
    // Connections still being served, so stop() can take them down. Guarded by a
    // mutex because accept loops run on several threads and each registers its
    // own connections.
    std::mutex m_live_mutex;
    std::vector<std::weak_ptr<LiveConnection>> m_live;
    // Atomic: stop() is the one member a signal handler or a second thread has
    // any business calling, and double-stopping must not race.
    std::atomic<bool> m_stopped{false};
};

} // namespace simple_http
