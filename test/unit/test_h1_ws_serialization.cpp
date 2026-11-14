// A WS upgrade answers 101 on the same transport a middleware may have just
// used for a normal response. Two transport writes in flight at once interleave
// their async_write_some calls, so the handshake bytes can be spliced into the
// previous response. This test drives the h1 engine over a transport that
// counts in-flight writes and never completes them, which makes the window
// deterministic instead of timing-dependent.

#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;

namespace {

namespace asio = boost::asio;

class OverlapTransport {
  public:
    explicit OverlapTransport(asio::any_io_executor ex) : m_ex(std::move(ex)) {}

    void push(std::string bytes) { m_in = std::move(bytes); }

    bool overlap() const { return m_overlap; }
    int writes() const { return m_writes; }

    template <typename Token> auto async_read_some(ByteSpan buffer, Token &&token) {
        auto init = [this, buffer](auto handler) {
            asio::post(m_ex, [this, buffer, handler = std::move(handler)]() mutable {
                if (m_pos >= m_in.size()) {
                    std::move(handler)(asio::error::eof, std::size_t{0});
                    return;
                }
                const std::size_t n = std::min<std::size_t>(buffer.size(), m_in.size() - m_pos);
                std::memcpy(buffer.data(), m_in.data() + m_pos, n);
                m_pos += n;
                std::move(handler)(error_code{}, n);
            });
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            detail::make_executor_initiation(std::move(init), m_ex), token);
    }

    template <typename Token> auto async_read(ByteSpan buffer, Token &&token) {
        auto init = [this, buffer](auto handler) {
            asio::post(m_ex, [this, buffer, handler = std::move(handler)]() mutable {
                const std::size_t n = std::min<std::size_t>(buffer.size(), m_in.size() - m_pos);
                std::memcpy(buffer.data(), m_in.data() + m_pos, n);
                m_pos += n;
                std::move(handler)(n == buffer.size() ? error_code{} : make_error_code(asio::error::eof), n);
            });
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            detail::make_executor_initiation(std::move(init), m_ex), token);
    }

    // Writes are never completed: the first one stays in flight, which is the
    // window in which a second initiation is the bug under test.
    template <typename Token> auto async_write(ConstByteSpan, Token &&token) {
        ++m_writes;
        if (m_inflight > 0)
            m_overlap = true;
        ++m_inflight;
        auto init = [this](auto handler) { m_pending.emplace_back(std::move(handler)); };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            detail::make_executor_initiation(std::move(init), m_ex), token);
    }

    template <typename Token> auto async_write_seq(boost::span<const ConstByteSpan>, Token &&token) {
        ++m_writes;
        if (m_inflight > 0)
            m_overlap = true;
        ++m_inflight;
        auto init = [this](auto handler) { m_pending.emplace_back(std::move(handler)); };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            detail::make_executor_initiation(std::move(init), m_ex), token);
    }

    template <typename Token> auto async_shutdown(Token &&token) {
        auto init = [](auto handler) { std::move(handler)(error_code{}); };
        return asio::async_initiate<Token, void(error_code)>(detail::make_executor_initiation(std::move(init), m_ex),
                                                             token);
    }

    auto get_executor() { return m_ex; }
    asio::ip::tcp::endpoint peer() const { return {}; }
    SslHandle tls_handle() const { return std::nullopt; }
    void close() {}

  private:
    asio::any_io_executor m_ex;
    std::string m_in;
    std::size_t m_pos{0};
    int m_inflight{0};
    int m_writes{0};
    bool m_overlap{false};
    std::vector<asio::any_completion_handler<void(error_code, std::size_t)>> m_pending;
};

} // namespace

TEST_CASE("h1/ws: the 101 is serialized with a response written before the upgrade", "[h1][ws][write]") {
    asio::io_context ctx;
    auto transport = std::make_shared<OverlapTransport>(ctx.get_executor());
    transport->push("GET /ws HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "\r\n");

    auto engine = std::make_shared<Http1Engine<OverlapTransport>>(transport);

    CallbackWsHandler ws_handler = [](RequestPtr, std::shared_ptr<WebSocket>, HandlerCompletion done) {
        std::move(done)();
    };
    WsHandlerFn route = [ws_handler](RequestPtr req, ResponsePtr res, SslHandle, WsUpgrade upgrade,
                                     WsUpgradeCompletion done) {
        // A route/middleware that writes before accepting the upgrade is the
        // shape that starts a normal response write and then the 101.
        res->status(status::ok).async_send("pre", asio::detached);
        upgrade(req, res, ws_handler, std::move(done));
    };
    WsLookup lookup = [route](std::string_view path) -> std::optional<WsHandlerFn> {
        if (path == "/ws")
            return route;
        return std::nullopt;
    };

    Dispatcher dispatch = [](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) { std::move(done)(); };
    engine->run(std::move(dispatch), {}, std::move(lookup), {}, {}, [] {});
    ctx.run_for(std::chrono::milliseconds(50));

    // The 101 must wait behind the response write that is already in flight,
    // not start a second transport write of its own.
    CHECK(transport->writes() == 1);
    CHECK_FALSE(transport->overlap());
}

// S6: a WS route (or one of its middleware) that throws synchronously used to
// propagate out of handle_websocket() and then out of io_context::run(), which
// terminates the worker process. It must be turned into a 500 like an ordinary
// handler exception.
TEST_CASE("h1/ws: a route that throws synchronously is answered with 500", "[h1][ws]") {
    asio::io_context ctx;
    auto transport = std::make_shared<test::MockTransport>(ctx.get_executor());
    transport->push("GET /ws HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "\r\n");

    WsHandlerFn route = [](RequestPtr, ResponsePtr, SslHandle, WsUpgrade, WsUpgradeCompletion) {
        throw std::runtime_error("middleware boom");
    };
    WsLookup lookup = [route](std::string_view) -> std::optional<WsHandlerFn> { return route; };
    Dispatcher dispatch = [](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) { std::move(done)(); };

    auto engine = std::make_shared<Http1Engine<test::MockTransport>>(transport);
    bool threw = false;
    try {
        engine->run(std::move(dispatch), {}, std::move(lookup), {}, {}, [] {});
        ctx.run_for(std::chrono::milliseconds(200));
    } catch (const std::exception &) {
        threw = true;
    }
    CHECK_FALSE(threw);
    CHECK(transport->written().find("500 Internal Server Error") != std::string::npos);
}
