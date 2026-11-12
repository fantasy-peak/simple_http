// H1 request-body decoder fuzz target.
//
// Prepends a syntactically valid chunked request head to arbitrary bytes and
// drives the request-body state machine through a handler that drains it. This
// reaches the chunk-size/ext parser, the CRLF enforcement, the trailer section
// and the max_body_bytes accounting — none of which the head-parser target
// covers.

#include <algorithm>
#include <boost/asio.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "simple_http.h"

#include "../support/coro_compat.h"

namespace {

namespace asio = boost::asio;
using namespace simple_http;
using namespace simple_http::test;

// Minimal in-memory transport: the engine reads the seeded bytes and reports
// EOF, so a body that is still waiting for more input terminates instead of
// hanging. Writes are discarded; the target only cares about parser behavior.
class FuzzTransport {
  public:
    explicit FuzzTransport(asio::any_io_executor executor) : m_executor(std::move(executor)) {}

    void push(std::string bytes) { m_in = std::move(bytes); }

    template <typename Token> auto async_read_some(ByteSpan buffer, Token &&token) {
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            [this, buffer](auto handler) {
                asio::co_spawn(m_executor, do_read_some(buffer),
                               [handler = std::move(handler)](std::exception_ptr, IoResult r) mutable {
                                   std::move(handler)(r.first, r.second);
                               });
            },
            token);
    }

    template <typename Token> auto async_read(ByteSpan buffer, Token &&token) {
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            [this, buffer](auto handler) {
                asio::co_spawn(m_executor, do_read(buffer),
                               [handler = std::move(handler)](std::exception_ptr, IoResult r) mutable {
                                   std::move(handler)(r.first, r.second);
                               });
            },
            token);
    }

    template <typename Token> auto async_write(ConstByteSpan buffer, Token &&token) {
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            [this, buffer](auto handler) {
                asio::co_spawn(m_executor, do_write(buffer),
                               [handler = std::move(handler)](std::exception_ptr, IoResult r) mutable {
                                   std::move(handler)(r.first, r.second);
                               });
            },
            token);
    }

    template <typename Token> auto async_write_seq(boost::span<const ConstByteSpan> buffers, Token &&token) {
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            [this, buffers](auto handler) {
                asio::co_spawn(m_executor, do_write_seq(buffers),
                               [handler = std::move(handler)](std::exception_ptr, IoResult r) mutable {
                                   std::move(handler)(r.first, r.second);
                               });
            },
            token);
    }

    template <typename Token> auto async_shutdown(Token &&token) {
        return asio::async_initiate<Token, void(error_code)>([](auto handler) { std::move(handler)(error_code{}); },
                                                             token);
    }

    auto get_executor() { return m_executor; }
    asio::ip::tcp::endpoint peer() const { return {}; }
    SslHandle tls_handle() const { return std::nullopt; }
    void close() {}

  private:
    asio::awaitable<IoResult> do_read_some(ByteSpan buffer) {
        if (m_offset >= m_in.size()) {
            co_return IoResult{make_error_code(asio::error::eof), 0};
        }
        const std::size_t take = std::min(buffer.size(), m_in.size() - m_offset);
        std::memcpy(buffer.data(), m_in.data() + m_offset, take);
        m_offset += take;
        co_return IoResult{error_code{}, take};
    }

    asio::awaitable<IoResult> do_read(ByteSpan buffer) {
        std::size_t got = 0;
        while (got < buffer.size()) {
            auto [ec, n] = co_await do_read_some(buffer.subspan(got));
            got += n;
            if (ec) {
                co_return IoResult{ec, got};
            }
        }
        co_return IoResult{error_code{}, got};
    }

    asio::awaitable<IoResult> do_write(ConstByteSpan buffer) {
        m_written.append(reinterpret_cast<const char *>(buffer.data()), buffer.size());
        co_return IoResult{error_code{}, buffer.size()};
    }

    asio::awaitable<IoResult> do_write_seq(boost::span<const ConstByteSpan> buffers) {
        std::size_t total = 0;
        for (const ConstByteSpan buffer : buffers) {
            m_written.append(reinterpret_cast<const char *>(buffer.data()), buffer.size());
            total += buffer.size();
        }
        co_return IoResult{error_code{}, total};
    }

  private:
    asio::any_io_executor m_executor;
    std::string m_in;
    std::size_t m_offset{0};
    std::string m_written;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size > (1u << 20)) {
        return 0;
    }

    asio::io_context ctx;
    auto transport = std::make_shared<FuzzTransport>(ctx.get_executor());
    std::string input = "POST / HTTP/1.1\r\nHost: fuzz\r\nTransfer-Encoding: chunked\r\n\r\n";
    input.append(reinterpret_cast<const char *>(data), size);
    transport->push(std::move(input));

    EngineLimits limits;
    limits.max_body_bytes = 4096; // reachable quickly from a mutated input
    limits.idle_timeout = std::chrono::seconds{0};

    auto engine = std::make_shared<Http1Engine<FuzzTransport>>(transport, limits);
    // `run` stores a reference to the Dispatcher, and the coroutine outlives the
    // statement that starts it: bind the lambda to a named std::function so it
    // cannot dangle across ctx.run_for().
    auto dispatch = [](std::shared_ptr<Request> request, std::shared_ptr<ResponseWriter> response,
                       SslHandle) -> asio::awaitable<void> {
        auto body = co_await body_read_all(request->body());
        volatile std::size_t sink = body ? body->size() : 0;
        (void)sink;
        if (body) {
            (void)co_await response->status(status::ok).async_send("ok", asio::use_awaitable);
        }
    };
    auto callback = detail::make_callback_handler(dispatch);
    engine->run(std::move(callback), {}, {}, {}, {}, [] {});
    // The transport reports EOF once the seeded bytes run out, so a well-formed
    // or truncated body always terminates the engine.
    ctx.run_for(std::chrono::milliseconds(100));
    return 0;
}
