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
#include <utility>
#include <vector>

#include "../engine/dispatcher.h"

namespace simple_http {

using Next = CallbackHandler;

// The terminal stage is the Router's own dispatch (route matching and the
// handler), so a middleware calling next(...) is exactly "wrap the handler".
using Middleware = std::function<void(RequestPtr, ResponsePtr, SslHandle, Next, HandlerCompletion)>;

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
