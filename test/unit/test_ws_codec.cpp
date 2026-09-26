// proto/: the WebSocket frame codec and the message layer on top of it
// (reassembly, Ping→Pong, Close→Close), driven over a mock transport.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

// A client-to-server frame: mask bit set, a 4-byte key, payload XORed with it.
//
// WsFrameParser only ever sees this direction, and now rejects an unmasked frame
// (RFC 6455 §5.1). The ws_encode_* helpers produce the *other* direction — server
// frames, never masked — so feeding their output to the parser was testing a
// combination the wire cannot produce. What the two directions share (the length
// encoding, the opcode) is what these cases are actually about.
std::string client_frame(WsOpcode opcode, std::string_view payload) {
    static constexpr unsigned char kMask[4] = {0x11, 0x22, 0x33, 0x44};
    const std::size_t n = payload.size();
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | static_cast<unsigned char>(opcode)));
    if (n < 126) {
        frame.push_back(static_cast<char>(0x80 | n));
    } else if (n < 65536) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((n >> 8) & 0xFF));
        frame.push_back(static_cast<char>(n & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
        }
    }
    frame.append(reinterpret_cast<const char*>(kMask), 4);
    for (std::size_t i = 0; i < n; ++i) {
        frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ kMask[i % 4]));
    }
    return frame;
}

std::string hex_of(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

}  // namespace

// --- handshake ---------------------------------------------------------------

TEST_CASE("ws/handshake: the RFC 6455 accept key", "[ws]") {
    // RFC 6455 §1.3 example.
    CHECK(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    // Another key: the digest changes with the nonce.
    CHECK(ws_accept_key("x3JJHMbDL1EzLkh9GBhXDw==") == "HSmrc0sMlYUkAGmm5OPpG2HaGWk=");
    CHECK(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") != ws_accept_key("different"));
}

TEST_CASE("ws/mask: unmasking in place, byte for byte", "[ws]") {
    // The 8-byte fast path and the byte-wise tail must agree with plain XOR.
    for (std::size_t len : {0u, 1u, 2u, 3u, 4u, 7u, 8u, 9u, 15u, 16u, 17u, 1024u}) {
        std::string data;
        for (std::size_t i = 0; i < len; ++i)
            data.push_back(static_cast<char>(i * 7 + 3));

        const unsigned char key[4] = {0x37, 0xFA, 0x21, 0x3D};
        std::string masked = data;
        ws_unmask(masked.data(), masked.size(), key);

        std::string expected = data;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            expected[i] = static_cast<char>(static_cast<unsigned char>(expected[i]) ^ key[i % 4]);
        }
        CHECK(masked == expected);

        ws_unmask(masked.data(), masked.size(), key);  // and back
        CHECK(masked == data);
    }

    // An all-zero key is a no-op; an all-0xFF key flips every byte.
    const unsigned char zero[4] = {0, 0, 0, 0};
    std::string plain = "hello world";
    std::string copy = plain;
    ws_unmask(copy.data(), copy.size(), zero);
    CHECK(copy == plain);

    const unsigned char ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    ws_unmask(copy.data(), copy.size(), ones);  // XOR with all ones flips every bit
    for (std::size_t i = 0; i < copy.size(); ++i) {
        CHECK(static_cast<unsigned char>(copy[i]) == (static_cast<unsigned char>(plain[i]) ^ 0xFF));
    }

    // High-bit bytes must not sign-extend (they would flip the wrong bits).
    const unsigned char key[4] = {0x80, 0x01, 0xFE, 0x7F};
    std::string high = "\x80\xFF\x00\x7F";
    const std::string high_original = high;
    ws_unmask(high.data(), high.size(), key);
    ws_unmask(high.data(), high.size(), key);
    CHECK(high == high_original);
}

// --- frame encoding ----------------------------------------------------------

TEST_CASE("ws/encode: length framing boundaries", "[ws]") {
    auto header_for = [](std::uint64_t len) {
        char buf[10] = {};
        const std::size_t n = ws_encode_header(buf, WsOpcode::Binary, len);
        return std::string{buf, n};
    };

    CHECK(hex_of(header_for(0)) == "8200");  // FIN|binary, 7-bit length 0
    CHECK(hex_of(header_for(125)) == "827d");
    CHECK(hex_of(header_for(126)) == "827e007e");  // 16-bit form
    CHECK(hex_of(header_for(0xFFFF)) == "827effff");
    CHECK(hex_of(header_for(0x10000)) == "827f0000000000010000");  // 64-bit form

    // Control opcodes share the framing.
    char buf[10] = {};
    CHECK(ws_encode_header(buf, WsOpcode::Ping, 0) == 2);
    CHECK(hex_of(std::string{buf, 2}) == "8900");
    CHECK(hex_of(std::string{buf, ws_encode_header(buf, WsOpcode::Close, 2)}) == "8802");
}

TEST_CASE("ws/encode: frames round-trip through the parser", "[ws]") {
    const std::string payload = "payload with \x01\x02 binary bytes";
    for (auto opcode : {WsOpcode::Text, WsOpcode::Binary}) {
        const std::string frame = client_frame(opcode, payload);
        WsFrameParser parser;
        parser.append(frame);
        WsFrame out;
        REQUIRE(parser.next(out) == WsFrameParser::Status::Frame);
        CHECK(out.opcode == opcode);
        CHECK(out.fin);
        CHECK(out.payload == payload);
    }

    const std::string close = client_frame(WsOpcode::Close, ws_close_payload(1001));
    WsFrameParser parser;
    parser.append(close);
    WsFrame out;
    REQUIRE(parser.next(out) == WsFrameParser::Status::Frame);
    CHECK(out.opcode == WsOpcode::Close);
    CHECK(out.payload == ws_close_payload(1001));  // the two helpers agree
    CHECK(hex_of(ws_close_payload(1000)) == "03e8");
    CHECK(hex_of(ws_close_payload(1001)) == "03e9");
}

// --- frame parsing -----------------------------------------------------------

TEST_CASE("ws/parser: incremental input and several frames per buffer", "[ws]") {
    const std::string a = client_frame(WsOpcode::Text, "first");
    const std::string b = client_frame(WsOpcode::Binary, "second");
    const std::string two = a + b;

    {
        WsFrameParser parser;
        parser.append(two);
        WsFrame out;
        REQUIRE(parser.next(out) == WsFrameParser::Status::Frame);
        CHECK(out.payload == "first");
        REQUIRE(parser.next(out) == WsFrameParser::Status::Frame);
        CHECK(out.opcode == WsOpcode::Binary);
        CHECK(out.payload == "second");
    }
    {
        // One byte at a time, with the frame split across calls.
        WsFrameParser parser;
        WsFrame out;
        for (std::size_t i = 0; i + 1 < two.size(); ++i) {
            parser.append(std::string_view{two}.substr(i, 1));
            const auto status = parser.next(out);
            if (status == WsFrameParser::Status::Frame) {
                CHECK(out.payload == "first");
            } else {
                REQUIRE(status == WsFrameParser::Status::NeedMore);
            }
        }
        parser.append(std::string_view{two}.substr(two.size() - 1));
        // The first frame already came out; the remainder completes the second.
        while (parser.next(out) == WsFrameParser::Status::NeedMore) {
        }
        CHECK(out.payload == "second");
    }
}

TEST_CASE("ws/parser: client frames arrive masked", "[ws]") {
    const std::string framed = ws_client_frame(WsOpcode::Text, "masked hello");
    WsFrameParser parser;
    parser.append(framed);
    WsFrame out;
    REQUIRE(parser.next(out) == WsFrameParser::Status::Frame);
    CHECK(out.opcode == WsOpcode::Text);
    CHECK(out.payload == "masked hello");  // the parser unmasks for us
}

TEST_CASE("ws/parser: protocol violations are errors, not data", "[ws]") {
    {
        // A non-final control frame (RFC 6455 §5.5).
        WsFrameParser parser;
        parser.append(ws_client_frame(WsOpcode::Ping, "x", /*fin=*/false));
        WsFrame out;
        CHECK(parser.next(out) == WsFrameParser::Status::Error);
    }
    {
        // A control frame payload above 125 octets.
        WsFrameParser parser;
        parser.append(ws_client_frame(WsOpcode::Ping, std::string(126, 'p')));
        WsFrame out;
        CHECK(parser.next(out) == WsFrameParser::Status::Error);
    }
    {
        // An unknown opcode.
        WsFrameParser parser;
        parser.append(ws_client_frame(static_cast<WsOpcode>(0x3), "x"));
        WsFrame out;
        CHECK(parser.next(out) == WsFrameParser::Status::Error);
    }
    {
        // A payload larger than the configured bound: refused as soon as the
        // length is known, without waiting for the body to arrive.
        WsFrameParser parser{/*max_payload=*/16};
        std::string header;
        char buf[10] = {};
        header.append(buf, ws_encode_header(buf, WsOpcode::Binary, 1024));
        parser.append(header);  // length only, no payload
        WsFrame out;
        CHECK(parser.next(out) == WsFrameParser::Status::Error);
    }
}

// --- the message layer over a mock transport ---------------------------------

TEST_CASE("ws/backend: messages, fragmentation, Ping and Close", "[ws]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    auto backend =
        std::make_shared<WsBackendImpl<MockTransport>>(transport, /*max_payload=*/4096, std::chrono::seconds(5));
    auto ws = std::make_shared<WebSocket>(backend);
    asio::co_spawn(ctx, ws->run_writer(), asio::detached);  // the pump the handle writes through
    drain(ctx);

    SECTION("a single masked text frame is delivered as one message") {
        transport->push(ws_client_frame(WsOpcode::Text, "hello"));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE(message->has_value());
        CHECK((*message)->data == "hello");
        CHECK((*message)->text);
    }

    SECTION("a binary frame keeps its type") {
        transport->push(ws_client_frame(WsOpcode::Binary, "raw"));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE(message->has_value());
        CHECK((*message)->data == "raw");
        CHECK_FALSE((*message)->text);
    }

    SECTION("fragments are reassembled into one message") {
        transport->push(ws_client_frame(WsOpcode::Text, "frag1-", /*fin=*/false));
        transport->push(ws_client_frame(WsOpcode::Continuation, "frag2-", /*fin=*/false));
        transport->push(ws_client_frame(WsOpcode::Continuation, "frag3", /*fin=*/true));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE(message->has_value());
        CHECK((*message)->data == "frag1-frag2-frag3");
    }

    SECTION("a Ping is answered with a Pong and reading continues") {
        transport->push(ws_client_frame(WsOpcode::Ping, "ping-payload"));
        transport->push(ws_client_frame(WsOpcode::Text, "after ping"));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE(message->has_value());
        CHECK((*message)->data == "after ping");

        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(written.size() >= 2);
        CHECK(static_cast<unsigned char>(written[0]) == 0x8A);  // FIN | Pong
        CHECK(written.find("ping-payload") != std::string::npos);
    }

    SECTION("a Close is answered with a Close and ends the read") {
        transport->push(ws_client_frame(WsOpcode::Close, ws_close_payload(1000)));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE_FALSE(message->has_value());
        CHECK(message->error() == asio::error::eof);

        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(!written.empty());
        CHECK(static_cast<unsigned char>(written[0]) == 0x88);  // FIN | Close
    }

    SECTION("a data frame before the previous message finished is an error") {
        transport->push(ws_client_frame(WsOpcode::Text, "unfinished", /*fin=*/false));
        transport->push(ws_client_frame(WsOpcode::Text, "interrupting", /*fin=*/true));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE_FALSE(message->has_value());
        CHECK(message->error() == asio::error::invalid_argument);
    }

    SECTION("a continuation without a started message is an error") {
        transport->push(ws_client_frame(WsOpcode::Continuation, "orphan", /*fin=*/true));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE_FALSE(message->has_value());
        CHECK(message->error() == asio::error::invalid_argument);
    }

    SECTION("reassembling past max_payload is refused") {
        transport->push(ws_client_frame(WsOpcode::Text, std::string(3000, 'a'), /*fin=*/false));
        transport->push(ws_client_frame(WsOpcode::Continuation, std::string(3000, 'b'), /*fin=*/true));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE_FALSE(message->has_value());
        CHECK(message->error() == asio::error::message_size);
    }

    SECTION("a small payload is written as a single unmasked frame") {
        REQUIRE(run_until(ctx, ws->write("out", /*text=*/true)));
        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(written.size() >= 5);
        CHECK(static_cast<unsigned char>(written[0]) == 0x81);  // FIN | Text
        CHECK(static_cast<unsigned char>(written[1]) == 3);     // server frames are never masked
        CHECK(written.substr(2) == "out");
    }
}
