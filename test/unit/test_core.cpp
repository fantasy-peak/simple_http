// core/: the protocol-agnostic primitives every other layer builds on.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

TEST_CASE("core/types: control-byte detection", "[core]") {
    using simple_http::contains_ctl;
    CHECK_FALSE(contains_ctl(""));
    CHECK_FALSE(contains_ctl("plain text"));
    CHECK_FALSE(contains_ctl("a\tb"));  // tab is legal in a field value
    CHECK(contains_ctl("a\rb"));
    CHECK(contains_ctl("a\nb"));
    CHECK(contains_ctl(std::string_view{"a\0b", 3}));  // NUL is not a terminator here
}

TEST_CASE("core/types: names and versions", "[core]") {
    CHECK(to_string(Version::Http1) == "HTTP/1.0");
    CHECK(to_string(Version::Http11) == "HTTP/1.1");
    CHECK(to_string(Version::Http2) == "HTTP/2");
    CHECK(to_string(Version::Http3) == "HTTP/3");
}

TEST_CASE("core/http_method: canonical tokens only", "[core]") {
    CHECK(method_from_string("GET") == Method::Get);
    CHECK(method_from_string("POST") == Method::Post);
    CHECK(method_from_string("DELETE") == Method::Delete);
    CHECK(method_from_string("PATCH") == Method::Patch);
    // Methods are case-sensitive (RFC 9110 §9.1), so a lowercased one is not known.
    CHECK(method_from_string("get") == Method::Unknown);
    CHECK(method_from_string("") == Method::Unknown);
    CHECK(method_from_string("PROPFIND") == Method::Unknown);

    CHECK(to_string(Method::Get) == "GET");
    CHECK(to_string(Method::Head) == "HEAD");
    CHECK(to_string(Method::Connect) == "CONNECT");
    CHECK(to_string(Method::Unknown).empty());
}

TEST_CASE("core/http_status: reason phrases", "[core]") {
    CHECK(reason_phrase(200) == "OK");
    CHECK(reason_phrase(204) == "No Content");
    CHECK(reason_phrase(304) == "Not Modified");
    CHECK(reason_phrase(404) == "Not Found");
    CHECK(reason_phrase(431) == "Request Header Fields Too Large");
    CHECK(reason_phrase(500) == "Internal Server Error");
    CHECK(reason_phrase(504) == "Gateway Timeout");
    CHECK(reason_phrase(599) == "Unknown");
    CHECK(reason_phrase(0) == "Unknown");
}

TEST_CASE("core/base64: url alphabet is unpadded and round-trips", "[core]") {
    CHECK(base64_url_encode("") == "");
    CHECK(base64_url_encode("f") == "Zg");
    CHECK(base64_url_encode("fo") == "Zm8");
    CHECK(base64_url_encode("foo") == "Zm9v");
    // Bytes that produce the URL-safe characters.
    const std::string high_bit{"\xfb\xff\xbf", 3};
    const std::string encoded = base64_url_encode(high_bit);
    CHECK(encoded.find('+') == std::string::npos);
    CHECK(encoded.find('/') == std::string::npos);
    CHECK(base64_url_decode(encoded) == high_bit);

    // Every byte value survives the round trip (catches sign-extension bugs).
    std::string all;
    for (int i = 0; i < 256; ++i)
        all.push_back(static_cast<char>(i));
    CHECK(base64_url_decode(base64_url_encode(all)) == all);
}

TEST_CASE("core/base64: url decoding stops at the first foreign byte", "[core]") {
    // The HTTP2-Settings header is base64url; a stray '=' or space ends it.
    CHECK(base64_url_decode("Zm9v") == "foo");
    CHECK(base64_url_decode("Zm9v==") == "foo");
    CHECK(base64_url_decode("Zm9v extra") == "foo");
    CHECK(base64_url_decode("") == "");
}

TEST_CASE("core/base64: standard alphabet is padded", "[core]") {
    CHECK(base64_encode("") == "");
    CHECK(base64_encode("f") == "Zg==");
    CHECK(base64_encode("fo") == "Zm8=");
    CHECK(base64_encode("foo") == "Zm9v");
    CHECK(base64_encode("foob") == "Zm9vYg==");
    CHECK(base64_encode("fooba") == "Zm9vYmE=");
    CHECK(base64_encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("core/mime and version constants", "[core]") {
    CHECK(mime::text_plain == "text/plain");
    CHECK(mime::app_json == "application/json");
    CHECK(mime::app_octet_stream == "application/octet-stream");
    CHECK(server_version.starts_with("simple_http_server/"));
    CHECK(client_version.starts_with("simple_http_client/"));
    CHECK(version_major == SIMPLE_HTTP_VERSION_MAJOR);
}

TEST_CASE("core/limits: defaults are protocol-legal", "[core]") {
    EngineLimits limits;
    CHECK(limits.idle_timeout.count() > 0);
    CHECK(limits.max_header_bytes >= 8192);
    CHECK(limits.max_body_bytes >= 1024 * 1024);
    // RFC 9113 §6.5.2: MAX_FRAME_SIZE lies in [2^14, 2^24-1].
    CHECK(limits.h2_max_frame_size >= 16384);
    CHECK(limits.h2_max_frame_size <= 16777215);
    // RFC 9113 §6.9.2: a window is a 31-bit signed quantity.
    CHECK(limits.h2_initial_window > 0);
    CHECK(limits.h2_initial_window <= 0x7FFFFFFF);
    CHECK(limits.h2_max_concurrent_streams > 0);
}

TEST_CASE("core/logging: level filter, formatting and source location", "[core]") {
    ScopedLog capture;  // note: `log` is also the library's logging function
    set_log_level(LogLevel::Info);
    log(LogLevel::Debug, __FILE__, __LINE__, "dropped {}", 1);  // below the level
    CHECK(capture.records.empty());

    log(LogLevel::Error, "f.cpp", 42, "code {} message {}", 404, "nope");
    REQUIRE(capture.records.size() == 1);
    CHECK(capture.records[0].level == LogLevel::Error);
    CHECK(capture.records[0].file == "f.cpp");
    CHECK(capture.records[0].line == 42);
    CHECK(capture.records[0].message == "code 404 message nope");

    CHECK(to_string(LogLevel::Debug) == "Debug");
    CHECK(to_string(LogLevel::Info) == "Info");
    CHECK(to_string(LogLevel::Error) == "Error");
}

TEST_CASE("core/io_pool: round-robin, main context and clean shutdown", "[core]") {
    CHECK_THROWS_AS(IoCtxPool{0}, std::runtime_error);

    IoCtxPool pool{2};
    pool.add_main_context();  // the acceptor context: created last, returned by main_context()
    auto* first = pool.next_ptr().get();
    auto* second = pool.next_ptr().get();
    auto* third = pool.next_ptr().get();
    CHECK(first != second);
    CHECK(first == third);  // the cursor wraps over the worker contexts
    // The acceptor context is created last and must never be handed out as a worker.
    auto* main_ctx = pool.main_context().get();
    CHECK(main_ctx != first);
    for (int i = 0; i < 6; ++i)
        CHECK(pool.next_ptr().get() != main_ctx);
}
