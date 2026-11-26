#pragma once

// Server: listeners, connection lifecycle and the router facade.
//
// Threading follows model A: the IoCtxPool has one single-threaded io_context
// per worker, and every worker owns one acceptor (the group shares the port
// through SO_REUSEPORT). A socket accepted by a worker's acceptor is served on
// that same worker, so all of its engine, writer and handler work happens
// there for the connection's whole life.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <sys/socket.h>
#endif
#include <variant>
#include <vector>

#include "../core/asio_ssl.h"
#include "../core/io_pool.h"
#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/result.h"
#include "../core/span.h"
#include "../core/types.h"
#include "../engine/h1/h1_engine.h"
#include "../engine/h2/h2_engine.h"
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
#include "../engine/h3/h3_engine.h"
#include "../quic/endpoint.h"
#include "../quic/tls.h"
#endif
#include "../handler/router.h"
#include "../transport/tcp_transport.h"
#include "../transport/tls_context.h"
#include "../transport/tls_transport.h"
#include "connection.h"

namespace simple_http {

// An IP endpoint. Leave v6 false on an IPv6 host for dual-stack.
struct InetAddress {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    bool v6{false};
};

// A UNIX-domain socket at a filesystem path (reserved; not served yet).
struct UnixAddress {
    std::string path;
};

using Listen = std::variant<InetAddress, UnixAddress>;

enum class PlaintextHttp {
    Http1Only,
    Http1WithH2cUpgrade,
    Http2PriorKnowledgeOnly,
    Http1AndHttp2,
};

enum class TlsHttp {
    Http1Only,
    Http2Only,
    Http1AndHttp2,
};

struct TcpListenerConfig {
    std::string name;
    Listen address;
    std::optional<TlsConfig> tls; // nullopt = plaintext
    PlaintextHttp plaintext{PlaintextHttp::Http1AndHttp2};
    TlsHttp alpn{TlsHttp::Http1AndHttp2};
    std::size_t max_connections{0};
    std::optional<EngineLimits> limits;
    // TCP_NODELAY for connections accepted by this listener; ServerConfig's
    // value is copied into the implicit listener.
    bool tcp_nodelay{true};
    std::function<void(asio::ip::tcp::socket &)> socket_setup;
    std::function<void(asio::ip::tcp::acceptor &)> listener_setup;
};

using ListenerConfig = TcpListenerConfig;

enum class StartPolicy { AllOrNothing, BestEffort };

enum class server_errc { bind_failed = 1, tls_setup_failed, invalid_config };

inline const error_category &server_category() {
    static const struct : error_category {
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
    std::string scheme; // http / https
    std::string host;
    std::uint16_t port{0};
    bool tls{false};
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
    std::vector<ListenerConfig> listeners;
    std::optional<Listen> listen{InetAddress{}};
    std::optional<TlsConfig> tls;
    // Each worker gets one single-threaded io_context and one acceptor; a
    // connection accepted there is served there.
    unsigned worker_threads{4};
    std::size_t max_connections{0};
    std::chrono::milliseconds shutdown_grace{5000};
    bool auto_alt_svc{true};
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // The HTTP/3 listener. QUIC has no plaintext mode, so `tls` must be set.
    std::optional<InetAddress> quic{};
    quic::QuicEndpointConfig quic_options{};
#endif
    bool tcp_nodelay{true};
    PlaintextProtocols plaintext_protocols{PlaintextProtocols::Both};
    EngineLimits limits{};
    std::function<void(asio::ip::tcp::socket &)> socket_setup;
    std::function<void(asio::ip::tcp::acceptor &)> listener_setup;
};

class Server {
  public:
    explicit Server(ServerConfig cfg = {}) : m_cfg(std::move(cfg)), m_router(std::make_shared<Router>()) {}
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    ~Server() {
        try {
            stop();
        } catch (...) {
            // A destructor must not let an exception escape: stop() joins the
            // worker threads (std::system_error is possible) and calls the
            // user's LogSink, either of which may throw.
        }
    }

    // --- routing facade (forwards to the Router) ---

    template <typename F, typename Doc = openapi::NoDoc>
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
    Server &use(Middleware middleware) {
        m_router->use(std::move(middleware));
        return *this;
    }
    // Forwards the callback/coroutine shape exactly like Router::use.
    template <typename F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, Middleware>, int> = 0>
    Server &use(F &&middleware) {
        m_router->use(std::forward<F>(middleware));
        return *this;
    }
    Server &cors(CorsConfig config) {
        m_router->cors(std::move(config));
        return *this;
    }
    template <typename F> Server &group(std::string prefix, F &&register_routes) {
        m_router->group(std::move(prefix), std::forward<F>(register_routes));
        return *this;
    }
    template <typename F> Server &group(std::string prefix, std::vector<Middleware> middlewares, F &&register_routes) {
        m_router->group(std::move(prefix), std::move(middlewares), std::forward<F>(register_routes));
        return *this;
    }
    template <typename F> Server &host(std::string pattern, F &&register_routes) {
        m_router->host(std::move(pattern), std::forward<F>(register_routes));
        return *this;
    }
    template <typename F> Server &host(std::string pattern, std::vector<Middleware> middlewares, F &&register_routes) {
        m_router->host(std::move(pattern), std::move(middlewares), std::forward<F>(register_routes));
        return *this;
    }
    template <typename F> Server &ws_route(std::string path, F &&handler) {
        m_router->ws_route(std::move(path), make_ws_handler(std::forward<F>(handler)));
        return *this;
    }
    template <typename F> Server &ws_route(std::string path, std::vector<Middleware> middlewares, F &&handler) {
        m_router->ws_route(std::move(path), std::move(middlewares), make_ws_handler(std::forward<F>(handler)));
        return *this;
    }
    template <typename F> Server &ws_route_regex(const std::string &pattern, F &&handler) {
        m_router->ws_route_regex(pattern, make_ws_handler(std::forward<F>(handler)));
        return *this;
    }
    template <typename F>
    Server &ws_route_regex(const std::string &pattern, std::vector<Middleware> middlewares, F &&handler) {
        m_router->ws_route_regex(pattern, std::move(middlewares), make_ws_handler(std::forward<F>(handler)));
        return *this;
    }
    template <typename T> Server &state(std::shared_ptr<T> value) {
        m_router->state(std::move(value));
        return *this;
    }

    // OpenAPI: the document is collected by Router::route from each route's
    // openapi::doc() descriptor; these forward the info fields and serving.
    openapi::OpenApiSpec &openapi() { return m_router->openapi(); }
    Server &serve_openapi(std::string path = "/openapi.json") {
        m_router->serve_openapi(std::move(path));
        return *this;
    }
    Server &serve_swagger_ui(std::string path = "/swagger", std::string spec_url = "/openapi.json") {
        m_router->serve_swagger_ui(std::move(path), std::move(spec_url));
        return *this;
    }
    // Byte-level WebSocket reverse proxy (RFC 6455 pass-through).
    Server &ws_proxy(std::string path, WsProxyTarget target) {
        m_router->ws_proxy(std::move(path), std::move(target));
        return *this;
    }
    Server &ws_proxy_regex(const std::string &pattern, WsProxyTarget target) {
        m_router->ws_proxy_regex(pattern, std::move(target));
        return *this;
    }

    Server &http_proxy(std::string path, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        m_router->http_proxy(std::move(path), std::move(target), std::move(client_cfg));
        return *this;
    }
    Server &http_proxy_regex(const std::string &pattern, HttpProxyTarget target, ClientConfig client_cfg = {}) {
        m_router->http_proxy_regex(pattern, std::move(target), std::move(client_cfg));
        return *this;
    }
    Server &static_files(std::shared_ptr<StaticFiles> site) {
        m_router->static_files(std::move(site));
        return *this;
    }
    Router &router() { return *m_router; }
    const Router &router() const { return *m_router; }

    // --- listening ---

    Server &listen(TcpListenerConfig cfg) {
        m_cfg.listeners.push_back(std::move(cfg));
        return *this;
    }
    Server &listen(InetAddress address) {
        TcpListenerConfig cfg;
        cfg.address = std::move(address);
        cfg.tls = m_cfg.tls;
        return listen(std::move(cfg));
    }
    Server &add_listener(TcpListenerConfig cfg) { return listen(std::move(cfg)); }

    result<StartReport, StartupError> start(StartPolicy policy = StartPolicy::AllOrNothing) {
        if (m_started) {
            return StartupError{{ListenerFailure{"server", make_error_code(server_errc::invalid_config)}}};
        }
        std::vector<TcpListenerConfig> listeners = m_cfg.listeners;
        if (listeners.empty()) {
            if (!m_cfg.listen) {
                return StartupError{{ListenerFailure{"server", make_error_code(server_errc::invalid_config)}}};
            }
            TcpListenerConfig cfg;
            cfg.address = *m_cfg.listen;
            cfg.tls = m_cfg.tls;
            cfg.max_connections = m_cfg.max_connections;
            // Leave `limits` empty: the implicit listener inherits the server
            // defaults when limits are resolved, after every transport has
            // bound. Copying them here would freeze the value before the QUIC
            // port (and therefore Alt-Svc) is known.
            cfg.tcp_nodelay = m_cfg.tcp_nodelay;
            cfg.socket_setup = m_cfg.socket_setup;
            cfg.listener_setup = m_cfg.listener_setup;
            switch (m_cfg.plaintext_protocols) {
            case PlaintextProtocols::Http1:
                cfg.plaintext = PlaintextHttp::Http1Only;
                break;
            case PlaintextProtocols::Http2:
                cfg.plaintext = PlaintextHttp::Http2PriorKnowledgeOnly;
                break;
            case PlaintextProtocols::Both:
                cfg.plaintext = PlaintextHttp::Http1AndHttp2;
                break;
            }
            listeners.push_back(std::move(cfg));
        }

        // Reject invalid HTTP/2 SETTINGS before any socket is bound: the engine
        // serializes these values verbatim, and a peer that receives an
        // out-of-range SETTINGS_MAX_FRAME_SIZE / INITIAL_WINDOW_SIZE must close
        // the connection. (The client config source uses the same helper.)
        if (!m_cfg.limits.h2_settings_valid()) {
            return StartupError{{ListenerFailure{"server", make_error_code(server_errc::invalid_config)}}};
        }
        for (const auto &cfg : listeners) {
            const EngineLimits &limits = cfg.limits ? *cfg.limits : m_cfg.limits;
            if (!limits.h2_settings_valid()) {
                return StartupError{{ListenerFailure{cfg.name.empty() ? "server" : cfg.name,
                                                     make_error_code(server_errc::invalid_config)}}};
            }
        }

        auto core = std::make_shared<Core>();
        core->router = m_router;
        core->pool = std::make_shared<IoCtxPool>(m_cfg.worker_threads);
        core->workers.resize(core->pool->size());
        store_core(core);
        build_core();
        m_stopping.store(false, std::memory_order_release);

        StartReport report;
        std::vector<std::shared_ptr<ListenerState>> started;
        for (auto &cfg : listeners) {
            auto listener = std::make_shared<ListenerState>();
            listener->cfg = cfg;
            listener->name = cfg.name;
            listener->core = core;
            listener->stopping = &m_stopping;

            const auto *inet = std::get_if<InetAddress>(&cfg.address);
            if (!inet) {
                SIMPLE_HTTP_ERROR_LOG("server: UNIX-domain listeners are not ported yet");
                report.failures.push_back(ListenerFailure{cfg.name.empty() ? "unix" : cfg.name,
                                                          make_error_code(server_errc::invalid_config)});
                continue;
            }
            if (cfg.tls) {
                try {
                    std::vector<std::string_view> alpn;
                    switch (cfg.alpn) {
                    case TlsHttp::Http1Only:
                        alpn = {"http/1.1"};
                        break;
                    case TlsHttp::Http2Only:
                        alpn = {"h2"};
                        break;
                    case TlsHttp::Http1AndHttp2:
                    default:
                        alpn = {"h2", "http/1.1"};
                        break;
                    }
                    listener->tls =
                        std::make_shared<TlsContext>(*cfg.tls, span<const std::string_view>{alpn.data(), alpn.size()},
                                                     cfg.alpn == TlsHttp::Http2Only);
                } catch (const std::exception &e) {
                    SIMPLE_HTTP_ERROR_LOG("server: TLS setup failed: {}", e.what());
                    report.failures.push_back(ListenerFailure{cfg.name.empty() ? inet->host : cfg.name,
                                                              make_error_code(server_errc::tls_setup_failed)});
                    continue;
                }
            }

            error_code ec;
            const auto address = asio::ip::make_address(inet->host, ec);
            if (ec) {
                report.failures.push_back(ListenerFailure{cfg.name.empty() ? inet->host : cfg.name,
                                                          make_error_code(server_errc::bind_failed)});
                continue;
            }
            // One acceptor per worker, each created on (and pinned to) its
            // worker context. SO_REUSEPORT lets the kernel hash new
            // connections across the group; without it a single acceptor on
            // the first worker is the only portable option.
#ifdef SO_REUSEPORT
            constexpr bool kernel_fanout = true;
#else
            constexpr bool kernel_fanout = false;
#endif
            const std::size_t acceptor_count = kernel_fanout ? core->pool->size() : 1;
            std::uint16_t bound_port = inet->port;
            bool bound_ok = true;
            for (std::size_t ai = 0; ai < acceptor_count; ++ai) {
                auto &ctx = core->pool->at(ai);
                auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(ctx.get_executor());
                // With port 0 the kernel picks the port on the first bind; the
                // rest of the group must join *that* port, otherwise every
                // acceptor would land on a different ephemeral port and the
                // reported port would only serve one of them.
                const asio::ip::tcp::endpoint endpoint{address, bound_port};
                acceptor->open(endpoint.protocol(), ec);
                if (!ec) {
                    acceptor->set_option(asio::socket_base::reuse_address{true}, ec);
                }
#ifdef SO_REUSEPORT
                if (!ec && kernel_fanout) {
                    int one = 1;
                    (void)::setsockopt(acceptor->native_handle(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
                }
#endif
                if (!ec) {
                    acceptor->bind(endpoint, ec);
                }
                if (!ec) {
                    acceptor->listen(asio::socket_base::max_listen_connections, ec);
                }
                if (ec) {
                    SIMPLE_HTTP_ERROR_LOG("server: bind {}:{} [{}] failed: {}", inet->host, inet->port, ai,
                                          ec.message());
                    bound_ok = false;
                    break;
                }
                if (cfg.listener_setup) {
                    cfg.listener_setup(*acceptor);
                }
                if (ai == 0) {
                    bound_port = acceptor->local_endpoint(ec).port();
                    if (ec) {
                        SIMPLE_HTTP_ERROR_LOG("server: local_endpoint failed after bind: {}", ec.message());
                        bound_ok = false;
                        break;
                    }
                }
                listener->acceptors.push_back(AcceptorSlot{std::move(acceptor), ai});
            }
            if (!bound_ok) {
                close_acceptors(*listener);
                report.failures.push_back(ListenerFailure{cfg.name.empty() ? inet->host : cfg.name, ec});
                continue;
            }
            switch (cfg.plaintext) {
            case PlaintextHttp::Http2PriorKnowledgeOnly:
                listener->protocols = PlaintextProtocols::Http2;
                break;
            case PlaintextHttp::Http1Only:
                listener->protocols = PlaintextProtocols::Http1;
                break;
            case PlaintextHttp::Http1WithH2cUpgrade:
                listener->protocols = PlaintextProtocols::Http1;
                listener->h2c_upgrade = true;
                break;
            case PlaintextHttp::Http1AndHttp2:
            default:
                // Sniff the preface; anything that is not a prior-knowledge
                // preface starts as HTTP/1.x and may still Upgrade: h2c.
                listener->protocols = PlaintextProtocols::Both;
                listener->h2c_upgrade = true;
                break;
            }
            listener->max_connections = cfg.max_connections != 0 ? cfg.max_connections : m_cfg.max_connections;
            m_port.store(bound_port, std::memory_order_release);
            started.push_back(listener);
            report.bound.push_back(
                BoundEndpoint{listener->name, cfg.tls ? "https" : "http", inet->host, bound_port, cfg.tls.has_value()});
        }

        if (report.bound.empty() || (policy == StartPolicy::AllOrNothing && !report.failures.empty())) {
            for (auto &listener : started) {
                close_acceptors(*listener);
            }
            return StartupError{std::move(report.failures)};
        }

        m_listeners = std::move(started);
        // Phase 1: bind every transport. TCP acceptors are already bound above;
        // QUIC sockets bind here. Nothing is served yet.
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        if (m_cfg.quic) {
            const auto err = bind_quic_listener(core);
            if (err) {
                report.failures.push_back(ListenerFailure{"quic", make_error_code(*err)});
                if (policy == StartPolicy::AllOrNothing) {
                    for (auto &listener : m_listeners) {
                        close_acceptors(*listener);
                    }
                    m_listeners.clear();
                    return StartupError{std::move(report.failures)};
                }
            }
        }
#endif
        // Phase 2: resolve the effective limits now that every bound port is
        // known (Alt-Svc names the real QUIC port). The result is read-only
        // from here on: engines copy it when a connection is created.
        resolve_listener_limits();
        // Phase 3: start serving.
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        start_quic_serving();
#endif
        for (auto &listener : m_listeners) {
            for (const auto &slot : listener->acceptors) {
                start_accept(listener, slot);
            }
        }
        core->pool->start();
        m_started = true;
        return report;
    }

    void stop() {
        if (!m_started) {
            return;
        }
        // Teardown from a connection handler would join this worker thread to
        // itself (std::system_error, then terminate) and destroy state the
        // running request still uses. Refuse it; the caller must post stop()
        // to a thread outside the pool.
        if (const auto probe = core_snapshot(); probe && probe->pool && probe->pool->on_worker_thread()) {
            SIMPLE_HTTP_ERROR_LOG("server: stop() called from a worker thread; post it to another thread");
            return;
        }
        m_stopping.store(true, std::memory_order_release);
        for (auto &listener : m_listeners) {
            if (!listener)
                continue;
            for (auto &slot : listener->acceptors) {
                // Asio objects are not safe to touch from two threads: the
                // acceptor's async_accept lives on a worker (or the main)
                // context, while stop() runs on the caller's thread. Post the
                // close; the captured shared_ptr keeps the acceptor alive until
                // the handler runs (or is destroyed with the context).
                auto acceptor = slot.acceptor;
                asio::post(acceptor->get_executor(), [acceptor] {
                    error_code ignored;
                    acceptor->close(ignored);
                });
            }
        }
        // Hold one reference for the whole teardown. Observability accessors on
        // other threads take the same mutex to copy the pointer; without it a
        // concurrent active_connections() would race the reset below (a
        // shared_ptr read/write race, not merely a stale count).
        const auto core = core_snapshot();
        if (core) {
            // Honour shutdown_grace for requests already in a handler, then
            // cancel every remaining connection. Without the cancel, an idle
            // keep-alive read keeps the worker alive until IoCtxPool's generic
            // two-second drain grace expires, so stop() always costs ~2 s per
            // worker regardless of the configured grace.
            if (m_cfg.shutdown_grace.count() > 0) {
                const auto deadline = std::chrono::steady_clock::now() + m_cfg.shutdown_grace;
                while (core->requests.load(std::memory_order_acquire) > 0 &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            }
            core->close_all(core);
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        for (auto &rt : m_quic_runtimes) {
            for (auto &ep : rt->endpoints) {
                auto exec = ep->get_executor();
                asio::post(exec, [ep] { ep->shutdown_on_executor(); });
            }
        }
#endif
        if (core && core->pool) {
            core->pool->stop();
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        m_quic_runtimes.clear();
        m_quic_port.store(0, std::memory_order_release);
#endif
        m_listeners.clear();
        clear_core();
        m_started = false;
    }

    std::uint16_t port() const { return m_port.load(std::memory_order_acquire); }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    std::uint16_t quic_port() const { return m_quic_port.load(std::memory_order_acquire); }
#endif
    std::size_t active_connections() const {
        const auto core = core_snapshot();
        return core ? core->connections->load() : 0;
    }
    std::size_t active_requests() const {
        const auto core = core_snapshot();
        return core ? core->requests.load() : 0;
    }

  private:
    struct Core {
        std::shared_ptr<Router> router;
        Dispatcher dispatch;
        WsLookup ws_exact;
        WsProxyLookup ws_proxy;
        WsLookup ws_regex;
        std::shared_ptr<IoCtxPool> pool;
        std::shared_ptr<std::atomic<std::size_t>> connections{std::make_shared<std::atomic<std::size_t>>(0)};
        std::atomic<std::size_t> requests{0};

        // Connections never leave the worker that accepted them, so the
        // closer registry is sharded by worker: registration and release both
        // run on that worker's thread and touch its vector without a lock.
        // stop() reaches a shard by posting a task to the same context.
        struct WorkerState {
            std::vector<std::weak_ptr<std::function<void()>>> closers;
        };
        std::vector<WorkerState> workers;

        // `accept_waiters` re-arms an acceptor that stopped at
        // max_connections once a slot frees. A connection may be released on
        // one worker while the paused acceptor lives on another, so this one
        // stays shared and is guarded.
        std::mutex lifecycle_mutex;
        std::vector<std::function<void()>> accept_waiters;

        // Worker-thread only.
        void register_closer(std::size_t worker, const std::shared_ptr<std::function<void()>> &closer) {
            auto &closers = workers[worker].closers;
            // Drop entries whose connection is gone; otherwise a long-running
            // server grows this vector by one slot per connection until stop().
            closers.erase(
                std::remove_if(closers.begin(), closers.end(),
                               [](const std::weak_ptr<std::function<void()>> &entry) { return entry.expired(); }),
                closers.end());
            closers.push_back(closer);
        }

        // Worker-thread only.
        void close_worker_connections(std::size_t worker) {
            auto &closers = workers[worker].closers;
            std::vector<std::shared_ptr<std::function<void()>>> live;
            live.reserve(closers.size());
            for (auto &entry : closers) {
                if (auto closer = entry.lock()) {
                    live.push_back(std::move(closer));
                }
            }
            closers.clear();
            // The post that got us here runs on this worker, so closing the
            // transport inline is safe.
            for (auto &closer : live) {
                (*closer)();
            }
        }

        void on_accept_capacity(std::function<void()> waiter) {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            accept_waiters.push_back(std::move(waiter));
        }

        void resume_accept_waiters() {
            std::function<void()> waiter;
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                if (!accept_waiters.empty()) {
                    waiter = std::move(accept_waiters.back());
                    accept_waiters.pop_back();
                }
            }
            if (waiter) {
                waiter();
            }
        }

        void connection_released(const std::shared_ptr<std::atomic<std::size_t>> &listener_active = {}) {
            // Per-listener max_connections is enforced against this counter;
            // the server-wide one stays for active_connections() and QUIC.
            if (listener_active) {
                listener_active->fetch_sub(1, std::memory_order_acq_rel);
            }
            connections->fetch_sub(1, std::memory_order_acq_rel);
            resume_accept_waiters();
        }

        // Called from the stopping thread. Each worker closes its own
        // connections in a posted task; asio objects must not be touched from
        // here. The weak capture keeps a queued task from owning the pool.
        void close_all(const std::shared_ptr<Core> &self) {
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                accept_waiters.clear();
            }
            for (std::size_t i = 0; i < workers.size(); ++i) {
                std::weak_ptr<Core> weak = self;
                asio::post(pool->at(i), [weak, i] {
                    if (auto core = weak.lock()) {
                        core->close_worker_connections(i);
                    }
                });
            }
        }
    };

    // One acceptor per worker. Each acceptor lives on the context that will
    // also serve the sockets it accepts; `worker` is that context's index.
    struct AcceptorSlot {
        std::shared_ptr<asio::ip::tcp::acceptor> acceptor;
        std::size_t worker{0};
    };

    struct ListenerState {
        TcpListenerConfig cfg;
        std::string name;
        std::vector<AcceptorSlot> acceptors;
        std::shared_ptr<TlsContext> tls;
        std::shared_ptr<Core> core;
        std::atomic<bool> *stopping{nullptr};
        std::size_t max_connections{0};
        // Resolved once every transport has bound (Alt-Svc needs the real QUIC
        // port) and never mutated afterwards. Worker threads only copy it when
        // an h1/h2 engine is created for a new connection.
        EngineLimits limits;
        // Connections currently served by this listener. Shared so a release
        // callback still decrements it after the listener itself is gone.
        std::shared_ptr<std::atomic<std::size_t>> active{std::make_shared<std::atomic<std::size_t>>(0)};
        PlaintextProtocols protocols{PlaintextProtocols::Both};
        bool h2c_upgrade{false};
    };

    // Shared by the bind loop, the AllOrNothing rollback and the QUIC rollback.
    static void close_acceptors(ListenerState &listener) {
        for (auto &slot : listener.acceptors) {
            error_code ignored;
            slot.acceptor->close(ignored);
        }
        listener.acceptors.clear();
    }

    static void start_accept(const std::shared_ptr<ListenerState> &listener, const AcceptorSlot &slot) {
        // At the limit, do not issue another accept: the kernel backlog applies
        // backpressure. Re-arm from connection_released() when a slot frees.
        if (listener->max_connections != 0 &&
            listener->active->load(std::memory_order_acquire) >= listener->max_connections) {
            auto exec = slot.acceptor->get_executor();
            std::weak_ptr<ListenerState> weak_waiter = listener;
            listener->core->on_accept_capacity([weak_waiter, slot, exec] {
                asio::post(exec, [weak_waiter, slot] {
                    if (auto again = weak_waiter.lock()) {
                        start_accept(again, slot);
                    }
                });
            });
            return;
        }
        auto &acceptor = *slot.acceptor;
        // Create the socket on the acceptor's own context: a socket keeps the
        // executor it was constructed with, and this is the worker that will
        // run the connection from here on.
        auto socket = std::make_shared<asio::ip::tcp::socket>(acceptor.get_executor());
        // The pending accept lives inside the acceptor, which the listener owns.
        // A strong capture here would make listener -> acceptor -> handler ->
        // listener a cycle whenever a connection is pending at teardown.
        std::weak_ptr<ListenerState> weak_listener = listener;
        acceptor.async_accept(*socket, [weak_listener, socket, slot](error_code ec) {
            auto listener = weak_listener.lock();
            if (!listener) {
                return;
            }
            if (ec) {
                if (ec != asio::error::operation_aborted && !listener->stopping->load()) {
                    // Resource pressure: do not spin on accept.
                    SIMPLE_HTTP_ERROR_LOG("server: accept failed: {}", ec.message());
                    auto timer = std::make_shared<asio::steady_timer>(slot.acceptor->get_executor());
                    timer->expires_after(std::chrono::milliseconds(50));
                    timer->async_wait(
                        [weak_listener = std::weak_ptr<ListenerState>(listener), timer, slot](error_code) {
                            if (auto again = weak_listener.lock()) {
                                start_accept(again, slot);
                            }
                        });
                }
                return;
            }
            if (listener->stopping->load()) {
                error_code ignored;
                socket->close(ignored);
                return;
            }
            serve_connection(listener, slot.worker, std::move(socket));
            start_accept(listener, slot);
        });
    }

    // Registers a closer for stop(). `worker` is the connection's own thread;
    // close_all() runs the callback there via a posted task, so the transport
    // can be touched inline.
    template <typename Transport>
    static std::shared_ptr<std::function<void()>> register_closer(const std::shared_ptr<Core> &core, std::size_t worker,
                                                                  std::shared_ptr<Transport> transport) {
        auto closer = std::make_shared<std::function<void()>>([transport] { transport->close(); });
        core->register_closer(worker, closer);
        return closer;
    }

    // Runs on the accept worker. Socket options, the user hook and the engine
    // all stay on this thread, which is why the socket is never re-posted or
    // re-allocated between accept and service.
    static void serve_connection(const std::shared_ptr<ListenerState> &listener, std::size_t worker,
                                 std::shared_ptr<asio::ip::tcp::socket> socket) {
        error_code ec;
        socket->set_option(asio::ip::tcp::no_delay{listener->cfg.tcp_nodelay}, ec);
        if (listener->cfg.socket_setup) {
            // A user callback MUST NOT be allowed to throw out of the accept
            // handler: the exception would escape io_context::run() and call
            // std::terminate on the worker.
            try {
                listener->cfg.socket_setup(*socket);
            } catch (const std::exception &e) {
                SIMPLE_HTTP_ERROR_LOG("server: socket_setup threw: {}", e.what());
                error_code ignored;
                socket->close(ignored);
                return;
            } catch (...) {
                SIMPLE_HTTP_ERROR_LOG("server: socket_setup threw unknown exception");
                error_code ignored;
                socket->close(ignored);
                return;
            }
        }
        auto core = listener->core;
        core->connections->fetch_add(1, std::memory_order_acq_rel);
        listener->active->fetch_add(1, std::memory_order_acq_rel);
        if (listener->tls) {
            auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket),
                                                                                     listener->tls->context());
            const auto peer = stream->lowest_layer().remote_endpoint(ec);
            auto transport = std::make_shared<TlsStreamTransport>(stream, peer);
            auto closer = register_closer(core, worker, transport);
            std::weak_ptr<ListenerState> weak_listener = listener;
            std::weak_ptr<Core> weak_core = core;
            // A peer that completes the TCP handshake and then sends no (or a
            // dribbled) ClientHello must not hold a connection slot forever.
            // The same idle budget the engines use after the handshake bounds
            // this phase; closing the transport unwinds the pending handshake.
            auto handshake_timer = std::make_shared<asio::steady_timer>(transport->get_executor());
            auto handshake_done = std::make_shared<std::atomic<bool>>(false);
            const auto handshake_timeout = listener->limits.idle_timeout;
            if (handshake_timeout.count() > 0) {
                handshake_timer->expires_after(handshake_timeout);
                handshake_timer->async_wait([transport, handshake_done](error_code tec) {
                    if (!tec && !handshake_done->load(std::memory_order_acquire))
                        transport->close();
                });
            }
            auto active = listener->active;
            transport->async_handshake([weak_listener, weak_core, active, transport, closer, handshake_timer,
                                        handshake_done](error_code handshake_ec) {
                handshake_done->store(true, std::memory_order_release);
                handshake_timer->cancel();
                auto alive = weak_listener.lock();
                if (handshake_ec || !alive) {
                    transport->close();
                    if (alive) {
                        alive->core->connection_released(active);
                    } else if (auto c = weak_core.lock()) {
                        c->connection_released(active);
                    }
                    return;
                }
                if (transport->alpn_selected() == "h2") {
                    run_h2(alive, std::move(transport), std::string{}, closer);
                    return;
                }
                // An HTTP/2-only listener must not fall back to h1 when the
                // client sent no ALPN at all: OpenSSL does not invoke the
                // select callback in that case, so the policy is enforced here.
                if (alive->cfg.alpn == TlsHttp::Http2Only) {
                    transport->close();
                    alive->core->connection_released(active);
                    return;
                }
                run_h1(alive, std::move(transport), std::string{}, false, closer);
            });
            return;
        }
        const auto peer = socket->remote_endpoint(ec);
        auto transport = std::make_shared<TcpStreamTransport>(std::move(socket), peer);
        auto closer = register_closer(core, worker, transport);
        switch (listener->protocols) {
        case PlaintextProtocols::Http2:
            run_h2(listener, std::move(transport), std::string{}, closer);
            return;
        case PlaintextProtocols::Http1:
            run_h1(listener, std::move(transport), std::string{}, listener->h2c_upgrade, closer);
            return;
        case PlaintextProtocols::Both:
        default:
            sniff_plaintext(listener, std::move(transport), closer);
            return;
        }
    }

    // Prior-knowledge h2 is recognised by the start of the client preface. Read
    // exactly that many bytes (a TCP segment can split it); anything else is
    // HTTP/1.x, with the bytes replayed into its parser.
    static void sniff_plaintext(const std::shared_ptr<ListenerState> &listener,
                                std::shared_ptr<TcpStreamTransport> transport,
                                std::shared_ptr<std::function<void()>> closer) {
        static constexpr std::string_view h2_prefix = "PRI * HTTP/2.0";
        auto buffer = std::make_shared<std::array<std::byte, 14>>();
        std::weak_ptr<ListenerState> weak_listener = listener;
        std::weak_ptr<Core> weak_core = listener->core;
        auto active = listener->active;
        auto on_read = [weak_listener, weak_core, active, transport, buffer, closer](error_code ec, std::size_t n) {
            auto alive = weak_listener.lock();
            if (ec && n == 0) {
                transport->close();
                if (alive) {
                    alive->core->connection_released(active);
                } else if (auto c = weak_core.lock()) {
                    c->connection_released(active);
                }
                return;
            }
            if (!alive) {
                // The listener was torn down while this connection was in the
                // sniff phase. serve_connection already counted it; release
                // that slot even though the listener itself is gone.
                transport->close();
                if (auto c = weak_core.lock()) {
                    c->connection_released(active);
                }
                return;
            }
            const std::string_view bytes{reinterpret_cast<const char *>(buffer->data()), n};
            if (n == h2_prefix.size() && bytes == h2_prefix) {
                run_h2(alive, std::move(transport), std::string{bytes}, closer);
                return;
            }
            run_h1(alive, std::move(transport), std::string{bytes}, alive->h2c_upgrade, closer);
        };
        const auto idle = listener->limits.idle_timeout;
        if (idle.count() > 0) {
            transport->async_read(ByteSpan{buffer->data(), buffer->size()},
                                  asio::cancel_after(idle, std::move(on_read)));
        } else {
            transport->async_read(ByteSpan{buffer->data(), buffer->size()}, std::move(on_read));
        }
    }

    // Called once, after every transport has bound. The base is the listener's
    // own override when it has one, otherwise the server defaults; Alt-Svc is
    // then filled in from the QUIC port that actually bound. An explicit
    // Alt-Svc (server-wide or per listener) wins — auto mode only fills an
    // empty value. render_alt_svc() honours the h3_alt_svc / max-age knobs of
    // whichever limits won.
    void resolve_listener_limits() {
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        const std::uint16_t quic_port = m_quic_port.load(std::memory_order_acquire);
        for (auto &rt : m_quic_runtimes) {
            EngineLimits limits = m_cfg.limits;
            if (m_cfg.auto_alt_svc && limits.alt_svc.empty()) {
                limits.alt_svc = limits.render_alt_svc(quic_port);
            }
            *rt->limits = std::move(limits);
        }
#endif
        for (auto &listener : m_listeners) {
            EngineLimits limits = listener->cfg.limits ? *listener->cfg.limits : m_cfg.limits;
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
            if (m_cfg.auto_alt_svc && limits.alt_svc.empty()) {
                limits.alt_svc = limits.render_alt_svc(quic_port);
            }
#endif
            listener->limits = std::move(limits);
        }
    }

    template <typename Transport>
    static void run_h1(const std::shared_ptr<ListenerState> &listener, std::shared_ptr<Transport> transport,
                       std::string initial, bool allow_h2c, std::shared_ptr<std::function<void()>> closer) {
        auto core = listener->core;
        auto dispatch = core->dispatch;
        auto ws_exact = core->ws_exact;
        auto ws_proxy = core->ws_proxy;
        auto ws_regex = core->ws_regex;
        auto engine = std::make_shared<Http1Engine<Transport>>(transport, listener->limits);
        engine->allow_h2c_upgrade(allow_h2c);
        std::weak_ptr<Core> weak_core = core;
        // Keep the closer alive for the whole connection: the registry only
        // holds a weak reference so a finished connection is not leaked.
        auto active = listener->active;
        engine->run(std::move(dispatch), std::move(initial), std::move(ws_exact), std::move(ws_proxy),
                    std::move(ws_regex), [weak_core, active, closer] {
                        if (auto c = weak_core.lock())
                            c->connection_released(active);
                    });
    }

    template <typename Transport>
    static void run_h2(const std::shared_ptr<ListenerState> &listener, std::shared_ptr<Transport> transport,
                       std::string initial, std::shared_ptr<std::function<void()>> closer) {
        auto core = listener->core;
        auto dispatch = core->dispatch;
        auto ws_exact = core->ws_exact;
        auto ws_proxy = core->ws_proxy;
        auto ws_regex = core->ws_regex;
        auto engine = std::make_shared<Http2Engine<Transport>>(transport, listener->limits);
        std::weak_ptr<Core> weak_core = core;
        auto active = listener->active;
        engine->run(std::move(dispatch), std::move(initial), std::move(ws_exact), std::move(ws_proxy),
                    std::move(ws_regex), [weak_core, active, closer] {
                        if (auto c = weak_core.lock())
                            c->connection_released(active);
                    });
    }

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    using QuicConnectionType = quic::QuicConnection<asio::io_context::executor_type>;
    using QuicEndpointType = quic::QuicEndpoint<asio::io_context::executor_type>;

    struct QuicRuntime {
        InetAddress address;
        std::shared_ptr<quic::QuicTlsContext> tls;
        std::shared_ptr<std::atomic<std::size_t>> active;
        std::shared_ptr<quic::SharedCidRegistry<QuicConnectionType>> cids;
        std::vector<std::shared_ptr<QuicEndpointType>> endpoints;
        // Shared with the serve callback: the endpoints are constructed while
        // binding, but the limits are only final after the bound port is known
        // (Alt-Svc). Phase 2 writes it once, before any endpoint starts, and
        // every h3 engine copies the value when a connection arrives.
        std::shared_ptr<EngineLimits> limits;
    };

    typename QuicEndpointType::ServeFn make_h3_serve(std::shared_ptr<EngineLimits> limits) {
        auto router = m_router;
        auto serve = [router, limits = std::move(limits)](std::shared_ptr<QuicConnectionType> conn,
                                                          std::function<void()> done) {
            auto engine = std::make_shared<Http3Engine<QuicConnectionType>>(conn, *limits);
            auto dispatch = [router](RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion completion) {
                router->dispatch(std::move(req), std::move(res), ssl, std::move(completion));
            };
            engine->run(std::move(dispatch), std::move(done));
        };
        return serve;
    }

    // Binds the UDP sockets and records the port, but does not start the
    // receive loops: the server resolves Alt-Svc (which needs that port) first
    // and only then calls start_quic_serving().
    std::optional<server_errc> bind_quic_listener(const std::shared_ptr<Core> &core) {
        if (!m_cfg.tls) {
            SIMPLE_HTTP_ERROR_LOG("server: HTTP/3 needs TLS (QUIC has no plaintext mode)");
            return server_errc::invalid_config;
        }
        auto rt = std::make_shared<QuicRuntime>();
        rt->address = *m_cfg.quic;
        rt->limits = std::make_shared<EngineLimits>(m_cfg.limits);
        quic::QuicEarlyDataConfig early;
        early.enabled = m_cfg.quic_options.connection.enable_0rtt;
        if (early.enabled) {
            const h3::Settings settings = h3::make_settings(*rt->limits);
            early.ticket_appdata = h3::encode_ticket_settings(settings);
            early.validate_ticket_appdata = [settings](std::string_view appdata) {
                return h3::ticket_settings_compatible_for_0rtt(appdata, settings);
            };
        }
        try {
            rt->tls = std::make_shared<quic::QuicTlsContext>(*m_cfg.tls, std::move(early));
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("server: QUIC TLS setup failed: {}", e.what());
            return server_errc::tls_setup_failed;
        }
        error_code ec;
        const auto addr = asio::ip::make_address(rt->address.host, ec);
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("server: QUIC bind({}): {}", rt->address.host, ec.message());
            return server_errc::invalid_config;
        }
        rt->active = std::make_shared<std::atomic<std::size_t>>(0);
        auto endpoint_config = m_cfg.quic_options;
        endpoint_config.max_connections = m_cfg.max_connections;
        endpoint_config.active = rt->active;
        // The same counter the TCP accept path uses: max_connections is a
        // server-wide budget, and active_connections() must include QUIC.
        endpoint_config.global_active = core->connections;
        endpoint_config.global_max_connections = m_cfg.max_connections;
        std::weak_ptr<Core> weak_core = core;
        endpoint_config.on_released = [weak_core] {
            if (auto c = weak_core.lock())
                c->resume_accept_waiters();
        };
        if (RAND_bytes(endpoint_config.shared_secret.data(), static_cast<int>(endpoint_config.shared_secret.size())) !=
            1) {
            SIMPLE_HTTP_ERROR_LOG("server: QUIC could not generate the listener secret");
            return server_errc::tls_setup_failed;
        }
        endpoint_config.use_shared_secret = true;
        rt->cids = std::make_shared<quic::SharedCidRegistry<QuicConnectionType>>();
        // One QUIC socket per worker, mirroring the TCP acceptors. Like the TCP
        // path, a configured port 0 is resolved on the first bind and the other
        // sockets join the port the kernel chose.
#ifdef SO_REUSEPORT
        const std::size_t sockets = core->pool->size();
#else
        const std::size_t sockets = 1;
#endif
        const bool reuse_port = sockets > 1;
        std::uint16_t bind_port = rt->address.port;
        for (std::size_t i = 0; i < sockets; ++i) {
            auto &ctx = core->pool->at(i);
            const asio::ip::udp::endpoint endpoint{addr, bind_port};
            auto quic = std::make_shared<QuicEndpointType>(ctx.get_executor(), rt->tls->native_handle(),
                                                           endpoint_config, make_h3_serve(rt->limits), rt->cids);
            if (!quic->open(endpoint, reuse_port, ec)) {
                SIMPLE_HTTP_ERROR_LOG("server: QUIC bind({}:{}) [{}]: {}", rt->address.host, bind_port, i,
                                      ec.message());
                for (auto &bound : rt->endpoints)
                    bound->close();
                return server_errc::bind_failed;
            }
            if (i == 0) {
                bind_port = quic->port();
                if (bind_port == 0) {
                    SIMPLE_HTTP_ERROR_LOG("server: QUIC local_endpoint failed after bind");
                    quic->close();
                    return server_errc::bind_failed;
                }
            }
            rt->endpoints.push_back(std::move(quic));
        }
        const std::uint16_t bound_port = rt->endpoints.empty() ? 0 : rt->endpoints.front()->port();
        m_quic_port.store(bound_port, std::memory_order_release);
        SIMPLE_HTTP_INFO_LOG("listening on udp {}:{} (quic, alpn=h3, sockets={})", rt->address.host, rt->address.port,
                             rt->endpoints.size());
        m_quic_runtimes.push_back(std::move(rt));
        return std::nullopt;
    }

    void start_quic_serving() {
        for (auto &rt : m_quic_runtimes) {
            for (auto &endpoint : rt->endpoints) {
                endpoint->start();
            }
        }
    }
#endif

    void build_core() {
        auto core = core_snapshot();
        // dispatch is stored on Core while the lambda runs for every request;
        // a strong capture of Core here is a self-cycle that keeps the pool,
        // every io_context and the router alive after stop(). Requests in
        // flight keep the Server alive through the normal shutdown path; a
        // dispatch that loses the race with teardown just completes.
        std::weak_ptr<Core> weak_core = core;
        auto router = core->router;
        core->dispatch = [weak_core, router](RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion done) {
            auto alive = weak_core.lock();
            if (!alive) {
                std::move(done)();
                return;
            }
            alive->requests.fetch_add(1, std::memory_order_acq_rel);
            router->dispatch(std::move(req), std::move(res), ssl, [weak_core, done = std::move(done)]() mutable {
                if (auto c = weak_core.lock()) {
                    c->requests.fetch_sub(1, std::memory_order_acq_rel);
                }
                std::move(done)();
            });
        };
        core->ws_exact = [router](std::string_view path) -> std::optional<WsHandlerFn> {
            if (const WsRouteEntry *entry = router->find_ws_exact(path)) {
                return router->make_ws_handler_fn(*entry);
            }
            return std::nullopt;
        };
        core->ws_proxy = [router](std::string_view path) -> std::optional<WsProxyTarget> {
            return router->find_ws_proxy(path);
        };
        core->ws_regex = [router](std::string_view path) -> std::optional<WsHandlerFn> {
            if (const WsRouteEntry *entry = router->find_ws_regex(path)) {
                return router->make_ws_handler_fn(*entry);
            }
            return std::nullopt;
        };
    }

    // active_connections()/active_requests() are observability APIs callable
    // from any thread, while start()/stop() replace/reset the Core. Guarding the
    // shared_ptr is what makes that safe; the counters themselves are atomic.
    std::shared_ptr<Core> core_snapshot() const {
        std::lock_guard<std::mutex> lock(m_core_mutex);
        return m_core;
    }
    void store_core(std::shared_ptr<Core> core) {
        std::lock_guard<std::mutex> lock(m_core_mutex);
        m_core = std::move(core);
    }
    void clear_core() {
        std::lock_guard<std::mutex> lock(m_core_mutex);
        m_core.reset();
    }

    ServerConfig m_cfg;
    std::shared_ptr<Router> m_router;
    mutable std::mutex m_core_mutex;
    std::shared_ptr<Core> m_core;
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    std::vector<std::shared_ptr<QuicRuntime>> m_quic_runtimes;
    std::atomic<std::uint16_t> m_quic_port{0};
#endif
    std::vector<std::shared_ptr<ListenerState>> m_listeners;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_started{false};
    std::atomic<std::uint16_t> m_port{0};
};

} // namespace simple_http
