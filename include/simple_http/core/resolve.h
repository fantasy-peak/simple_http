#pragma once

// Name-resolution hook shared by the outbound client and the WebSocket reverse
// proxy (C++17 port).
//
// The default is asio::ip::tcp::resolver (getaddrinfo / the system resolver).
// An application that has its own asynchronous DNS client (a cache, c-ares,
// DoH, a private upstream, …) can provide the endpoints instead:
//
//     cfg.resolve = [](std::string host, std::uint16_t port,
//                      simple_http::asio::any_io_executor ex,
//                      simple_http::ResolveCompletion done) {
//         // Called on the caller's executor, which is `ex`. Start the lookup
//         // there (or arrange for its answer to be posted there) and invoke
//         // `done` exactly once. It may complete synchronously or from any
//         // thread; the result is marshalled back to `ex` either way.
//         my_dns.async_resolve(std::move(host), ex, std::move(done));
//     };
//
// The executor is passed explicitly rather than recovered from `done`: the
// completion handler is type-erased (asio::any_completion_handler), and its
// associated executor erases to any_completion_executor, which cannot be
// converted back into an any_io_executor. Handing the concrete executor over
// keeps an awaitable DNS client (a cache, c-ares, DoH, ...) usable from this
// callback-shaped hook without coroutine adapters or executor downcasts.
//
// The hook is a token-style initiating function rather than a coroutine so the
// port keeps working when built as C++17. An empty list with no error is
// treated as host_not_found by callers; IP literals need no query.

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
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

// (host, port, executor, completion). Empty host + empty port is never passed;
// an empty endpoint list with error_code{} means "no addresses".
using ResolveFn =
    std::function<void(std::string host, std::uint16_t port, asio::any_io_executor ex, ResolveCompletion done)>;

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
                // The caller's executor, handed to the hook so an asynchronous
                // DNS client can run the lookup on the connection's thread. No
                // type erasure is involved: this is the concrete executor the
                // composed operation was given.
                auto executor = self.get_executor();
                resolve(std::move(host), m_port, executor,
                        [self = std::move(self), executor](error_code rec,
                                                           std::vector<asio::ip::tcp::endpoint> endpoints) mutable {
                            // The hook may complete synchronously or from its
                            // own thread (c-ares, a resolver thread). Post
                            // unconditionally: an asio operation must not
                            // complete from its initiating function, and the
                            // answer has to land on the caller's executor.
                            asio::post(executor, [self = std::move(self), rec, endpoints = std::move(endpoints)]() mutable {
                                self.complete(rec, std::move(endpoints));
                            });
                        });
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
