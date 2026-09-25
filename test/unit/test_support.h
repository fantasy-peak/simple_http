#pragma once

// Shared test doubles and helpers for the unit tests.
//
// Everything here is in-memory: no sockets, no threads, and no global state left
// behind. Coroutine-based APIs are driven through run()/run_on(), which pump a
// private io_context with a deadline, so a hung coroutine fails a test instead of
// hanging the suite.

#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "simple_http.h"

namespace simple_http::test {

namespace asio = boost::asio;

// --- running awaitables ------------------------------------------------------

// Runs an awaitable on `ctx`, giving up after `timeout`. Returns nullopt when it
// did not finish in time (which a test asserts against, rather than hanging).
template <typename T>
std::optional<T> run_on(asio::io_context& ctx,
                        asio::awaitable<T> op,
                        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    // A context that ran out of work is left stopped; each pump needs a restart
    // (so a test can await several coroutines on one context).
    ctx.restart();
    std::optional<T> result;
    asio::co_spawn(ctx, std::move(op), [&result](const std::exception_ptr& ep, T value) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "run_on: coroutine threw: %s\n", e.what());  // visible in the report
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
inline bool run_on(asio::io_context& ctx,
                   asio::awaitable<void> op,
                   std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    ctx.restart();
    bool done = false;
    asio::co_spawn(ctx, std::move(op), [&done](const std::exception_ptr& ep) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception& e) {
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
std::optional<T> run_until(asio::io_context& ctx,
                           asio::awaitable<T> op,
                           std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::optional<T> result;
    bool finished = false;
    asio::co_spawn(ctx, std::move(op), [&](const std::exception_ptr& ep, T value) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception& e) {
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

inline bool run_until(asio::io_context& ctx,
                      asio::awaitable<void> op,
                      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    bool finished = false;
    asio::co_spawn(ctx, std::move(op), [&finished](const std::exception_ptr& ep) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception& e) {
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
inline void drain(asio::io_context& ctx, std::chrono::milliseconds slice = std::chrono::milliseconds(50)) {
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

// Turns an awaitable-producing lambda into a value (Catch2 macros cannot be used
// inside a coroutine, so tests do their awaiting in `run(...)` and assert after).
template <typename F>
concept AwaitableFactory = requires(F f) { f(); };

// --- a scripted byte stream --------------------------------------------------

// Satisfies the transport concept with preloaded input and recorded output, so the
// WebSocket backend (and anything else driven by a transport) can be tested
// without a socket. Reads hand out the queued chunks in order and report EOF once
// they run out; writes are appended to `written`.
class MockTransport {
  public:
    explicit MockTransport(asio::any_io_executor ex) : m_executor(std::move(ex)) {
    }

    // Queues bytes for the next reads.
    void push(std::string bytes) {
        m_in.push_back(std::move(bytes));
    }

    // Everything written so far, concatenated, and chunk by chunk.
    const std::string& written() const {
        return m_written;
    }

    const std::vector<std::string>& writes() const {
        return m_writes;
    }

    bool closed() const {
        return m_closed;
    }

    asio::awaitable<IoResult> async_read_some(ByteSpan buffer) {
        if (m_in.empty())
            co_return IoResult{make_error_code(asio::error::eof), 0};
        std::string& front = m_in.front();
        const std::size_t take = std::min(front.size(), buffer.size());
        std::memcpy(buffer.data(), front.data(), take);
        front.erase(0, take);
        if (front.empty())
            m_in.erase(m_in.begin());
        co_return IoResult{error_code{}, take};
    }

    asio::awaitable<IoResult> async_read(ByteSpan buffer) {
        std::size_t got = 0;
        while (got < buffer.size()) {
            auto [ec, n] = co_await async_read_some(buffer.subspan(got));
            got += n;
            if (ec)
                co_return IoResult{ec, got};
        }
        co_return IoResult{error_code{}, got};
    }

    asio::awaitable<IoResult> async_write(ConstByteSpan buffer) {
        m_writes.emplace_back(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        m_written.append(m_writes.back());
        co_return IoResult{error_code{}, buffer.size()};
    }

    asio::awaitable<IoResult> async_write_seq(std::span<const ConstByteSpan> buffers) {
        std::string chunk;
        for (const auto& buffer : buffers) {
            chunk.append(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        }
        m_writes.push_back(chunk);
        m_written.append(chunk);
        co_return IoResult{error_code{}, chunk.size()};
    }

    auto get_executor() {
        return m_executor;
    }

    asio::ip::tcp::endpoint peer() const {
        return {};
    }

    SslHandle tls_handle() const {
        return std::nullopt;
    }

    void close() {
        m_closed = true;
    }

  private:
    asio::any_io_executor m_executor;
    std::vector<std::string> m_in;
    std::vector<std::string> m_writes;
    std::string m_written;
    bool m_closed{false};
};

// --- a response writer that records instead of writing -----------------------

// Implements the ResponseWriter seam so Response (and anything that replies) can
// be exercised without a connection. Records the last call's arguments.
class FakeResponseWriter : public ResponseWriter {
  public:
    asio::awaitable<error_code> send(int status, Headers headers, std::string body) override {
        ++calls;
        last_status = status;
        last_headers = std::move(headers);
        last_body = std::move(body);
        sent_bodyless = false;
        co_return error_code{};
    }

    asio::awaitable<error_code> send_headers(int status, Headers headers) override {
        ++calls;
        last_status = status;
        last_headers = std::move(headers);
        begun = true;
        co_return error_code{};
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        chunks.push_back(std::move(data));
        co_return error_code{};
    }

    asio::awaitable<error_code> send_last(std::string data) override {
        chunks.push_back(std::move(data));
        finished = true;
        co_return error_code{};
    }

    asio::awaitable<error_code> send_bodyless(int status, Headers headers) override {
        ++calls;
        last_status = status;
        last_headers = std::move(headers);
        sent_bodyless = true;
        co_return error_code{};
    }

    asio::awaitable<error_code> send_continue() override {
        ++continues;
        co_return error_code{};
    }

    asio::awaitable<bool> connected() const override { co_return open; }

    asio::awaitable<void> close() override {
        open = false;
        co_return;
    }

    Version version() const override {
        return ver;
    }

    // recorded state
    int calls{0};
    int continues{0};
    int last_status{0};
    std::string last_body;
    Headers last_headers;
    std::vector<std::string> chunks;
    bool begun{false};
    bool finished{false};
    bool sent_bodyless{false};
    bool open{true};
    Version ver{Version::Http11};

    bool has_header(std::string_view name) const {
        return last_headers.contains(name);
    }

    std::string header(std::string_view name) const {
        auto value = last_headers.get(name);
        return value ? std::string{*value} : std::string{};
    }
};

// --- the library's global log hook -------------------------------------------

// LOG_CB and log_level are process-wide; swap them for the duration of a scope and
// put them back, so one test's capture cannot leak into another.
class ScopedLog {
  public:
    ScopedLog() : m_cb(LOG_CB), m_level(log_level.load(std::memory_order_relaxed)) {
        LOG_CB = [this](LogLevel level, std::string_view file, int line, std::string message) {
            records.push_back({level, std::string{file}, line, std::move(message)});
        };
    }

    ~ScopedLog() {
        LOG_CB = std::move(m_cb);
        log_level.store(m_level, std::memory_order_relaxed);
    }

    ScopedLog(const ScopedLog&) = delete;
    ScopedLog& operator=(const ScopedLog&) = delete;

    struct Record {
        LogLevel level;
        std::string file;
        int line;
        std::string message;
    };

    std::vector<Record> records;

  private:
    std::function<void(LogLevel, std::string_view, int, std::string)> m_cb;
    LogLevel m_level;
};

// --- WebSocket frame helpers -------------------------------------------------

// A client-frame header (masked, FIN set) followed by the masked payload: what a
// browser sends, so the server-side parser sees real traffic.
inline std::string ws_client_frame(WsOpcode opcode,
                                   std::string_view payload,
                                   bool fin = true,
                                   const unsigned char (&mask)[4] = {0x37, 0xfa, 0x21, 0x3d}) {
    std::string out;
    out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<unsigned char>(opcode)));
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
    out.append(reinterpret_cast<const char*>(mask), 4);
    std::string masked{payload};
    ws_unmask(masked.data(), masked.size(), mask);
    out.append(masked);
    return out;
}

}  // namespace simple_http::test
