#include <catch2/catch_test_macros.hpp>

#include "simple_http/client/sse.h"

using simple_http::SseParser;

TEST_CASE("sse: a single data line produces one default message", "[proto][sse]") {
    SseParser parser;
    const auto events = parser.feed("data: hello\n\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].event == "message");
    CHECK(events[0].data == "hello");
}

TEST_CASE("sse: multiple data lines are joined with newlines", "[proto][sse]") {
    SseParser parser;
    const auto events = parser.feed("data: a\ndata: b\n\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "a\nb");
}

TEST_CASE("sse: event, id and retry fields are captured", "[proto][sse]") {
    SseParser parser;
    const auto events = parser.feed("event: update\nid: 42\nretry: 1000\ndata: x\n\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].event == "update");
    CHECK(events[0].id == "42");
    CHECK(events[0].retry_ms == 1000);
    CHECK(events[0].data == "x");
}

TEST_CASE("sse: comments do not dispatch", "[proto][sse]") {
    SseParser parser;
    const auto events = parser.feed(": keep-alive\n\n");
    CHECK(events.empty());
}

TEST_CASE("sse: chunk boundaries and CRLF are handled", "[proto][sse]") {
    SseParser parser;
    CHECK(parser.feed("data: par").empty());
    CHECK(parser.feed("tial\r").empty());
    const auto events = parser.feed("\n\r\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "partial");
}

TEST_CASE("sse: finish flushes a trailing event", "[proto][sse]") {
    SseParser parser;
    CHECK(parser.feed("data: last").empty());
    const auto events = parser.finish();
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "last");
}

TEST_CASE("sse: an id persists across events", "[proto][sse]") {
    SseParser parser;
    const auto events = parser.feed("id: 7\ndata: a\n\ndata: b\n\n");
    REQUIRE(events.size() == 2);
    CHECK(events[0].id == "7");
    CHECK(events[1].id == "7");
}
