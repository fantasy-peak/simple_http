#pragma once

// Handler type system.
//
// A route handler is a coroutine in one of two interchangeable forms, selected
// automatically at compile time by arity (the Request and Response are shared
// so a handler may hand them to coroutines that outlive it):
//
//   no TLS : awaitable<void>(std::shared_ptr<Request>,
//   std::shared_ptr<Response>) TLS    :
//   awaitable<void>(std::shared_ptr<Request>, std::shared_ptr<Response>,
//   SslHandle)
//
// Handlers are coroutines and may suspend freely (timers, async I/O,
// streaming). They run on the connection executor; the Response's write
// operations are awaitable and hop back onto that executor, so a Response
// captured by the handler is safe to use from any thread or across suspension
// points.

#include <boost/asio/awaitable.hpp>
#include <concepts>
#include <functional>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "../proto/request.h"
#include "../proto/response.h"
#include "../proto/websocket.h"
#include "../transport/transport.h" // SslHandle

namespace simple_http {

namespace asio = boost::asio;

using RequestPtr = std::shared_ptr<Request>;
using ResponsePtr = std::shared_ptr<ResponseWriter>;

using CoroHandler = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr)>;
using CoroSslHandler = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle)>;

using Handler = std::variant<CoroHandler, CoroSslHandler>;

// A WebSocket handler: runs after a successful upgrade, driving the connection
// with whole-message read/write until it closes. Both the Request and the
// WebSocket are shared so the handler can hand them to additional coroutines
// (e.g. a separate writer) that may outlive the handler body.
using WsHandler = std::function<asio::awaitable<void>(RequestPtr, std::shared_ptr<WebSocket>)>;

// The rest of a middleware chain: what a middleware invokes to hand the request
// onward. The terminal stage is the Router's own dispatch — route matching and
// the handler — so a middleware calling next(req, res, ssl) is exactly "wrap the
// handler". The Request and Response are shared_ptr, so a middleware keeps its
// own references after next() returns and can still observe/write the response.
using Next = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle)>;

// Middleware — the Go net/http / axum `from_fn` / tower shape, adapted to
// coroutines. A middleware wraps the rest of the chain:
//
//   server.use([](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next)
//                  -> asio::awaitable<void> {
//       auto t0 = steady_clock::now();        // before the handler
//       co_await next(std::move(req), std::move(res), ssl);  // run the chain
//       log(res->status());                   // after — the response is observable
//   });
//
// Code before `co_await next(...)` is the "before" phase; code after it resumes
// is the "after" phase. Not calling next() at all answers the request here and
// short-circuits the rest of the chain (auth, a CORS preflight). Middleware
// registered with Router::use run in registration order, outermost first; the
// CORS policy — Router::cors — is always the outermost of them.
using Middleware = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle, Next)>;

// Composes middlewares around a terminal stage: the first element runs
// outermost, the last runs just before `final`. `compose_middleware([a, b], h)`
// runs a(b(h(...))). Used by the Router to build its dispatch chain once; a
// consumer assembling its own Dispatcher (instead of a Router) can reuse it.
inline Next compose_middleware(std::vector<Middleware> middlewares, Next final) {
    for (auto it = middlewares.rbegin(); it != middlewares.rend(); ++it) {
        Middleware mw = std::move(*it);
        Next next = std::move(final);
        final = [mw = std::move(mw), next = std::move(next)](RequestPtr req, ResponsePtr res,
                                                             SslHandle ssl) -> asio::awaitable<void> {
            co_await mw(std::move(req), std::move(res), ssl, next);
        };
    }
    return final;
}

namespace detail {

template <typename F>
concept CoroSsl = requires(F f, RequestPtr rq, ResponsePtr rs, SslHandle s) {
    { f(rq, rs, s) } -> std::same_as<asio::awaitable<void>>;
};
template <typename F>
concept Coro = requires(F f, RequestPtr rq, ResponsePtr rs) {
    { f(rq, rs) } -> std::same_as<asio::awaitable<void>>;
};

} // namespace detail

// Wraps a handler coroutine into a Handler variant, chosen by arity. An
// already-built Handler passes through.
template <typename F> Handler make_handler(F &&fn) {
    using D = std::decay_t<F>;
    if constexpr (std::is_same_v<D, Handler>) {
        return std::forward<F>(fn);
    } else if constexpr (detail::CoroSsl<D>) {
        return Handler{std::in_place_type<CoroSslHandler>, std::forward<F>(fn)};
    } else if constexpr (detail::Coro<D>) {
        return Handler{std::in_place_type<CoroHandler>, std::forward<F>(fn)};
    } else {
        static_assert(sizeof(F) == 0, "handler must be a coroutine callable as "
                                      "(std::shared_ptr<Request>, std::shared_ptr<Response>) or "
                                      "(std::shared_ptr<Request>, std::shared_ptr<Response>, SslHandle) "
                                      "returning asio::awaitable<void>");
    }
}

// Invokes a Handler: both forms are coroutines and are awaited. The Request and
// Response are shared so a handler may hand them to coroutines that outlive it.
//
// A handler that throws becomes a 500. Without this the exception unwinds out
// of the engine's dispatch loop and the connection is dropped mid-request: the
// client sees a protocol error rather than a server error, and a keep-alive
// connection dies with it. (Found by driving the server with httpx, which
// reports it as "Server disconnected without sending a response".)
inline asio::awaitable<void> invoke_handler(const Handler &handler, RequestPtr req, ResponsePtr res, SslHandle ssl) {
    // Kept back because `res` is moved into the handler; both point at the same
    // Response, so this is the same object the handler was given.
    const ResponsePtr fallback = res;
    // The catch blocks only record — `co_await` is not permitted inside a
    // coroutine's handler, so the 500 is sent after the try has been left.
    bool threw = false;
    std::string why;
    try {
        if (handler.index() == 0) {
            co_await std::get<CoroHandler>(handler)(std::move(req), std::move(res));
        } else {
            co_await std::get<CoroSslHandler>(handler)(std::move(req), std::move(res), ssl);
        }
    } catch (const std::exception &e) {
        threw = true;
        why = e.what();
    } catch (...) {
        threw = true;
        why = "non-std exception";
    }

    if (threw) {
        SIMPLE_HTTP_ERROR_LOG("handler threw: {}", why);
        if (fallback) {
            // A handler that throws has usually not answered yet. If it already
            // did, the writer refuses the second send, which is the right outcome
            // — better than dropping the connection either way.
            co_await fallback->status(status::internal_server_error).send("");
        }
    }
    co_return;
}

} // namespace simple_http
