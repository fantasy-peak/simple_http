#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "simple_http.h"

using namespace simple_http;

TEST_CASE("Base64UrlEncode and Decode", "[utils]") {
    SECTION("Normal string") {
        std::string input = "hello world";
        std::string encoded = base64UrlEncode(input);
        std::string decoded = base64UrlDecode(encoded);
        CHECK(input == decoded);
    }

    SECTION("Empty string") {
        std::string input = "";
        std::string encoded = base64UrlEncode(input);
        std::string decoded = base64UrlDecode(encoded);
        CHECK(input == decoded);
    }

    SECTION("Special characters (url safe)") {
        // Base64Url replaces + and / with - and _
        // And usually omits padding
        std::string input = "f?o/b*a&r+";
        std::string encoded = base64UrlEncode(input);
        
        CHECK_FALSE(encoded.find('+') != std::string::npos);
        CHECK_FALSE(encoded.find('/') != std::string::npos);
        
        std::string decoded = base64UrlDecode(encoded);
        CHECK(input == decoded);
    }

    SECTION("All alphabet characters") {
        std::string input = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
        std::string encoded = base64UrlEncode(input);
        std::string decoded = base64UrlDecode(encoded);
        CHECK(input == decoded);
    }
}

TEST_CASE("toLower", "[utils]") {
    std::string input = "HeLLo WORLD";
    toLower(input);
    CHECK(input == "hello world");

    input = "123!@# ABC";
    toLower(input);
    CHECK(input == "123!@# abc");
}

TEST_CASE("isHttp2", "[protocol]") {
    // HTTP/2 preface starts with "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
    CHECK(isHttp2("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"));
    CHECK(isHttp2("PRI * ... extra data ...")); 
    CHECK_FALSE(isHttp2("GET / HTTP/1.1\r\n"));
    CHECK_FALSE(isHttp2(""));
    CHECK_FALSE(isHttp2("PRI ")); // Too short
    CHECK_FALSE(isHttp2("PRJ * ")); // Wrong character
}

TEST_CASE("HttpRequestReader URI parsing", "[reader]") {
    asio::ip::tcp::endpoint endpoint;
    HttpRequestReader reader(Version::Http11, endpoint);

    SECTION("Simple path") {
        reader.setTarget("/hello");
        CHECK(reader.path() == "/hello");
        CHECK(reader.query() == "");
    }

    SECTION("Path and query") {
        reader.setTarget("/hello?name=world&age=20");
        CHECK(reader.path() == "/hello");
        CHECK(reader.query() == "name=world&age=20");
    }

    SECTION("Multiple question marks") {
        reader.setTarget("/hello?name=world?age=20");
        CHECK(reader.path() == "/hello");
        CHECK(reader.query() == "name=world?age=20");
    }

    SECTION("Path with fragment (not handled by simple splitPathAndQuery but let's see current behavior)") {
        reader.setTarget("/hello#fragment");
        // splitPathAndQuery only looks for '?'
        CHECK(reader.path() == "/hello#fragment");
        CHECK(reader.query() == "");
    }

    SECTION("Path with question mark and fragment") {
        reader.setTarget("/hello?q=1#fragment");
        CHECK(reader.path() == "/hello");
        CHECK(reader.query() == "q=1#fragment");
    }

    SECTION("Path with trailing question mark") {
        reader.setTarget("/hello?");
        CHECK(reader.path() == "/hello");
        CHECK(reader.query() == "");
    }

    SECTION("Only query?") {
        reader.setTarget("?query=1");
        CHECK(reader.path() == "");
        CHECK(reader.query() == "query=1");
    }
}

TEST_CASE("MIME types", "[constants]") {
    CHECK(mime::text_plain == "text/plain");
    CHECK(mime::app_json == "application/json");
    CHECK(mime::text_html == "text/html");
}

TEST_CASE("HttpRequestReader headers", "[reader]") {
    asio::ip::tcp::endpoint endpoint;
    HttpRequestReader reader(Version::Http11, endpoint);

    reader.setHeader("content-type", "application/json");
    reader.setHeader("x-custom-header", "Value123");

    SECTION("Case-insensitive retrieval") {
        CHECK(reader.getHeader("Content-Type") == "application/json");
        CHECK(reader.getHeader("content-type") == "application/json");
        CHECK(reader.getHeader("CONTENT-TYPE") == "application/json");
    }

    SECTION("Unknown header") {
        CHECK(reader.getHeader("Non-Existent") == "");
    }

    SECTION("Multiple headers") {
        CHECK(reader.getHeader("X-Custom-Header") == "Value123");
    }
}

TEST_CASE("LogLevel toString", "[utils]") {
    CHECK(toString(LogLevel::Debug) == "Debug");
    CHECK(toString(LogLevel::Info) == "Info");
    CHECK(toString(LogLevel::Error) == "Error");
    CHECK(toString(static_cast<LogLevel>(99)) == "Unknown");
}

TEST_CASE("makeHttpResponse", "[utils]") {
    auto res = makeHttpResponse(http::status::not_found, mime::app_json);
    CHECK(res->result() == http::status::not_found);
    CHECK(res->at(http::field::content_type) == "application/json");
    CHECK(res->at(http::field::server) == server_version);
}
