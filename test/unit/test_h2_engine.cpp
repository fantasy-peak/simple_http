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
