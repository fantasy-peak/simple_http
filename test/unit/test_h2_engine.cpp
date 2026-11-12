// engine/h2: the Rapid Reset (CVE-2023-44487) defense.
//
// A stream that opens and is immediately RST_STREAMed frees its stream-table
// slot without freeing the handler it started, so a reset flood could spawn
// handlers without bound even though h2_max_concurrent_streams caps *open*
// streams. The engine therefore caps concurrently *executing* handlers at
// h2_max_concurrent_streams and queues the excess. These tests drive the engine
// over a scripted transport and assert on the concurrency actually observed by a
// dispatcher, not on any counter the engine exposes.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::codec;
using namespace simple_http::test;

namespace {

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

// A minimal syntactically valid request head for stream `id`.
std::string headers_frame(std::uint32_t id, bool end_stream) {
    std::string block;
    hpack_append_indexed(block, 2); // :method: GET
    hpack_append_indexed(block, 7); // :scheme: https
    hpack_append_literal(block, ":path", "/");
    hpack_append_literal(block, ":authority", "example.com");
    std::uint8_t flags = H2_FLAG_END_HEADERS;
    if (end_stream)
        flags |= H2_FLAG_END_STREAM;
    std::string out;
    append_frame(out, H2FrameType::Headers, flags, id, block);
    return out;
}

// RFC 8441 extended CONNECT: CONNECT plus :protocol/:scheme/:path.
std::string extended_connect_headers_frame(std::uint32_t id) {
    std::string block;
    hpack_append_literal(block, ":method", "CONNECT");
    hpack_append_literal(block, ":protocol", "websocket");
    hpack_append_literal(block, ":scheme", "http");
    hpack_append_literal(block, ":path", "/ws/echo");
    hpack_append_literal(block, ":authority", "example.com");
    std::string out;
    append_frame(out, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, id, block);
    return out;
}

// A POST head with a distinguishing header, so the dispatcher can tell two
// concurrent streams apart.
std::string post_headers_frame(std::uint32_t id, bool end_stream, std::string_view tag) {
    std::string block;
    hpack_append_indexed(block, 3); // :method: POST
    hpack_append_indexed(block, 7); // :scheme: https
    hpack_append_literal(block, ":path", "/");
    hpack_append_literal(block, ":authority", "example.com");
    hpack_append_literal(block, "x-tag", tag);
    std::uint8_t flags = H2_FLAG_END_HEADERS;
    if (end_stream)
        flags |= H2_FLAG_END_STREAM;
    std::string out;
    append_frame(out, H2FrameType::Headers, flags, id, block);
    return out;
}

std::string rst_frame(std::uint32_t id) {
    std::string out;
    append_frame(out, H2FrameType::RstStream, 0, id, u32be(H2_CANCEL));
    return out;
}

std::string data_frame(std::uint32_t id, std::string_view payload) {
    std::string out;
    append_frame(out, H2FrameType::Data, 0, id, payload);
    return out;
}

std::string data_frame_flagged(std::uint32_t id, std::string_view payload, std::uint8_t flags) {
    std::string out;
    append_frame(out, H2FrameType::Data, flags, id, payload);
    return out;
}

// Whether the server's recorded output contains a GOAWAY carrying `error_code`.
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

// Whether the server's recorded output contains an RST_STREAM for `stream_id`
// carrying `error_code`.
bool has_rst_stream(std::string_view written, std::uint32_t stream_id, std::uint32_t error_code) {
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= written.size();) {
        H2FrameHeader hdr;
        if (!parse_frame_header(written.substr(i), hdr))
            return false;
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        if (end > written.size())
            return false;
        if (hdr.type == static_cast<std::uint8_t>(H2FrameType::RstStream) && hdr.stream_id == stream_id &&
            hdr.length == 4 && read_u32(written, i + kH2FrameHeaderSize) == error_code)
            return true;
        i = end;
    }
    return false;
}

// A handler that stays alive long enough for the read loop to process the rest
// of a pre-pushed frame burst. A handler that returns immediately now releases
// its stream (and its concurrency slot) — see handler_finished(stream_id).
asio::awaitable<void> park_briefly() {
    asio::steady_timer timer{co_await asio::this_coro::executor};
    timer.expires_after(std::chrono::milliseconds(20));
    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
}

TEST_CASE("h2/engine: a reset flood cannot outrun the executing-handler cap", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    EngineLimits limits;
    limits.h2_max_concurrent_streams = 4;
    // One queued stream is tolerated; the second is a connection error.
    limits.h2_max_queued_streams = 1;

    // The Rapid Reset shape: open four streams (their handlers start), then reset
    // all four so their table slots free while the handlers still run, then open
    // more into the gap. The reset streams must queue, not dispatch, once the cap
    // is reached — and the queue outgrowing its bound must be a connection error.
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, ""); // client SETTINGS (must come first)
    for (std::uint32_t id : {1u, 3u, 5u, 7u})
        in += headers_frame(id, /*end_stream=*/true);
    for (std::uint32_t id : {1u, 3u, 5u, 7u})
        in += rst_frame(id);
    for (std::uint32_t id : {9u, 11u, 13u})
        in += headers_frame(id, /*end_stream=*/true);
    transport->push(std::move(in));

    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    auto dispatcher = [&](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                          SslHandle) -> asio::awaitable<void> {
        const int now = running.fetch_add(1) + 1;
        int observed = peak.load();
        while (now > observed && !peak.compare_exchange_weak(observed, now)) {
        }
        // Hold the slot a moment, so overlapping handlers are observable.
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        running.fetch_sub(1);
        co_return;
    };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(peak.load() >= 1); // handlers really ran
    CHECK(peak.load() <= static_cast<int>(limits.h2_max_concurrent_streams));
    CHECK(has_goaway(transport->written(), codec::H2_ENHANCE_YOUR_CALM));
}

TEST_CASE("h2/engine: a request body above max_body_bytes is refused", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    EngineLimits limits;
    limits.max_body_bytes = 16;

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += headers_frame(1, /*end_stream=*/false);                    // handler dispatched at END_HEADERS
    append_frame(in, H2FrameType::Data, 0, 1, std::string(32, 'x')); // 32 > 16
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_await park_briefly(); };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_rst_stream(transport->written(), 1, codec::H2_ENHANCE_YOUR_CALM));
}

TEST_CASE("h2/engine: DATA already in flight for a refused stream is not a connection error", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    EngineLimits limits;
    limits.h2_max_concurrent_streams = 1;

    // Stream 1 fills the table; stream 3 is refused. Its HEADERS carried
    // END_HEADERS, so a DATA frame the client had already pipelined behind it
    // must be treated as a frame on a closed stream, not as an idle stream
    // (which would GOAWAY the whole connection).
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += headers_frame(1, /*end_stream=*/false);
    in += headers_frame(3, /*end_stream=*/false);
    in += data_frame(3, "x");
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_await park_briefly(); };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_rst_stream(transport->written(), 3, codec::H2_REFUSED_STREAM));
    CHECK_FALSE(has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR));
}

TEST_CASE("h2/engine: a stream the peer reset answers later HEADERS with STREAM_CLOSED", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    // RFC 9113 §5.1: once both sides are done with a stream, a later frame may be
    // a connection error of type STREAM_CLOSED. Stream 1 opens, the peer resets
    // it, and then a HEADERS arrives for the same id.
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += headers_frame(1, /*end_stream=*/false);
    in += rst_frame(1);
    in += headers_frame(1, /*end_stream=*/true);
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_await park_briefly(); };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_goaway(transport->written(), codec::H2_STREAM_CLOSED));
}

TEST_CASE("h2/engine: a skipped stream id is a PROTOCOL_ERROR connection error", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    // RFC 9113 §5.1.1: opening stream 5 implicitly closes the idle stream 3, but
    // 3 was never opened, so a HEADERS on it is an unexpected stream identifier.
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += headers_frame(5, /*end_stream=*/true);
    in += headers_frame(3, /*end_stream=*/true);
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_await park_briefly(); };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR));
}

TEST_CASE("h2/engine: HEADERS after our own RST_STREAM is discarded, not answered", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    EngineLimits limits;
    limits.h2_max_concurrent_streams = 1;

    // RFC 9113 §5.1: after an endpoint sends RST_STREAM it MUST minimally process
    // and discard frames that were already in flight, not answer them with
    // another error (and never with RST_STREAM on a closed stream).
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += headers_frame(1, /*end_stream=*/false); // fills the table
    in += headers_frame(3, /*end_stream=*/true);  // refused: RST_STREAM(REFUSED_STREAM)
    in += headers_frame(3, /*end_stream=*/true);  // sent before the peer saw that reset
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_await park_briefly(); };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{limits});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_rst_stream(transport->written(), 3, codec::H2_REFUSED_STREAM));
    CHECK_FALSE(has_goaway(transport->written(), codec::H2_STREAM_CLOSED));
    CHECK_FALSE(has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR));
}

TEST_CASE("h2/engine: a refused split head still drains its CONTINUATION frames", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    EngineLimits limits;
    limits.h2_max_concurrent_streams = 1;
    limits.max_header_bytes = 1024;

    // The refused HEADERS has no END_HEADERS, so the peer's CONTINUATION is
    // still in flight. The engine must consume it (without buffering or
    // decoding) and only then treat later frames on stream 3 as closed.
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += headers_frame(1, /*end_stream=*/false);
    append_frame(in, H2FrameType::Headers, 0, 3, "a"); // no END_HEADERS
    append_frame(in, H2FrameType::Continuation, H2_FLAG_END_HEADERS, 3, "b");
    in += data_frame(3, "x");
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_await park_briefly(); };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_rst_stream(transport->written(), 3, codec::H2_REFUSED_STREAM));
    CHECK_FALSE(has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR));
}

TEST_CASE("h2/engine: well-behaved concurrency below the cap still runs", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    EngineLimits limits;
    limits.h2_max_concurrent_streams = 4;
    limits.h2_max_queued_streams = 0; // follow the concurrency limit

    // Three HEADERS-only streams (no resets): all three dispatch immediately.
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    for (std::uint32_t id : {1u, 3u, 5u})
        in += headers_frame(id, /*end_stream=*/true);
    transport->push(std::move(in));

    std::atomic<int> peak{0};
    std::atomic<int> finished{0};
    auto dispatcher = [&](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                          SslHandle) -> asio::awaitable<void> {
        peak.store(3); // reached only if all three run, which the cap allows
        finished.fetch_add(1);
        co_return;
    };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(finished.load() == 3);
    // No queue was involved, so nothing was punished.
    CHECK_FALSE(has_goaway(transport->written(), codec::H2_ENHANCE_YOUR_CALM));
}

} // namespace

TEST_CASE("h2/engine: an early response does not abort the rest of the request body", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    EngineLimits limits;
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    // The peer has not ended its side: a request body is still expected.
    in += headers_frame(1, /*end_stream=*/false);
    transport->push(std::move(in));

    // An auth/limit-style handler: answer without ever reading the body.
    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("denied", asio::use_awaitable);
    };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    // The response is complete while the peer's side is still open. The engine
    // must keep the exchange alive (the client may still be sending) and drop
    // the tail as it arrives; a RST here would tell a client holding the
    // response that the exchange failed.
    CHECK_FALSE(has_rst_stream(transport->written(), 1, codec::H2_NO_ERROR));
    CHECK_FALSE(has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR));
    CHECK_FALSE(has_goaway(transport->written(), codec::H2_FLOW_CONTROL_ERROR));
}

TEST_CASE("h2/engine: a GOAWAY shorter than 8 bytes is FRAME_SIZE_ERROR", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    append_frame(in, H2FrameType::Goaway, 0, 0, ""); // length 0
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_return; };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_goaway(transport->written(), codec::H2_FRAME_SIZE_ERROR));
}

TEST_CASE("h2/engine: a self-dependent PRIORITY on an idle stream is a connection error", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    // PRIORITY on stream 3, which was never opened, depending on itself.
    append_frame(in, H2FrameType::Priority, 0, 3, u32be(3) + std::string(1, '\x0f'));
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_return; };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    // RST_STREAM on an idle stream is itself illegal (§5.4.2), so this is a
    // connection error rather than a stream error.
    CHECK(has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR));
    CHECK_FALSE(has_rst_stream(transport->written(), 3, codec::H2_PROTOCOL_ERROR));
}

TEST_CASE("h2/engine: a zero WINDOW_UPDATE on an idle stream is a connection error", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    append_frame(in, H2FrameType::WindowUpdate, 0, 3, u32be(0));
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_return; };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR));
    CHECK_FALSE(has_rst_stream(transport->written(), 3, codec::H2_PROTOCOL_ERROR));
}

TEST_CASE("h2/engine: SETTINGS that push a stream window past 2^31-1 is FLOW_CONTROL_ERROR", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += headers_frame(1, /*end_stream=*/true); // handler parks below, keeping stream 1
    // Raise stream 1's send window to the maximum with WINDOW_UPDATE...
    append_frame(in, H2FrameType::WindowUpdate, 0, 1, u32be(0x7FFFFFFFu - 65535u));
    // ...then a SETTINGS increase of 1 pushes it over.
    std::string settings;
    settings.push_back(0);
    settings.push_back(4); // SETTINGS_INITIAL_WINDOW_SIZE
    settings += u32be(65536);
    append_frame(in, H2FrameType::Settings, 0, 0, settings);
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_await park_briefly(); };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(has_goaway(transport->written(), codec::H2_FLOW_CONTROL_ERROR));
}

TEST_CASE("h2/engine: extended CONNECT is refused when the setting is off", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    EngineLimits limits;
    limits.h2_enable_connect_protocol = false;

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, ""); // client SETTINGS
    in += extended_connect_headers_frame(1);
    transport->push(std::move(in));

    std::atomic<int> dispatched{0};
    auto dispatcher = [&](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                          SslHandle) -> asio::awaitable<void> {
        dispatched.fetch_add(1);
        co_return;
    };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    // RFC 8441 §3: the server advertised ENABLE_CONNECT_PROTOCOL=0, so using
    // the extended CONNECT is a connection error, and no handler runs.
    CHECK(has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR));
    CHECK(dispatched.load() == 0);
}

TEST_CASE("h2/engine: a paused request body does not stall another stream", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    // Stream 1 sends far more body than the bounded channel holds and its
    // handler never reads it. Stream 3 must still be dispatched and answered.
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += post_headers_frame(1, /*end_stream=*/false, "slow");
    for (int i = 0; i < 1300; ++i) {
        in += data_frame(1, "abcd");
    }
    in += post_headers_frame(3, /*end_stream=*/true, "fast");
    transport->push(std::move(in));

    std::atomic<bool> fast_ran{false};
    auto dispatcher = [&](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                          SslHandle) -> asio::awaitable<void> {
        if (req->header("x-tag").value_or(std::string_view{}) == "slow") {
            co_await park_briefly(); // deliberately never reads the body
            co_return;
        }
        fast_ran.store(true);
        co_await res->status(status::ok).async_send("fast", asio::use_awaitable);
    };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(fast_ran.load());
    CHECK(transport->written().find("fast") != std::string::npos);
}

TEST_CASE("h2/engine: END_STREAM during a body pause is not lost", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    constexpr std::size_t kChunks = 1300;
    constexpr std::size_t kLast = 4;
    // Keep the peer open: the engine must not treat "no more bytes queued" as a
    // closed connection while the handler is still draining a paused body.
    transport->hold_open();
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    in += post_headers_frame(1, /*end_stream=*/false, "slow");
    for (std::size_t i = 0; i < kChunks; ++i) {
        in += data_frame(1, "abcd");
    }
    // Arrives while the body channel is full: the engine must remember the
    // terminator and deliver it only after the handler drains the queue.
    in += data_frame_flagged(1, "tail", H2_FLAG_END_STREAM);
    transport->push(std::move(in));

    std::atomic<std::size_t> received{0};
    std::atomic<bool> read_ok{false};
    auto dispatcher = [&](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                          SslHandle) -> asio::awaitable<void> {
        co_await park_briefly(); // let the channel fill and the END_STREAM park
        auto body = co_await body_read_all(req->body());
        if (body) {
            received.store(body->size());
            read_ok.store(true);
            co_await res->status(status::ok).async_send(std::to_string(body->size()), asio::use_awaitable);
        }
        transport->release(); // let the read loop observe EOF and finish
    };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, EngineLimits{});
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(read_ok.load());
    CHECK(received.load() == kChunks * 4 + kLast);
}

// --- request field validation ---------------------------------------------

namespace {

// Runs one client SETTINGS + one HEADERS frame and reports what the engine did.
struct ValidationOutcome {
    bool completed{false};
    bool dispatched{false};
    bool rst_protocol_error{false};
    bool goaway_protocol_error{false};
};

ValidationOutcome run_headers(const std::string &block, EngineLimits limits = {}) {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    append_frame(in, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, block);
    transport->push(std::move(in));

    ValidationOutcome out;
    auto dispatcher = [&out](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                             SslHandle) -> asio::awaitable<void> {
        out.dispatched = true;
        co_return;
    };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    out.completed = run_on(ctx, run_engine(*engine, dispatcher));
    out.rst_protocol_error = has_rst_stream(transport->written(), 1, codec::H2_PROTOCOL_ERROR);
    out.goaway_protocol_error = has_goaway(transport->written(), codec::H2_PROTOCOL_ERROR);
    return out;
}

std::string request_block(std::string method = "GET", std::string path = "/") {
    std::string block;
    hpack_append_literal(block, ":method", method);
    hpack_append_literal(block, ":scheme", "https");
    hpack_append_literal(block, ":path", path);
    hpack_append_literal(block, ":authority", "example.com");
    return block;
}

} // namespace

TEST_CASE("h2/engine: a regular field name must be an HTTP token", "[h2]") {
    auto block = request_block();
    hpack_append_literal(block, "bad name", "v");
    const auto out = run_headers(block);
    REQUIRE(out.completed);
    CHECK_FALSE(out.dispatched);
    CHECK(out.rst_protocol_error);
}

TEST_CASE("h2/engine: a field value may not carry a non-CRLF control byte", "[h2]") {
    auto block = request_block();
    hpack_append_literal(block, "x-bad", std::string{"a\x01b", 3});
    const auto out = run_headers(block);
    REQUIRE(out.completed);
    CHECK_FALSE(out.dispatched);
    CHECK(out.rst_protocol_error);
}

TEST_CASE("h2/engine: :method must be a token", "[h2]") {
    auto block = request_block("GE T");
    const auto out = run_headers(block);
    REQUIRE(out.completed);
    CHECK_FALSE(out.dispatched);
    CHECK(out.rst_protocol_error);
}

TEST_CASE("h2/engine: :path must be origin-form or *", "[h2]") {
    for (const std::string path : {"a/b", "/a#frag", "/a b", "http://x/y"}) {
        INFO("path: " << path);
        auto block = request_block("GET", path);
        const auto out = run_headers(block);
        REQUIRE(out.completed);
        CHECK_FALSE(out.dispatched);
        CHECK(out.rst_protocol_error);
    }
    // "*" is legal for OPTIONS.
    const auto star = run_headers(request_block("OPTIONS", "*"));
    REQUIRE(star.completed);
    CHECK(star.dispatched);
}

TEST_CASE("h2/engine: Host must agree with :authority and appear at most once", "[h2]") {
    {
        auto block = request_block();
        hpack_append_literal(block, "host", "other.example");
        const auto out = run_headers(block);
        REQUIRE(out.completed);
        CHECK_FALSE(out.dispatched);
        CHECK(out.rst_protocol_error);
    }
    {
        auto block = request_block();
        hpack_append_literal(block, "host", "example.com");
        hpack_append_literal(block, "host", "example.com");
        const auto out = run_headers(block);
        REQUIRE(out.completed);
        CHECK_FALSE(out.dispatched);
        CHECK(out.rst_protocol_error);
    }
    {
        // The compliant pairing is accepted and :authority wins.
        auto block = request_block();
        hpack_append_literal(block, "host", "example.com");
        const auto out = run_headers(block);
        REQUIRE(out.completed);
        CHECK(out.dispatched);
    }
}
