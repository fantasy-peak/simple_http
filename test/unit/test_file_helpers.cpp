// handler/file_response.h + handler/upload.h: the streaming file primitives.
//
// send_file and read_body_to_file are the low-level building blocks behind
// static serving and uploads. Their contracts are easy to break silently — a
// cap checked after the first chunk, a HEAD that still writes a body, a stat
// failure that leaks headers — so each is driven over a real ResponseWriter or
// Body here.

#include <boost/system/error_code.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "../support/static_fixture.h"
#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

std::string read_file(const std::string &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string chunks_of(const FakeResponseSink &writer) {
    std::string body;
    for (const auto &chunk : writer.chunks) {
        body += chunk;
    }
    return body;
}

} // namespace

TEST_CASE("file: send_file streams content with a derived type and length", "[file]") {
    StaticFixture fixture;
    fixture.write("assets/a.txt", "hello file");

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    ResponseWriter res{writer};
    auto ec = run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/assets/a.txt", asio::as_tuple(asio::use_awaitable))));
    REQUIRE(ec.has_value());
    CHECK_FALSE(*ec);

    CHECK(writer->begun);
    CHECK(writer->finished);
    CHECK(writer->header("content-type") == "text/plain; charset=utf-8");
    CHECK(writer->header("content-length") == "10");
    CHECK(chunks_of(*writer) == "hello file");
}

TEST_CASE("file: send_file honours max_bytes and custom options", "[file]") {
    StaticFixture fixture;
    fixture.write("a.txt", "hello file");

    SECTION("a file above max_bytes is refused before any header goes out") {
        asio::io_context ctx;
        auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
        ResponseWriter res{writer};
        auto ec = run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/a.txt", SendFileOptions{.max_bytes = 4}, asio::as_tuple(asio::use_awaitable))));
        REQUIRE(ec.has_value());
        CHECK(*ec == boost::system::errc::make_error_code(boost::system::errc::file_too_large));
        CHECK_FALSE(writer->begun);
        CHECK(writer->calls == 0);
        CHECK(writer->chunks.empty());
    }

    SECTION("a missing file leaves the response untouched") {
        asio::io_context ctx;
        auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
        ResponseWriter res{writer};
        auto ec = run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/missing.txt", asio::as_tuple(asio::use_awaitable))));
        REQUIRE(ec.has_value());
        CHECK(*ec == boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory));
        CHECK_FALSE(writer->begun);
        CHECK(writer->calls == 0);
    }

    SECTION("content_type overrides the extension and content-length is optional") {
        asio::io_context ctx;
        auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
        ResponseWriter res{writer};
        auto ec =
            run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/a.txt",
                                  SendFileOptions{.content_type = "application/x-test", .set_content_length = false}, asio::as_tuple(asio::use_awaitable))));
        REQUIRE(ec.has_value());
        CHECK_FALSE(*ec);
        CHECK(writer->header("content-type") == "application/x-test");
        CHECK_FALSE(writer->has_header("content-length"));
    }
}

TEST_CASE("file: send_file answers HEAD without a body", "[file]") {
    StaticFixture fixture;
    fixture.write("a.txt", "hello file");

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    writer->head = true;
    ResponseWriter res{writer};
    auto ec = run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/a.txt", asio::as_tuple(asio::use_awaitable))));
    REQUIRE(ec.has_value());
    CHECK_FALSE(*ec);

    CHECK(writer->sent_bodyless);
    CHECK_FALSE(writer->begun);
    CHECK(writer->chunks.empty());
    // HEAD keeps the GET metadata, so a cache still learns the size and type.
    CHECK(writer->header("content-type") == "text/plain; charset=utf-8");
    CHECK(writer->header("content-length") == "10");
}

TEST_CASE("file: read_body_to_file streams every chunk to disk", "[file]") {
    StaticFixture fixture;
    const std::string path = fixture.root_string() + "/upload.bin";

    asio::io_context ctx;
    Body body{ctx.get_executor()};
    REQUIRE(body.feed(std::string{"hello "}));
    REQUIRE(body.feed(std::string{"world"}));
    body.finish();

    auto result = run_on(ctx, as_expected(async_read_body_to_file(body, path, asio::as_tuple(asio::use_awaitable))));
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == 11);
    CHECK(read_file(path) == "hello world");
}

TEST_CASE("file: read_body_to_file caps the upload before it fills the disk", "[file]") {
    StaticFixture fixture;
    const std::string path = fixture.root_string() + "/upload.bin";

    asio::io_context ctx;
    Body body{ctx.get_executor()};
    REQUIRE(body.feed(std::string{"0123456789"}));
    body.finish();

    auto result = run_on(ctx, as_expected(async_read_body_to_file(body, path, /*max_bytes=*/4, asio::as_tuple(asio::use_awaitable))));
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(result->error() == boost::system::errc::make_error_code(boost::system::errc::file_too_large));
    // The chunk that crossed the bound was never written.
    CHECK(read_file(path).empty());
}

TEST_CASE("file: an empty body still creates the destination file", "[file]") {
    StaticFixture fixture;
    const std::string path = fixture.root_string() + "/empty.bin";

    asio::io_context ctx;
    Body body{ctx.get_executor()};
    body.finish();

    auto result = run_on(ctx, as_expected(async_read_body_to_file(body, path, asio::as_tuple(asio::use_awaitable))));
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == 0);
    CHECK(read_file(path).empty());
}
