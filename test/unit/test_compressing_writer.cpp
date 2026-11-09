// proto/compressing_writer.h: the decorator's method mapping and its boundary
// rules. The policy now comes from the Request (mounted middleware::compress
// set it), so each case builds a Request with an Accept-Encoding + policy and
// drives the sink. Every decision - what reached the engine, and with which
// headers - is observable on the recording FakeResponseSink.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

CompressionConfig policy(std::uint64_t min_bytes = 64) {
    CompressionConfig config;
    config.enabled = true;
    config.min_bytes = min_bytes;
    return config;
}

// A Request carrying the client's Accept-Encoding and, when given, the
// per-request compression policy a mounted middleware::compress would set.
std::shared_ptr<Request> make_request(asio::io_context &ctx, const std::string &accept,
                                      const CompressionConfig *cfg = nullptr, std::string target = "/x") {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor());
    req->set_target(std::move(target));
    if (!accept.empty()) {
        req->mutable_headers().add("accept-encoding", accept);
    }
    if (cfg) {
        req->set_state(*cfg);
    }
    return req;
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
// The sink's contract is still a completion handler; drive it directly so the
// test observes the exact status/headers the sink produces (a ResponseWriter
// would add its own defaults first).
template <typename F> void ok(asio::io_context &ctx, F &&start) {
    bool done = false;
    error_code result{make_error_code(asio::error::would_block)};
    start([&](error_code ec) {
        result = ec;
        done = true;
    });
    ctx.restart();
    ctx.run_for(std::chrono::seconds(5));
    REQUIRE(done);
    CHECK_FALSE(static_cast<bool>(result));
}

} // namespace

TEST_CASE("compressing_writer: installed only when the client accepts a codec", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());

    // A client that names no codec we produce can never be compressed: the
    // writer is returned untouched, so nothing is allocated.
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), make_request(ctx, ""), false) == inner);
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), make_request(ctx, "identity"), false) == inner);
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), make_request(ctx, "deflate"), false) == inner);
    // HEAD responses have no body to compress.
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), make_request(ctx, "gzip"), true) == inner);

    // A client that accepts gzip/br gets the decorator, even before we know
    // whether a policy covers the route: the decision is per response.
    auto req = make_request(ctx, "gzip, br");
    CHECK(maybe_compress_writer(inner, ctx.get_executor(), req, false) != inner);
}

TEST_CASE("compressing_writer: a mounted policy compresses a one-shot text response", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    const CompressionConfig cfg = policy();
    auto req = make_request(ctx, "gzip", &cfg);
    auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
    auto &writer = *writer_holder;

    const std::string body = big_text();
    ok(ctx, [&](auto done) { writer.send(200, text_headers(), body, std::move(done)); });

    CHECK(inner->calls == 1);
    CHECK(inner->header("content-encoding") == "gzip");
    CHECK(inner->has_header("vary"));
    CHECK(inner->last_body.size() < body.size());
    CHECK(decompress_all("gzip", inner->last_body) == body);
}

TEST_CASE("compressing_writer: no mounted policy means pass-through", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    // Client accepts a codec, but no route policy covers the request.
    auto req = make_request(ctx, "gzip, br", nullptr);
    auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
    auto &writer = *writer_holder;

    const std::string body = big_text();
    ok(ctx, [&](auto done) { writer.send(200, text_headers(), body, std::move(done)); });

    CHECK_FALSE(inner->has_header("content-encoding"));
    CHECK(inner->last_body == body);
}

TEST_CASE("compressing_writer: Content-Length follows the protocol", "[compression]") {
    const std::string body = big_text();

    SECTION("HTTP/2 gets the compressed length (its engine never recomputes)") {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
        inner->ver = Version::Http2;
        const CompressionConfig cfg = policy();
        auto req = make_request(ctx, "gzip", &cfg);
        auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
        auto &writer = *writer_holder;
        ok(ctx, [&](auto done) { writer.send(200, text_headers(), body, std::move(done)); });

        REQUIRE(inner->has_header("content-length"));
        CHECK(inner->header("content-length") == std::to_string(inner->last_body.size()));
    }

    SECTION("HTTP/1.x gets none (its engine writes its own)") {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
        inner->ver = Version::Http11;
        const CompressionConfig cfg = policy();
        auto req = make_request(ctx, "gzip", &cfg);
        auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
        auto &writer = *writer_holder;
        ok(ctx, [&](auto done) { writer.send(200, text_headers(), body, std::move(done)); });

        // A second Content-Length would be a smuggling vector downstream.
        CHECK_FALSE(inner->has_header("content-length"));
    }
}

TEST_CASE("compressing_writer: headers that forbid or predate compression", "[compression]") {
    const std::string body = big_text();

    const auto run_case = [&](Headers headers, int status, bool head, const char *label) {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
        const CompressionConfig cfg = policy();
        auto req = make_request(ctx, "gzip", &cfg);
        auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, head);
        auto &writer = *writer_holder;
        ok(ctx, [&](auto done) { writer.send(status, std::move(headers), body, std::move(done)); });

        INFO(label);
        CHECK_FALSE(inner->has_header("content-encoding"));
        CHECK(inner->last_body == body); // untouched
    };

    SECTION("already encoded by someone else") {
        // Encoded as "br" on purpose: if the decorator compressed anyway and
        // rewrote the field, it would read back as "gzip" and this catches it.
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
        const CompressionConfig cfg = policy();
        auto req = make_request(ctx, "gzip", &cfg);
        auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
        auto &writer = *writer_holder;

        Headers h = text_headers();
        h.add("content-encoding", "br");
        const std::string already = "pretend this is brotli on the wire";
        ok(ctx, [&](auto done) { writer.send(200, std::move(h), already, std::move(done)); });

        CHECK(inner->header("content-encoding") == "br"); // preserved, not double-encoded
        CHECK(inner->last_body == already);
        CHECK_FALSE(inner->has_header("vary")); // nothing was negotiated, so no Vary
    }
    SECTION("a byte range must stay addressable") { run_case(text_headers(), status::partial_content, false, "206"); }
    SECTION("Content-Range header") {
        Headers h = text_headers();
        h.add("content-range", "bytes 0-99/1000");
        run_case(std::move(h), status::ok, false, "content-range");
    }
    SECTION("Cache-Control: no-transform (RFC 9110 7.7)") {
        Headers h = text_headers();
        h.add("cache-control", "public, no-transform");
        run_case(std::move(h), status::ok, false, "no-transform");
    }
    SECTION("HEAD") { run_case(text_headers(), status::ok, true, "head"); }
    SECTION("a type that is already entropy-coded") {
        Headers h;
        h.add("content-type", "image/png");
        run_case(std::move(h), status::ok, false, "image/png");
    }
    SECTION("no content-type at all") { run_case(Headers{}, status::ok, false, "no content-type"); }
}

TEST_CASE("compressing_writer: small and incompressible bodies pass through", "[compression]") {
    SECTION("below min_bytes") {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
        const CompressionConfig cfg = policy(1024);
        auto req = make_request(ctx, "gzip", &cfg);
        auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
        auto &writer = *writer_holder;
        const std::string body = "tiny";
        ok(ctx, [&](auto done) { writer.send(200, text_headers(), body, std::move(done)); });

        CHECK_FALSE(inner->has_header("content-encoding"));
        CHECK(inner->last_body == body);
    }

    SECTION("compression would make it bigger") {
        asio::io_context ctx;
        auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
        const CompressionConfig cfg = policy();
        auto req = make_request(ctx, "gzip", &cfg);
        auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
        auto &writer = *writer_holder;
        const std::string body = incompressible(8192);
        ok(ctx, [&](auto done) { writer.send(200, text_headers(), body, std::move(done)); });

        // The win is checked before any header is touched, so this is a clean
        // pass-through rather than a body that grew.
        CHECK_FALSE(inner->has_header("content-encoding"));
        CHECK_FALSE(inner->has_header("vary"));
        CHECK(inner->last_body == body);
    }
}

TEST_CASE("compressing_writer: Vary and ETag are maintained", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    const CompressionConfig cfg = policy();
    auto req = make_request(ctx, "gzip", &cfg);
    auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
    auto &writer = *writer_holder;

    Headers headers = text_headers();
    headers.add("vary", "Origin");
    headers.add("etag", "\"v1\"");
    ok(ctx, [&](auto done) { writer.send(200, std::move(headers), big_text(), std::move(done)); });

    // Merged, not overwritten: the upstream's Vary still has to hold.
    CHECK(inner->header("vary") == "Origin, Accept-Encoding");
    // The body changed, so the strong validator no longer applies as-is.
    CHECK(inner->header("etag") == "W/\"v1\"");
}

TEST_CASE("compressing_writer: streaming", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    inner->ver = Version::Http2; // so the length rule is exercised the strict way
    const CompressionConfig cfg = policy();
    auto req = make_request(ctx, "gzip", &cfg);
    auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
    auto &writer = *writer_holder;
    ok(ctx, [&](auto done) { writer.send_headers(200, text_headers(), std::move(done)); });

    const std::string part1 = big_text();
    const std::string part2 = big_text();
    ok(ctx, [&](auto done) { writer.send_chunk(part1, std::move(done)); });
    ok(ctx, [&](auto done) { writer.send_last(part2, std::move(done)); });

    CHECK(inner->begun);
    CHECK(inner->finished);
    CHECK(inner->header("content-encoding") == "gzip");
    // A streamed length is unknown, so it must not be asserted.
    CHECK_FALSE(inner->has_header("content-length"));

    std::string streamed;
    for (const std::string &chunk : inner->chunks) {
        // An empty chunk would make HTTP/1.x emit its chunked terminator early.
        CHECK_FALSE(chunk.empty());
        streamed += chunk;
    }
    CHECK(decompress_all("gzip", streamed) == part1 + part2);
}

TEST_CASE("compressing_writer: a codec that buffers emits nothing, not an empty "
          "chunk",
          "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    const CompressionConfig cfg = policy();
    auto req = make_request(ctx, "gzip", &cfg);
    auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
    auto &writer = *writer_holder;
    ok(ctx, [&](auto done) { writer.send_headers(200, text_headers(), std::move(done)); });

    // One byte at a time: the codec will not produce output for most of these,
    // and each empty result must be swallowed rather than forwarded.
    const std::string body = big_text();
    for (char c : body) {
        ok(ctx, [&](auto done) { writer.send_chunk(std::string(1, c), std::move(done)); });
    }
    ok(ctx, [&](auto done) { writer.send_last({}, std::move(done)); });

    std::string streamed;
    for (const std::string &chunk : inner->chunks) {
        CHECK_FALSE(chunk.empty());
        streamed += chunk;
    }
    CHECK(decompress_all("gzip", streamed) == body);
}

TEST_CASE("compressing_writer: bodyless and control responses pass straight through", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    const CompressionConfig cfg = policy();
    auto req = make_request(ctx, "gzip", &cfg);
    auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
    auto &writer = *writer_holder;

    Headers headers = text_headers();
    ok(ctx, [&](auto done) { writer.send_bodyless(204, headers, std::move(done)); });
    CHECK(inner->sent_bodyless);
    CHECK_FALSE(inner->has_header("content-encoding"));

    ok(ctx, [&](auto done) { writer.send_continue(std::move(done)); });
    CHECK(inner->continues == 1);

    auto open = run_on(ctx, await_connected([&](auto done) { writer.connected(std::move(done)); }));
    REQUIRE(open.has_value());
    CHECK(*open);
    CHECK(writer.version() == Version::Http11);
    REQUIRE(run_on(ctx, await_error([&](auto done) { writer.close(std::move(done)); })));
    CHECK_FALSE(inner->open);
    CHECK_FALSE(inner->has_header("content-encoding"));
}

TEST_CASE("compressing_writer: brotli is used when negotiated", "[compression]") {
    asio::io_context ctx;
    auto inner = std::make_shared<FakeResponseSink>(ctx.get_executor());
    const CompressionConfig cfg = policy();
    auto req = make_request(ctx, "br", &cfg);
    auto writer_holder = std::make_shared<CompressingResponseSink>(inner, ctx.get_executor(), req, false);
    auto &writer = *writer_holder;

    const std::string body = big_text();
    ok(ctx, [&](auto done) { writer.send(200, text_headers(), body, std::move(done)); });

    CHECK(inner->header("content-encoding") == "br");
    CHECK(decompress_all("br", inner->last_body) == body);
}

TEST_CASE("middleware::compress: mounting sets the per-request policy", "[compression]") {
    asio::io_context ctx;

    auto run_mw = [&](const Middleware &mw, std::string target) {
        auto req = make_request(ctx, "gzip", nullptr, std::move(target));
        ResponsePtr res; // the middleware never touches the response
        Next next = [](RequestPtr, ResponsePtr, SslHandle, HandlerCompletion done) { std::move(done)(); };
        REQUIRE(run_on(ctx, await_void([&](auto done) { mw(req, res, std::nullopt, next, std::move(done)); })));
        return req;
    };

    // Mounted globally: every path gets the policy. Mounting is enabling, so a
    // default-constructed config turns compression on.
    {
        auto req = run_mw(middleware::compress(), "/anything");
        const auto *p = req->get_state<CompressionConfig>();
        REQUIRE(p != nullptr);
        CHECK(p->enabled);
    }
    // excluded_paths skips an exact target and a trailing-slash subtree, and
    // leaves everything else covered.
    {
        CompressionConfig cfg;
        cfg.excluded_paths = {"/health", "/assets/"};
        auto mw = middleware::compress(cfg);
        CHECK(run_mw(mw, "/health")->get_state<CompressionConfig>() == nullptr);
        CHECK(run_mw(mw, "/assets/app.js")->get_state<CompressionConfig>() == nullptr);
        CHECK(run_mw(mw, "/api/users")->get_state<CompressionConfig>() != nullptr);
    }
    // A skip predicate is the echo Skipper shape.
    {
        auto mw = middleware::compress({}, [](const Request &r) { return r.path() == "/events"; });
        CHECK(run_mw(mw, "/events")->get_state<CompressionConfig>() == nullptr);
        CHECK(run_mw(mw, "/other")->get_state<CompressionConfig>() != nullptr);
    }
}
