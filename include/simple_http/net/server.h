#pragma once

// Server: the public facade.
//
// Configure listeners and (optionally) TLS, register routes on the built-in
// Router, then run(). Each accepted connection is pinned to one single-threaded
// io_context from the pool (concurrency model A) and served by the protocol
// detected in the net/connection layer. The Router's dispatch is used as the
// engine Dispatcher, so one route table serves HTTP/1.x, HTTP/2 and h2c.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
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

struct Listen {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    bool v6{false};
};

struct ServerConfig {
    std::vector<Listen> listen{{"0.0.0.0", 8080, false}};
    std::optional<TlsConfig> tls;
    unsigned worker_threads{4};
    bool reuse_port{false};
    // All protocol-engine tunables (timeouts, size caps, HTTP/2 windows/streams).
    EngineLimits limits{};
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
          m_router(std::make_shared<Router>()) {
        if (m_config.tls) {
            m_tls.emplace(*m_config.tls);
        }
        m_pool->add_main_context();  // dedicated context for acceptors
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

    // Start all listeners. Resolves true if every listener bound successfully.
    asio::awaitable<bool> run() {
        bool all_ok = true;
        for (const auto& l : m_config.listen) {
            auto acceptor = make_acceptor(l);
            if (!acceptor) {
                all_ok = false;
                continue;
            }
            asio::co_spawn(*m_pool->main_context(), accept_loop(acceptor, l), asio::detached);
        }
        co_return all_ok;
    }

    // Synchronous convenience: start all listeners and block until bound.
    // Returns true if every listener bound successfully.
    bool start() {
        std::promise<bool> done;
        auto fut = done.get_future();
        asio::co_spawn(*m_pool->main_context(),
                       [this]() -> asio::awaitable<bool> { co_return co_await run(); },
                       [&done](std::exception_ptr, bool ok) { done.set_value(ok); });
        return fut.get();
    }

    void stop() {
        if (m_stopped) return;
        m_stopped = true;
        for (auto& acc : m_acceptors) {
            error_code ec;
            acc->close(ec);
        }
        m_pool->stop();
    }

    // The local port a bound listener is using (useful with port 0 / ephemeral).
    std::uint16_t port(std::size_t index = 0) const {
        if (index >= m_acceptors.size()) return 0;
        error_code ec;
        return m_acceptors[index]->local_endpoint(ec).port();
    }

    std::shared_ptr<IoCtxPool> pool() { return m_pool; }

  private:
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

    std::shared_ptr<asio::ip::tcp::acceptor> make_acceptor(const Listen& l) {
        error_code ec;
        auto addr = asio::ip::make_address(l.host, ec);
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("make_address({}): {}", l.host, ec.message());
            return nullptr;
        }
        asio::ip::tcp::endpoint ep{addr, l.port};
        auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(*m_pool->main_context());
        acceptor->open(ep.protocol(), ec);
        if (ec) return nullptr;
        if (l.v6) acceptor->set_option(asio::ip::v6_only(true), ec);
        acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
#ifdef SO_REUSEPORT
        if (m_config.reuse_port) {
            acceptor->set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true), ec);
        }
#endif
        acceptor->bind(ep, ec);
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("bind({}:{}): {}", l.host, l.port, ec.message());
            return nullptr;
        }
        acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("listen({}:{}): {}", l.host, l.port, ec.message());
            return nullptr;
        }
        m_acceptors.push_back(acceptor);
        SIMPLE_HTTP_INFO_LOG("listening on {}:{} (tls={})", l.host, l.port, m_tls.has_value());
        return acceptor;
    }

    asio::awaitable<void> accept_loop(std::shared_ptr<asio::ip::tcp::acceptor> acceptor, Listen /*l*/) {
        auto dispatch = make_dispatcher();
        auto ws_lookup = make_ws_lookup();
        for (;;) {
            auto& ctx = m_pool->next_ptr();  // pin the connection to one worker
            asio::ip::tcp::socket socket{*ctx};
            auto [ec] = co_await acceptor->async_accept(socket, asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (ec == asio::error::operation_aborted) break;
                continue;
            }
            if (m_config.socket_setup) {
                try {
                    m_config.socket_setup(socket);
                } catch (const std::exception& e) {
                    SIMPLE_HTTP_ERROR_LOG("socket_setup threw: {}", e.what());
                }
            }
            error_code pe;
            auto peer = socket.remote_endpoint(pe);

            if (m_tls) {
                auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(socket),
                                                                                        m_tls->context());
                auto transport = std::make_shared<TlsStreamTransport>(std::move(stream), peer);
                asio::co_spawn(*ctx, serve_tls(transport, dispatch, ws_lookup, m_config.limits), asio::detached);
            } else {
                auto sock_ptr = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
                auto transport = std::make_shared<TcpStreamTransport>(std::move(sock_ptr), peer);
                asio::co_spawn(*ctx, serve_plaintext(transport, dispatch, ws_lookup, m_config.limits),
                               asio::detached);
            }
        }
        co_return;
    }

    ServerConfig m_config;
    std::shared_ptr<IoCtxPool> m_pool;
    std::shared_ptr<Router> m_router;
    std::optional<TlsContext> m_tls;
    std::vector<std::shared_ptr<asio::ip::tcp::acceptor>> m_acceptors;
    bool m_stopped{false};
};

}  // namespace simple_http
