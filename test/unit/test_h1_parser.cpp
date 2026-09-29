// engine/h1: the byte-level head parsers shared by the server's HTTP/1.x engine
// and the client's HTTP/1.1 session.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

// --- request head ------------------------------------------------------------

TEST_CASE("h1/parser: a plain request head", "[h1]") {
    H1Parser parser;
    parser.feed("GET /a/b?c=d HTTP/1.1\r\nHost: example.com\r\nX-Trace: abc\r\n\r\nbody-bytes");

    REQUIRE(parser.parse_head() == H1Parser::State::Done);
    const auto& head = parser.head();
    CHECK(head.method == Method::Get);
    CHECK(head.method_token == "GET");
    CHECK(head.target == "/a/b?c=d");
    CHECK(head.version == Version::Http11);
    REQUIRE(head.headers.size() == 2);
    CHECK(head.headers.get("host") == "example.com");
    CHECK(head.headers.get("x-trace") == "abc");
    CHECK(parser.remainder() == "body-bytes");
    CHECK(parser.consumed() ==
          std::string_view{"GET /a/b?c=d HTTP/1.1\r\nHost: example.com\r\nX-Trace: abc\r\n\r\n"}.size());
}

TEST_CASE("h1/parser: incomplete input keeps asking for more", "[h1]") {
    H1Parser parser;
    parser.feed("GET / HTTP/1.1\r\nHost: x");
    CHECK(parser.parse_head() == H1Parser::State::NeedMore);
    CHECK(parser.buffered() == 23);  // "GET / HTTP/1.1\r\nHost: x"

    parser.feed("\r\n\r\n");
    CHECK(parser.parse_head() == H1Parser::State::Done);
    CHECK(parser.head().headers.get("host") == "x");
    CHECK(parser.remainder().empty());
}

TEST_CASE("h1/parser: bare LF line endings are tolerated", "[h1]") {
    H1Parser parser;
    parser.feed("POST /x HTTP/1.1\ncontent-length: 3\n\nabc");
    REQUIRE(parser.parse_head() == H1Parser::State::Done);
    CHECK(parser.head().method == Method::Post);
    CHECK(parser.head().headers.get("content-length") == "3");
    CHECK(parser.remainder() == "abc");
}

TEST_CASE("h1/parser: feeding one byte at a time finds the head end", "[h1]") {
    const std::string request = "GET /byte HTTP/1.1\r\nHost: h\r\n\r\nrest";
    H1Parser parser;
    for (std::size_t i = 0; i + 1 < request.size(); ++i) {
        parser.feed(std::string_view{request}.substr(i, 1));
        // The head ends at the last of the four CRLF bytes; until then: NeedMore.
        const std::size_t end = request.find("\r\n\r\n");
        if (i < end + 3) {
            REQUIRE(parser.parse_head() == H1Parser::State::NeedMore);
        }
    }
    parser.feed(std::string_view{request}.substr(request.size() - 1));
    REQUIRE(parser.parse_head() == H1Parser::State::Done);
    CHECK(parser.head().target == "/byte");
    CHECK(parser.remainder() == "rest");
}

TEST_CASE("h1/parser: versions and extension methods", "[h1]") {
    {
        H1Parser parser;
        parser.feed("GET / HTTP/1.0\r\n\r\n");
        REQUIRE(parser.parse_head() == H1Parser::State::Done);
        CHECK(parser.head().version == Version::Http1);
    }
    {
        H1Parser parser;
        parser.feed("PROPFIND /dav HTTP/1.1\r\n\r\n");  // an extension method
        REQUIRE(parser.parse_head() == H1Parser::State::Done);
        CHECK(parser.head().method == Method::Unknown);
        CHECK(parser.head().method_token == "PROPFIND");
    }
    {
        H1Parser parser;
        parser.feed("GET / HTTP/2.0\r\n\r\n");
        CHECK(parser.parse_head() == H1Parser::State::Error);
        CHECK(parser.error() == 40005);
    }
}

TEST_CASE("h1/parser: malformed heads are rejected with a reason code", "[h1]") {
    {
        H1Parser parser;
        parser.feed("GET/ HTTP/1.1\r\n\r\n");  // no space after the method
        CHECK(parser.parse_head() == H1Parser::State::Error);
        CHECK(parser.error() == 40001);
    }
    {
        H1Parser parser;
        parser.feed("GET  HTTP/1.1\r\n\r\n");  // empty target
        CHECK(parser.parse_head() == H1Parser::State::Error);
        CHECK(parser.error() == 40002);
    }
    {
        H1Parser parser;
        parser.feed("GET / HTTP/1.1\r\nHost example.com\r\n\r\n");  // missing ':'
        CHECK(parser.parse_head() == H1Parser::State::Error);
        CHECK(parser.error() == 40003);
    }
    {
        H1Parser parser;
        parser.feed("GET / HTTP/1.1\r\n" + std::string(201, 'a') + ": v\r\n\r\n");
        CHECK(parser.parse_head() == H1Parser::State::Error);
        CHECK(parser.error() == 40004);  // field names are bounded
    }
    {
        H1Parser parser;
        // obs-fold (a continuation line) has no ':' of its own: rejected, not spliced.
        parser.feed("GET / HTTP/1.1\r\nHost: x\r\n  folded\r\n\r\n");
        CHECK(parser.parse_head() == H1Parser::State::Error);
        CHECK(parser.error() == 40003);
    }
}

TEST_CASE("h1/parser: header value whitespace is trimmed", "[h1]") {
    H1Parser parser;
    parser.feed("GET / HTTP/1.1\r\nA:   spaced   \r\nB:\t\ttabbed\t\r\nC:\r\n\r\n");
    REQUIRE(parser.parse_head() == H1Parser::State::Done);
    CHECK(parser.head().headers.get("a") == "spaced");
    CHECK(parser.head().headers.get("b") == "tabbed");
    CHECK(parser.head().headers.get("c") == "");
}

TEST_CASE("h1/parser: reset_after_head serves the next pipelined request", "[h1]") {
    H1Parser parser;
    parser.feed("GET /one HTTP/1.1\r\n\r\nGET /two HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(parser.parse_head() == H1Parser::State::Done);
    CHECK(parser.head().target == "/one");

    parser.reset_after_head();
    REQUIRE(parser.parse_head() == H1Parser::State::Done);
    CHECK(parser.head().target == "/two");
    CHECK(parser.head().headers.get("host") == "h");
    CHECK(parser.remainder().empty());
}

// --- response head -----------------------------------------------------------

TEST_CASE("h1/response parser: a plain response head", "[h1]") {
    H1ResponseParser parser;
    parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nServer: x\r\n\r\nhello");

    REQUIRE(parser.parse_head() == H1ResponseParser::State::Done);
    CHECK(parser.head().status == 200);
    CHECK(parser.head().version == Version::Http11);
    CHECK(parser.head().headers.get("content-length") == "5");
    CHECK(parser.head().headers.get("server") == "x");
    CHECK(parser.remainder() == "hello");
}

TEST_CASE("h1/response parser: reason phrase is optional, versions and codes", "[h1]") {
    {
        H1ResponseParser parser;
        parser.feed("HTTP/1.1 200\r\n\r\n");
        REQUIRE(parser.parse_head() == H1ResponseParser::State::Done);
        CHECK(parser.head().status == 200);
    }
    {
        H1ResponseParser parser;
        parser.feed("HTTP/1.0 301 Moved Permanently\r\n\r\n");
        REQUIRE(parser.parse_head() == H1ResponseParser::State::Done);
        CHECK(parser.head().version == Version::Http1);
        CHECK(parser.head().status == 301);
    }
    for (int code : {100, 101, 103, 204, 304, 599}) {
        H1ResponseParser parser;
        parser.feed("HTTP/1.1 " + std::to_string(code) + " whatever\r\n\r\n");
        REQUIRE(parser.parse_head() == H1ResponseParser::State::Done);
        CHECK(parser.head().status == code);
    }
}

TEST_CASE("h1/response parser: malformed heads", "[h1]") {
    {
        H1ResponseParser parser;
        parser.feed("HTTP/2 200 OK\r\n\r\n");  // not an HTTP/1.x version
        CHECK(parser.parse_head() == H1ResponseParser::State::Error);
    }
    {
        H1ResponseParser parser;
        parser.feed("HTTP/1.1 20 OK\r\n\r\n");  // two-digit status
        CHECK(parser.parse_head() == H1ResponseParser::State::Error);
    }
    {
        H1ResponseParser parser;
        parser.feed("HTTP/1.1 2x0 OK\r\n\r\n");
        CHECK(parser.parse_head() == H1ResponseParser::State::Error);
    }
    {
        H1ResponseParser parser;
        parser.feed("HTTP/1.1 999 OK\r\n\r\n");  // out of range
        CHECK(parser.parse_head() == H1ResponseParser::State::Error);
    }
    {
        H1ResponseParser parser;
        parser.feed("HTTP/1.1 200 OK\r\nBroken header\r\n\r\n");
        CHECK(parser.parse_head() == H1ResponseParser::State::Error);
    }
    {
        H1ResponseParser parser;
        parser.feed("garbage\r\n\r\n");
        CHECK(parser.parse_head() == H1ResponseParser::State::Error);
    }
}

TEST_CASE("h1/response parser: incremental input and bare LF", "[h1]") {
    H1ResponseParser parser;
    const std::string response = "HTTP/1.1 204 No Content\nx-empty: 1\n\n";
    for (std::size_t i = 0; i < response.size(); ++i) {
        parser.feed(std::string_view{response}.substr(i, 1));
        const auto state = parser.parse_head();
        if (i + 1 < response.size()) {
            REQUIRE(state == H1ResponseParser::State::NeedMore);
        } else {
            REQUIRE(state == H1ResponseParser::State::Done);
        }
    }
    CHECK(parser.head().status == 204);
    CHECK(parser.head().headers.get("x-empty") == "1");
}

TEST_CASE("h1/response parser: reset_after_head skips an informational response", "[h1]") {
    H1ResponseParser parser;
    parser.feed("HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    REQUIRE(parser.parse_head() == H1ResponseParser::State::Done);
    CHECK(parser.head().status == 103);
    CHECK(parser.head().headers.get("link") == "</a>");

    parser.reset_after_head();  // what the client does before the final head
    REQUIRE(parser.parse_head() == H1ResponseParser::State::Done);
    CHECK(parser.head().status == 200);
    CHECK(parser.head().headers.get("content-length") == "2");
    CHECK(parser.remainder() == "ok");
}

TEST_CASE("ws proxy: the replayed upgrade request keeps its shape", "[h1]") {
    // engine/h1/ws_proxy.h rebuilds the backend request from the parsed head.
    ParsedHead head;
    head.method = Method::Get;
    head.method_token = "GET";
    head.target = "/chat?reconnectionToken=abc";
    head.version = Version::Http11;
    head.headers.add("upgrade", "websocket");
    head.headers.add("sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");

    const std::string rebuilt = rebuild_request_head(head);
    CHECK(rebuilt.starts_with("GET /chat?reconnectionToken=abc HTTP/1.1\r\n"));
    CHECK(rebuilt.find("upgrade: websocket\r\n") != std::string::npos);
    CHECK(rebuilt.find("sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos);
    CHECK(rebuilt.ends_with("\r\n\r\n"));

    const std::string rewritten = rebuild_request_head(head, "/chat");
    CHECK(rewritten.starts_with("GET /chat HTTP/1.1\r\n"));  // the override replaces the target

    ParsedHead http10 = head;
    http10.version = Version::Http1;
    CHECK(rebuild_request_head(http10).starts_with("GET /chat?reconnectionToken=abc HTTP/1.0\r\n"));
}

TEST_CASE("h1/parser: forbidden field-value bytes, SWAR fast path matches the naive check", "[h1]") {
    namespace hd = h1_detail;
    auto naive = [](std::string_view v) {
        for (char ch : v) {
            auto c = static_cast<unsigned char>(ch);
            if (c == '\t') continue;
            if (c < 0x20 || c == 0x7F) return true;
        }
        return false;
    };

    // Every single byte value, alone.
    for (int b = 0; b < 256; ++b) {
        const std::string s(1, static_cast<char>(b));
        CHECK(hd::has_forbidden_field_value_byte(s) == naive(s));
    }

    // All two-byte combinations over a representative subset of the byte range,
    // so masked lanes straddle the 8-byte window at every offset.
    for (unsigned a = 0; a < 256; a += 7) {
        for (unsigned b = 0; b < 256; b += 11) {
            const std::string s{static_cast<char>(a), static_cast<char>(b)};
            CHECK(hd::has_forbidden_field_value_byte(s) == naive(s));
        }
    }

    // Boundary lengths around the 8-byte scan window, with a forbidden byte at
    // the tail (crossing mask edges).
    for (std::size_t len : {0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 33u}) {
        const int tails[] = {0x01, 0x1F, 0x20, 0x7F, 0x09, static_cast<int>('A')};
        for (int tail : tails) {
            std::string s(static_cast<std::size_t>(len), 'A');
            if (len > 0) {
                s[0] = static_cast<char>(tail);        // head
                s.back() = static_cast<char>(tail);    // tail
            }
            CHECK(hd::has_forbidden_field_value_byte(s) == naive(s));
        }
    }

    // Tab-only and tab-heavy values stay legal.
    CHECK_FALSE(hd::has_forbidden_field_value_byte("\t\t\t"));
    CHECK_FALSE(hd::has_forbidden_field_value_byte("a\tb\tc"));

    // Deterministic pseudo-random fuzz over the full byte range.
    std::uint32_t x = 0x12345678u;
    for (int iter = 0; iter < 50000; ++iter) {
        x = x * 1103515245u + 12345u;
        std::size_t len = (x >> 16) % 40;
        std::string s(len, '\0');
        for (std::size_t j = 0; j < len; ++j) {
            x = x * 1103515245u + 12345u;
            s[j] = static_cast<char>(x >> 24);
        }
        CHECK(hd::has_forbidden_field_value_byte(s) == naive(s));
    }
}
