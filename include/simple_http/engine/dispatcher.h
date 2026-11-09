#pragma once

// Dispatcher: the seam between an engine and the handler layer (C++17 port).
//
// This is the one header in the port allowed to mention coroutine types. The
// coroutine branch is compiled only when Boost.Asio provides co_await
// (BOOST_ASIO_HAS_CO_AWAIT, i.e. a C++20-or-later build); such a build gets
// automatic adaptation of coroutine handlers, so the same registration call
// works with either style:
//
//   server.route({Method::Get}, "/hello",
//       [](RequestPtr, ResponsePtr res, HandlerCompletion done) {   // callback
//           res->status(200);
//           // finish_with adapts an "I am done" handler to the error_code
//           // signature an I/O operation completes with.
//           res->async_send("hello", finish_with(std::move(done)));
//       });
//
//   server.route({Method::Get}, "/hello",
//       [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {  // C++20
//           res->status(200);
//           co_await res->async_send("hello", asio::use_awaitable);
//       });
//
// Both forms are normalized to CallbackHandler. The coroutine adapter spawns
// the handler on the request's owner executor with the request's cancellation
// slot bound, and turns an exception into the same 500/close behaviour the
// C++23 implementation had.

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "../core/http_status.h"
#include "../core/logging.h"
#include "../core/resolve.h"
#include "../proto/request.h"
#include "../proto/response.h"
#include "../proto/websocket.h"
#include "../transport/transport.h" // SslHandle

namespace simple_http {

namespace asio = boost::asio;

using RequestPtr = std::shared_ptr<Request>;
using ResponsePtr = std::shared_ptr<ResponseWriter>;

// Handler completion: what a handler calls once it has finished (successfully
// or not). It carries no error; a handler reports failures through the
// response, and exceptions are turned into 500s by the adapter below.
using HandlerCompletion = asio::any_completion_handler<void()>;

// The normalized callback shape. SslHandle is nullopt for a plaintext
// connection.
using CallbackHandler = std::function<void(RequestPtr, ResponsePtr, SslHandle, HandlerCompletion)>;

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
// The coroutine shape, available only when the translation unit is compiled
// with coroutine support.
using CoroHandler = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle)>;
using Handler = std::variant<CallbackHandler, CoroHandler>;
#else
using Handler = CallbackHandler;
#endif

// The engine invokes the dispatcher with the request's completion handler.
using Dispatcher = CallbackHandler;

// Raw WebSocket route handlers and the upgrade seam.
using CallbackWsHandler = std::function<void(RequestPtr, std::shared_ptr<WebSocket>, HandlerCompletion)>;
using WsUpgradeCompletion = asio::any_completion_handler<void(error_code, bool)>;
using WsUpgrade = std::function<void(RequestPtr, ResponsePtr, CallbackWsHandler, WsUpgradeCompletion)>;
using WsHandlerFn = std::function<void(RequestPtr, ResponsePtr, SslHandle, WsUpgrade, WsUpgradeCompletion)>;
using WsLookup = std::function<std::optional<WsHandlerFn>(std::string_view path)>;

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
using CoroWsHandler = std::function<asio::awaitable<void>(RequestPtr, std::shared_ptr<WebSocket>)>;
using WsHandler = std::variant<CallbackWsHandler, CoroWsHandler>;
#else
using WsHandler = CallbackWsHandler;
#endif

namespace detail {

// Detection idiom instead of std::invoke_result_t: the latter is a hard error
// for a callable that is not invocable with the probed arguments, and
// make_handler probes every shape.
template <typename F, typename = void> struct result_ssl {
    static constexpr bool valid = false;
    static constexpr bool is_void = false;
    static constexpr bool is_coro = false;
};
template <typename F> struct result_ssl<F, std::void_t<std::invoke_result_t<F &, RequestPtr, ResponsePtr, SslHandle>>> {
    using type = std::invoke_result_t<F &, RequestPtr, ResponsePtr, SslHandle>;
    static constexpr bool valid = true;
    static constexpr bool is_void = std::is_same_v<type, void>;
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    static constexpr bool is_coro = std::is_same_v<type, asio::awaitable<void>>;
#else
    static constexpr bool is_coro = false;
#endif
};

template <typename F, typename = void> struct result_two {
    static constexpr bool valid = false;
    static constexpr bool is_void = false;
    static constexpr bool is_coro = false;
};
template <typename F> struct result_two<F, std::void_t<std::invoke_result_t<F &, RequestPtr, ResponsePtr>>> {
    using type = std::invoke_result_t<F &, RequestPtr, ResponsePtr>;
    static constexpr bool valid = true;
    static constexpr bool is_void = std::is_same_v<type, void>;
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    static constexpr bool is_coro = std::is_same_v<type, asio::awaitable<void>>;
#else
    static constexpr bool is_coro = false;
#endif
};

template <typename F>
constexpr bool is_callback_handler_ssl =
    std::is_invocable_v<F &, RequestPtr, ResponsePtr, SslHandle, HandlerCompletion>;
template <typename F>
constexpr bool is_callback_handler = std::is_invocable_v<F &, RequestPtr, ResponsePtr, HandlerCompletion>;

template <typename F> constexpr bool is_sync_handler_ssl = result_ssl<F>::valid && result_ssl<F>::is_void;
template <typename F> constexpr bool is_sync_handler = result_two<F>::valid && result_two<F>::is_void;
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
template <typename F> constexpr bool is_coro_handler_ssl = result_ssl<F>::valid && result_ssl<F>::is_coro;
template <typename F> constexpr bool is_coro_handler = result_two<F>::valid && result_two<F>::is_coro;
#endif

template <typename F, typename = void> struct ws_result {
    static constexpr bool valid = false;
    static constexpr bool is_coro = false;
};
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
template <typename F>
struct ws_result<F, std::void_t<std::invoke_result_t<F &, RequestPtr, std::shared_ptr<WebSocket>>>> {
    using type = std::invoke_result_t<F &, RequestPtr, std::shared_ptr<WebSocket>>;
    static constexpr bool valid = true;
    static constexpr bool is_coro = std::is_same_v<type, asio::awaitable<void>>;
};
#endif

// Wraps any accepted handler shape into the callback form.
template <typename F> CallbackHandler make_callback_handler(F &&fn) {
    using D = std::decay_t<F>;
    if constexpr (is_callback_handler_ssl<D>) {
        return CallbackHandler(std::forward<F>(fn));
    } else if constexpr (is_callback_handler<D>) {
        return CallbackHandler(
            [fn = std::forward<F>(fn)](RequestPtr req, ResponsePtr res, SslHandle, HandlerCompletion done) mutable {
                fn(std::move(req), std::move(res), std::move(done));
            });
    } else if constexpr (is_sync_handler_ssl<D>) {
        return CallbackHandler(
            [fn = std::forward<F>(fn)](RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion done) mutable {
                fn(std::move(req), std::move(res), ssl);
                std::move(done)();
            });
    } else if constexpr (is_sync_handler<D>) {
        return CallbackHandler(
            [fn = std::forward<F>(fn)](RequestPtr req, ResponsePtr res, SslHandle, HandlerCompletion done) mutable {
                fn(std::move(req), std::move(res));
                std::move(done)();
            });
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    } else if constexpr (is_coro_handler_ssl<D>) {
        return CallbackHandler(
            [fn = std::forward<F>(fn)](RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion done) mutable {
                execute_coroutine(req, res, std::move(done), [fn, req, res, ssl]() mutable {
                    return fn(std::move(req), std::move(res), ssl);
                });
            });
    } else if constexpr (is_coro_handler<D>) {
        return CallbackHandler([fn = std::forward<F>(fn)](RequestPtr req, ResponsePtr res, SslHandle ssl,
                                                          HandlerCompletion done) mutable {
            (void)ssl;
            execute_coroutine(req, res, std::move(done),
                              [fn, req, res]() mutable { return fn(std::move(req), std::move(res)); });
        });
#endif
    } else {
        static_assert(sizeof(F) == 0,
                      "handler must be callable as (RequestPtr, ResponsePtr[, SslHandle], HandlerCompletion) "
                      "or return void / asio::awaitable<void> from (RequestPtr, ResponsePtr[, SslHandle])");
    }
}

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
// Detection traits and execution helper for handlers whose extracted values are
// prepended to (RequestPtr, ResponsePtr[, SslHandle]). They live here, next to
// the rest of the coroutine adapter, so that extract.h -- like every other
// header -- never has to name a coroutine type.
template <typename F, typename Tuple, typename = void> struct extracted_coro_ssl : std::false_type {};
template <typename F, typename... Vs>
struct extracted_coro_ssl<
    F, std::tuple<Vs...>,
    std::enable_if_t<
        std::is_same_v<std::invoke_result_t<F &, Vs..., RequestPtr, ResponsePtr, SslHandle>, asio::awaitable<void>>>>
    : std::true_type {};

template <typename F, typename Tuple, typename = void> struct extracted_coro : std::false_type {};
template <typename F, typename... Vs>
struct extracted_coro<
    F, std::tuple<Vs...>,
    std::enable_if_t<std::is_same_v<std::invoke_result_t<F &, Vs..., RequestPtr, ResponsePtr>, asio::awaitable<void>>>>
    : std::true_type {};

// Spawns a coroutine handler on the request's owner executor with the request's
// cancellation slot bound, and applies the historical exception policy: a
// cancelled request is left alone; an unsent response becomes a 500; a started
// response is closed instead of splicing a second status line onto the wire.
template <typename F>
void execute_coroutine(const RequestPtr &req, const ResponsePtr &res, HandlerCompletion done, F &&fn) {
    const RequestPtr req_keep = req;
    const ResponsePtr fallback = res;
    auto executor = req_keep ? req_keep->executor() : asio::any_io_executor{};
    auto token = asio::bind_cancellation_slot(
        req_keep ? req_keep->cancellation_slot() : asio::cancellation_slot{},
        [done = std::move(done), req_keep, fallback](std::exception_ptr ep) mutable {
            if (!ep) {
                std::move(done)();
                return;
            }
            if (req_keep && req_keep->cancelled()) {
                std::move(done)();
                return;
            }
            SIMPLE_HTTP_ERROR_LOG("handler threw: {}", "exception");
            if (!fallback) {
                std::move(done)();
                return;
            }
            if (fallback->started()) {
                fallback->async_close([done = std::move(done)](error_code) mutable { std::move(done)(); });
            } else {
                fallback->status(status::internal_server_error);
                fallback->async_send("", [done = std::move(done)](error_code) mutable { std::move(done)(); });
            }
        });
    asio::co_spawn(executor, std::forward<F>(fn), std::move(token));
}

template <typename F, typename Tuple>
void execute_extracted_coroutine(const RequestPtr &req, const ResponsePtr &res, HandlerCompletion done,
                                 std::shared_ptr<F> fn, Tuple args) {
    execute_coroutine(req, res, std::move(done),
                      [fn, args = std::move(args)]() mutable { return std::apply(*fn, std::move(args)); });
}

#endif

} // namespace detail

// Normalizes any accepted handler callable (already-built Handler, callback,
// synchronous function, or -- in a C++20 build -- a coroutine) into a Handler.
template <typename F> Handler make_handler(F &&fn) {
    using D = std::decay_t<F>;
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    if constexpr (std::is_same_v<D, Handler>) {
        return std::forward<F>(fn);
    } else if constexpr (std::is_same_v<D, CoroHandler>) {
        return Handler{std::forward<F>(fn)};
    } else {
        return Handler{detail::make_callback_handler(std::forward<F>(fn))};
    }
#else
    if constexpr (std::is_same_v<D, Handler>) {
        return std::forward<F>(fn);
    } else {
        return detail::make_callback_handler(std::forward<F>(fn));
    }
#endif
}

// Runs a Handler and always completes `done` (including on exceptions from a
// callback handler).
inline void invoke_handler(const Handler &handler, RequestPtr req, ResponsePtr res, SslHandle ssl,
                           HandlerCompletion done) {
    const RequestPtr req_keep = req;
    const ResponsePtr fallback = res;
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    if (std::holds_alternative<CoroHandler>(handler)) {
        const CoroHandler &fn = std::get<CoroHandler>(handler);
        detail::execute_coroutine(req, res, std::move(done),
                                  [fn, req, res, ssl]() mutable { return fn(std::move(req), std::move(res), ssl); });
        return;
    }
#endif
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    const CallbackHandler &fn = std::get<0>(handler);
#else
    const CallbackHandler &fn = handler;
#endif
    // HandlerCompletion is move-only, and it is moved into the handler below.
    // Keep the owner here behind a one-shot wrapper so the exception path can
    // still complete: otherwise a synchronous throw destroys the only copy and
    // the engine never runs after_handler/handler_finished (h1 stalls, h2/h3
    // leak a concurrency slot).
    auto completion = std::make_shared<HandlerCompletion>(std::move(done));
    auto completed = std::make_shared<bool>(false);
    auto finish = [completion, completed]() {
        if (*completed) {
            return;
        }
        *completed = true;
        auto callback = std::move(*completion);
        if (callback) {
            std::move(callback)();
        }
    };
    HandlerCompletion handed([finish]() mutable { finish(); });
    try {
        fn(std::move(req), std::move(res), ssl, std::move(handed));
    } catch (...) {
        if (req_keep && req_keep->cancelled()) {
            finish();
            return;
        }
        SIMPLE_HTTP_ERROR_LOG("handler threw synchronously");
        if (!fallback) {
            finish();
            return;
        }
        if (fallback->started()) {
            fallback->async_close([finish](error_code) mutable { finish(); });
        } else {
            fallback->status(status::internal_server_error);
            fallback->async_send("", [finish](error_code) mutable { finish(); });
        }
    }
}

// --- WebSocket handler normalization ---

template <typename F> WsHandler make_ws_handler(F &&fn) {
    using D = std::decay_t<F>;
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    if constexpr (std::is_same_v<D, WsHandler>) {
        return std::forward<F>(fn);
    } else if constexpr (detail::ws_result<D>::valid && detail::ws_result<D>::is_coro) {
        return WsHandler(CoroWsHandler(std::forward<F>(fn)));
    } else {
        return WsHandler(CallbackWsHandler(std::forward<F>(fn)));
    }
#else
    if constexpr (std::is_same_v<D, WsHandler>) {
        return std::forward<F>(fn);
    } else {
        return WsHandler(std::forward<F>(fn));
    }
#endif
}

// Runs a raw WebSocket handler and completes `done` when it finishes.
inline void invoke_ws_handler(const WsHandler &handler, RequestPtr req, std::shared_ptr<WebSocket> ws,
                              HandlerCompletion done) {
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    if (std::holds_alternative<CoroWsHandler>(handler)) {
        const CoroWsHandler &fn = std::get<CoroWsHandler>(handler);
        auto executor = req ? req->executor() : asio::any_io_executor{};
        asio::co_spawn(
            executor, [fn, req, ws]() mutable { return fn(std::move(req), std::move(ws)); },
            asio::bind_cancellation_slot(req ? req->cancellation_slot() : asio::cancellation_slot{},
                                         [done = std::move(done)](std::exception_ptr) mutable { std::move(done)(); }));
        return;
    }
#endif
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    const CallbackWsHandler &fn = std::get<0>(handler);
#else
    const CallbackWsHandler &fn = handler;
#endif
    // Same one-shot wrapper as invoke_handler: a synchronous throw must still
    // complete the upgrade/request, or the engine waits forever.
    auto completion = std::make_shared<HandlerCompletion>(std::move(done));
    auto completed = std::make_shared<bool>(false);
    auto finish = [completion, completed]() {
        if (*completed) {
            return;
        }
        *completed = true;
        auto callback = std::move(*completion);
        if (callback) {
            std::move(callback)();
        }
    };
    HandlerCompletion handed([finish]() mutable { finish(); });
    try {
        fn(std::move(req), std::move(ws), std::move(handed));
    } catch (...) {
        SIMPLE_HTTP_ERROR_LOG("websocket handler threw synchronously");
        finish();
    }
}

// Adapts a HandlerCompletion (void()) to the void(error_code) completion an I/O
// operation produces. The error is ignored deliberately: the handler's contract
// is "I am done"; a write failure already closed the connection.
inline WriteCompletion finish_with(HandlerCompletion done) {
    return WriteCompletion([done = std::move(done)](error_code) mutable { std::move(done)(); });
}

// Adapts the stored WsHandler variant to the callback form the engine's upgrade
// terminal invokes after the handshake.
inline CallbackWsHandler to_callback_ws_handler(WsHandler handler) {
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    if (std::holds_alternative<CoroWsHandler>(handler)) {
        CoroWsHandler fn = std::get<CoroWsHandler>(std::move(handler));
        return [fn = std::move(fn)](RequestPtr req, std::shared_ptr<WebSocket> ws, HandlerCompletion done) mutable {
            auto executor = req ? req->executor() : asio::any_io_executor{};
            asio::co_spawn(
                executor, [fn, req, ws]() mutable { return fn(std::move(req), std::move(ws)); },
                asio::bind_cancellation_slot(
                    req ? req->cancellation_slot() : asio::cancellation_slot{},
                    [done = std::move(done)](std::exception_ptr) mutable { std::move(done)(); }));
        };
    }
    return std::get<0>(handler);
#else
    return handler;
#endif
}

// --- proxy lookup seams (unchanged in shape from the C++23 version) ---

struct WsProxyTarget {
    std::string host;
    std::uint16_t port{0};
    std::string rewrite_path;
    bool preserve_host{true};
    std::string host_header;
    bool preserve_inbound_forwarding{false};
    std::chrono::milliseconds connect_timeout{10000};
    ResolveFn resolve;
    bool tcp_nodelay{true};
};

using WsProxyLookup = std::function<std::optional<WsProxyTarget>(std::string_view path)>;

struct HttpProxyHooks {
    std::function<void(const Request &in, Request &out)> rewrite;
    std::function<error_code(int &status, Headers &headers, const Request &in)> modify_response;
    std::function<void(std::shared_ptr<ResponseWriter>, const Request &, error_code, HandlerCompletion)> error_handler;
};

struct HttpProxyTarget {
    std::string host;
    std::uint16_t port{0};
    std::string rewrite_path;
    bool tls{false};
    bool h2c{false};
    bool h2c_prior_knowledge{false};
    bool preserve_host{false};
    std::string host_header;
    bool preserve_inbound_forwarding{false};
    std::size_t max_attempts{3};
    HttpProxyHooks hooks;
    bool use_env_proxy{false};
};

using HttpProxyLookup = std::function<std::optional<HttpProxyTarget>(std::string_view path)>;

} // namespace simple_http
