// Regression tests for the 2026-11-18 audit: each case reproduces the defect
// before the fix and pins the corrected behaviour afterwards.

#include <array>
#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../support/static_fixture.h"
#include "simple_http.h"
#include "test_support.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif

using namespace simple_http;
using namespace simple_http::codec;
using namespace simple_http::test;

namespace {

namespace asio = boost::asio;

void append_frame(std::string &out, H2FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                  std::string_view payload) {
    serialize_frame_header(out, static_cast<std::uint32_t>(payload.size()), static_cast<std::uint8_t>(type), flags,
                           stream_id);
    out.append(payload);
}

std::string h2_preface_with_settings() {
    std::string out{codec::kH2ClientPreface};
    append_frame(out, H2FrameType::Settings, 0, 0, "");
    return out;
}

bool has_goaway(std::string_view written, std::uint32_t error_code) {
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= written.size();) {
        H2FrameHeader hdr;
        if (!parse_frame_header(written.substr(i), hdr))
            return false;
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        if (end > written.size())
            return false;
        if (hdr.type == static_cast<std::uint8_t>(H2FrameType::Goaway) && hdr.length == 8 &&
            read_u32(written, i + kH2FrameHeaderSize + 4) == error_code)
            return true;
        i = end;
    }
    return false;
}

bool has_window_update(std::string_view written, std::uint32_t stream_id) {
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= written.size();) {
        H2FrameHeader hdr;
        if (!parse_frame_header(written.substr(i), hdr))
            return false;
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        if (end > written.size())
            return false;
        if (hdr.type == static_cast<std::uint8_t>(H2FrameType::WindowUpdate) && hdr.stream_id == stream_id &&
            hdr.length == 4 && read_u32(written, i + kH2FrameHeaderSize) != 0)
            return true;
        i = end;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// M1: a trailing HEADERS block must not reserve over the request headers a
// running handler is still viewing.
// ---------------------------------------------------------------------------
TEST_CASE("review/h2: trailers do not invalidate handler-held request header views", "[h2][review]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->hold_open();

    std::string first;
    {
        std::string block;
        hpack_append_indexed(block, 3); // :method: POST
        hpack_append_indexed(block, 7); // :scheme: https
        hpack_append_literal(block, ":path", "/");
        hpack_append_literal(block, ":authority", "example.com");
        hpack_append_literal(block, "x-tag", "keep-me");
        append_frame(first, H2FrameType::Headers, H2_FLAG_END_HEADERS, 1, block);
    }

    // Trailer block with plenty of fields: reserving this many would reallocate
    // the request header vector.
    std::string trailers;
    {
        std::string block;
        for (int i = 0; i < 64; ++i) {
            hpack_append_literal(block, "x-trailer-" + std::to_string(i), "value-" + std::to_string(i));
        }
        append_frame(trailers, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, block);
    }

    bool pointer_stable = false;
    bool checked = false;
    auto dispatcher = simple_http::detail::make_callback_handler(
        [&](RequestPtr req, ResponsePtr, SslHandle) -> asio::awaitable<void> {
            auto tag = req->header("x-tag");
            const char *before = tag ? tag->data() : nullptr;
            transport->push(trailers);
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(std::chrono::milliseconds(50));
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            auto after = req->header("x-tag");
            pointer_stable = before != nullptr && after.has_value() && after->data() == before;
            checked = after.has_value();
            transport->release();
            co_return;
        });

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    transport->push(h2_preface_with_settings() + first);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));
    CHECK(checked);
    CHECK(pointer_stable);
}

// ---------------------------------------------------------------------------
// M2: a synchronously throwing WebSocket route must not escape io_context.run().
// ---------------------------------------------------------------------------
TEST_CASE("review/h2: a throwing ws route does not escape the io_context", "[h2][ws][review]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string block;
    hpack_append_literal(block, ":method", "CONNECT");
    hpack_append_literal(block, ":protocol", "websocket");
    hpack_append_literal(block, ":scheme", "http");
    hpack_append_literal(block, ":path", "/ws/echo");
    hpack_append_literal(block, ":authority", "example.com");
    std::string in = h2_preface_with_settings();
    append_frame(in, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, block);
    transport->push(std::move(in));

    WsHandlerFn route = [](RequestPtr, ResponsePtr, SslHandle, WsUpgrade, WsUpgradeCompletion) {
        throw std::runtime_error("ws middleware boom");
    };
    WsLookup lookup = [route](std::string_view) -> std::optional<WsHandlerFn> { return route; };
    Dispatcher dispatch = [](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) { std::move(done)(); };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    bool threw = false;
    try {
        engine->run(std::move(dispatch), {}, std::move(lookup), {}, {}, [] {});
        ctx.run_for(std::chrono::milliseconds(200));
    } catch (const std::exception &) {
        threw = true;
    }
    CHECK_FALSE(threw);
    CHECK(transport->closed());
}

// ---------------------------------------------------------------------------
// M3: h1 must not start a TLS shutdown while a response write is in flight.
// ---------------------------------------------------------------------------
namespace {
class ParkedWriteTransport {
  public:
    explicit ParkedWriteTransport(asio::any_io_executor ex) : m_ex(std::move(ex)) {}

    void push(std::string bytes) { m_in = std::move(bytes); }

    bool shutdown_while_write() const { return m_shutdown_while_write; }

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
            simple_http::detail::make_executor_initiation(std::move(init), m_ex), token);
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
            simple_http::detail::make_executor_initiation(std::move(init), m_ex), token);
    }

    template <typename Token> auto async_write(ConstByteSpan, Token &&token) {
        ++m_inflight;
        return park(std::move(token));
    }

    template <typename Token> auto async_write_seq(boost::span<const ConstByteSpan>, Token &&token) {
        ++m_inflight;
        return park(std::move(token));
    }

    template <typename Token> auto async_shutdown(Token &&token) {
        if (m_inflight > 0)
            m_shutdown_while_write = true;
        auto init = [](auto handler) { std::move(handler)(error_code{}); };
        return asio::async_initiate<Token, void(error_code)>(
            simple_http::detail::make_executor_initiation(std::move(init), m_ex), token);
    }

    auto get_executor() { return m_ex; }
    asio::ip::tcp::endpoint peer() const { return {}; }
    SslHandle tls_handle() const { return std::nullopt; }

    void close() {
        auto pending = std::move(m_pending);
        m_pending.clear();
        for (auto &handler : pending) {
            --m_inflight;
            if (handler)
                std::move(handler)(make_error_code(asio::error::operation_aborted), 0);
        }
    }

  private:
    template <typename Token> auto park(Token &&token) {
        auto init = [this](auto handler) { m_pending.emplace_back(std::move(handler)); };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            simple_http::detail::make_executor_initiation(std::move(init), m_ex), token);
    }

    asio::any_io_executor m_ex;
    std::string m_in;
    std::size_t m_pos{0};
    int m_inflight{0};
    bool m_shutdown_while_write{false};
    std::vector<asio::any_completion_handler<void(error_code, std::size_t)>> m_pending;
};
} // namespace

TEST_CASE("review/h1: shutdown waits for an in-flight response write", "[h1][review]") {
    asio::io_context ctx;
    auto transport = std::make_shared<ParkedWriteTransport>(ctx.get_executor());
    transport->push("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");

    Dispatcher dispatch = [](RequestPtr, ResponsePtr res, SslHandle, HandlerCompletion done) {
        res->status(status::ok).async_send("hello", asio::detached);
        std::move(done)();
    };
    auto engine = std::make_shared<Http1Engine<ParkedWriteTransport>>(transport);
    engine->run(std::move(dispatch), {}, {}, {}, {}, [] {});
    ctx.run_for(std::chrono::milliseconds(100));

    CHECK_FALSE(transport->shutdown_while_write());
    transport->close();
    ctx.restart();
    ctx.run_for(std::chrono::milliseconds(50));
}

// ---------------------------------------------------------------------------
// M6: HEAD must not read the whole file from disk.
// ---------------------------------------------------------------------------
TEST_CASE("review/static: HEAD does not touch the filesystem for a non-preloaded file", "[static][review]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg;
    cfg.table.root = fx.root_string();
    cfg.table.preload_max_file_bytes = 64; // big.bin stays on disk
    auto site = std::make_shared<StaticFiles>(std::move(cfg));
    std::string error;
    REQUIRE(site->load(error));
    CHECK(site->find("/big.bin") != nullptr);

    // Nothing the HEAD request does should need the file itself, so removing it
    // must not turn the response into a 404.
    const auto moved = fx.root_string() + ".head-moved";
    std::filesystem::rename(fx.root(), moved);

    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target("/big.bin");
    req->set_method(Method::Head);
    req->body().finish();

    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    writer->head = true;
    auto res = std::make_shared<ResponseWriter>(writer);
    auto handled = run_on(ctx, await_bool([&](auto done) { site->try_serve(req, res, std::move(done)); }));
    REQUIRE(handled.has_value());
    CHECK(*handled);
    CHECK(writer->last_status == status::ok);
    CHECK(writer->sent_bodyless);

    std::filesystem::rename(moved, fx.root());
}

// ---------------------------------------------------------------------------
// M7: a file replaced with a FIFO after the scan must not block the worker in
// open().
// ---------------------------------------------------------------------------
#if !defined(_WIN32)
TEST_CASE("review/static: an on-demand read refuses a FIFO without blocking", "[static][review]") {
    StaticFixture fx;
    const auto fifo = fx.root() / "pipe.bin";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

    std::atomic<bool> done{false};
    std::optional<std::string> result = std::string{"sentinel"};
    std::thread worker([&] {
        result = simple_http::detail::read_file_exact(fifo.string(), 16);
        done.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (done.load()) {
        worker.join();
        CHECK_FALSE(result.has_value());
    } else {
        // Pre-fix: open(O_RDONLY) on a writer-less FIFO blocks forever.
        worker.detach();
        FAIL("read_file_exact blocked on a FIFO");
    }
}
#endif

// ---------------------------------------------------------------------------
// M12: an invalid gzip level must fall back to identity, not emit an empty gzip.
// ---------------------------------------------------------------------------
TEST_CASE("review/compression: an invalid gzip level falls back to identity", "[compression][review]") {
    CompressionConfig cfg;
    cfg.enabled = true;
    cfg.gzip_level = 10;

    CHECK(make_encoder("gzip", cfg) == nullptr);
    const std::string body = "this body must not disappear";
    CHECK(compress_all("gzip", body, cfg) == body);

    cfg.gzip_level = -2;
    CHECK(make_encoder("gzip", cfg) == nullptr);
}

// ---------------------------------------------------------------------------
// L2: an out-of-range Method value must not index past the method table.
// ---------------------------------------------------------------------------
TEST_CASE("review/router: an out-of-range Method value is rejected, not indexed", "[router][review]") {
    Router router;
    Handler handler = make_handler([](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
    });
    // Pre-fix this writes past MethodTable::by_method[10].
    router.route({static_cast<Method>(200)}, "/bogus", handler);

    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target("/bogus");
    req->set_method(Method::Get);
    req->body().finish();
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    auto res = std::make_shared<ResponseWriter>(writer);
    REQUIRE(run_on(ctx, await_dispatch(router, req, res)));
    CHECK(writer->last_status == status::not_found);

    // The bit helper itself must not shift by >= 16 either.
    CHECK(method_bit(static_cast<Method>(200)) == 0);
}

// ---------------------------------------------------------------------------
// L3: a null bucket in rate_limit must pass through instead of dereferencing.
// ---------------------------------------------------------------------------
TEST_CASE("review/rate_limit: a null bucket forwards the request", "[middleware][review]") {
    asio::io_context ctx;
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target("/");
    req->set_method(Method::Get);
    req->body().finish();
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    auto res = std::make_shared<ResponseWriter>(writer);

    bool next_called = false;
    auto mw = middleware::rate_limit(std::shared_ptr<middleware::TokenBucket>{});
    bool completed = false;
    mw(
        req, res, std::nullopt,
        [&](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) {
            next_called = true;
            std::move(done)();
        },
        asio::any_completion_handler<void()>([&]() { completed = true; }));
    CHECK(next_called);
    CHECK(completed);
}

// ---------------------------------------------------------------------------
// L5: IoCtxPool::at() must fail hard outside the worker range in release too.
// ---------------------------------------------------------------------------
TEST_CASE("review/core: IoCtxPool::at rejects an out-of-range worker index", "[core][review]") {
    IoCtxPool pool(2);
    CHECK_THROWS_AS(pool.at(2), std::out_of_range);
}

// ---------------------------------------------------------------------------
// L6: more than eight scatter-gather buffers must not be silently truncated.
// ---------------------------------------------------------------------------
namespace {
class CollectStream {
  public:
    using executor_type = asio::any_io_executor;
    explicit CollectStream(executor_type ex) : m_ex(std::move(ex)) {}
    std::size_t total() const { return m_total; }
    executor_type get_executor() { return m_ex; }

    template <typename ConstBufferSequence, typename Token>
    auto async_write_some(const ConstBufferSequence &buffers, Token &&token) {
        auto init = [this, buffers](auto handler) {
            std::size_t n = 0;
            for (auto it = asio::buffer_sequence_begin(buffers); it != asio::buffer_sequence_end(buffers); ++it) {
                n += it->size();
            }
            m_total += n;
            asio::post(m_ex, [handler = std::move(handler), n]() mutable { std::move(handler)(error_code{}, n); });
        };
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            simple_http::detail::make_executor_initiation(std::move(init), m_ex), token);
    }

  private:
    executor_type m_ex;
    std::size_t m_total{0};
};
} // namespace

TEST_CASE("review/transport: more than eight write buffers are all written", "[transport][review]") {
    asio::io_context ctx;
    CollectStream stream{ctx.get_executor()};

    std::array<std::string, 10> parts;
    std::array<ConstByteSpan, 10> views{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        parts[i] = std::string(3, static_cast<char>('a' + i));
        views[i] = ConstByteSpan{reinterpret_cast<const std::byte *>(parts[i].data()), parts[i].size()};
    }
    boost::span<const ConstByteSpan> buffers{views.data(), views.size()};

    auto op = [&]() -> asio::awaitable<void> {
        co_await simple_http::detail::async_stream_write_seq(stream, buffers, asio::use_awaitable);
    };
    REQUIRE(run_on(ctx, op()));
    CHECK(stream.total() == 30);
}

// ---------------------------------------------------------------------------
// L7: async_error_only must forward the caller's cancellation slot to the
// wrapped operation.
// ---------------------------------------------------------------------------
namespace {
struct CancellationSlotProbe {
    bool *observed;
    template <typename Handler> void operator()(Handler &&handler) {
        *observed = asio::get_associated_cancellation_slot(handler).is_connected();
    }
};
} // namespace

TEST_CASE("review/transport: async_error_only forwards the cancellation slot", "[transport][review]") {
    asio::io_context ctx;
    asio::cancellation_signal signal;
    bool inner_slot_connected = false;

    simple_http::detail::async_error_only(ctx.get_executor(),
                                          asio::bind_cancellation_slot(signal.slot(), asio::detached),
                                          CancellationSlotProbe{&inner_slot_connected});
    CHECK(inner_slot_connected);
}

// ---------------------------------------------------------------------------
// body: a fully buffered push-mode body is drained iteratively, in one stack
// frame, without an async read per chunk.
// ---------------------------------------------------------------------------
TEST_CASE("review/body: read_all drains a fully buffered body", "[body][review]") {
    asio::io_context ctx;
    Body body{ctx.get_executor(), 20000};

    std::string expected;
    for (int i = 0; i < 10000; ++i) {
        std::string chunk(8, static_cast<char>('a' + i % 26));
        expected += chunk;
        REQUIRE(body.feed(std::move(chunk)));
    }
    body.finish();

    auto result = run_on(ctx, body_read_all(body));
    REQUIRE(result.has_value());
    if (*result) {
        CHECK((*result)->size() == expected.size());
        CHECK(**result == expected);
    }
}

TEST_CASE("review/body: read_all decodes a buffered compressed body", "[body][review]") {
    asio::io_context ctx;
    CompressionConfig cfg;
    cfg.enabled = true;
    const std::string plain(50000, 'x');
    const std::string packed = compress_all("gzip", plain, cfg);

    Body body{ctx.get_executor(), 4096};
    REQUIRE(body.enable_decoding(make_decoder("gzip"), 1u << 20));
    for (std::size_t i = 0; i < packed.size();) {
        const std::size_t n = std::min<std::size_t>(7, packed.size() - i);
        REQUIRE(body.feed(packed.substr(i, n)));
        i += n;
    }
    body.finish();

    auto result = run_on(ctx, body_read_all(body));
    REQUIRE(result.has_value());
    if (*result) {
        CHECK(**result == plain);
    }
}

// ---------------------------------------------------------------------------
// L1: time arithmetic on attacker-supplied claims saturates.
// ---------------------------------------------------------------------------
TEST_CASE("review/core: saturating time arithmetic", "[core][review]") {
    CHECK(saturating_add(std::numeric_limits<std::int64_t>::max(), 1) == std::numeric_limits<std::int64_t>::max());
    CHECK(saturating_add(std::numeric_limits<std::int64_t>::max() - 5, 5) == std::numeric_limits<std::int64_t>::max());
    CHECK(saturating_add(std::numeric_limits<std::int64_t>::min(), -1) == std::numeric_limits<std::int64_t>::min());
    CHECK(saturating_add(-3, 5) == 2);
}

// ---------------------------------------------------------------------------
// L9: untrusted multipart filenames are reduced to one safe component.
// ---------------------------------------------------------------------------
TEST_CASE("review/upload: sanitize_filename strips directories and dot names", "[upload][review]") {
    CHECK(sanitize_filename("../../etc/passwd") == "passwd");
    CHECK(sanitize_filename("C:\\Windows\\system.ini") == "system.ini");
    CHECK(sanitize_filename("plain.txt") == "plain.txt");
    CHECK(sanitize_filename("..").empty());
    CHECK(sanitize_filename(".").empty());
    CHECK(sanitize_filename("").empty());
    CHECK(sanitize_filename("bad\r\nname").empty());
}

// ---------------------------------------------------------------------------
// L10: a WS route that writes a response and then throws must not have a
// second response spliced onto the same connection.
// ---------------------------------------------------------------------------
TEST_CASE("review/h1: a ws route that already wrote cannot add a second response", "[h1][ws][review]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("GET /ws HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "\r\n");

    WsHandlerFn route = [](RequestPtr, ResponsePtr res, SslHandle, WsUpgrade, WsUpgradeCompletion) {
        res->status(status::ok).async_send("pre", asio::detached);
        throw std::runtime_error("boom after write");
    };
    WsLookup lookup = [route](std::string_view) -> std::optional<WsHandlerFn> { return route; };
    Dispatcher dispatch = [](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) { std::move(done)(); };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    bool threw = false;
    try {
        engine->run(std::move(dispatch), {}, std::move(lookup), {}, {}, [] {});
        ctx.run_for(std::chrono::milliseconds(200));
    } catch (const std::exception &) {
        threw = true;
    }
    CHECK_FALSE(threw);
    CHECK(transport->written().find("500 Internal Server Error") == std::string::npos);
}

// ---------------------------------------------------------------------------
// M5: concurrent stop() must have exactly one winner and join each worker once.
// ---------------------------------------------------------------------------
TEST_CASE("review/server: concurrent stop is single-winner", "[server][review]") {
    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", 0, false};
    cfg.worker_threads = 2;
    cfg.plaintext_protocols = PlaintextProtocols::Http1;
    cfg.shutdown_grace = std::chrono::milliseconds{50};
    Server server{cfg};
    REQUIRE(server.start().has_value());

    std::array<std::thread, 4> stoppers;
    for (auto &thread : stoppers) {
        thread = std::thread([&server] { server.stop(); });
    }
    for (auto &thread : stoppers) {
        thread.join();
    }
    CHECK(server.active_connections() == 0);
}

// ---------------------------------------------------------------------------
// Q1.1 / Q1.2: configured QUIC limits are clamped to ngtcp2's real bounds.
// ---------------------------------------------------------------------------
#if defined(SIMPLE_HTTP_ENABLE_HTTP3)
TEST_CASE("review/quic: payload size and CID length are clamped", "[quic][review]") {
    using namespace simple_http::quic;
    CHECK(effective_max_udp_payload_size(0) == NGTCP2_MAX_UDP_PAYLOAD_SIZE);
    CHECK(effective_max_udp_payload_size(100000) == NGTCP2_MAX_TX_UDP_PAYLOAD_SIZE);
    CHECK(effective_max_udp_payload_size(1472) == 1472);
    CHECK(effective_connection_id_length(64) == NGTCP2_MAX_CIDLEN);
    CHECK(effective_connection_id_length(8) == 8);

    QuicConnectionConfig cfg;
    cfg.max_udp_payload_size = 100000;
    auto params = make_ngtcp2_params(cfg, 1);
    CHECK(params.settings.max_tx_udp_payload_size == NGTCP2_MAX_TX_UDP_PAYLOAD_SIZE);
    CHECK(params.transport.max_udp_payload_size == NGTCP2_MAX_TX_UDP_PAYLOAD_SIZE);

    cfg.max_udp_payload_size = 0;
    params = make_ngtcp2_params(cfg, 1);
    CHECK(params.settings.max_tx_udp_payload_size == NGTCP2_MAX_UDP_PAYLOAD_SIZE);
}
#endif

// ---------------------------------------------------------------------------
// P1: a compression error after the decoded list exceeded its bound must be a
// connection error, not a silent stream reset.
// ---------------------------------------------------------------------------
TEST_CASE("review/h2: a compression error masked by list-too-large is a GOAWAY", "[h2][review]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string block;
    for (int i = 0; i < 4; ++i) {
        hpack_append_literal(block, "x-field-" + std::to_string(i), "0123456789abcdef");
    }
    // An indexed field with index 0 is invalid HPACK, and it comes after the
    // decoded list has already crossed the configured bound.
    block.push_back(static_cast<char>(0x80));
    std::string in = h2_preface_with_settings();
    append_frame(in, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, block);
    transport->push(std::move(in));

    EngineLimits limits;
    limits.h2_max_header_list_size = 64;
    Dispatcher dispatch = [](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) { std::move(done)(); };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    engine->run(std::move(dispatch), {}, {}, {}, {}, [] {});
    ctx.run_for(std::chrono::milliseconds(100));

    CHECK(has_goaway(transport->written(), H2_COMPRESSION_ERROR));
}

// ---------------------------------------------------------------------------
// P2: the body replayed by an h2c upgrade was never debited against the h2
// windows, so consuming it must not mint connection/stream credit.
// ---------------------------------------------------------------------------
TEST_CASE("review/h2: consuming an h2c-replayed body does not inflate the windows", "[h2][review]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->hold_open();

    const std::string body(70000, 'x');
    std::size_t received = 0;
    Dispatcher dispatch = simple_http::detail::make_callback_handler(
        [&](RequestPtr req, ResponsePtr res, SslHandle) -> asio::awaitable<void> {
            auto read = co_await body_read_all(req->body());
            if (read)
                received = read->size();
            co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
        });

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    engine->run_h2c(std::move(dispatch), {}, Method::Post, "POST", "/", Headers{}, body, h2_preface_with_settings(), {},
                    {}, {}, [] {});
    ctx.run_for(std::chrono::milliseconds(200));

    CHECK(received == body.size());
    CHECK_FALSE(has_window_update(transport->written(), 0));
    CHECK_FALSE(has_window_update(transport->written(), 1));

    transport->release();
    transport->close();
    ctx.restart();
    ctx.run_for(std::chrono::milliseconds(50));
}
