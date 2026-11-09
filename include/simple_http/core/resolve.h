#pragma once

// Name-resolution hook shared by the outbound client and the WebSocket reverse
// proxy (C++17 port).
//
// The default is asio::ip::tcp::resolver (getaddrinfo / the system resolver).
// An application that has its own asynchronous DNS client (a cache, c-ares,
// DoH, a private upstream, …) can provide the endpoints instead:
//
//     cfg.resolve = [](std::string host, std::string port,
//                      simple_http::ResolveCompletion done) {
//         // Must be non-blocking and pass endpoints in the order they should
//         // be tried. It may complete synchronously or from any thread; the
//         // result is posted back to the caller's executor.
//         my_dns.async_resolve(std::move(host), std::move(done));
//     };
//
// The callback form is deliberately a token-style initiating function rather
// than a coroutine: the port must work when built as C++17, and the completion
// handler carries the caller's associated executor / allocator. The completion
// is always marshalled to that executor, so an asynchronous hook never touches
// connection state from its own thread. An empty list with no error is treated as host_not_found by callers;
// IP literals need no query.

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace simple_http {

namespace asio = boost::asio;

using ResolveCompletion = asio::any_completion_handler<void(error_code, std::vector<asio::ip::tcp::endpoint>)>;

// (host, port, completion). Empty host + empty port is never passed; an empty
// endpoint list with error_code{} means "no addresses".
using ResolveFn = std::function<void(std::string host, std::string port, ResolveCompletion done)>;

namespace detail {

class ResolveOp {
  public:
    ResolveOp(ResolveFn resolve, std::string host, std::uint16_t port, asio::any_io_executor ex)
        : m_resolve(std::move(resolve)), m_host(std::move(host)), m_port(port), m_resolver(std::move(ex)) {}

    asio::any_io_executor get_executor() noexcept { return m_resolver.get_executor(); }

    template <typename Self>
    void operator()(Self &self, error_code ec = {},
                    asio::ip::tcp::resolver::results_type results = asio::ip::tcp::resolver::results_type{}) {
        if (!m_started) {
            m_started = true;
            if (m_resolve) {
                // Move the composed operation into the completion handler the
                // hook receives. That move also moves this ResolveOp's members
                // (m_resolve, m_host), so snapshot everything the invocation
                // needs first -- otherwise the call below would run off the
                // moved-from function/string (bad_function_call, empty host).
                ResolveFn resolve = m_resolve;
                std::string host = m_host;
                std::string service = std::to_string(m_port);
                // Bind the composed operation's executor to the handler the
                // hook receives: a plain lambda carries no associated executor,
                // so an asynchronous hook (a DNS cache, c-ares, DoH, ...) would
                // otherwise complete on whatever thread it runs on instead of
                // the caller's. That is a data race with everything else on the
                // connection executor, not just a lost wakeup.
                auto executor = self.get_executor();
                resolve(std::move(host), std::move(service),
                        asio::bind_executor(
                            executor,
                            [self = std::move(self), executor](error_code rec,
                                                               std::vector<asio::ip::tcp::endpoint> endpoints) mutable {
                                // The hook may complete synchronously or from
                                // its own thread. bind_executor only sets the
                                // associated executor: it does not dispatch, so
                                // the completion would still run inline or on
                                // the hook's thread (racing the connection
                                // executor). Post unconditionally.
                                asio::post(
                                    executor,
                                    [self = std::move(self), rec, endpoints = std::move(endpoints)]() mutable {
                                        self.complete(rec, std::move(endpoints));
                                    });
                            }));
                return;
            }
            m_resolver.async_resolve(m_host, std::to_string(m_port), std::move(self));
            return;
        }
        std::vector<asio::ip::tcp::endpoint> endpoints;
        if (!ec) {
            endpoints.reserve(results.size());
            for (const auto &entry : results) {
                endpoints.push_back(entry.endpoint());
            }
        }
        self.complete(ec, std::move(endpoints));
    }

  private:
    ResolveFn m_resolve;
    std::string m_host;
    std::uint16_t m_port;
    asio::ip::tcp::resolver m_resolver;
    bool m_started{false};
};

} // namespace detail

// Resolves `host` for `port`, using `resolve` when set and the system resolver
// otherwise. Signature: void(error_code, std::vector<asio::ip::tcp::endpoint>).
template <typename Token>
auto async_resolve_endpoints(ResolveFn resolve, std::string host, std::uint16_t port, asio::any_io_executor ex,
                             Token &&token) {
    auto object = ex;
    return asio::async_compose<Token, void(error_code, std::vector<asio::ip::tcp::endpoint>)>(
        detail::ResolveOp{std::move(resolve), std::move(host), port, std::move(ex)}, token, object);
}

} // namespace simple_http
