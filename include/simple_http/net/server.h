#pragma once

// Server: listeners, connection lifecycle and the router facade (C++17 port).
//
// This first version serves HTTP/1.x over TCP, plaintext or TLS, with local
// WebSocket upgrades. HTTP/2 (and therefore h2c/ALPN h2), HTTP/3, UNIX sockets
// and the reverse-proxy/static/OpenAPI stages are deferred; the configuration
// types keep their shape so fixtures and applications port incrementally.
//
// Threading follows model A: every accepted connection is handed to one worker
// io_context from an IoCtxPool (one thread per context), and all of its engine,
// writer and handler work happens there.

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/system/result.hpp>
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

#include "../core/io_pool.h"
#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/result.h"
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

namespace asio = boost::asio;

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
    bool reuse_port{true};
    std::size_t acceptors{0};
    std::size_t max_connections{0};
    std::optional<EngineLimits> limits;
    std::function<void(asio::ip::tcp::socket &)> socket_setup;
    std::function<void(asio::ip::tcp::acceptor &)> listener_setup;
};

using ListenerConfig = TcpListenerConfig;

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
    std::string scheme; // http / https
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
    std::vector<ListenerConfig> listeners;
    std::optional<Listen> listen{InetAddress{}};
    std::optional<TlsConfig> tls;
    unsigned worker_threads{4};
    std::size_t max_connections{0};
    bool reuse_port{true};
    std::size_t acceptors{0};
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

    ~Server() { stop(); }

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
    template <typename F> Server &fallback(F &&handler) {
        m_router->fallback(std::forward<F>(handler));
        return *this;
    }
    Server &use(Middleware middleware) {
        m_router->use(std::move(middleware));
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
            cfg.reuse_port = m_cfg.reuse_port;
            cfg.acceptors = m_cfg.acceptors;
            cfg.max_connections = m_cfg.max_connections;
            cfg.limits = m_cfg.limits;
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

        auto core = std::make_shared<Core>();
        core->router = m_router;
        core->limits = m_cfg.limits;
        core->pool = std::make_shared<IoCtxPool>(m_cfg.worker_threads);
        core->pool->add_main_context();
        m_core = core;
        build_core();
        m_stopping.store(false, std::memory_order_release);

        StartReport report;
        std::vector<std::shared_ptr<ListenerState>> started;
        for (auto &cfg : listeners) {
            auto listener = std::make_shared<ListenerState>();
            listener->cfg = cfg;
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
                    listener->tls = std::make_shared<TlsContext>(
                        *cfg.tls, boost::span<const std::string_view>{alpn.data(), alpn.size()});
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
            const auto endpoint = asio::ip::tcp::endpoint{address, inet->port};
            // reuse_port: one acceptor per worker, each on its own worker
            // context with SO_REUSEPORT (the kernel hashes connections). Without
            // it, one acceptor on the main context round-robins the pool.
            const std::size_t acceptor_count =
                cfg.reuse_port ? (cfg.acceptors == 0 ? core->pool->size() : std::min(cfg.acceptors, core->pool->size()))
                               : 1;
            std::uint16_t bound_port = inet->port;
            bool bound_ok = true;
            for (std::size_t ai = 0; ai < acceptor_count; ++ai) {
                asio::io_context *pinned = cfg.reuse_port ? core->pool->at(ai).get() : nullptr;
                auto &ctx = pinned != nullptr ? *pinned : *core->pool->main_context();
                auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(ctx.get_executor());
                acceptor->open(endpoint.protocol(), ec);
                if (!ec) {
                    acceptor->set_option(asio::socket_base::reuse_address{true}, ec);
                }
#ifdef SO_REUSEPORT
                if (!ec && cfg.reuse_port) {
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
                        bound_port = inet->port;
                    }
                }
                listener->acceptors.push_back(AcceptorSlot{std::move(acceptor), pinned});
            }
            if (!bound_ok) {
                for (auto &slot : listener->acceptors) {
                    error_code ignored;
                    slot.acceptor->close(ignored);
                }
                listener->acceptors.clear();
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
            report.bound.push_back(BoundEndpoint{listener->name, cfg.tls ? "https" : "http", inet->host, bound_port,
                                                 cfg.tls.has_value(), listener->acceptors.size(), cfg.reuse_port});
        }

        if (report.bound.empty() || (policy == StartPolicy::AllOrNothing && !report.failures.empty())) {
            for (auto &listener : started) {
                for (auto &slot : listener->acceptors) {
                    error_code ignored;
                    slot.acceptor->close(ignored);
                }
            }
            return StartupError{std::move(report.failures)};
        }

        m_listeners = std::move(started);
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        if (m_cfg.quic) {
            const auto err = start_quic_listener(core);
            if (err) {
                report.failures.push_back(ListenerFailure{"quic", make_error_code(*err)});
                if (policy == StartPolicy::AllOrNothing) {
                    for (auto &listener : m_listeners) {
                        for (auto &slot : listener->acceptors) {
                            error_code ignored;
                            slot.acceptor->close(ignored);
                        }
                    }
                    m_listeners.clear();
                    return StartupError{std::move(report.failures)};
                }
            }
        }
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
        m_stopping.store(true, std::memory_order_release);
        for (auto &listener : m_listeners) {
            if (listener) {
                for (auto &slot : listener->acceptors) {
                    error_code ignored;
                    slot.acceptor->close(ignored);
                }
            }
        }
        if (m_core) {
            // Honour shutdown_grace for requests already in a handler, then
            // cancel every remaining connection. Without the cancel, an idle
            // keep-alive read keeps the worker alive until IoCtxPool's generic
            // two-second drain grace expires, so stop() always costs ~2 s per
            // worker regardless of the configured grace.
            if (m_cfg.shutdown_grace.count() > 0) {
                const auto deadline = std::chrono::steady_clock::now() + m_cfg.shutdown_grace;
                while (m_core->requests.load(std::memory_order_acquire) > 0 &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            }
            m_core->close_all();
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        for (auto &rt : m_quic_runtimes) {
            for (auto &ep : rt->endpoints) {
                auto exec = ep->get_executor();
                asio::post(exec, [ep] { ep->shutdown_on_executor(); });
            }
        }
#endif
        if (m_core && m_core->pool) {
            m_core->pool->stop();
        }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
        m_quic_runtimes.clear();
        m_quic_port.store(0, std::memory_order_release);
#endif
        m_listeners.clear();
        m_core.reset();
        m_started = false;
    }

    std::uint16_t port() const { return m_port.load(std::memory_order_acquire); }
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    std::uint16_t quic_port() const { return m_quic_port.load(std::memory_order_acquire); }
#endif
    std::size_t active_connections() const { return m_core ? m_core->connections.load() : 0; }
    std::size_t active_requests() const { return m_core ? m_core->requests.load() : 0; }

  private:
    struct Core {
        std::shared_ptr<Router> router;
        EngineLimits limits;
        Dispatcher dispatch;
        WsLookup ws_exact;
        WsProxyLookup ws_proxy;
        WsLookup ws_regex;
        std::shared_ptr<IoCtxPool> pool;
        std::atomic<std::size_t> connections{0};
        std::atomic<std::size_t> requests{0};

        // Connection lifecycle registry. `closers` lets stop() cancel idle
        // keep-alive connections instead of waiting out IoCtxPool's generic
        // drain grace; `accept_waiters` re-arms an acceptor that stopped at
        // max_connections once a slot frees. Both are guarded because accept
        // callbacks and engine completion callbacks run on different workers.
        std::mutex lifecycle_mutex;
        std::vector<std::weak_ptr<std::function<void()>>> closers;
        std::vector<std::function<void()>> accept_waiters;

        void register_closer(const std::shared_ptr<std::function<void()>> &closer) {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            closers.push_back(closer);
        }

        void on_accept_capacity(std::function<void()> waiter) {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            accept_waiters.push_back(std::move(waiter));
        }

        void connection_released() {
            connections.fetch_sub(1, std::memory_order_acq_rel);
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

        void close_all() {
            std::vector<std::shared_ptr<std::function<void()>>> live;
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                for (auto &entry : closers) {
                    if (auto closer = entry.lock()) {
                        live.push_back(std::move(closer));
                    }
                }
                closers.clear();
                accept_waiters.clear();
            }
            // Each closer posts the transport close onto its own connection
            // executor; asio objects must not be closed from this thread.
            for (auto &closer : live) {
                (*closer)();
            }
        }
    };

    // One acceptor. With reuse_port set there is one per worker, pinned to it
    // (SO_REUSEPORT lets the kernel pick); otherwise a single acceptor on the
    // main context round-robins incoming connections over the pool.
    struct AcceptorSlot {
        std::shared_ptr<asio::ip::tcp::acceptor> acceptor;
        asio::io_context *pinned{nullptr};
    };

    struct ListenerState {
        TcpListenerConfig cfg;
        std::string name;
        std::vector<AcceptorSlot> acceptors;
        std::shared_ptr<TlsContext> tls;
        std::shared_ptr<Core> core;
        std::atomic<bool> *stopping{nullptr};
        std::size_t max_connections{0};
        PlaintextProtocols protocols{PlaintextProtocols::Both};
        bool h2c_upgrade{false};
    };

    static void start_accept(const std::shared_ptr<ListenerState> &listener, const AcceptorSlot &slot) {
        // At the limit, do not issue another accept: the kernel backlog applies
        // backpressure. Re-arm from connection_released() when a slot frees.
        if (listener->max_connections != 0 &&
            listener->core->connections.load(std::memory_order_acquire) >= listener->max_connections) {
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
        // Pick the worker now and accept into a socket *created on it*: a socket
        // keeps the executor it was constructed with, so accepting the default
        // way (which uses the acceptor's context) would pin every connection's
        // I/O to the accept thread regardless of where the handler was posted.
        // A pinned acceptor (SO_REUSEPORT fan-out) already runs on its worker.
        auto &worker = slot.pinned ? *slot.pinned : listener->core->pool->next();
        auto socket = std::make_shared<asio::ip::tcp::socket>(worker);
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
            serve_socket(listener, std::move(*socket));
            start_accept(listener, slot);
        });
    }

    static void serve_socket(const std::shared_ptr<ListenerState> &listener, asio::ip::tcp::socket socket) {
        error_code ec;
        socket.set_option(asio::ip::tcp::no_delay{true}, ec);
        if (listener->cfg.socket_setup) {
            listener->cfg.socket_setup(socket);
        }
        auto core = listener->core;
        core->connections.fetch_add(1, std::memory_order_acq_rel);
        // The socket was created on its worker (see start_accept), so its own
        // executor is the one to run the engine on. Weak captures: a post that
        // is still queued at teardown must not keep the listener (and through
        // it the Core/pool/io_context) alive.
        std::weak_ptr<ListenerState> weak_listener = listener;
        std::weak_ptr<Core> weak_core = core;
        asio::post(socket.get_executor(), [weak_listener, weak_core, socket = std::move(socket)]() mutable {
            if (auto alive = weak_listener.lock()) {
                serve_on_worker(alive, std::move(socket));
                return;
            }
            if (auto c = weak_core.lock()) {
                c->connection_released();
            }
        });
    }

    // Registers a closer that stop() can invoke from any thread. The close is
    // posted to the connection's executor because asio sockets/TLS streams are
    // not safe to touch from the stopping thread.
    template <typename Transport>
    static std::shared_ptr<std::function<void()>> register_closer(const std::shared_ptr<Core> &core,
                                                                  std::shared_ptr<Transport> transport) {
        auto exec = transport->get_executor();
        auto closer = std::make_shared<std::function<void()>>([transport, exec] {
            asio::post(exec, [transport] { transport->close(); });
        });
        core->register_closer(closer);
        return closer;
    }

    static void serve_on_worker(const std::shared_ptr<ListenerState> &listener, asio::ip::tcp::socket socket) {
        auto core = listener->core;
        if (listener->tls) {
            auto stream =
                std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(socket), listener->tls->context());
            error_code ec;
            const auto peer = stream->lowest_layer().remote_endpoint(ec);
            auto transport = std::make_shared<TlsStreamTransport>(stream, peer);
            auto closer = register_closer(core, transport);
            std::weak_ptr<ListenerState> weak_listener = listener;
            std::weak_ptr<Core> weak_core = core;
            transport->async_handshake([weak_listener, weak_core, transport, closer](error_code handshake_ec) {
                auto alive = weak_listener.lock();
                if (handshake_ec || !alive) {
                    transport->close();
                    if (alive) {
                        alive->core->connection_released();
                    } else if (auto c = weak_core.lock()) {
                        c->connection_released();
                    }
                    return;
                }
                if (transport->alpn_selected() == "h2") {
                    run_h2(alive, std::move(transport), std::string{}, closer);
                    return;
                }
                run_h1(alive, std::move(transport), std::string{}, false, closer);
            });
            return;
        }
        error_code ec;
        const auto peer = socket.remote_endpoint(ec);
        auto transport =
            std::make_shared<TcpStreamTransport>(std::make_shared<asio::ip::tcp::socket>(std::move(socket)), peer);
        auto closer = register_closer(core, transport);
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
        auto on_read = [weak_listener, weak_core, transport, buffer, closer](error_code ec, std::size_t n) {
            auto alive = weak_listener.lock();
            if (ec && n == 0) {
                transport->close();
                if (alive) {
                    alive->core->connection_released();
                } else if (auto c = weak_core.lock()) {
                    c->connection_released();
                }
                return;
            }
            if (!alive) {
                // The listener was torn down while this connection was in the
                // sniff phase. serve_socket already counted it; release that
                // slot even though the listener itself is gone.
                transport->close();
                if (auto c = weak_core.lock()) {
                    c->connection_released();
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
        const auto idle = listener->core->limits.idle_timeout;
        if (idle.count() > 0) {
            transport->async_read(ByteSpan{buffer->data(), buffer->size()},
                                  asio::cancel_after(idle, std::move(on_read)));
        } else {
            transport->async_read(ByteSpan{buffer->data(), buffer->size()}, std::move(on_read));
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
        auto engine = std::make_shared<Http1Engine<Transport>>(transport, core->limits);
        engine->allow_h2c_upgrade(allow_h2c);
        std::weak_ptr<Core> weak_core = core;
        // Keep the closer alive for the whole connection: the registry only
        // holds a weak reference so a finished connection is not leaked.
        engine->run(std::move(dispatch), std::move(initial), std::move(ws_exact), std::move(ws_proxy),
                    std::move(ws_regex), [weak_core, closer] {
                        if (auto c = weak_core.lock())
                            c->connection_released();
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
        auto engine = std::make_shared<Http2Engine<Transport>>(transport, core->limits);
        std::weak_ptr<Core> weak_core = core;
        engine->run(std::move(dispatch), std::move(initial), std::move(ws_exact), std::move(ws_proxy),
                    std::move(ws_regex), [weak_core, closer] {
                        if (auto c = weak_core.lock())
                            c->connection_released();
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
        EngineLimits limits;
        bool reuse_port{true};
    };

    typename QuicEndpointType::ServeFn make_h3_serve(EngineLimits limits) {
        auto router = m_router;
        return [router, limits](std::shared_ptr<QuicConnectionType> conn, std::function<void()> done) {
            auto engine = std::make_shared<Http3Engine<QuicConnectionType>>(conn, limits);
            auto dispatch = [router](RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion completion) {
                router->dispatch(std::move(req), std::move(res), ssl, std::move(completion));
            };
            engine->run(std::move(dispatch), std::move(done));
        };
    }

    std::optional<server_errc> start_quic_listener(const std::shared_ptr<Core> &core) {
        if (!m_cfg.tls) {
            SIMPLE_HTTP_ERROR_LOG("server: HTTP/3 needs TLS (QUIC has no plaintext mode)");
            return server_errc::invalid_config;
        }
        auto rt = std::make_shared<QuicRuntime>();
        rt->address = *m_cfg.quic;
        rt->limits = m_cfg.limits;
        rt->reuse_port = m_cfg.reuse_port;
        try {
            rt->tls = std::make_shared<quic::QuicTlsContext>(*m_cfg.tls);
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
        const asio::ip::udp::endpoint endpoint{addr, rt->address.port};
        rt->active = std::make_shared<std::atomic<std::size_t>>(0);
        auto endpoint_config = m_cfg.quic_options;
        endpoint_config.max_connections = m_cfg.max_connections;
        endpoint_config.active = rt->active;
        if (RAND_bytes(endpoint_config.shared_secret.data(), static_cast<int>(endpoint_config.shared_secret.size())) !=
            1) {
            SIMPLE_HTTP_ERROR_LOG("server: QUIC could not generate the listener secret");
            return server_errc::tls_setup_failed;
        }
        endpoint_config.use_shared_secret = true;
        rt->cids = std::make_shared<quic::SharedCidRegistry<QuicConnectionType>>();
        const std::size_t sockets = rt->reuse_port ? (core->pool->size() == 0 ? 1 : core->pool->size()) : 1;
        for (std::size_t i = 0; i < sockets; ++i) {
            auto &ctx = rt->reuse_port ? core->pool->at(i) : core->pool->main_context();
            auto quic = std::make_shared<QuicEndpointType>(ctx->get_executor(), rt->tls->native_handle(),
                                                           endpoint_config, make_h3_serve(rt->limits), rt->cids);
            if (!quic->open(endpoint, rt->reuse_port, ec)) {
                SIMPLE_HTTP_ERROR_LOG("server: QUIC bind({}:{}) [{}]: {}", rt->address.host, rt->address.port, i,
                                      ec.message());
                for (auto &bound : rt->endpoints)
                    bound->close();
                return server_errc::bind_failed;
            }
            quic->start();
            rt->endpoints.push_back(std::move(quic));
        }
        const std::uint16_t bound_port = rt->endpoints.empty() ? 0 : rt->endpoints.front()->port();
        m_quic_port.store(bound_port, std::memory_order_release);
        if (m_cfg.auto_alt_svc && bound_port != 0) {
            // Every engine renders Alt-Svc from the shared limits, so setting it
            // here makes h1/h2 responses point clients at this QUIC listener.
            core->limits.alt_svc = core->limits.render_alt_svc(bound_port);
        }
        SIMPLE_HTTP_INFO_LOG("listening on udp {}:{} (quic, alpn=h3, sockets={})", rt->address.host, rt->address.port,
                             rt->endpoints.size());
        m_quic_runtimes.push_back(std::move(rt));
        return std::nullopt;
    }
#endif

    void build_core() {
        auto core = m_core;
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

    ServerConfig m_cfg;
    std::shared_ptr<Router> m_router;
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
