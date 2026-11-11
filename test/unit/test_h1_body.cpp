// engine/h1: request-side chunked body decoding (RFC 9112 §7.1).
//
// The chunked decoder parses untrusted framing: chunk-size lines (including
// extensions), the CRLF after each chunk, the terminator's trailer section, and
// the max_body_bytes bound. A disagreement here is a request-smuggling vector,
// so each malformed shape must fail closed.

#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

struct BodyOutcome {
    bool completed{false};
    bool ok{false};
    error_code ec;
    std::string body;
    std::string trailer;

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
