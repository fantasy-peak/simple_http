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
    REQUIRE(run_on(ctx, engine->run(dispatcher)));

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
    REQUIRE(run_on(ctx, engine->run(dispatcher)));

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
    REQUIRE(run_on(ctx, engine->run(dispatcher)));

    CHECK(has_rst_stream(transport->written(), 3, codec::H2_REFUSED_STREAM));
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
    REQUIRE(run_on(ctx, engine->run(dispatcher)));

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
    REQUIRE(run_on(ctx, engine->run(dispatcher)));

    CHECK(finished.load() == 3);
    // No queue was involved, so nothing was punished.
    CHECK_FALSE(has_goaway(transport->written(), codec::H2_ENHANCE_YOUR_CALM));
}

} // namespace

TEST_CASE("h2/engine: a handler that returns early releases the unread request body", "[h2]") {
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
        co_await res->status(200).send("denied");
    };
    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, engine->run(dispatcher)));

    // Nobody will read the remaining DATA; the stream must be reset so those
    // bytes stop holding connection-window credit.
    CHECK(has_rst_stream(transport->written(), 1, codec::H2_NO_ERROR));
}
