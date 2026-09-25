// proto/compressing_writer.h: the decorator's method mapping and its boundary
// rules. Driven against the recording FakeResponseWriter so every decision -
// what reached the engine, and with which headers - is observable.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

CompressionConfig enabled(std::uint64_t min_bytes = 64) {
    CompressionConfig config;
    config.enabled = true;
    config.min_bytes = min_bytes;
    return config;
}

// Repetitive text: compresses to a fraction of its size.
std::string big_text() {
    std::string body;
    for (int i = 0; i < 200; ++i) {
        body += "line of compressible text;";
    }
    return body;
}

// Deterministic pseudo-random bytes: incompressible, so gzip makes them larger.
std::string incompressible(std::size_t n) {
    std::string out;
    out.reserve(n);
    std::uint32_t x = 12345;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        out.push_back(static_cast<char>((x >> 16) & 0xFF));
    }
    return out;
}

Headers text_headers() {
    Headers headers;
    headers.add("content-type", "text/plain");
    return headers;
}

// Runs one writer call and asserts it reported success.
template <typename T>
void ok(asio::io_context& ctx, asio::awaitable<T> op) {
    auto result = run_on(ctx, std::move(op));
    REQUIRE(result.has_value());
    CHECK_FALSE(static_cast<bool>(*result));
}

}  // namespace

TEST_CASE("compressing_writer: off means the writer is not wrapped", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseWriter>();

    // Disabled, and enabled-but-nothing-acceptable, both hand back the same
    // object: no allocation, no behaviour change.
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), CompressionConfig{}, "gzip, br", false) == inner);
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), enabled(), "", false) == inner);
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), enabled(), "identity", false) == inner);
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), enabled(), "deflate", false) == inner);
}

TEST_CASE("compressing_writer: a one-shot text response is compressed", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseWriter>();
    CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);

    const std::string body = big_text();
    ok(ctx, writer.send(200, text_headers(), body));

    CHECK(inner->calls == 1);
    CHECK(inner->header("content-encoding") == "gzip");
    CHECK(inner->has_header("vary"));
    CHECK(inner->last_body.size() < body.size());
    CHECK(decompress_all("gzip", inner->last_body) == body);
}

TEST_CASE("compressing_writer: Content-Length follows the protocol", "[compression]") {
    const std::string body = big_text();

    SECTION("HTTP/2 gets the compressed length (its engine never recomputes)") {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseWriter>();
        inner->ver = Version::Http2;
        CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);
        ok(ctx, writer.send(200, text_headers(), body));

        REQUIRE(inner->has_header("content-length"));
        CHECK(inner->header("content-length") == std::to_string(inner->last_body.size()));
    }

    SECTION("HTTP/1.x gets none (its engine writes its own)") {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseWriter>();
        inner->ver = Version::Http11;
        CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);
        ok(ctx, writer.send(200, text_headers(), body));

        // A second Content-Length would be a smuggling vector downstream.
        CHECK_FALSE(inner->has_header("content-length"));
    }
}

TEST_CASE("compressing_writer: headers that forbid or predate compression", "[compression]") {
    const std::string body = big_text();

    const auto run_case = [&](Headers headers, int status, bool head, const char* label) {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseWriter>();
        CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", head);
        ok(ctx, writer.send(status, std::move(headers), body));

        INFO(label);
        CHECK_FALSE(inner->has_header("content-encoding"));
        CHECK(inner->last_body == body);  // untouched
    };

    SECTION("already encoded by someone else") {
        // Encoded as "br" on purpose: if the decorator compressed anyway and
        // rewrote the field, it would read back as "gzip" and this catches it.
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseWriter>();
        CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);

        Headers h = text_headers();
        h.add("content-encoding", "br");
        const std::string already = "pretend this is brotli on the wire";
        ok(ctx, writer.send(200, std::move(h), already));

        CHECK(inner->header("content-encoding") == "br");  // preserved, not double-encoded
        CHECK(inner->last_body == already);
        CHECK_FALSE(inner->has_header("vary"));  // nothing was negotiated, so no Vary
    }
    SECTION("a byte range must stay addressable") {
        run_case(text_headers(), 206, false, "206");
    }
    SECTION("Content-Range header") {
        Headers h = text_headers();
        h.add("content-range", "bytes 0-99/1000");
        run_case(std::move(h), 200, false, "content-range");
    }
    SECTION("Cache-Control: no-transform (RFC 9110 7.7)") {
        Headers h = text_headers();
        h.add("cache-control", "public, no-transform");
        run_case(std::move(h), 200, false, "no-transform");
    }
    SECTION("HEAD") {
        run_case(text_headers(), 200, true, "head");
    }
    SECTION("a type that is already entropy-coded") {
        Headers h;
        h.add("content-type", "image/png");
        run_case(std::move(h), 200, false, "image/png");
    }
    SECTION("no content-type at all") {
        run_case(Headers{}, 200, false, "no content-type");
    }
}

TEST_CASE("compressing_writer: small and incompressible bodies pass through", "[compression]") {
    SECTION("below min_bytes") {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseWriter>();
        CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(1024), "gzip", false);
        const std::string body = "tiny";
        ok(ctx, writer.send(200, text_headers(), body));

        CHECK_FALSE(inner->has_header("content-encoding"));
        CHECK(inner->last_body == body);
    }

    SECTION("compression would make it bigger") {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseWriter>();
        CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);
        const std::string body = incompressible(8192);
        ok(ctx, writer.send(200, text_headers(), body));

        // The win is checked before any header is touched, so this is a clean
        // pass-through rather than a body that grew.
        CHECK_FALSE(inner->has_header("content-encoding"));
        CHECK_FALSE(inner->has_header("vary"));
        CHECK(inner->last_body == body);
    }
}

TEST_CASE("compressing_writer: Vary and ETag are maintained", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseWriter>();
    CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);

    Headers headers = text_headers();
    headers.add("vary", "Origin");
    headers.add("etag", "\"v1\"");
    ok(ctx, writer.send(200, std::move(headers), big_text()));

    // Merged, not overwritten: the upstream's Vary still has to hold.
    CHECK(inner->header("vary") == "Origin, Accept-Encoding");
    // The body changed, so the strong validator no longer applies as-is.
    CHECK(inner->header("etag") == "W/\"v1\"");
}

TEST_CASE("compressing_writer: streaming", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseWriter>();
    inner->ver = Version::Http2;  // so the length rule is exercised the strict way
    CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);
    ok(ctx, writer.send_headers(200, text_headers()));

    const std::string part1 = big_text();
    const std::string part2 = big_text();
    ok(ctx, writer.send_chunk(part1));
    ok(ctx, writer.send_last(part2));

    CHECK(inner->begun);
    CHECK(inner->finished);
    CHECK(inner->header("content-encoding") == "gzip");
    // A streamed length is unknown, so it must not be asserted.
    CHECK_FALSE(inner->has_header("content-length"));

    std::string streamed;
    for (const std::string& chunk : inner->chunks) {
        // An empty chunk would make HTTP/1.x emit its chunked terminator early.
        CHECK_FALSE(chunk.empty());
        streamed += chunk;
    }
    CHECK(decompress_all("gzip", streamed) == part1 + part2);
}

TEST_CASE("compressing_writer: a codec that buffers emits nothing, not an empty chunk", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseWriter>();
    CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);
    ok(ctx, writer.send_headers(200, text_headers()));

    // One byte at a time: the codec will not produce output for most of these,
    // and each empty result must be swallowed rather than forwarded.
    const std::string body = big_text();
    for (char c : body) {
        ok(ctx, writer.send_chunk(std::string(1, c)));
    }
    ok(ctx, writer.send_last({}));

    std::string streamed;
    for (const std::string& chunk : inner->chunks) {
        CHECK_FALSE(chunk.empty());
        streamed += chunk;
    }
    CHECK(decompress_all("gzip", streamed) == body);
}

TEST_CASE("compressing_writer: bodyless and control responses pass straight through", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseWriter>();
    CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "gzip", false);

    Headers headers = text_headers();
    ok(ctx, writer.send_bodyless(204, headers));
    CHECK(inner->sent_bodyless);
    CHECK_FALSE(inner->has_header("content-encoding"));

    ok(ctx, writer.send_continue());
    CHECK(inner->continues == 1);

    auto open = run_on(ctx, writer.connected());
    REQUIRE(open.has_value());
    CHECK(*open);
    CHECK(writer.version() == Version::Http11);
    REQUIRE(run_on(ctx, writer.close()));
    CHECK_FALSE(inner->open);
    CHECK_FALSE(inner->has_header("content-encoding"));
}

TEST_CASE("compressing_writer: brotli is used when negotiated", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseWriter>();
    CompressingResponseWriter writer(inner, ctx.get_executor(), enabled(), "br", false);

    const std::string body = big_text();
    ok(ctx, writer.send(200, text_headers(), body));

    CHECK(inner->header("content-encoding") == "br");
    CHECK(decompress_all("br", inner->last_body) == body);
}
