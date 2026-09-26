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
#include <cstddef>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/ssl.hpp>

#include "../core/io_pool.h"
#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../handler/router.h"
#include "../transport/tcp_transport.h"
#include "../transport/tls_context.h"
#include "../transport/tls_transport.h"
#include "connection.h"

namespace simple_http {

namespace asio = boost::asio;

// An IP endpoint. Leave v6 false on an IPv6 host for dual-stack: `::` then also
// accepts IPv4-mapped connections, so one listener covers both families.
struct InetAddress {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    bool v6{false};  // IPV6_V6ONLY
};

// A UNIX-domain socket at a filesystem path.
struct UnixAddress {
    std::string path;
};

// Where the server listens. Exactly one of the two — a configuration that named
// both a port and a path would have no meaning, so the type makes it
// unrepresentable rather than a rule to remember.
//
// Serving several addresses or protocol stacks is a multi-process concern:
// start another Server, or another process sharing the port via
// ServerConfig::reuse_port.
using Listen = std::variant<InetAddress, UnixAddress>;

struct ServerConfig {
    Listen listen{InetAddress{}};
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
    // Policy for the client that reverse-proxy routes (`http_proxy*`) use: TLS
    // trust and client certificates for HTTPS backends, timeouts, pool sizing.
    // Defaults suit public backends (system CA, verification on).
    ClientConfig proxy_client{};
    // Optional per-accepted-socket hook (TCP_NODELAY / keepalive / buffer sizes …).
    // Invoked right after accept, before the transport or TLS handshake touches the
    // socket. Use the error_code overloads of set_option to avoid throwing.
    std::function<void(asio::ip::tcp::socket&)> socket_setup;
};

class Server {
  public:
    explicit Server(ServerConfig config)
        : m_config(std::move(config)),
          m_pool(std::make_shared<IoCtxPool>(m_config.worker_threads == 0 ? 1 : m_config.worker_threads)),
          m_router(std::make_shared<Router>(m_config.proxy_client)) {
        if (m_config.tls) {
            m_tls.emplace(*m_config.tls);
        }
        if (!m_config.reuse_port) {
            // Dedicated acceptor context, created last so it is never handed
            // out as a worker. With reuse_port the workers accept for
            // themselves and this thread would sit idle.
            m_pool->add_main_context();
        }
        m_pool->start();
    }

    ~Server() { stop(); }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // --- route registration (fluent, forwarded to the Router) ---
    template <typename F>
    Server& route(std::string path, F&& handler) {
        m_router->route(std::move(path), std::forward<F>(handler));
        return *this;
    }
    template <typename F>
    Server& route_regex(const std::string& pattern, F&& handler) {
        m_router->route_regex(pattern, std::forward<F>(handler));
        return *this;
    }
    template <typename F>
    Server& fallback(F&& handler) {
        m_router->fallback(std::forward<F>(handler));
        return *this;
    }
    Server& before(Filter f) {
        m_router->before(std::move(f));
        return *this;
    }
    Server& cors(Filter f) {
        m_router->cors(std::move(f));
        return *this;
    }

    // --- WebSocket route registration ---
    Server& ws_route(std::string path, WsHandler handler) {
        m_router->ws_route(std::move(path), std::move(handler));
        return *this;
    }
    Server& ws_route_regex(const std::string& pattern, WsHandler handler) {
        m_router->ws_route_regex(pattern, std::move(handler));
        return *this;
    }

    // --- WebSocket proxy-route registration (byte-level pass-through) ---
    // An Upgrade: websocket request on `path` is spliced verbatim to the backend
    // TCP endpoint host:port. Frames, fragmentation, masking and control frames
    // all pass through untouched.
    Server& ws_proxy(std::string path, std::string host, std::uint16_t port, std::string rewrite_path = {}) {
        m_router->ws_proxy(std::move(path),
                           WsProxyTarget{std::move(host), port, std::move(rewrite_path)});
        return *this;
    }
    Server& ws_proxy_regex(const std::string& pattern, std::string host, std::uint16_t port,
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
    // capture groups). The short form proxies to a plaintext HTTP/1.1 backend;
    // pass an HttpProxyTarget to reach a TLS (ALPN picks h2 when the backend
    // offers it) or h2c backend instead.
    Server& http_proxy(std::string path, std::string host, std::uint16_t port, std::string rewrite_path = {}) {
        return http_proxy(std::move(path), HttpProxyTarget{std::move(host), port, std::move(rewrite_path)});
    }
    Server& http_proxy_regex(const std::string& pattern, std::string host, std::uint16_t port,
                             std::string rewrite_path = {}) {
        return http_proxy_regex(pattern, HttpProxyTarget{std::move(host), port, std::move(rewrite_path)});
    }
    Server& http_proxy(std::string path, HttpProxyTarget target) {
        m_router->http_proxy(std::move(path), std::move(target));
        return *this;
    }
    Server& http_proxy_regex(const std::string& pattern, HttpProxyTarget target) {
        m_router->http_proxy_regex(pattern, std::move(target));
        return *this;
    }

    // Start the listener. Resolves true if the socket bound successfully.
    asio::awaitable<bool> run() { co_return start_listeners(); }

    // Synchronous convenience: start the listener and block until bound.
    // Returns true if it bound successfully.
    bool start() { return start_listeners(); }

    void stop() {
        if (m_stopped.exchange(true)) return;

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
        for (auto& listener : m_acceptors) {
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
                    [](const auto& socket) {
                        error_code ec;
                        socket->close(ec);
                    },
                    acceptor);
                mark_done();
                continue;
            }
            asio::post(*listener.ctx, [acceptor, mark_done] {
                std::visit(
                    [](const auto& socket) {
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
        m_pool->stop();
        m_acceptors.clear();
    }

    // The local port a TCP listener is using (useful with port 0 / ephemeral).
    // Zero for a UNIX-domain socket, which has no port to report.
    std::uint16_t port() const {
        if (m_acceptors.empty()) return 0;
        return std::visit(
            [](const auto& acceptor) {
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
    // An acceptor of either family, and the context its accept loop runs on.
    //
    // Holding both behind one type is what keeps start and stop single: the
    // socket types are unrelated, but nothing above this line has to care. The
    // variant holds one alternative on a platform without AF_UNIX, and every
    // std::visit over it still compiles.
    using AnyAcceptor = std::variant<std::shared_ptr<asio::ip::tcp::acceptor>,
                                     std::shared_ptr<asio::local::stream_protocol::acceptor>>;

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
            if (const WsHandler* h = router->find_ws(path)) {
                return *h;  // copy the handler for the engine to run
            }
            return std::nullopt;
        };
    }

    WsProxyLookup make_ws_proxy_lookup() {
        auto router = m_router;
        return [router](std::string_view path) -> std::optional<WsProxyTarget> {
            return router->find_ws_proxy(path);  // already expands capture groups
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
    std::shared_ptr<asio::ip::tcp::acceptor> bind_acceptor(asio::io_context& ctx, const std::string& host,
                                                           std::uint16_t port, bool v6_only, error_code& ec) {
        auto addr = asio::ip::make_address(host, ec);
        if (ec) return nullptr;
        asio::ip::tcp::endpoint ep{addr, port};
        auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(ctx);
        acceptor->open(ep.protocol(), ec);
        if (ec) return nullptr;
        if (v6_only) {
            acceptor->set_option(asio::ip::v6_only(true), ec);
            if (ec) return nullptr;
        }
        acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (ec) return nullptr;
        if (m_config.reuse_port) {
            // Must precede bind(), and every socket in the group must set it:
            // one that doesn't cannot share the port. A failure here is how a
            // platform without SO_REUSEPORT announces itself, and the caller
            // falls back to a single acceptor — silently dropping the error
            // would leave the fan-out half-bound and half-serving.
#ifdef SO_REUSEPORT
            acceptor->set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true), ec);
            if (ec) return nullptr;
#else
            ec = asio::error::operation_not_supported;
            return nullptr;
#endif
        }
        acceptor->bind(ep, ec);
        if (ec) return nullptr;
        acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) return nullptr;
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
    std::shared_ptr<asio::ip::tcp::acceptor> probe_acceptor(const InetAddress& address, error_code& ec) {
        auto ctx = acceptor_context();
        auto acceptor = bind_acceptor(*ctx, address.host, address.port, address.v6, ec);
        if (acceptor || address.v6 || !is_ipv6_host(address.host)) return acceptor;
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
    bool fan_out(const std::string& host, std::uint16_t port, bool v6_only) {
        const std::size_t bound = m_acceptors.size();  // just the probe's
        for (std::size_t i = bound; i < m_pool->size(); ++i) {
            auto& ctx = m_pool->at(i);
            error_code ec;
            auto acceptor = bind_acceptor(*ctx, host, port, v6_only, ec);
            if (!acceptor) {
                SIMPLE_HTTP_ERROR_LOG("reuse_port bind({}:{}) [{}]: {}", host, port, i, ec.message());
                while (m_acceptors.size() > bound) {
                    const auto acceptor = m_acceptors.back().acceptor;
                    std::visit(
                        [](const auto& socket) {
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

    // One entry point for both families. The variant decides which bind runs;
    // everything after it — the accept loops, the logging — is shared, because
    // by then the difference has already been erased.
    bool start_listeners() {
        return std::visit([this](const auto& address) { return start_listener(address); }, m_config.listen);
    }

    bool start_listener(const InetAddress& address) {
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
    bool start_listener(const UnixAddress& address) {
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
        std::filesystem::remove(address.path, ignored);  // best-effort

        auto ctx = acceptor_context();
        asio::local::stream_protocol::endpoint endpoint{address.path};
        auto acceptor = std::make_shared<asio::local::stream_protocol::acceptor>(*ctx);

        error_code ec;
        acceptor->open(endpoint.protocol(), ec);
        if (!ec) acceptor->bind(endpoint, ec);
        if (!ec) acceptor->listen(asio::socket_base::max_listen_connections, ec);
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

    // Starts one accept loop per bound socket. The loop is templated on the
    // acceptor/socket pair; this is the only place that names both, and the
    // protocol hands the socket type back rather than making it a second
    // parameter to thread through by hand.
    void spawn_accept_loops(bool partitioned) {
        for (auto& listener : m_acceptors) {
            std::visit(
                [&](const auto& acceptor) {
                    using Acceptor = std::remove_reference_t<decltype(*acceptor)>;
                    using Socket = typename Acceptor::protocol_type::socket;
                    // Pinned only when the fan-out really happened: re-dispatching
                    // a connection the kernel already assigned to this socket
                    // would give back the affinity the fan-out bought.
                    asio::co_spawn(*listener.ctx, accept_loop<Acceptor, Socket>(acceptor, partitioned ? listener.ctx : nullptr),
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
        for (;;) {
            // Pinned: the kernel already picked this socket, so keep the
            // connection here. Otherwise round-robin the pool, which spreads
            // long-lived connections more evenly than the kernel's hash does.
            asio::io_context& ctx = pinned ? *pinned : *m_pool->next_ptr();
            Socket socket{ctx};
            auto [ec] = co_await acceptor->async_accept(socket, asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (ec == asio::error::operation_aborted) break;
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
                    socket.set_option(asio::ip::tcp::no_delay(true), ne);  // best-effort
                }
                if (m_config.socket_setup) {
                    try {
                        m_config.socket_setup(socket);
                    } catch (const std::exception& e) {
                        SIMPLE_HTTP_ERROR_LOG("socket_setup threw: {}", e.what());
                    }
                }
                error_code pe;
                peer = socket.remote_endpoint(pe);
            }

            if (m_tls) {
                auto stream = std::make_shared<asio::ssl::stream<Socket>>(std::move(socket), m_tls->context());
                auto transport = std::make_shared<TlsTransport<Socket>>(std::move(stream), peer);
                asio::co_spawn(ctx, serve_tls(transport, dispatch, ws_lookup, m_config.limits, ws_proxy_lookup),
                               asio::detached);
            } else {
                auto sock_ptr = std::make_shared<Socket>(std::move(socket));
                auto transport = std::make_shared<TcpTransport<Socket>>(std::move(sock_ptr), peer);
                asio::co_spawn(ctx,
                               serve_plaintext(transport, dispatch, ws_lookup, m_config.limits, ws_proxy_lookup,
                                               m_config.plaintext_protocols),
                               asio::detached);
            }
        }
        co_return;
    }

    static std::string local_address(const asio::ip::tcp::acceptor& acceptor) {
        error_code ec;
        return acceptor.local_endpoint(ec).address().to_string();
    }

    static std::uint16_t local_port(const asio::ip::tcp::acceptor& acceptor) {
        error_code ec;
        return acceptor.local_endpoint(ec).port();
    }

    static bool is_ipv6_host(const std::string& host) {
        error_code ec;
        auto addr = asio::ip::make_address(host, ec);
        return !ec && addr.is_v6();
    }

    ServerConfig m_config;
    std::shared_ptr<IoCtxPool> m_pool;
    std::shared_ptr<Router> m_router;
    std::optional<TlsContext> m_tls;
    std::vector<Listener> m_acceptors;
    // Atomic: stop() is the one member a signal handler or a second thread has
    // any business calling, and double-stopping must not race.
    std::atomic<bool> m_stopped{false};
};

}  // namespace simple_http
