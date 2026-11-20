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

#include "asio_compat.h"
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace simple_http {

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
                // The hook is user code (a cache, c-ares, DoH, ...) and may
                // throw. It owns the completion once passed in, so a throw would
                // otherwise leave the operation suspended forever -- and, when
                // the dial was started from an asynchronous continuation, escape
                // io_context::run(). Keep the composed operation behind a
                // shared_ptr plus a one-shot flag: a throw can still fail the
                // operation, and a hook that completes and then throws cannot
                // complete it twice.
                auto holder = std::make_shared<Self>(std::move(self));
                auto fired = std::make_shared<bool>(false);
                auto finish = [holder, fired, executor](error_code rec,
                                                        std::vector<asio::ip::tcp::endpoint> endpoints) {
                    // The hook may complete synchronously or from its own
                    // thread (c-ares, a resolver thread). Dispatch
                    // unconditionally: an asio operation must not complete from
                    // its initiating function, and the answer has to land on
                    // the caller's executor.
                    asio::dispatch(executor, [holder, fired, rec, endpoints = std::move(endpoints)]() mutable {
                        if (std::exchange(*fired, true))
                            return;
                        std::move(*holder).complete(rec, std::move(endpoints));
                    });
                };
                try {
                    resolve(std::move(host), m_port, executor,
                            [finish](error_code rec, std::vector<asio::ip::tcp::endpoint> endpoints) mutable {
                                finish(rec, std::move(endpoints));
                            });
                } catch (const std::exception &) {
                    finish(make_error_code(asio::error::host_not_found), std::vector<asio::ip::tcp::endpoint>{});
                } catch (...) {
                    finish(make_error_code(asio::error::host_not_found), std::vector<asio::ip::tcp::endpoint>{});
                }
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

// Resolves with a whole-dial budget. The underlying resolver is not
// cancellable, but the one-shot completion means a late answer is ignored and
// the caller stops waiting. Without this, a black-holed DNS server (or a
// custom ResolveFn that never calls back) hung dial() forever. `timeout_error`
// is the caller's error category (client_errc::connect_timeout for the HTTP
// client); core does not depend on the client layer.
struct ResolveBoundedState {
    std::shared_ptr<asio::steady_timer> timer;
    asio::any_completion_handler<void(error_code, std::vector<asio::ip::tcp::endpoint>)> done;
    bool finished{false};
};

inline void
async_resolve_bounded(ResolveFn resolve, std::string host, std::uint16_t port, asio::any_io_executor ex,
                      std::chrono::milliseconds limit, error_code timeout_error,
                      asio::any_completion_handler<void(error_code, std::vector<asio::ip::tcp::endpoint>)> done) {
    auto state = std::make_shared<ResolveBoundedState>();
    state->done = std::move(done);
    auto finish = [state](error_code ec, std::vector<asio::ip::tcp::endpoint> endpoints) {
        if (state->finished)
            return;
        state->finished = true;
        if (state->timer)
            state->timer->cancel();
        auto completion = std::move(state->done);
        std::move(completion)(ec, std::move(endpoints));
    };
    if (limit.count() > 0) {
        state->timer = std::make_shared<asio::steady_timer>(ex);
        state->timer->expires_after(limit);
        state->timer->async_wait([state, timeout_error](error_code ec) {
            if (ec || state->finished)
                return;
            state->finished = true;
            auto completion = std::move(state->done);
            std::move(completion)(timeout_error, std::vector<asio::ip::tcp::endpoint>{});
        });
    }
    async_resolve_endpoints(std::move(resolve), std::move(host), port, ex,
                            [finish](error_code ec, std::vector<asio::ip::tcp::endpoint> endpoints) mutable {
                                finish(ec, std::move(endpoints));
                            });
}

} // namespace simple_http
