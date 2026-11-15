// Response trailers (RFC 9110 §6.5): the Response API that accumulates them and
// the wire shape each engine produces.
//
//   * proto/response: trailer() accumulates, finish()/send() route through
//     send_trailers, and a rejected field poisons the whole response.
//   * h1 engine: the chunked terminator carries the trailer section
//     ("0\r\n<fields>\r\n").
//   * h2 engine: the final DATA frame does NOT carry END_STREAM; a trailing
//     HEADERS frame with END_STREAM follows, HPACK-encoded.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::codec;
using namespace simple_http::test;

namespace {

void append_frame(std::string &out, H2FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                  std::string_view payload) {
    serialize_frame_header(out, static_cast<std::uint32_t>(payload.size()), static_cast<std::uint8_t>(type), flags,
                           stream_id);
    out.append(payload);
}

// Whether the recorded client->server stream contains a GOAWAY.
bool written_has_goaway(const std::string &written) {
    std::string_view w{written};
    if (w.substr(0, kH2ClientPreface.size()) == kH2ClientPreface)
        w.remove_prefix(kH2ClientPreface.size());
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= w.size();) {
        H2FrameHeader hdr;
        if (!parse_frame_header(w.substr(i), hdr))
            return false;
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        if (end > w.size())
            return false;
        if (hdr.type == static_cast<std::uint8_t>(H2FrameType::Goaway))
            return true;
        i = end;
    }
    return false;
}

// --- exercised through FakeResponseSink (no connection) ---------------------

TEST_CASE("proto/response: trailer() is emitted by finish_trailers()", "[proto]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    ResponseWriter res{writer};

    res.trailer("grpc-status", "0").trailer("server-timing", "db;dur=3");
    CHECK(res.has_trailers());

    REQUIRE(run_on(ctx, res.status(status::ok).async_begin(asio::use_awaitable)));
    REQUIRE(run_on(ctx, res.async_write("body", asio::use_awaitable)));
    REQUIRE(run_on(ctx, res.async_finish_trailers(asio::use_awaitable)));

    CHECK(writer->begun);
    CHECK(writer->chunks == std::vector<std::string>{"body"});
    CHECK(writer->sent_trailers);
    CHECK(writer->last_trailers.get("grpc-status").value_or("") == "0");
    CHECK(writer->last_trailers.get("server-timing").value_or("") == "db;dur=3");
}

TEST_CASE("proto/response: finish_trailers(Headers) rejects CR/LF/NUL", "[proto]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    ResponseWriter res{writer};
    REQUIRE(run_on(ctx, res.status(status::ok).async_begin(asio::use_awaitable)));

    Headers trailers;
    trailers.add("x-ok", "fine");
    trailers.add("x-bad", "a\r\nInjected: 1");
    auto ec = run_on(ctx, as_ec(res.async_finish_trailers(std::move(trailers), asio::as_tuple(asio::use_awaitable))));
    REQUIRE(ec.has_value());
    CHECK(*ec == make_error_code(asio::error::invalid_argument));
    CHECK_FALSE(writer->sent_trailers);
}

TEST_CASE("proto/response: a second response is refused", "[proto]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    ResponseWriter res{writer};

    REQUIRE(run_on(ctx, res.status(status::ok).async_send("one", asio::use_awaitable)));
    CHECK(res.started());
    auto second = run_on(
        ctx, as_ec(res.status(status::internal_server_error).async_send("two", asio::as_tuple(asio::use_awaitable))));
    REQUIRE(second.has_value());
    CHECK(second->value() == asio::error::invalid_argument);
    CHECK(writer->calls == 1);
    CHECK(writer->last_status == status::ok);
    CHECK(writer->last_body == "one");
}

TEST_CASE("proto/response: finish() delivers accumulated trailers", "[proto]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    ResponseWriter res{writer};

    res.trailer("grpc-status", "0");
    REQUIRE(run_on(ctx, res.status(status::ok).async_begin(asio::use_awaitable)));
    REQUIRE(run_on(ctx, res.async_write("part", asio::use_awaitable)));
    // A final body chunk rides as an ordinary chunk before the trailer section.
    REQUIRE(run_on(ctx, res.async_finish("tail", asio::use_awaitable)));

    CHECK(writer->chunks == std::vector<std::string>{"part", "tail"});
    CHECK(writer->sent_trailers);
    CHECK_FALSE(writer->finished); // send_last was replaced by send_trailers
    CHECK(writer->last_trailers.get("grpc-status").value_or("") == "0");
}

TEST_CASE("proto/response: one-shot send() with trailers starts a stream", "[proto]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    ResponseWriter res{writer};

    res.trailer("x-tail", "yes");
    REQUIRE(run_on(ctx, res.status(status::ok).async_send("payload", asio::use_awaitable)));

    CHECK(writer->begun);      // framed as a stream, not one-shot
    CHECK(writer->calls == 2); // send_headers + send_trailers
    CHECK(writer->chunks == std::vector<std::string>{"payload"});
    CHECK(writer->sent_trailers);
    CHECK(writer->last_trailers.get("x-tail").value_or("") == "yes");
}

TEST_CASE("proto/response: a CR/LF in a trailer field poisons the response", "[proto]") {
    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseSink>(ctx.get_executor());
    ResponseWriter res{writer};

    res.trailer("x-bad", "a\r\nInjected: 1");
    auto ec = run_on(ctx, as_ec(res.status(status::ok).async_finish_trailers(asio::as_tuple(asio::use_awaitable))));
    REQUIRE(ec.has_value());
    CHECK(ec->value() == asio::error::invalid_argument);
    CHECK_FALSE(writer->sent_trailers);
}

// --- h1 engine: the chunked terminator carries the trailers ------------------

TEST_CASE("h1/engine: trailers ride the chunked terminator", "[h1]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("GET / HTTP/1.1\r\nHost: x\r\n\r\n");

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        co_await res->status(status::ok).content_type("application/grpc").async_begin(asio::use_awaitable);
        co_await res->async_write("hello", asio::use_awaitable);
        res->trailer("grpc-status", "0");
        co_await res->async_finish(asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    const std::string &out = transport->written();
    CHECK(out.find("transfer-encoding: chunked") != std::string::npos);
    CHECK(out.find("5\r\nhello\r\n") != std::string::npos);
    // The body is closed by "0" + the trailer fields + the blank line. (The
    // trailer-less terminator "0\r\n\r\n" does not occur: it is always followed
    // by a field here.)
    CHECK(out.find("0\r\ngrpc-status: 0\r\n\r\n") != std::string::npos);
}

// --- h2 engine: END_STREAM moves from the last DATA to the trailing HEADERS ---

TEST_CASE("h2/engine: trailers are a trailing HEADERS frame with END_STREAM", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    {
        // A minimal GET / request head on stream 1, END_STREAM set.
        std::string block;
        hpack_append_indexed(block, 2); // :method GET
        hpack_append_indexed(block, 7); // :scheme https
        hpack_append_literal(block, ":path", "/");
        hpack_append_literal(block, ":authority", "example.com");
        append_frame(in, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, block);
    }
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        co_await res->status(status::ok).content_type("application/grpc").async_begin(asio::use_awaitable);
        co_await res->async_write("hello", asio::use_awaitable);
        res->trailer("grpc-status", "0");
        co_await res->async_finish(asio::use_awaitable);
    };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    // Walk stream 1's frames in order: response HEADERS, DATA (no END_STREAM),
    // trailing HEADERS (END_STREAM) whose block decodes to the trailer field.
    struct Frame {
        std::uint8_t type;
        std::uint8_t flags;
        std::string payload;
    };
    std::vector<Frame> stream1;
    const std::string &out = transport->written();
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= out.size();) {
        H2FrameHeader hdr;
        REQUIRE(parse_frame_header(out.substr(i), hdr));
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        REQUIRE(end <= out.size());
        if (hdr.stream_id == 1 && (hdr.type == static_cast<std::uint8_t>(H2FrameType::Headers) ||
                                   hdr.type == static_cast<std::uint8_t>(H2FrameType::Data))) {
            stream1.push_back({hdr.type, hdr.flags, out.substr(i + kH2FrameHeaderSize, hdr.length)});
        }
        i = end;
    }

    REQUIRE(stream1.size() >= 3);
    const auto headers = static_cast<std::uint8_t>(H2FrameType::Headers);
    const auto data = static_cast<std::uint8_t>(H2FrameType::Data);
    CHECK(stream1[0].type == headers);
    CHECK(stream1[1].type == data);
    CHECK(stream1[1].payload == "hello");
    CHECK((stream1[1].flags & H2_FLAG_END_STREAM) == 0); // END_STREAM withheld
    CHECK(stream1[2].type == headers);
    CHECK((stream1[2].flags & H2_FLAG_END_STREAM) != 0);
    CHECK((stream1[2].flags & H2_FLAG_END_HEADERS) != 0);

    HpackDecoder decoder;
    std::vector<HpackHeader> trailers;
    REQUIRE(decoder.decode(stream1[2].payload, trailers));
    bool saw_status = false;
    for (const auto &field : trailers) {
        if (field.name == "grpc-status" && field.value == "0")
            saw_status = true;
    }
    CHECK(saw_status);
}

// --- protocols without a place for a trailer section: the body must stay intact ---

TEST_CASE("h1/engine: an HTTP/1.0 streamed body is close-delimited, not chunk-framed", "[h1]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    // HTTP/1.0 has no chunked framing. The request asks for keep-alive, so only
    // the connection close can delimit the body: the engine must close after the
    // response and not answer the pipelined second request.
    transport->push("GET / HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"
                    "GET / HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        co_await res->status(status::ok).content_type("text/plain").async_begin(asio::use_awaitable);
        co_await res->async_write("first ", asio::use_awaitable);
        co_await res->async_write("second", asio::use_awaitable);
        res->trailer("x-checksum", "abc123");
        co_await res->async_finish(asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    const std::string &out = transport->written();
    CHECK(out.find("transfer-encoding") == std::string::npos);
    // The old bug wrote the chunk framing into the body ("6\r\nfirst \r\n...").
    CHECK(out.find("\r\n6\r\nfirst") == std::string::npos);
    const auto head_end = out.find("\r\n\r\n");
    REQUIRE(head_end != std::string::npos);
    // Raw bytes, and the trailer fields dropped (there is nowhere to put them).
    CHECK(out.substr(head_end + 4) == "first second");
    // Exactly one response: the close-delimited body ended the connection.
    CHECK(out.find("HTTP/1.0 200 OK") == out.rfind("HTTP/1.0 200 OK"));
}

TEST_CASE("h1/engine: one-shot send() with trailers on HTTP/1.0 keeps its Content-Length", "[h1]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("GET / HTTP/1.0\r\nHost: x\r\n\r\n");

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        res->trailer("x-checksum", "abc123");
        co_await res->status(status::ok).content_type("text/plain").async_send("hello", asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    const std::string &out = transport->written();
    CHECK(out.find("content-length: 5") != std::string::npos);
    CHECK(out.find("transfer-encoding") == std::string::npos);
    CHECK(out.find("x-checksum") == std::string::npos); // nowhere to carry it
    const auto head_end = out.find("\r\n\r\n");
    REQUIRE(head_end != std::string::npos);
    CHECK(out.substr(head_end + 4) == "hello");
}

TEST_CASE("h1/engine: a HEAD response with trailers keeps its Content-Length", "[h1]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n");

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        res->trailer("x-checksum", "abc123");
        co_await res->status(status::ok).content_type("text/plain").async_send("hello", asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    const std::string &out = transport->written();
    CHECK(out.find("content-length: 5") != std::string::npos);
    CHECK(out.find("transfer-encoding") == std::string::npos);
    CHECK(out.find("connection: close") == std::string::npos);
    const auto head_end = out.find("\r\n\r\n");
    REQUIRE(head_end != std::string::npos);
    CHECK(out.substr(head_end + 4).empty()); // headers only
}

TEST_CASE("h1/engine: a 204 with trailers stays bodyless", "[h1]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("GET / HTTP/1.1\r\nHost: x\r\n\r\n");

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        res->trailer("x-checksum", "abc123");
        co_await res->status(status::no_content).async_send("must not appear", asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    const std::string &out = transport->written();
    CHECK(out.find("204 No Content") != std::string::npos);
    CHECK(out.find("transfer-encoding") == std::string::npos);
    CHECK(out.find("must not appear") == std::string::npos);
    const auto head_end = out.find("\r\n\r\n");
    REQUIRE(head_end != std::string::npos);
    CHECK(out.substr(head_end + 4).empty());
}

TEST_CASE("h2/engine: a HEAD response with trailers sends headers and reports success", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    {
        std::string block;
        hpack_append_literal(block, ":method", "HEAD");
        hpack_append_indexed(block, 7); // :scheme https
        hpack_append_literal(block, ":path", "/");
        hpack_append_literal(block, ":authority", "example.com");
        append_frame(in, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, block);
    }
    transport->push(std::move(in));

    error_code send_ec = make_error_code(asio::error::would_block); // sentinel
    auto dispatcher = [&send_ec](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                                 SslHandle) -> asio::awaitable<void> {
        res->trailer("x-checksum", "abc123");
        send_ec = co_await as_ec(res->status(status::ok)
                                     .content_type("text/plain")
                                     .async_send("hello", asio::as_tuple(asio::use_awaitable)));
    };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    // A HEAD response was sent with END_STREAM on its HEADERS frame, so the
    // trailers had nowhere to go - but that is not an error: the alternative
    // returned not_connected here while the response itself was fine.
    CHECK_FALSE(send_ec);
    int headers_frames = 0;
    int data_frames = 0;
    const std::string &out = transport->written();
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= out.size();) {
        H2FrameHeader hdr;
        REQUIRE(parse_frame_header(out.substr(i), hdr));
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        REQUIRE(end <= out.size());
        if (hdr.stream_id == 1 && hdr.type == static_cast<std::uint8_t>(H2FrameType::Headers)) {
            ++headers_frames;
            CHECK((hdr.flags & H2_FLAG_END_STREAM) != 0);
        }
        if (hdr.stream_id == 1 && hdr.type == static_cast<std::uint8_t>(H2FrameType::Data))
            ++data_frames;
        i = end;
    }
    CHECK(headers_frames == 1);
    CHECK(data_frames == 0);
}

TEST_CASE("h2/engine: a streamed HEAD response ends the stream without an error", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    {
        std::string block;
        hpack_append_literal(block, ":method", "HEAD");
        hpack_append_indexed(block, 7); // :scheme https
        hpack_append_literal(block, ":path", "/");
        hpack_append_literal(block, ":authority", "example.com");
        append_frame(in, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, block);
    }
    transport->push(std::move(in));

    error_code begin_ec = make_error_code(asio::error::would_block);  // sentinel
    error_code write_ec = make_error_code(asio::error::would_block);  // sentinel
    error_code finish_ec = make_error_code(asio::error::would_block); // sentinel
    auto dispatcher = [&](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                          SslHandle) -> asio::awaitable<void> {
        begin_ec = co_await as_ec(
            res->status(status::ok).content_type("text/plain").async_begin(asio::as_tuple(asio::use_awaitable)));
        write_ec = co_await as_ec(res->async_write("dropped", asio::as_tuple(asio::use_awaitable)));
        res->trailer("x-checksum", "abc123");
        finish_ec = co_await as_ec(res->async_finish_trailers(asio::as_tuple(asio::use_awaitable)));
    };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    // begin() ends the stream for HEAD; the following writes must still be
    // accepted as no-ops rather than reported as not_connected.
    CHECK_FALSE(begin_ec);
    CHECK_FALSE(write_ec);
    CHECK_FALSE(finish_ec);
    int data_frames = 0;
    const std::string &out = transport->written();
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= out.size();) {
        H2FrameHeader hdr;
        REQUIRE(parse_frame_header(out.substr(i), hdr));
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        REQUIRE(end <= out.size());
        if (hdr.stream_id == 1 && hdr.type == static_cast<std::uint8_t>(H2FrameType::Data))
            ++data_frames;
        i = end;
    }
    CHECK(data_frames == 0);
}

TEST_CASE("h2/engine: a 204 response never carries a DATA frame", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());

    std::string in{codec::kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    {
        std::string block;
        hpack_append_indexed(block, 2); // :method GET
        hpack_append_indexed(block, 7); // :scheme https
        hpack_append_literal(block, ":path", "/");
        hpack_append_literal(block, ":authority", "example.com");
        append_frame(in, H2FrameType::Headers, H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, block);
    }
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        co_await res->status(status::no_content).async_send("must not appear", asio::use_awaitable);
    };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    // 204 has no content (RFC 9110 §15.3.5): the header block ends the stream.
    int headers_frames = 0;
    int data_frames = 0;
    bool saw_204 = false;
    const std::string &out = transport->written();
    for (std::size_t i = 0; i + kH2FrameHeaderSize <= out.size();) {
        H2FrameHeader hdr;
        REQUIRE(parse_frame_header(out.substr(i), hdr));
        const std::size_t end = i + kH2FrameHeaderSize + hdr.length;
        REQUIRE(end <= out.size());
        if (hdr.stream_id == 1 && hdr.type == static_cast<std::uint8_t>(H2FrameType::Headers)) {
            ++headers_frames;
            CHECK((hdr.flags & H2_FLAG_END_STREAM) != 0);
            HpackDecoder decoder;
            std::vector<HpackHeader> fields;
            REQUIRE(decoder.decode(out.substr(i + kH2FrameHeaderSize, hdr.length), fields));
            for (const auto &field : fields) {
                if (field.name == ":status" && field.value == "204")
                    saw_204 = true;
            }
        }
        if (hdr.stream_id == 1 && hdr.type == static_cast<std::uint8_t>(H2FrameType::Data))
            ++data_frames;
        i = end;
    }
    CHECK(saw_204);
    CHECK(headers_frames == 1);
    CHECK(data_frames == 0);
}

// --- h1 response framing edge cases -----------------------------------------

TEST_CASE("h1/engine: an empty write does not terminate a chunked body", "[h1]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("GET / HTTP/1.1\r\nHost: x\r\n\r\n");

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_begin(asio::use_awaitable);
        co_await res->async_write("", asio::use_awaitable); // SSE heartbeat
        co_await res->async_write("tail", asio::use_awaitable);
        co_await res->async_finish(asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    const std::string &out = transport->written();
    CHECK(out.find("4\r\ntail\r\n") != std::string::npos);
    // The only "0\r\n\r\n" is the one finish() writes, at the very end.
    REQUIRE(out.rfind("0\r\n\r\n") != std::string::npos);
    CHECK(out.find("0\r\n\r\n") == out.rfind("0\r\n\r\n"));
    CHECK(out.size() == out.rfind("0\r\n\r\n") + 5);
}

TEST_CASE("h1/engine: an unfinished streamed response is not reused", "[h1]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
                    "GET /b HTTP/1.1\r\nHost: x\r\n\r\n");

    int dispatched = 0;
    auto dispatcher = [&dispatched](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                                    SslHandle) -> asio::awaitable<void> {
        ++dispatched;
        // begin() without finish(): the chunked body has no terminator, so the
        // connection cannot carry another response.
        co_await res->status(status::ok).async_begin(asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    CHECK(dispatched == 1);
    CHECK(transport->closed());
}

TEST_CASE("h1/engine: a HEAD error response has no body", "[h1]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->push("HEAD / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter> res,
                         SslHandle) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("unexpected", asio::use_awaitable);
    };

    auto engine = std::make_shared<Http1Engine<MockTransport>>(transport);
    REQUIRE(run_on(ctx, run_engine(*engine, dispatcher)));

    const std::string &out = transport->written();
    CHECK(out.rfind("HTTP/1.1 400", 0) == 0);
    // Header section only: no "Bad Request" body after the blank line.
    REQUIRE(out.rfind("\r\n\r\n") != std::string::npos);
    CHECK(out.size() == out.rfind("\r\n\r\n") + 4);
}

// A stream can be reset while its split trailing HEADERS is still arriving (the
// handler returns, times out or calls close()). Its st.header_block is erased
// with the stream, but the remaining CONTINUATION frames still have to reach
// the HPACK decoder: the peer's dynamic table was already updated when it
// encoded them. Treating them as frames for an unknown stream turned a legal
// sequence into GOAWAY(PROTOCOL_ERROR) for the whole connection.
TEST_CASE("h2/engine: a reset stream still drains a split header block", "[h2]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    transport->hold_open();

    std::string in{kH2ClientPreface};
    append_frame(in, H2FrameType::Settings, 0, 0, "");
    {
        std::string block;
        hpack_append_indexed(block, 2); // :method GET
        hpack_append_indexed(block, 7); // :scheme https
        hpack_append_literal(block, ":path", "/x");
        hpack_append_literal(block, ":authority", "example.com");
        // Complete request head, but no END_STREAM: a body follows.
        append_frame(in, H2FrameType::Headers, H2_FLAG_END_HEADERS, 1, block);
    }
    append_frame(in, H2FrameType::Data, 0, 1, "x");

    std::string trailer_block;
    hpack_append_literal(trailer_block, "x-check", "1");
    const std::size_t half = trailer_block.size() / 2;
    append_frame(in, H2FrameType::Headers, 0, 1, trailer_block.substr(0, half));
    transport->push(std::move(in));

    auto dispatcher = [](std::shared_ptr<Request>, std::shared_ptr<ResponseWriter>,
                         SslHandle) -> asio::awaitable<void> { co_return; };

    auto engine = std::make_shared<Http2Engine<MockTransport>>(transport);
    asio::co_spawn(ctx, run_engine(*engine, dispatcher), asio::detached);
    auto pump = [&ctx] {
        ctx.restart();
        ctx.run_for(std::chrono::milliseconds(100));
    };
    pump(); // head + DATA + first trailer fragment; the handler returns (reset)

    std::string rest;
    append_frame(rest, H2FrameType::Continuation, H2_FLAG_END_HEADERS, 1, trailer_block.substr(half));
    transport->push(std::move(rest));
    pump();

    CHECK_FALSE(written_has_goaway(transport->written()));

    transport->release();
    pump(); // EOF: the engine finishes and releases the coroutine
}

} // namespace
