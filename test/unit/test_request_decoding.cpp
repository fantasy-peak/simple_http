// Server-side request decompression: Content-Encoding parsing, Body's read-side
// decoder (pull and pushed modes), the decoded-size budget, and the
// middleware::decompress_request opt-in. Everything is in memory.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

std::string packed(std::string_view encoding, std::string_view body) {
    return compress_all(encoding, body, CompressionConfig{});
}

// A pull-mode Body that yields `wire` in the given chunk sizes.
Body make_pull_body(asio::io_context &ctx, const std::string &wire, std::size_t chunk) {
    Body body{ctx.get_executor()};
    body.set_pull_provider(
        [&wire, chunk, offset = std::size_t{0}]() mutable -> asio::awaitable<std::expected<ReadResult, error_code>> {
            if (offset >= wire.size()) {
                co_return ReadResult::end();
            }
            const std::size_t n = std::min(chunk, wire.size() - offset);
            std::string out = wire.substr(offset, n);
            offset += n;
            co_return ReadResult::chunk(std::move(out));
        });
    return body;
}

std::shared_ptr<Request> make_request(asio::io_context &ctx, std::string target = "/x") {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    req->set_target(std::move(target));
    return req;
}

} // namespace

TEST_CASE("request decoding: Content-Encoding parsing", "[request-decoding]") {
    CHECK(parse_content_coding("gzip") == ContentCoding::Gzip);
    CHECK(parse_content_coding("GZIP") == ContentCoding::Gzip);
    CHECK(parse_content_coding(" x-gzip ") == ContentCoding::Gzip);
    CHECK(parse_content_coding("br") == ContentCoding::Brotli);
    CHECK(parse_content_coding("identity") == ContentCoding::Identity);
    CHECK(parse_content_coding("") == ContentCoding::Unsupported);
    CHECK(parse_content_coding("deflate") == ContentCoding::Unsupported);
    CHECK(parse_content_coding("gzip, br") == ContentCoding::Unsupported);
}

TEST_CASE("request decoding: pull-mode Body decodes streaming chunks", "[request-decoding]") {
    asio::io_context ctx;
    const std::string text = "the quick brown fox jumps over the lazy dog";
    const std::string wire = packed("gzip", text);

    for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7}, std::size_t{1024}}) {
        Body body = make_pull_body(ctx, wire, chunk);
        REQUIRE(body.enable_decoding(make_decoder("gzip"), 0));
        auto result = run_on(ctx, body.read_all());
        REQUIRE(result.has_value());
        REQUIRE(result->has_value());
        CHECK(**result == text);
    }
}

TEST_CASE("request decoding: pushed Body decodes and credits wire bytes", "[request-decoding]") {
    asio::io_context ctx;
    const std::string text = std::string(4096, 'a') + "tail";
    const std::string wire = packed("gzip", text);

    Body body{ctx.get_executor()};
    std::size_t credited = 0;
    body.set_on_consumed([&](std::size_t n) { credited += n; });
    REQUIRE(body.enable_decoding(make_decoder("gzip"), 0));

    for (std::size_t offset = 0; offset < wire.size(); offset += 13) {
        std::string chunk = wire.substr(offset, 13);
        REQUIRE(body.feed(std::move(chunk)));
    }
    body.finish();

    auto result = run_on(ctx, body.read_all());
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == text);
    // Flow control must account for the compressed wire bytes, not the decoded
    // size; otherwise h2/h3 would hand back more window than the peer spent.
    CHECK(credited == wire.size());
}

TEST_CASE("request decoding: enabled after frames are already queued", "[request-decoding]") {
    asio::io_context ctx;
    const std::string text = "late decoder installation";
    const std::string wire = packed("gzip", text);

    Body body{ctx.get_executor()};
    std::string chunk = wire;
    REQUIRE(body.feed(std::move(chunk)));
    body.finish();
    // HTTP/2 and HTTP/3 can queue raw DATA before the handler (and therefore
    // the middleware) runs. Installing the decoder before the first read must
    // still decode the queued frames.
    REQUIRE(body.enable_decoding(make_decoder("gzip"), 0));

    auto result = run_on(ctx, body.read_all());
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == text);
}

TEST_CASE("request decoding: malformed and truncated streams fail", "[request-decoding]") {
    asio::io_context ctx;
    SECTION("not gzip at all") {
        Body body{ctx.get_executor()};
        REQUIRE(body.enable_decoding(make_decoder("gzip"), 0));
        std::string bad = "definitely not gzip";
        REQUIRE(body.feed(std::move(bad)));
        body.finish();
        auto result = run_on(ctx, body.read_all());
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->has_value());
        CHECK(result->error() == asio::error::invalid_argument);
    }
    SECTION("truncated member") {
        const std::string wire = packed("gzip", "a body that will be truncated");
        Body body{ctx.get_executor()};
        REQUIRE(body.enable_decoding(make_decoder("gzip"), 0));
        std::string bad = wire.substr(0, wire.size() - 4);
        REQUIRE(body.feed(std::move(bad)));
        body.finish();
        auto result = run_on(ctx, body.read_all());
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->has_value());
        CHECK(result->error() == asio::error::invalid_argument);
    }
}

TEST_CASE("request decoding: decoded size budget is enforced", "[request-decoding]") {
    asio::io_context ctx;
    const std::string text(2 * 1024 * 1024, 'x');
    const std::string wire = packed("gzip", text);

    Body body{ctx.get_executor()};
    REQUIRE(body.enable_decoding(make_decoder("gzip"), 1024));
    std::string chunk = wire;
    REQUIRE(body.feed(std::move(chunk)));
    body.finish();

    auto result = run_on(ctx, body.read_all());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(result->error() == asio::error::message_size);
}

TEST_CASE("request decoding: enable_decoding refuses after a read", "[request-decoding]") {
    asio::io_context ctx;
    Body body{ctx.get_executor()};
    body.finish();
    auto eof = run_on(ctx, body.read());
    REQUIRE(eof.has_value());
    REQUIRE(eof->has_value());
    CHECK((*eof)->eof);
    CHECK_FALSE(body.enable_decoding(make_decoder("gzip"), 0));
}

TEST_CASE("decompress_request: gzip body is decoded and headers are rewritten", "[request-decoding]") {
    asio::io_context ctx;
    const std::string text = "decoded by the middleware";
    const std::string wire = packed("gzip", text);

    auto req = make_request(ctx);
    req->mutable_headers().add("content-encoding", "gzip");
    req->mutable_headers().add("content-length", std::to_string(wire.size()));
    std::string chunk = wire;
    REQUIRE(req->body().feed(std::move(chunk)));
    req->body().finish();

    auto sink = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(sink);

    std::string seen;
    bool called = false;
    Next next = [&](RequestPtr r, ResponsePtr, SslHandle) -> asio::awaitable<void> {
        called = true;
        auto all = co_await r->body().read_all();
        if (all) {
            seen = *all;
        }
        co_return;
    };

    auto mw = middleware::decompress_request();
    REQUIRE(run_on(ctx, mw(req, res, SslHandle{}, next)));
    CHECK(called);
    CHECK(seen == text);
    CHECK_FALSE(req->header("content-encoding").has_value());
    CHECK_FALSE(req->header("content-length").has_value());
    CHECK(sink->calls == 0);
}

TEST_CASE("decompress_request: unsupported coding is 415 before the handler", "[request-decoding]") {
    asio::io_context ctx;
    auto req = make_request(ctx);
    req->mutable_headers().add("content-encoding", "deflate");
    auto sink = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(sink);

    bool called = false;
    Next next = [&](RequestPtr, ResponsePtr, SslHandle) -> asio::awaitable<void> {
        called = true;
        co_return;
    };

    auto mw = middleware::decompress_request();
    REQUIRE(run_on(ctx, mw(req, res, SslHandle{}, next)));
    CHECK_FALSE(called);
    CHECK(sink->last_status == 415);
    CHECK(sink->header("accept-encoding") == "gzip, br");
}

TEST_CASE("decompress_request: excluded paths stay on the raw representation", "[request-decoding]") {
    asio::io_context ctx;
    const std::string text = "proxy passthrough";
    const std::string wire = packed("gzip", text);

    auto req = make_request(ctx, "/proxy/upstream");
    req->mutable_headers().add("content-encoding", "gzip");
    std::string chunk = wire;
    REQUIRE(req->body().feed(std::move(chunk)));
    req->body().finish();

    auto sink = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(sink);

    std::string seen;
    Next next = [&](RequestPtr r, ResponsePtr, SslHandle) -> asio::awaitable<void> {
        auto all = co_await r->body().read_all();
        if (all) {
            seen = *all;
        }
        co_return;
    };

    auto mw = middleware::decompress_request(RequestDecompressionConfig{.excluded_paths = {"/proxy/"}});
    REQUIRE(run_on(ctx, mw(req, res, SslHandle{}, next)));
    CHECK(seen == wire);
    CHECK(req->header("content-encoding").value_or("") == "gzip");
}

TEST_CASE("decompress_request: a body already read cannot be decoded", "[request-decoding]") {
    asio::io_context ctx;
    const std::string wire = packed("gzip", "too late");
    auto req = make_request(ctx);
    req->mutable_headers().add("content-encoding", "gzip");
    std::string chunk = wire;
    REQUIRE(req->body().feed(std::move(chunk)));
    req->body().finish();

    auto consumed = run_on(ctx, req->body().read_all());
    REQUIRE(consumed.has_value());

    auto sink = std::make_shared<FakeResponseSink>();
    auto res = std::make_shared<ResponseWriter>(sink);
    Next next = [](RequestPtr, ResponsePtr, SslHandle) -> asio::awaitable<void> { co_return; };

    auto mw = middleware::decompress_request();
    REQUIRE(run_on(ctx, mw(req, res, SslHandle{}, next)));
    CHECK(sink->last_status == 500);
}

TEST_CASE("request decoding: bounded decoder stops at the budget", "[request-decoding]") {
    const std::string text(256 * 1024, 'z');
    const std::string wire = packed("gzip", text);
    auto decoder = make_decoder("gzip");
    REQUIRE(decoder != nullptr);
    bool exceeded = false;
    auto out = decoder->write_bounded(wire, 1024, exceeded);
    CHECK(exceeded);
    CHECK(out.size() <= 1024);
}
