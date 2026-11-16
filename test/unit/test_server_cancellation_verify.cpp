// Verification of request-scoped cancellation (proto/request.h) while a server
// handler is parked in a read or a write.
//
// These tests drive the real Body / ResponseWriter / h1 / h2 code, not a mock of
// the cancellation machinery. Each case records whether the pending operation
// actually completed with operation_aborted, then the test asserts the observed
// contract of the current implementation.

#include <boost/asio.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/cancel_after.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::codec;
using namespace simple_http::test;

namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;

// Fires req->cancel() from a coroutine on the request's owner executor, the way
// middleware::timeout (or an application timer) would.
void cancel_after(asio::any_io_executor ex, RequestPtr req, std::chrono::milliseconds delay) {
    asio::co_spawn(
        ex,
        [req, delay]() -> asio::awaitable<void> {
            asio::steady_timer t{co_await asio::this_coro::executor};
            t.expires_after(delay);
            co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
            req->cancel();
        },
        asio::detached);
}

// The cancellation slot binding execute_coroutine() uses: the handler coroutine
// gets the request's slot through co_spawn, so every await inside it sees the
// cancellation emission through its associated cancellation state.
template <typename F> void spawn_with_request_slot(asio::io_context &ctx, const RequestPtr &req, F &&fn) {
    asio::co_spawn(ctx, std::forward<F>(fn),
                   asio::bind_cancellation_slot(req->cancellation_slot(), [](std::exception_ptr) {}));
}

std::string u32be(std::uint32_t v) {
    return {static_cast<char>((v >> 24) & 0xFF), static_cast<char>((v >> 16) & 0xFF),
            static_cast<char>((v >> 8) & 0xFF), static_cast<char>(v & 0xFF)};
}

void append_frame(std::string &out, H2FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                  std::string_view payload) {
    serialize_frame_header(out, static_cast<std::uint32_t>(payload.size()), static_cast<std::uint8_t>(type), flags,
                           stream_id);
    out.append(payload);
}

// POST / with no END_STREAM, so the handler is allowed to park reading the body.
std::string post_headers_frame(std::uint32_t id) {
    std::string block;
    hpack_append_indexed(block, 3); // :method: POST
    hpack_append_indexed(block, 7); // :scheme: https
    hpack_append_literal(block, ":path", "/");
    hpack_append_literal(block, ":authority", "example.com");
    std::string out;
    append_frame(out, H2FrameType::Headers, H2_FLAG_END_HEADERS, id, block);
    return out;
}

std::string rst_frame(std::uint32_t id) {
    std::string out;
    append_frame(out, H2FrameType::RstStream, 0, id, u32be(H2_CANCEL));
    return out;
}

// A transport that parks writes until the test releases them, so a handler can
// be observed while parked inside the real h1 sink + SerializedWriter chain.
class ParkedWriteTransport {
  public:
    explicit ParkedWriteTransport(asio::any_io_executor ex) : m_executor(std::move(ex)) {}

    void push(std::string bytes) { m_in.push_back(std::move(bytes)); }
    void hold_open() { m_hold_open = true; }
    void release() { m_hold_open = false; }

    // Complete the write that is parked right now and keep parking later ones.
    void complete_current_write(error_code ec) {
        m_write_ec = ec;
        if (m_pending_write) {
            auto done = std::move(m_pending_write);
            done(m_write_ec, m_pending_bytes);
        }
    }

    // Stop parking writes: complete the current one and every later one.
    void release_writes(error_code ec) {
        m_release_all = true;
        complete_current_write(ec);
    }

    template <typename Token> auto async_read_some(ByteSpan buffer, Token &&token) {
        auto initiation = [this, buffer](auto handler) {
            asio::co_spawn(m_executor, do_read_some(buffer),
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

    template <typename Token> auto async_write(ConstByteSpan buffer, Token &&token) {
        auto initiation = [this, buffer](auto handler) {
            if (m_release_all) {
                std::move(handler)(m_write_ec, buffer.size());
                return;
            }
            m_pending_bytes = buffer.size();
            m_pending_write = asio::any_completion_handler<void(error_code, std::size_t)>(std::move(handler));
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            ExecInit<decltype(initiation)>{std::move(initiation), m_executor}, token);
    }

    template <typename Token> auto async_write_seq(boost::span<const ConstByteSpan> buffers, Token &&token) {
        std::size_t total = 0;
        for (const auto &buffer : buffers) {
            total += buffer.size();
        }
        auto initiation = [this, total](auto handler) {
            if (m_release_all) {
                std::move(handler)(m_write_ec, total);
                return;
            }
            m_pending_bytes = total;
            m_pending_write = asio::any_completion_handler<void(error_code, std::size_t)>(std::move(handler));
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            ExecInit<decltype(initiation)>{std::move(initiation), m_executor}, token);
    }

    auto get_executor() { return m_executor; }
    asio::ip::tcp::endpoint peer() const { return {}; }
    SslHandle tls_handle() const { return std::nullopt; }
    bool closed() const { return m_closed; }

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

    asio::any_io_executor m_executor;
    std::vector<std::string> m_in;
    asio::any_completion_handler<void(error_code, std::size_t)> m_pending_write;
    std::size_t m_pending_bytes{0};
    error_code m_write_ec{};
    bool m_release_all{false};
    bool m_hold_open{false};
    bool m_closed{false};
};

// A sink whose chunk write stays pending until the test completes it. Used to
// check whether request cancellation reaches a parked ResponseSink operation.
class ParkingSink final : public ResponseSink {
  public:
    explicit ParkingSink(asio::any_io_executor ex) : m_executor(std::move(ex)) {}

    void send(int, Headers, std::string, WriteCompletion done) override { std::move(done)(error_code{}); }
    void send_headers(int, Headers, WriteCompletion done) override { std::move(done)(error_code{}); }
    void send_chunk(std::string, WriteCompletion done) override {
        slot_connected = asio::get_associated_cancellation_slot(done).is_connected();
        m_chunk = std::move(done);
    }
    void send_last(std::string, WriteCompletion done) override { m_last = std::move(done); }
    void send_bodyless(int, Headers, WriteCompletion done) override { std::move(done)(error_code{}); }
    void connected(ConnectedCompletion done) const override { std::move(done)(error_code{}, true); }
    void close(WriteCompletion done) override {
        m_open = false;
        std::move(done)(error_code{});
    }
    asio::any_io_executor get_executor() const override { return m_executor; }
    Version version() const override { return Version::Http2; }

    WriteCompletion m_chunk;
    WriteCompletion m_last;
    bool slot_connected{false};
    bool m_open{true};

  private:
    asio::any_io_executor m_executor;
};

} // namespace

TEST_CASE("cancel-verify: request cancel aborts a parked pushed-body read", "[cancel-verify]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});

    bool read_done = false;
    error_code read_ec = make_error_code(asio::error::would_block);
    cancel_after(ctx.get_executor(), req, 10ms);
    spawn_with_request_slot(ctx, req, [&]() -> asio::awaitable<void> {
        auto [ec, result] = co_await req->body().async_read(asio::as_tuple(asio::use_awaitable));
        read_ec = ec;
        read_done = true;
    });
    ctx.run_for(1s);

    CHECK(read_done);
    // The channel's channel_cancelled is normalized at the composed-operation
    // boundary: the public contract is Asio's operation_aborted.
    CHECK(read_ec == asio::error::operation_aborted);
}

TEST_CASE("cancel-verify: request cancel aborts an h1 parked body read", "[cancel-verify]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    // A complete request head with a body length but no body bytes, then hold
    // the peer open: the handler parks in the pull provider's socket read.
    transport->push("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
    transport->hold_open();

    bool read_done = false;
    error_code read_ec = make_error_code(asio::error::would_block);

    auto dispatcher = [&](RequestPtr req, ResponsePtr, SslHandle) -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        asio::co_spawn(
            ex,
            [&, req]() -> asio::awaitable<void> {
                asio::steady_timer t{co_await asio::this_coro::executor};
                t.expires_after(10ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                req->cancel();
                // Fallback only: if cancellation did not reach the transport
                // read, release the peer so the test fails instead of hanging.
                t.expires_after(100ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                if (!read_done) {
                    transport->release();
                }
            },
            asio::detached);

        auto [ec, result] = co_await req->body().async_read(asio::as_tuple(asio::use_awaitable));
        read_ec = ec;
        read_done = true;
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(read_done);
    // The cancellation slot travels through the h1 pull provider into the
    // transport read. The peer is never released on the success path, so this
    // completion came from cancellation, not from a transport EOF.
    CHECK(read_ec == asio::error::operation_aborted);
    // A cancelled request body cannot be drained, so the connection is closed.
    CHECK(transport->closed());
}

TEST_CASE("cancel-verify: h2 RST_STREAM aborts a parked handler body read", "[cancel-verify]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->hold_open();
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, ""); // client SETTINGS must come first
    in += post_headers_frame(1);
    transport->push(std::move(in));

    bool read_done = false;
    bool cancelled_flag = false;
    error_code read_ec = make_error_code(asio::error::would_block);

    auto dispatcher = [&](RequestPtr req, ResponsePtr, SslHandle) -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        asio::co_spawn(
            ex,
            [&, req]() -> asio::awaitable<void> {
                asio::steady_timer t{co_await asio::this_coro::executor};
                t.expires_after(10ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                transport->push(rst_frame(1));
                t.expires_after(40ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                cancelled_flag = req->cancelled();
                transport->release();
            },
            asio::detached);

        auto [ec, result] = co_await req->body().async_read(asio::as_tuple(asio::use_awaitable));
        read_ec = ec;
        read_done = true;
    };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));
    ctx.run_for(100ms); // let the detached checker finish

    CHECK(cancelled_flag);
    CHECK(read_done);
    // Observed: erase_stream() fails the Body with connection_reset before it
    // signals cancellation, so the parked read sees connection_reset rather
    // than operation_aborted.
    CHECK(read_ec == asio::error::connection_reset);
}

TEST_CASE("cancel-verify: request cancel aborts an h1 parked response write", "[cancel-verify]") {
    asio::io_context ctx;
    auto transport = std::make_shared<ParkedWriteTransport>(ctx.get_executor());
    transport->push("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    transport->hold_open();

    bool begin_done = false;
    bool write_done = false;
    bool done_at_cancel_check = false;
    error_code write_ec = make_error_code(asio::error::would_block);
    RequestPtr captured;

    auto dispatcher = [&](RequestPtr req, ResponsePtr res, SslHandle) -> asio::awaitable<void> {
        captured = req;
        auto ex = co_await asio::this_coro::executor;
        asio::co_spawn(
            ex,
            [&]() -> asio::awaitable<void> {
                asio::steady_timer t{co_await asio::this_coro::executor};
                // Let the header write finish, so the handler parks on the chunk
                // write rather than on async_begin().
                t.expires_after(10ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                transport->complete_current_write(error_code{});
                t.expires_after(30ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                if (captured) {
                    captured->cancel();
                }
                t.expires_after(30ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                done_at_cancel_check = write_done;
                // The cancelled write completed already; this only releases the
                // abandoned transport write so the engine can finish.
                transport->complete_current_write(error_code{});
                t.expires_after(30ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                transport->release();
            },
            asio::detached);

        auto [ec] = co_await res->status(status::ok).async_begin(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            write_ec = ec;
        } else {
            begin_done = true;
            auto [wec] = co_await res->async_write("chunk", asio::as_tuple(asio::use_awaitable));
            write_ec = wec;
        }
        write_done = true;
        // Stay alive until the checker has observed write_done; otherwise the
        // engine would tear down before the detached checker's 70 ms sample.
        asio::steady_timer hold{co_await asio::this_coro::executor};
        hold.expires_after(120ms);
        co_await hold.async_wait(asio::as_tuple(asio::use_awaitable));
    };

    auto engine = std::make_shared<Http1Engine<ParkedWriteTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(begin_done);
    CHECK(done_at_cancel_check);
    CHECK(write_done);
    CHECK(write_ec == asio::error::operation_aborted);
    // HTTP/1.x cannot retract queued bytes: cancellation closes the connection.
    CHECK(transport->closed());
}

TEST_CASE("cancel-verify: ResponseWriter preserves the caller's cancellation slot at the sink", "[cancel-verify]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->body().finish();
    auto sink = std::make_shared<ParkingSink>(ctx.get_executor());
    auto res = std::make_shared<ResponseWriter>(sink);

    bool write_done = false;
    bool done_at_cancel_check = false;
    error_code write_ec = make_error_code(asio::error::would_block);

    cancel_after(ctx.get_executor(), req, 10ms);
    spawn_with_request_slot(ctx, req, [&]() -> asio::awaitable<void> {
        co_await res->status(status::ok).async_begin(asio::as_tuple(asio::use_awaitable));
        auto [ec] = co_await res->async_write("chunk", asio::as_tuple(asio::use_awaitable));
        write_ec = ec;
        write_done = true;
    });
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer t{co_await asio::this_coro::executor};
            t.expires_after(60ms);
            co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
            done_at_cancel_check = write_done;
            if (sink->m_chunk) {
                std::move(sink->m_chunk)(error_code{});
            }
        },
        asio::detached);
    ctx.run_for(1s);

    // The sink must be able to implement the policy: the caller's slot reaches
    // it through the type-erased WriteCompletion. This test sink deliberately
    // ignores cancellation, so the write completes normally once released.
    CHECK(sink->slot_connected);
    CHECK_FALSE(done_at_cancel_check);
    CHECK(write_done);
    CHECK_FALSE(write_ec);
}

TEST_CASE("cancel-verify: asio::cancel_after aborts an h1 body read without cancelling the request",
          "[cancel-verify]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
    transport->hold_open();

    bool read_done = false;
    bool request_cancelled = true;
    error_code read_ec = make_error_code(asio::error::would_block);

    auto dispatcher = [&](RequestPtr req, ResponsePtr, SslHandle) -> asio::awaitable<void> {
        auto [ec, result] =
            co_await req->body().async_read(asio::cancel_after(30ms, asio::as_tuple(asio::use_awaitable)));
        read_ec = ec;
        read_done = true;
        request_cancelled = req->cancelled();
        transport->release();
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(read_done);
    // The per-operation timer composes with Asio's cancellation protocol: only
    // this read is aborted, the request itself stays live.
    CHECK(read_ec == asio::error::operation_aborted);
    CHECK_FALSE(request_cancelled);
}

TEST_CASE("cancel-verify: request cancel aborts a queued h1 body read too", "[cancel-verify]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
    transport->hold_open();

    bool first_done = false;
    bool second_done = false;
    error_code first_ec = make_error_code(asio::error::would_block);
    error_code second_ec = make_error_code(asio::error::would_block);

    auto dispatcher = [&](RequestPtr req, ResponsePtr, SslHandle) -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        // The pull provider serializes body reads, so this one is queued behind
        // the dispatcher's own read. Bind the request slot explicitly: an inner
        // co_spawn does not inherit its parent's cancellation state.
        asio::co_spawn(
            ex,
            [req, &second_done, &second_ec]() -> asio::awaitable<void> {
                auto [ec, result] = co_await req->body().async_read(asio::as_tuple(asio::use_awaitable));
                second_ec = ec;
                second_done = true;
            },
            asio::bind_cancellation_slot(req->cancellation_slot(), asio::detached));
        asio::co_spawn(
            ex,
            [req]() -> asio::awaitable<void> {
                asio::steady_timer t{co_await asio::this_coro::executor};
                t.expires_after(30ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                req->cancel();
            },
            asio::detached);

        auto [ec, result] = co_await req->body().async_read(asio::as_tuple(asio::use_awaitable));
        first_ec = ec;
        first_done = true;
        // Give the queued reader's posted cancellation completion a turn.
        asio::steady_timer t{ex};
        t.expires_after(50ms);
        co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
        transport->release();
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(first_done);
    CHECK(first_ec == asio::error::operation_aborted);
    CHECK(second_done);
    CHECK(second_ec == asio::error::operation_aborted);
}

TEST_CASE("cancel-verify: request cancel aborts an h2 writer parked on backpressure", "[cancel-verify]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->hold_open();
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += post_headers_frame(1);
    transport->push(std::move(in));

    bool write_done = false;
    error_code write_ec = make_error_code(asio::error::would_block);

    auto dispatcher = [&](RequestPtr req, ResponsePtr res, SslHandle) -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        asio::co_spawn(
            ex,
            [req, &transport, &write_done]() -> asio::awaitable<void> {
                asio::steady_timer t{co_await asio::this_coro::executor};
                // By now the producer has queued past kOutHighWatermark and is
                // parked in await_out_space().
                t.expires_after(100ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                req->cancel();
                t.expires_after(100ms);
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                (void)write_done;
                transport->release();
            },
            asio::detached);

        co_await res->status(status::ok).async_begin(asio::as_tuple(asio::use_awaitable));
        const std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 40; ++i) {
            auto [ec] = co_await res->async_write(chunk, asio::as_tuple(asio::use_awaitable));
            if (ec) {
                write_ec = ec;
                write_done = true;
                co_return;
            }
        }
        write_ec = error_code{};
        write_done = true;
    };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(write_done);
    CHECK(write_ec == asio::error::operation_aborted);
}
