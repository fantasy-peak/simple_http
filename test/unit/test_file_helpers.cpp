// handler/file_response.h + handler/upload.h: the streaming file primitives.
//
// send_file and read_body_to_file are the low-level building blocks behind
// static serving and uploads. Their contracts are easy to break silently — a
// cap checked after the first chunk, a HEAD that still writes a body, a stat
// failure that leaks headers — so each is driven over a real ResponseWriter or
// Body here.

#include <boost/system/error_code.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "../support/static_fixture.h"
#include "simple_http.h"
#include "test_support.h"
#include <simple_http/core/asio_compat.h>

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
    auto ec = run_on(
        ctx, as_ec(async_send_file(res, fixture.root_string() + "/assets/a.txt", asio::as_tuple(asio::use_awaitable))));
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
        auto ec =
            run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/a.txt", SendFileOptions{.max_bytes = 4},
                                              asio::as_tuple(asio::use_awaitable))));
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
        auto ec = run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/missing.txt",
                                                    asio::as_tuple(asio::use_awaitable))));
        REQUIRE(ec.has_value());
        CHECK(*ec == boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory));
        CHECK_FALSE(writer->begun);
        CHECK(writer->calls == 0);
    }

    SECTION("content_type overrides the extension and content-length is optional") {
        asio::io_context ctx;
        auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
        ResponseWriter res{writer};
        auto ec = run_on(ctx, as_ec(async_send_file(
                                  res, fixture.root_string() + "/a.txt",
                                  SendFileOptions{.content_type = "application/x-test", .set_content_length = false},
                                  asio::as_tuple(asio::use_awaitable))));
        REQUIRE(ec.has_value());
        CHECK_FALSE(*ec);
        CHECK(writer->header("content-type") == "application/x-test");
        CHECK_FALSE(writer->has_header("content-length"));
    }
}

TEST_CASE("file: send_file streams a requested byte range", "[file]") {
    StaticFixture fixture;
    fixture.write("range.txt", "0123456789");

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    ResponseWriter res{writer};
    res.content_type("text/plain"); // the caller already built the head
    SendFileOptions opts;
    opts.offset = 2;
    opts.length = 3;
    opts.apply_content_type = false;
    opts.set_content_length = false;
    auto ec = run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/range.txt", opts,
                                                asio::as_tuple(asio::use_awaitable))));
    REQUIRE(ec.has_value());
    CHECK_FALSE(*ec);
    CHECK(chunks_of(*writer) == "234");
    CHECK(writer->header("content-type") == "text/plain");
    CHECK_FALSE(writer->has_header("content-length"));
}

TEST_CASE("file: send_file answers HEAD without a body", "[file]") {
    StaticFixture fixture;
    fixture.write("a.txt", "hello file");

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    writer->head = true;
    ResponseWriter res{writer};
    auto ec =
        run_on(ctx, as_ec(async_send_file(res, fixture.root_string() + "/a.txt", asio::as_tuple(asio::use_awaitable))));
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

    auto result = run_on(
        ctx, as_expected(async_read_body_to_file(body, path, /*max_bytes=*/4, asio::as_tuple(asio::use_awaitable))));
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(result->error() == boost::system::errc::make_error_code(boost::system::errc::file_too_large));
    // The upload wrote to a temporary file, which is removed on failure; the
    // target is never created.
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("file: a failed upload leaves an existing target untouched", "[file]") {
    StaticFixture fixture;
    fixture.write("upload.bin", "ORIGINAL-CONTENT");
    const std::string path = fixture.root_string() + "/upload.bin";

    asio::io_context ctx;
    Body body{ctx.get_executor()};
    REQUIRE(body.feed(std::string{"0123456789"}));
    body.finish();

    auto result = run_on(
        ctx, as_expected(async_read_body_to_file(body, path, /*max_bytes=*/4, asio::as_tuple(asio::use_awaitable))));
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(result->error() == boost::system::errc::make_error_code(boost::system::errc::file_too_large));
    CHECK(read_file(path) == "ORIGINAL-CONTENT");

    // No temporary file is left behind.
    std::size_t entries = 0;
    for (const auto &entry : std::filesystem::directory_iterator(fixture.root())) {
        ++entries;
        CHECK(entry.path().filename() == "upload.bin");
    }
    CHECK(entries == 1);
}

// client/multipart_stream.h: a short read at end-of-file sets both eofbit and
// failbit. The chunk loop treats gcount()==0 as the end, so only badbit is a
// real error; classifying fail() as io_error made every upload end with one.
TEST_CASE("file: reaching EOF is not a read error", "[file]") {
    StaticFixture fixture;
    fixture.write("ten.bin", "0123456789");
    std::ifstream in{fixture.root() / "ten.bin", std::ios::binary};
    REQUIRE(in);

    std::string buffer(4, '\0');
    std::size_t total = 0;
    for (;;) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        total += n;
        if (n == 0)
            break;
    }

    CHECK(total == 10);
    // The final zero-byte read is a clean EOF; the old `fail()` test reported
    // io_error here.
    CHECK(in.eof());
    CHECK_FALSE(detail::file_read_error(in));
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
