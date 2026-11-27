// engine/h1: request-side chunked body decoding (RFC 9112 §7.1).
//
// The chunked decoder parses untrusted framing: chunk-size lines (including
// extensions), the CRLF after each chunk, the terminator's trailer section, and
// the max_body_bytes bound. A disagreement here is a request-smuggling vector,
// so each malformed shape must fail closed.

#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "simple_http.h"
#include "test_support.h"
#include <simple_http/core/asio_compat.h>

using namespace simple_http;
using namespace simple_http::test;

namespace {

struct BodyOutcome {
    bool completed{false};
    bool ok{false};
    error_code ec;
    std::string body;
    std::string trailer;
    std::string response;

    bool had_trailer{false};
};

BodyOutcome run_chunked(std::string request, EngineLimits limits = {}) {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push(std::move(request));

    BodyOutcome out;
    auto dispatcher = [&out](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                             SslHandle) -> asio::awaitable<void> {
        auto body = co_await body_read_all(req->body());
        if (!body) {
            out.ec = body.error();
            co_return;
        }
        out.ok = true;
        out.body = std::move(*body);
        out.had_trailer = req->trailers().contains("x-check");
        if (auto value = req->trailers().get("x-check")) {
            out.trailer = std::string{*value};
        }
        co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport, limits);
    out.completed = run_on(ctx, run_engine(*engine, dispatcher));
    out.response = transport->written();
    return out;
}

} // namespace

TEST_CASE("h1/body: chunked request decodes extensions, CRLF and trailers", "[h1]") {
    const auto out = run_chunked("POST / HTTP/1.1\r\n"
                                 "Host: x\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "\r\n"
                                 "5;ext=1\r\n"
                                 "hello\r\n"
                                 "6\r\n"
                                 " world\r\n"
                                 "0\r\n"
                                 "X-Check: yes\r\n"
                                 "\r\n");
    REQUIRE(out.completed);
    REQUIRE(out.ok);
    CHECK(out.body == "hello world");
    CHECK(out.had_trailer);
    CHECK(out.trailer == "yes");
}

TEST_CASE("h1/body: a non-hex chunk size is rejected", "[h1]") {
    const auto out = run_chunked("POST / HTTP/1.1\r\n"
                                 "Host: x\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "\r\n"
                                 "zz\r\n"
                                 "body\r\n"
                                 "0\r\n"
                                 "\r\n");
    CHECK(out.completed);
    CHECK_FALSE(out.ok);
    CHECK(out.ec == asio::error::invalid_argument);
}

TEST_CASE("h1/body: chunk data must be followed by CRLF", "[h1]") {
    // Accepting any two bytes here would let a CRLF-strict front-end and a
    // lenient back-end disagree about the body boundary.
    const auto out = run_chunked("POST / HTTP/1.1\r\n"
                                 "Host: x\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "\r\n"
                                 "5\r\n"
                                 "helloXX"
                                 "0\r\n"
                                 "\r\n");
    CHECK(out.completed);
    CHECK_FALSE(out.ok);
    CHECK(out.ec == asio::error::invalid_argument);
}

TEST_CASE("h1/body: a truncated chunk is EOF", "[h1]") {
    const auto out = run_chunked("POST / HTTP/1.1\r\n"
                                 "Host: x\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "\r\n"
                                 "5\r\n"
                                 "hel");
    CHECK(out.completed);
    CHECK_FALSE(out.ok);
    CHECK(out.ec == asio::error::eof);
}

TEST_CASE("h1/body: the cumulative chunked size is bounded", "[h1]") {
    EngineLimits limits;
    limits.max_body_bytes = 16;
    const auto out = run_chunked("POST / HTTP/1.1\r\n"
                                 "Host: x\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "\r\n"
                                 "a\r\n"
                                 "0123456789\r\n"
                                 "10\r\n"
                                 "0123456789abcdef\r\n"
                                 "0\r\n"
                                 "\r\n",
                                 limits);
    CHECK(out.completed);
    CHECK_FALSE(out.ok);
    CHECK(out.ec == asio::error::message_size);
}

TEST_CASE("h1/body: an over-long trailer line is bounded", "[h1]") {
    // The line has no newline, so read_line() stops at max_header_bytes instead
    // of growing the buffer forever. In the trailer section that is a malformed
    // body end (message_size), not EOF.
    std::string request = "POST / HTTP/1.1\r\n"
                          "Host: x\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "0\r\n"
                          "X-Check: ";
    request.append(70000, 'a');
    const auto out = run_chunked(std::move(request));
    CHECK(out.completed);
    CHECK_FALSE(out.ok);
    CHECK(out.ec == asio::error::message_size);
}

TEST_CASE("h1/body: connection-specific trailer fields are ignored", "[h1]") {
    const auto out = run_chunked("POST / HTTP/1.1\r\n"
                                 "Host: x\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "\r\n"
                                 "0\r\n"
                                 "Connection: close\r\n"
                                 "X-Check: yes\r\n"
                                 "\r\n");
    REQUIRE(out.completed);
    REQUIRE(out.ok);
    CHECK(out.body.empty());
    CHECK(out.had_trailer);
    CHECK(out.trailer == "yes");
}

TEST_CASE("h1/body: the trailer budget is per message, not per connection", "[h1]") {
    // Two keep-alive requests whose trailer sections are each under
    // max_header_bytes but together exceed it. The budget used to live on the
    // connection, so the second request failed with message_size.
    EngineLimits limits;
    limits.max_header_bytes = 120;
    const std::string value(50, 'a');
    const std::string request = "POST / HTTP/1.1\r\n"
                                "Host: x\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "1\r\n"
                                "a\r\n"
                                "0\r\n"
                                "X-Check: " +
                                value +
                                "\r\n"
                                "\r\n";

    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push(request + request);

    std::vector<std::string> bodies;
    auto dispatcher = [&bodies](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                                SslHandle) -> asio::awaitable<void> {
        auto body = co_await body_read_all(req->body());
        if (!body) {
            co_await res->status(status::bad_request).async_send("bad", asio::use_awaitable);
            co_return;
        }
        bodies.push_back(std::move(*body));
        co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport, limits);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0] == "a");
    CHECK(bodies[1] == "a");
}

// RFC 9112 §6.1: Transfer-Encoding is one comma-separated coding list, and
// chunked must be the final coding. Reading only the first field (or only
// "does chunked appear") lets two readers disagree about the body boundary.
TEST_CASE("h1/body: Transfer-Encoding must be a single final chunked coding", "[h1]") {
    auto frames = [](const std::string &fields, int expected) {
        auto out = run_chunked("POST / HTTP/1.1\r\nHost: x\r\n" + fields + "\r\n");
        CHECK(out.completed);
        CHECK_FALSE(out.ok);
        CHECK(out.response.rfind("HTTP/1.1 " + std::to_string(expected), 0) == 0);
    };

    // chunked is present but not last (two fields, then one field).
    frames("Transfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n", 400);
    frames("Transfer-Encoding: chunked, gzip\r\n", 400);
    // chunked last but preceded by an unsupported coding.
    frames("Transfer-Encoding: gzip, chunked\r\n", 501);
    // No chunked at all.
    frames("Transfer-Encoding: gzip\r\n", 501);
    // Repeated chunked.
    frames("Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n", 400);
}

// --- post-handler drain limit (Go net/http maxPostHandlerReadBytes) ---------
//
// A handler that ignores the body used to make the server consume up to
// max_body_bytes (64 MiB by default, unbounded when that limit is disabled)
// just so the connection could be reused. Go net/http caps that at 256 KiB and
// answers with `Connection: close` instead; these cases pin the same policy.

namespace {

struct DrainOutcome {
    bool completed{false};
    int dispatched{0};
    std::string response;
    bool closed{false};

    // Whether a follow-up request queued on the same connection was served.
    bool reused() const { return dispatched > 1; }
};

DrainOutcome run_without_reading(std::string request, bool read_body = false) {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push(std::move(request));

    DrainOutcome out;
    auto dispatcher = [&out, read_body](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                                        SslHandle) -> asio::awaitable<void> {
        ++out.dispatched;
        if (read_body) {
            (void)co_await body_read_all(req->body());
        }
        co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
    };
    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    out.completed = run_on(ctx, run_engine(*engine, dispatcher));
    out.response = transport->written();
    out.closed = transport->closed();
    return out;
}

constexpr std::size_t kDrainCap = static_cast<std::size_t>(kH1DrainLimitBytes);

std::string fixed_request(std::size_t declared, std::string bytes = {}) {
    return "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(declared) + "\r\n\r\n" + bytes;
}

std::string chunked_request(const std::string &body, std::string suffix = {}) {
    char hex[32];
    auto [end, ec] = std::to_chars(hex, hex + sizeof(hex), body.size(), 16);
    (void)ec;
    std::string out = "POST /a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    out.append(hex, static_cast<std::size_t>(end - hex));
    out.append("\r\n");
    out.append(body);
    out.append("\r\n0\r\n\r\n");
    out.append(suffix);
    return out;
}

constexpr std::string_view kNextRequest = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";

} // namespace

TEST_CASE("h1/engine: a Content-Length body at the drain limit is not drained", "[h1]") {
    // Head only: if the engine tried to read the declared body this would block
    // (or fail on EOF), and a client that never sends it could pin the
    // connection. The response head must say the connection is closing.
    const auto out = run_without_reading(fixed_request(kDrainCap));
    REQUIRE(out.completed);
    CHECK(out.dispatched == 1);
    CHECK(out.response.find("connection: close") != std::string::npos);
    CHECK(out.closed);
}

TEST_CASE("h1/engine: a Content-Length body at the drain limit is not consumed for reuse", "[h1]") {
    // The body (and a queued follow-up request) are already on the wire, but
    // the tolerance is reached, so the server stops instead of draining 256 KiB
    // to reuse the connection. The follow-up must never be dispatched.
    const std::string request = fixed_request(kDrainCap, std::string(kDrainCap, 'x')) + std::string(kNextRequest);
    const auto out = run_without_reading(request);
    REQUIRE(out.completed);
    CHECK(out.dispatched == 1);
    CHECK_FALSE(out.reused());
    CHECK(out.response.find("connection: close") != std::string::npos);
    CHECK(out.closed);
}

TEST_CASE("h1/engine: an unread body under the drain limit keeps the connection", "[h1]") {
    // One byte below the tolerance: Go drains it and reuses the connection, so
    // the queued follow-up request is served.
    const std::string request =
        fixed_request(kDrainCap - 1, std::string(kDrainCap - 1, 'x')) + std::string(kNextRequest);
    const auto out = run_without_reading(request);
    REQUIRE(out.completed);
    CHECK(out.dispatched == 2);
    CHECK(out.response.find("connection: close") == std::string::npos);
}

TEST_CASE("h1/engine: a chunked body at the drain limit keeps the connection", "[h1]") {
    const std::string request = chunked_request(std::string(kDrainCap, 'y'), std::string(kNextRequest));
    const auto out = run_without_reading(request);
    REQUIRE(out.completed);
    CHECK(out.dispatched == 2);
}

TEST_CASE("h1/engine: a chunked body over the drain limit is not drained", "[h1]") {
    const std::string request = chunked_request(std::string(kDrainCap + 1, 'y'), std::string(kNextRequest));
    const auto out = run_without_reading(request);
    REQUIRE(out.completed);
    CHECK(out.dispatched == 1);
    CHECK_FALSE(out.reused());
    CHECK(out.closed);
}

TEST_CASE("h1/engine: a body the handler read is reusable above the drain limit", "[h1]") {
    // The cap applies to bytes left unread, not to the request size: a handler
    // that consumed the body itself leaves nothing to drain.
    const std::string request =
        fixed_request(kDrainCap + 1, std::string(kDrainCap + 1, 'z')) + std::string(kNextRequest);
    const auto out = run_without_reading(request, /*read_body=*/true);
    REQUIRE(out.completed);
    CHECK(out.dispatched == 2);
}
