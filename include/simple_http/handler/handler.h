#pragma once

// Handler type system (C++17 port).
//
// A route handler is one of the shapes accepted by make_handler in
// engine/dispatcher.h: a callback taking a completion handler, a plain
// synchronous function, or -- when the library is compiled as C++20 -- a
// coroutine. make_handler normalizes all of them to CallbackHandler.
//
// Middleware keeps the Go net/http / tower shape, expressed with completion
// handlers instead of coroutines:
//
//   server.use([](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next,
//                 HandlerCompletion done) {
//       auto t0 = steady_clock::now();              // before phase
//       next(std::move(req), std::move(res), ssl,   // run the rest of the chain
//            [t0, done = std::move(done), res]() mutable {
//                log(res->status());                 // after phase
//                std::move(done)();
//            });
//   });
//
// Code before calling next is the "before" phase; code in the completion
// handed to next is the "after" phase. Not calling next answers the request
// here and short-circuits the rest of the chain. Middlewares registered with
// Router::use run in registration order, outermost first.

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "../engine/dispatcher.h"

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
#include <boost/asio/compose.hpp>
#include <boost/asio/use_awaitable.hpp>
#endif

namespace simple_http {

using Next = CallbackHandler;

// The terminal stage is the Router's own dispatch (route matching and the
// handler), so a middleware calling next(...) is exactly "wrap the handler".
using Middleware = std::function<void(RequestPtr, ResponsePtr, SslHandle, Next, HandlerCompletion)>;

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
// Coroutine form: `co_await next(req, res, ssl)` instead of passing a
// completion handler. Only available when Boost.Asio provides co_await
// (C++20 builds); a C++17 build keeps the callback form above.
using CoroNext = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle)>;
using CoroMiddleware = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle, CoroNext)>;
#endif

namespace middleware_detail {

template <typename F, typename = void> struct is_callback_middleware : std::false_type {};
template <typename F>
struct is_callback_middleware<F, std::void_t<std::invoke_result_t<F &, RequestPtr, ResponsePtr, SslHandle, Next,
                                                                  HandlerCompletion>>> : std::true_type {};

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
template <typename F, typename = void> struct is_coro_middleware : std::false_type {};
template <typename F>
struct is_coro_middleware<F, std::void_t<std::invoke_result_t<F &, RequestPtr, ResponsePtr, SslHandle, CoroNext>>>
    : std::bool_constant<std::is_same_v<std::invoke_result_t<F &, RequestPtr, ResponsePtr, SslHandle, CoroNext>,
                                        asio::awaitable<void>>> {};

// A composed operation that runs the rest of the chain and completes when the
// type-erased Next does. `async_compose` makes the same implementation usable
// with use_awaitable (or any future token) and propagates the associated
// executor / allocator / cancellation slot exactly like an Asio operation.
struct AwaitNextOp {
    // Shared: async_compose may move/copy the composed operation, and a moved
    // std::function member would leave the op with an empty `next`.
    std::shared_ptr<Next> next;
    RequestPtr req;
    ResponsePtr res;
    SslHandle ssl;

    template <typename Self> void operator()(Self &self) {
        (*next)(std::move(req), std::move(res), ssl,
                HandlerCompletion([self = std::move(self)]() mutable { self.complete(); }));
    }
};

template <typename Token>
auto async_next(Next next, RequestPtr req, ResponsePtr res, SslHandle ssl, Token &&token) {
    return asio::async_compose<Token, void()>(
        AwaitNextOp{std::make_shared<Next>(std::move(next)), std::move(req), std::move(res), ssl},
        std::forward<Token>(token));
}

inline CoroNext make_coro_next(Next next) {
    return [next = std::move(next)](RequestPtr req, ResponsePtr res, SslHandle ssl) mutable -> asio::awaitable<void> {
        co_await async_next(std::move(next), std::move(req), std::move(res), ssl, asio::use_awaitable);
    };
}
#endif

} // namespace middleware_detail

// Accepts either form and normalizes to the callback-shaped Middleware:
//   - callback: (RequestPtr, ResponsePtr, SslHandle, Next, HandlerCompletion)
//   - coroutine (C++20): (RequestPtr, ResponsePtr, SslHandle, CoroNext) -> awaitable<void>
template <typename F> Middleware make_middleware(F &&fn) {
    using D = std::decay_t<F>;
    if constexpr (std::is_same_v<D, Middleware>) {
        return std::forward<F>(fn);
    } else if constexpr (middleware_detail::is_callback_middleware<D>::value) {
        return Middleware(std::forward<F>(fn));
    }
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    else if constexpr (middleware_detail::is_coro_middleware<D>::value) {
        return Middleware([fn = std::forward<F>(fn)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next,
                                                     HandlerCompletion done) mutable {
            auto coro_next = middleware_detail::make_coro_next(std::move(next));
            detail::execute_coroutine(req, res, std::move(done), [fn, req, res, ssl, coro_next]() mutable {
                return fn(std::move(req), std::move(res), ssl, coro_next);
            });
        });
    }
#endif
    else {
        static_assert(sizeof(F) == 0,
                      "middleware must be callable as (RequestPtr, ResponsePtr, SslHandle, Next, HandlerCompletion) "
                      "or (C++20) return asio::awaitable<void> from (RequestPtr, ResponsePtr, SslHandle, CoroNext)");
    }
}

// Variadic helper for group()/route()/ws_route(), which take a
// std::vector<Middleware>: mixed callback and coroutine elements are
// normalized here.
template <typename... MWs> std::vector<Middleware> make_middlewares(MWs &&...mws) {
    std::vector<Middleware> out;
    out.reserve(sizeof...(MWs));
    (out.push_back(make_middleware(std::forward<MWs>(mws))), ...);
    return out;
}

// Composes middlewares around a terminal stage: the first element runs
// outermost, the last runs just before `final`. `compose_middleware([a, b], h)`
// runs a(b(h(...))).
inline Next compose_middleware(std::vector<Middleware> middlewares, Next final_next) {
    for (auto it = middlewares.rbegin(); it != middlewares.rend(); ++it) {
        Middleware mw = std::move(*it);
        Next next = std::move(final_next);
        final_next = [mw = std::move(mw), next = std::move(next)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                                  HandlerCompletion done) mutable {
            mw(std::move(req), std::move(res), ssl, next, std::move(done));
        };
    }
    return final_next;
}

} // namespace simple_http
