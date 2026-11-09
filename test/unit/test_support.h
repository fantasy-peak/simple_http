#pragma once

// Shared test doubles and helpers for the unit tests.
//
// Everything here is in-memory: no sockets, no threads, and no global state
// left behind. Coroutine-based APIs are driven through run()/run_on(), which
// pump a private io_context with a deadline, so a hung coroutine fails a test
// instead of hanging the suite.

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <expected>
#include <optional>
#include <tuple>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "simple_http.h"

namespace simple_http::test {

namespace asio = boost::asio;

// --- running awaitables ------------------------------------------------------

// Runs an awaitable on `ctx`, giving up after `timeout`. Returns nullopt when
// it did not finish in time (which a test asserts against, rather than
// hanging).
template <typename T>
std::optional<T> run_on(asio::io_context &ctx, asio::awaitable<T> op,
                        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    // A context that ran out of work is left stopped; each pump needs a restart
    // (so a test can await several coroutines on one context).
    ctx.restart();
    std::optional<T> result;
    asio::co_spawn(ctx, std::move(op), [&result](const std::exception_ptr &ep, T value) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "run_on: coroutine threw: %s\n",
                         e.what()); // visible in the report
            return;
        } catch (...) {
            std::fprintf(stderr, "run_on: coroutine threw an unknown exception\n");
            return;
        }
        result = std::move(value);
    });
    ctx.run_for(timeout);
    return result;
}

// Same, for a void awaitable: returns whether it completed.
inline bool run_on(asio::io_context &ctx, asio::awaitable<void> op,
                   std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    ctx.restart();
    bool done = false;
    asio::co_spawn(ctx, std::move(op), [&done](const std::exception_ptr &ep) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "run_on: coroutine threw: %s\n", e.what());
            return;
        } catch (...) {
            std::fprintf(stderr, "run_on: coroutine threw an unknown exception\n");
            return;
        }
        done = true;
    });
    ctx.run_for(timeout);
    return done;
}

// Runs an awaitable to completion while pumping `ctx`, which may also hold
// long-lived work (the WebSocket write pump parks on a channel forever, so a
// plain run_for would sit out the whole timeout). Returns as soon as the
// coroutine finishes; the deadline bounds the wait.
template <typename T>
std::optional<T> run_until(asio::io_context &ctx, asio::awaitable<T> op,
                           std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::optional<T> result;
    bool finished = false;
    asio::co_spawn(ctx, std::move(op), [&](const std::exception_ptr &ep, T value) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "run_until: coroutine threw: %s\n", e.what());
            finished = true;
            return;
        }
        result = std::move(value);
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        ctx.restart();
        ctx.run_for(std::chrono::milliseconds(5));
    }
    return result;
}

inline bool run_until(asio::io_context &ctx, asio::awaitable<void> op,
                      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    bool finished = false;
    asio::co_spawn(ctx, std::move(op), [&finished](const std::exception_ptr &ep) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "run_until: coroutine threw: %s\n", e.what());
        }
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        ctx.restart();
        ctx.run_for(std::chrono::milliseconds(5));
    }
    return finished;
}

// Pumps the context for a bounded slice, letting detached work (a write pump)
// make progress without waiting on it to finish.
inline void drain(asio::io_context &ctx, std::chrono::milliseconds slice = std::chrono::milliseconds(50)) {
    ctx.restart();
    ctx.run_for(slice);
}

// Runs an awaitable on a private io_context (for tests that need no other I/O).
template <typename T>
std::optional<T> run(asio::awaitable<T> op, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    asio::io_context ctx;
    return run_on(ctx, std::move(op), timeout);
}

inline bool run(asio::awaitable<void> op, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    asio::io_context ctx;
    return run_on(ctx, std::move(op), timeout);
}

// Turns an awaitable-producing lambda into a value (Catch2 macros cannot be
// used inside a coroutine, so tests do their awaiting in `run(...)` and assert
// after).
template <typename F>
concept AwaitableFactory = requires(F f) { f(); };

// --- driving callback seams from a coroutine test ----------------------------

// Router::dispatch and middleware Next are callback-based in the token port.
// These wrap them as awaitables so a test can keep `run_on(ctx, ...)`.
inline asio::awaitable<void> await_dispatch(const Router &router, RequestPtr req, ResponsePtr res,
                                            SslHandle ssl = std::nullopt) {
    auto token = asio::use_awaitable;
    co_await asio::async_initiate<decltype(token), void()>(
        [&](auto handler) {
            router.dispatch(std::move(req), std::move(res), ssl,
                            [handler = std::move(handler)]() mutable { std::move(handler)(); });
        },
        token);
}

inline asio::awaitable<void> await_next(const Next &next, RequestPtr req, ResponsePtr res,
                                        SslHandle ssl = std::nullopt) {
    auto token = asio::use_awaitable;
    co_await asio::async_initiate<decltype(token), void()>(
        [&](auto handler) {
            next(std::move(req), std::move(res), ssl,
                 [handler = std::move(handler)]() mutable { std::move(handler)(); });
        },
        token);
}


// --- expected/value shims for the token API --------------------------------
//
// The token port completed with void(error_code[, T]) signatures. The tests
// were written against awaitables returning std::expected, so these thin
// coroutine adapters let a call site keep its value-or-error shape without
// reintroducing a library-level compatibility layer.

template <typename T>
asio::awaitable<std::expected<T, error_code>> as_expected(asio::awaitable<std::tuple<error_code, T>> op) {
    auto [ec, value] = co_await std::move(op);
    if (ec)
        co_return std::unexpected{ec};
    co_return std::move(value);
}

inline asio::awaitable<std::expected<void, error_code>>
as_expected_void(asio::awaitable<std::tuple<error_code>> op) {
    auto [ec] = co_await std::move(op);
    if (ec)
        co_return std::unexpected{ec};
    co_return std::expected<void, error_code>{};
}

inline asio::awaitable<error_code> as_ec(asio::awaitable<std::tuple<error_code>> op) {
    auto [ec] = co_await std::move(op);
    co_return ec;
}

inline asio::awaitable<std::expected<std::string, error_code>> body_read_all(Body &body) {
    return as_expected(body.async_read_all(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<std::expected<ReadResult, error_code>> body_read(Body &body) {
    return as_expected(body.async_read(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<std::expected<WsMessage, error_code>> ws_read(WebSocket &ws) {
    return as_expected(ws.async_read(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_write(WebSocket &ws, std::string data, bool text) {
    return as_ec(ws.async_write(std::move(data), text, asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_write_view(WebSocket &ws, std::string_view data, bool text) {
    return as_ec(ws.async_write_view(data, text, asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_write_text(WebSocket &ws, std::string data) {
    return as_ec(ws.async_write_text(std::move(data), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_write_binary(WebSocket &ws, std::string data) {
    return as_ec(ws.async_write_binary(std::move(data), asio::as_tuple(asio::use_awaitable)));
}

// Starts an engine's run() loop and completes when the engine reports done.
template <typename Engine, typename HandlerFn>
asio::awaitable<void> run_engine(Engine &engine, HandlerFn &&fn, std::string initial = {}) {
    auto token = asio::use_awaitable;
    auto dispatcher = detail::make_callback_handler(std::forward<HandlerFn>(fn));
    co_await asio::async_initiate<decltype(token), void()>(
        [&](auto handler) {
            // engine.run's on_done is a std::function (copyable); the completion
            // handler is move-only, so keep it behind a shared_ptr.
            auto completion =
                std::make_shared<asio::any_completion_handler<void()>>(std::move(handler));
            engine.run(std::move(dispatcher), std::move(initial), {}, {}, {},
                       [completion]() mutable { std::move(*completion)(); });
        },
        token);
}


// Some library seams are type-erased completion handlers rather than token
// templates (StaticFiles::try_serve, serve_not_found). These bridge them back
// to awaitables for the tests.
template <typename Start> asio::awaitable<bool> await_bool(Start &&start) {
    auto token = asio::use_awaitable;
    co_return co_await asio::async_initiate<decltype(token), void(bool)>(
        [&](auto handler) { start(std::move(handler)); }, token);
}

template <typename Start> asio::awaitable<void> await_void(Start &&start) {
    auto token = asio::use_awaitable;
    co_await asio::async_initiate<decltype(token), void()>(
        [&](auto handler) { start(std::move(handler)); }, token);
}


template <typename Start> asio::awaitable<bool> await_connected(Start &&start) {
    auto token = asio::use_awaitable;
    co_return co_await asio::async_initiate<decltype(token), void(error_code, bool)>(
        [&](auto handler) { start(std::move(handler)); }, token);
}

template <typename Start> asio::awaitable<error_code> await_error(Start &&start) {
    auto token = asio::as_tuple(asio::use_awaitable);
    auto result = co_await asio::async_initiate<decltype(token), void(error_code)>(
        [&](auto handler) { start(std::move(handler)); }, token);
    co_return std::get<0>(result);
}


inline asio::awaitable<error_code> ws_ping(WebSocket &ws, std::string payload) {
    return as_ec(ws.async_ping(std::move(payload), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_close(WebSocket &ws) {
    return as_ec(ws.async_close(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_close(WebSocket &ws, std::uint16_t code, std::string reason) {
    return as_ec(ws.async_close(code, std::move(reason), asio::as_tuple(asio::use_awaitable)));
}


inline asio::awaitable<error_code> ws_enable_compression(WebSocket &ws, bool enable) {
    return as_ec(ws.async_enable_write_compression(enable, asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_set_compression_level(WebSocket &ws, int level) {
    return as_ec(ws.async_set_compression_level(level, asio::as_tuple(asio::use_awaitable)));
}


// --- coroutine middleware ------------------------------------------------
//
// Middleware is callback-shaped in the token port. The unit tests express the
// same before/after logic as coroutines, so this test-side adapter bridges the
// two: `next` is awaitable again, and the coroutine is spawned with the
// request's cancellation slot. Exceptions use the handler policy (500 if the
// response has not started, close otherwise).

class CoroutineNext {
  public:
    explicit CoroutineNext(Next next) : m_next(std::move(next)) {}

    asio::awaitable<void> operator()(RequestPtr req, ResponsePtr res, SslHandle ssl) const {
        auto token = asio::use_awaitable;
        co_await asio::async_initiate<decltype(token), void()>(
            [&](auto handler) {
                m_next(std::move(req), std::move(res), ssl,
                       [handler = std::move(handler)]() mutable { std::move(handler)(); });
            },
            token);
    }

  private:
    Next m_next;
};

template <typename F> Middleware coro_middleware(F fn) {
    return [fn = std::move(fn)](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next,
                                HandlerCompletion done) {
        auto executor = req ? req->executor() : asio::any_io_executor{};
        auto req_keep = req;
        auto res_keep = res;
        asio::co_spawn(
            executor,
            [fn = std::move(fn), req = std::move(req), res = std::move(res), ssl,
             next = std::move(next)]() mutable -> asio::awaitable<void> {
                co_await fn(std::move(req), std::move(res), ssl, CoroutineNext(std::move(next)));
            },
            asio::bind_cancellation_slot(
                req_keep ? req_keep->cancellation_slot() : asio::cancellation_slot{},
                [done = std::move(done), req_keep, res_keep](std::exception_ptr ep) mutable {
                    if (!ep) {
                        std::move(done)();
                        return;
                    }
                    if (req_keep && req_keep->cancelled()) {
                        std::move(done)();
                        return;
                    }
                    if (!res_keep) {
                        std::move(done)();
                        return;
                    }
                    if (res_keep->started()) {
                        res_keep->async_close([done = std::move(done)](error_code) mutable { std::move(done)(); });
                    } else {
                        res_keep->status(status::internal_server_error);
                        res_keep->async_send("", [done = std::move(done)](error_code) mutable { std::move(done)(); });
                    }
                }));
    };
}

// --- a scripted byte stream --------------------------------------------------

// Satisfies the transport concept with preloaded input and recorded output, so
// the WebSocket backend (and anything else driven by a transport) can be tested
// without a socket. Reads hand out the queued chunks in order and report EOF
// once they run out; writes are appended to `written`.
// Wraps an initiation so it exposes its executor, which cancellation tokens
// such as asio::cancel_after require from any non-Asio initiation.
template <typename F> struct ExecInit {
    using executor_type = asio::any_io_executor;
    F fn;
    asio::any_io_executor ex;
    executor_type get_executor() const noexcept { return ex; }
    template <typename Handler> void operator()(Handler &&handler) { fn(std::forward<Handler>(handler)); }
};

class MockTransport {
  public:
    explicit MockTransport(asio::any_io_executor ex) : m_executor(std::move(ex)) {}

    // Queues bytes for the next reads.
    void push(std::string bytes) { m_in.push_back(std::move(bytes)); }

    // Keep the peer "open" after the queued bytes run out: reads wait instead
    // of reporting EOF. Lets a test hold a connection alive while a handler
    // drains a paused body; release() ends the wait and the next read reports
    // EOF.
    void hold_open() { m_hold_open = true; }
    void release() { m_hold_open = false; }

    // Bytes still queued for reading (not yet handed out by async_read_some).
    // Lets a test observe that a bounded consumer stopped pulling from the
    // transport instead of buffering everything.
    std::size_t pending() const {
        std::size_t bytes = 0;
        for (const auto &chunk : m_in) {
            bytes += chunk.size();
        }
        return bytes;
    }

    // Everything written so far, concatenated, and chunk by chunk.
    const std::string &written() const { return m_written; }

    const std::vector<std::string> &writes() const { return m_writes; }

    bool closed() const { return m_closed; }

    template <typename Token> auto async_read_some(ByteSpan buffer, Token &&token) {
        auto initiation = [this, buffer](auto handler) {
            // Honour the caller's cancellation slot the way a real transport
            // does: co_spawn binds it to the coroutine, so a per-operation
            // deadline can actually unblock a parked read.
            auto slot = asio::get_associated_cancellation_slot(handler);
            auto completion = [handler = std::move(handler)](std::exception_ptr ep, IoResult r) mutable {
                if (ep) {
                    std::move(handler)(make_error_code(asio::error::operation_aborted), 0);
                    return;
                }
                std::move(handler)(r.first, r.second);
            };
            asio::co_spawn(m_executor, do_read_some(buffer),
                           asio::bind_cancellation_slot(slot, std::move(completion)));
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            ExecInit<decltype(initiation)>{std::move(initiation), m_executor}, token);
    }

    template <typename Token> auto async_read(ByteSpan buffer, Token &&token) {
        auto initiation = [this, buffer](auto handler) {
            auto slot = asio::get_associated_cancellation_slot(handler);
            auto completion = [handler = std::move(handler)](std::exception_ptr ep, IoResult r) mutable {
                if (ep) {
                    std::move(handler)(make_error_code(asio::error::operation_aborted), 0);
                    return;
                }
                std::move(handler)(r.first, r.second);
            };
            asio::co_spawn(m_executor, do_read(buffer),
                           asio::bind_cancellation_slot(slot, std::move(completion)));
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            ExecInit<decltype(initiation)>{std::move(initiation), m_executor}, token);
    }

    template <typename Token> auto async_write(ConstByteSpan buffer, Token &&token) {
        auto initiation = [this, buffer](auto handler) {
            asio::co_spawn(m_executor, do_write(buffer),
                           [handler = std::move(handler)](std::exception_ptr ep, IoResult r) mutable {
                               if (ep) {
                                   std::move(handler)(make_error_code(asio::error::operation_aborted), 0);
                                   return;
                               }
                               std::move(handler)(r.first, r.second);
                           });
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            ExecInit<decltype(initiation)>{std::move(initiation), m_executor}, token);
    }

    template <typename Token> auto async_write_seq(boost::span<const ConstByteSpan> buffers, Token &&token) {
        auto initiation = [this, buffers](auto handler) {
            asio::co_spawn(m_executor, do_write_seq(buffers),
                           [handler = std::move(handler)](std::exception_ptr ep, IoResult r) mutable {
                               if (ep) {
                                   std::move(handler)(make_error_code(asio::error::operation_aborted), 0);
                                   return;
                               }
                               std::move(handler)(r.first, r.second);
                           });
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            ExecInit<decltype(initiation)>{std::move(initiation), m_executor}, token);
    }

    auto get_executor() { return m_executor; }

    asio::ip::tcp::endpoint peer() const { return {}; }

    SslHandle tls_handle() const { return std::nullopt; }

    template <typename Token> auto async_shutdown(Token &&token) {
        auto initiation = [](auto handler) { std::move(handler)(error_code{}); };
        return asio::async_initiate<Token, void(error_code)>(
            ExecInit<decltype(initiation)>{std::move(initiation), m_executor}, token);
    }
    void close() { m_closed = true; }

  private:
    asio::awaitable<IoResult> do_read_some(ByteSpan buffer) {
        while (m_in.empty() && m_hold_open) {
            asio::steady_timer timer{m_executor, std::chrono::milliseconds(1)};
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        }
        if (m_in.empty())
            co_return IoResult{make_error_code(asio::error::eof), 0};
        std::string &front = m_in.front();
        const std::size_t take = std::min(front.size(), buffer.size());
        std::memcpy(buffer.data(), front.data(), take);
        front.erase(0, take);
        if (front.empty())
            m_in.erase(m_in.begin());
        co_return IoResult{error_code{}, take};
    }

    asio::awaitable<IoResult> do_read(ByteSpan buffer) {
        std::size_t got = 0;
        while (got < buffer.size()) {
            auto [ec, n] = co_await do_read_some(buffer.subspan(got));
            got += n;
            if (ec)
                co_return IoResult{ec, got};
        }
        co_return IoResult{error_code{}, got};
    }

    asio::awaitable<IoResult> do_write(ConstByteSpan buffer) {
        m_writes.emplace_back(reinterpret_cast<const char *>(buffer.data()), buffer.size());
        m_written.append(m_writes.back());
        co_return IoResult{error_code{}, buffer.size()};
    }

    asio::awaitable<IoResult> do_write_seq(boost::span<const ConstByteSpan> buffers) {
        std::string chunk;
        for (const auto &buffer : buffers) {
            chunk.append(reinterpret_cast<const char *>(buffer.data()), buffer.size());
        }
        m_writes.push_back(chunk);
        m_written.append(chunk);
        co_return IoResult{error_code{}, chunk.size()};
    }

  private:
    asio::any_io_executor m_executor;
    std::vector<std::string> m_in;
    std::vector<std::string> m_writes;
    std::string m_written;
    bool m_hold_open{false};
    bool m_closed{false};
};

// --- a response writer that records instead of writing -----------------------

// Implements the ResponseWriter seam so Response (and anything that replies)
// can be exercised without a connection. Records the last call's arguments.
class FakeResponseSink : public ResponseSink {
  public:
    explicit FakeResponseSink(asio::any_io_executor executor = asio::system_executor{})
        : m_executor(std::move(executor)) {}

    void send(int status, Headers headers, std::string body, WriteCompletion done) override {
        ++calls;
        last_status = status;
        last_headers = std::move(headers);
        last_body = std::move(body);
        sent_bodyless = false;
        std::move(done)(error_code{});
    }

    void send_headers(int status, Headers headers, WriteCompletion done) override {
        ++calls;
        last_status = status;
        last_headers = std::move(headers);
        begun = true;
        std::move(done)(error_code{});
    }

    void send_chunk(std::string data, WriteCompletion done) override {
        chunks.push_back(std::move(data));
        std::move(done)(error_code{});
    }

    void send_last(std::string data, WriteCompletion done) override {
        chunks.push_back(std::move(data));
        finished = true;
        std::move(done)(error_code{});
    }

    void send_trailers(Headers trailers, WriteCompletion done) override {
        ++calls;
        last_trailers = std::move(trailers);
        sent_trailers = true;
        std::move(done)(error_code{});
    }

    void send_bodyless(int status, Headers headers, WriteCompletion done) override {
        ++calls;
        last_status = status;
        last_headers = std::move(headers);
        sent_bodyless = true;
        std::move(done)(error_code{});
    }

    void send_continue(WriteCompletion done) override {
        ++continues;
        std::move(done)(error_code{});
    }

    bool head_request() const noexcept override { return head; }

    void connected(ConnectedCompletion done) const override {
        std::move(done)(error_code{}, open);
    }

    void close(WriteCompletion done) override {
        open = false;
        std::move(done)(error_code{});
    }

    asio::any_io_executor get_executor() const override { return m_executor; }

    Version version() const override { return ver; }

    asio::any_io_executor m_executor;

    // recorded state
    int calls{0};
    int continues{0};
    int last_status{0};
    std::string last_body;
    Headers last_headers;
    Headers last_trailers;
    std::vector<std::string> chunks;
    bool begun{false};
    bool finished{false};
    bool sent_bodyless{false};
    bool sent_trailers{false};
    bool head{false}; // report head_request() == true
    bool open{true};
    Version ver{Version::Http11};

    bool has_header(std::string_view name) const { return last_headers.contains(name); }

    std::string header(std::string_view name) const {
        auto value = last_headers.get(name);
        return value ? std::string{*value} : std::string{};
    }
};

// --- the library's log sink
// ---------------------------------------------------

// The installed sink is process-wide; swap it for the duration of a scope and
// put it back, so one test's capture cannot leak into another. `minimum` is the
// threshold the capture reports through LogSink::enabled, which is how a test
// exercises level filtering now that the sink owns that decision.
class ScopedLog {
  public:
    explicit ScopedLog(LogLevel minimum = LogLevel::Trace) : m_minimum(minimum) {
        m_previous = log_sink();
        set_log_sink(std::make_shared<Capture>(this));
    }

    ~ScopedLog() { set_log_sink(std::move(m_previous)); }

    ScopedLog(const ScopedLog &) = delete;
    ScopedLog &operator=(const ScopedLog &) = delete;

    struct Record {
        LogLevel level;
        std::string file;
        int line;
        std::string message;
        // Attr values rendered as text (the sink's own choice would differ).
        std::vector<std::pair<std::string, std::string>> attrs;

        // The rendered value of a structured attr, or empty when absent.
        std::string attr(std::string_view key) const {
            for (const auto &[k, v] : attrs) {
                if (k == key) {
                    return v;
                }
            }
            return {};
        }
    };

    std::vector<Record> records;

  private:
    // Everything is copied out here: the facade only guarantees a record's
    // strings for the duration of the call.
    class Capture final : public LogSink {
      public:
        explicit Capture(ScopedLog *owner) : m_owner(owner) {}

        bool enabled(LogLevel level) const noexcept override {
            return static_cast<int>(level) >= static_cast<int>(m_owner->m_minimum);
        }

        void log(const LogRecord &record) override {
            Record rec{record.level,
                       std::string{record.where.file_name()},
                       static_cast<int>(record.where.line()),
                       std::string{record.message},
                       {}};
            for (const Attr &attr : record.attrs) {
                rec.attrs.emplace_back(std::string{attr.key}, detail::attr_text(attr));
            }
            m_owner->records.push_back(std::move(rec));
        }

      private:
        ScopedLog *m_owner;
    };

    LogLevel m_minimum;
    std::shared_ptr<LogSink> m_previous;
};

// --- WebSocket frame helpers -------------------------------------------------

// A client-frame header (masked, FIN set) followed by the masked payload: what
// a browser sends, so the server-side parser sees real traffic. `rsv1` sets
// the RSV1 bit — the permessage-deflate compressed-message marker.
inline std::string ws_client_frame(WsOpcode opcode, std::string_view payload, bool fin = true,
                                   const unsigned char (&mask)[4] = {0x37, 0xfa, 0x21, 0x3d}, bool rsv1 = false) {
    std::string out;
    const std::uint8_t flags = (fin ? 0x80 : 0x00) | (rsv1 ? 0x40 : 0x00) | static_cast<unsigned char>(opcode);
    out.push_back(static_cast<char>(flags));
    if (payload.size() <= 125) {
        out.push_back(static_cast<char>(0x80 | static_cast<unsigned char>(payload.size())));
    } else if (payload.size() <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        out.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((payload.size() >> shift) & 0xFF));
        }
    }
    out.append(reinterpret_cast<const char *>(mask), 4);
    std::string masked{payload};
    ws_unmask(masked.data(), masked.size(), mask);
    out.append(masked);
    return out;
}

} // namespace simple_http::test
