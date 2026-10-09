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
// WsFrameParser only ever sees this direction, and now rejects an unmasked
// frame (RFC 6455 §5.1). The ws_encode_* helpers produce the *other* direction
// — server frames, never masked — so feeding their output to the parser was
// testing a combination the wire cannot produce. What the two directions share
// (the length encoding, the opcode) is what these cases are actually about.
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
    frame.append(reinterpret_cast<const char *>(kMask), 4);
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

} // namespace

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

        ws_unmask(masked.data(), masked.size(), key); // and back
        CHECK(masked == data);
    }

    // An all-zero key is a no-op; an all-0xFF key flips every byte.
    const unsigned char zero[4] = {0, 0, 0, 0};
    std::string plain = "hello world";
    std::string copy = plain;
    ws_unmask(copy.data(), copy.size(), zero);
    CHECK(copy == plain);

    const unsigned char ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    ws_unmask(copy.data(), copy.size(),
              ones); // XOR with all ones flips every bit
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

    CHECK(hex_of(header_for(0)) == "8200"); // FIN|binary, 7-bit length 0
    CHECK(hex_of(header_for(125)) == "827d");
    CHECK(hex_of(header_for(126)) == "827e007e"); // 16-bit form
    CHECK(hex_of(header_for(0xFFFF)) == "827effff");
    CHECK(hex_of(header_for(0x10000)) == "827f0000000000010000"); // 64-bit form

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
    CHECK(out.payload == ws_close_payload(1001)); // the two helpers agree
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
    CHECK(out.payload == "masked hello"); // the parser unmasks for us
}

TEST_CASE("ws/parser: a partial payload is delivered once across reads", "[ws]") {
    // A continuation frame carrying a 3-octet UTF-8 codepoint whose last octet
    // arrives in a second read. next() re-parses the same in-flight frame on
    // every call; it must keep the partial-delivery offset rather than reset it,
    // or take_partial_payload() re-delivers (and the caller re-validates) the
    // whole prefix each time — which rejected valid fragmented text messages.
    const std::string frame = client_frame(WsOpcode::Continuation, "\xe2\x82\xac");
    REQUIRE(frame.size() == 9); // 2 header + 4 mask key + 3 payload

    WsFrameParser parser;
    WsFrame out;
    parser.append(std::string_view{frame}.substr(0, 7)); // header + key + 1 payload octet
    REQUIRE(parser.next(out) == WsFrameParser::Status::NeedMore);
    auto partial = parser.take_partial_payload();
    CHECK(partial.first);
    REQUIRE(partial.bytes.size() == 1);
    CHECK(static_cast<unsigned char>(partial.bytes[0]) == 0xE2);

    parser.append(std::string_view{frame}.substr(7)); // the remaining two octets
    REQUIRE(parser.next(out) == WsFrameParser::Status::Frame);
    CHECK(out.opcode == WsOpcode::Continuation);
    CHECK(out.payload == std::string{"\xe2\x82\xac"});
    CHECK(out.already_delivered == 1); // the caller skips the octet it already saw
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
        parser.append(header); // length only, no payload
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
    asio::co_spawn(ctx, ws->run_writer(),
                   asio::detached); // the pump the handle writes through
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
        CHECK(static_cast<unsigned char>(written[0]) == 0x8A); // FIN | Pong
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
        CHECK(static_cast<unsigned char>(written[0]) == 0x88); // FIN | Close
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
        CHECK(static_cast<unsigned char>(written[0]) == 0x81); // FIN | Text
        CHECK(static_cast<unsigned char>(written[1]) == 3);    // server frames are never masked
        CHECK(written.substr(2) == "out");
    }
}

TEST_CASE("ws: incremental UTF-8 validation", "[ws][utf8]") {
    using octets = std::string;

    auto valid = [](std::string_view s) {
        Utf8Validator v;
        return v.feed(s) && v.complete();
    };

    // ASCII-only runs, including a long one that exercises the 8-octet fast path.
    CHECK(valid(""));
    CHECK(valid("hello"));
    CHECK(valid(octets(1024, 'x')));
    CHECK(valid(octets(100, 'a') + " " + octets(100, '\t')));

    // Valid multi-byte sequences.
    CHECK(valid(octets("\xC3\xA9")));                 // é
    CHECK(valid(octets("\xE2\x82\xAC")));             // €
    CHECK(valid(octets("\xF0\x90\x8D\x88")));         // U+10348
    CHECK(valid("a" + octets("\xE2\x82\xAC") + "b")); // UTF-8 around ASCII

    // A sequence split across feed() calls carries its state between them.
    Utf8Validator frag;
    CHECK(frag.feed(octets("\xE2\x82")));
    CHECK_FALSE(frag.complete());
    CHECK(frag.feed(octets("\xAC-tail")));
    CHECK(frag.complete());

    // Feed boundaries never split the ASCII fast path across a sequence start:
    // the lead byte after an ASCII run is where the state machine takes over.
    Utf8Validator split_ascii;
    CHECK(split_ascii.feed(octets("abc\xC3")));
    CHECK(split_ascii.feed(octets("")));
    CHECK_FALSE(split_ascii.complete());
    CHECK(split_ascii.feed(octets("\xA9")));
    CHECK(split_ascii.complete());

    // Rejected: stray continuation / overlong lead bytes at a boundary.
    CHECK_FALSE(valid(octets("\x80")));
    CHECK_FALSE(valid(octets("\xBF")));
    CHECK_FALSE(valid(octets("\xC0\xAF"))); // overlong 2-byte
    CHECK_FALSE(valid("x\xC1\x81"));        // overlong mid-string

    // Rejected: overlong 3-byte, surrogates, and beyond U+10FFFF.
    CHECK_FALSE(valid(octets("\xE0\x80\x80")));     // overlong €-shaped
    CHECK_FALSE(valid(octets("\xED\xA0\x80")));     // U+D800 surrogate
    CHECK_FALSE(valid(octets("\xED\xBF\xBF")));     // U+DFFF surrogate
    CHECK_FALSE(valid(octets("\xF0\x80\x80\x80"))); // overlong 4-byte
    CHECK_FALSE(valid(octets("\xF4\x90\x80\x80"))); // U+110000
    CHECK_FALSE(valid(octets("\xF5\x80\x80\x80"))); // invalid lead

    // Rejected: continuation bytes out of the allowed window.
    CHECK_FALSE(valid("\xC3" + octets("\x40")));         // continuation < 0x80
    CHECK_FALSE(valid("\xF0" + octets("\x8F\x80\x80"))); // first continuation < 0x90
    CHECK_FALSE(valid("\xE2" + octets("\xBF\x40")));     // last continuation < 0x80

    // Truncated sequences: feed() stays pending until complete() is asked.
    Utf8Validator truncated;
    CHECK(truncated.feed(octets("\xE2\x82")));
    CHECK_FALSE(truncated.complete());
}

// --- ping & close-with-code on the handle ------------------------------------

TEST_CASE("ws/backend: ping and close-with-code", "[ws]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    auto backend =
        std::make_shared<WsBackendImpl<MockTransport>>(transport, /*max_payload=*/4096, std::chrono::seconds(5));
    auto ws = std::make_shared<WebSocket>(backend);
    asio::co_spawn(ctx, ws->run_writer(), asio::detached); // the pump writes through
    drain(ctx);

    SECTION("ping writes a Ping frame once") {
        REQUIRE(run_until(ctx, ws->ping("probe")));
        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(written.size() >= 5);
        CHECK(static_cast<unsigned char>(written[0]) == 0x89); // FIN | Ping
        CHECK(static_cast<unsigned char>(written[1]) == 5);    // unmasked, 5 octets
        CHECK(written.substr(2) == "probe");
    }

    SECTION("a Ping payload over 125 octets is refused (control-frame cap)") {
        const auto ec = run_until(ctx, ws->ping(std::string(126, 'a')));
        REQUIRE(ec.has_value());
        CHECK(*ec == asio::error::invalid_argument);
        drain(ctx);
        CHECK(transport->written().empty()); // nothing went out
    }

    SECTION("close() with a code and reason serializes both") {
        REQUIRE(run_until(ctx, ws->close(1001, "going away")));
        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(written.size() >= 4);
        CHECK(static_cast<unsigned char>(written[0]) == 0x88); // FIN | Close
        CHECK(static_cast<unsigned char>(written[2]) == 0x03); // code 1001 = 03 E9
        CHECK(static_cast<unsigned char>(written[3]) == 0xE9);
        CHECK(written.substr(4) == "going away");
    }

    SECTION("a code the peer may never receive is refused") {
        for (std::uint16_t bad : {1004u, 1005u, 1006u, 1015u, 2000u, 5000u}) {
            const auto ec = run_until(ctx, ws->close(bad, ""));
            REQUIRE(ec.has_value());
            CHECK(*ec == asio::error::invalid_argument);
        }
    }

    SECTION("a reason that is not UTF-8 is refused") {
        const auto ec = run_until(ctx, ws->close(1000, std::string{"\xFF\xFE"}));
        REQUIRE(ec.has_value());
        CHECK(*ec == asio::error::invalid_argument);
    }

    SECTION("a reason over 123 octets is refused") {
        const auto ec = run_until(ctx, ws->close(1000, std::string(124, 'x')));
        REQUIRE(ec.has_value());
        CHECK(*ec == asio::error::invalid_argument);
    }
}

// --- permessage-deflate (RFC 7692) --------------------------------------------

TEST_CASE("ws/deflate: negotiation parses offers and renders responses", "[ws][ws-deflate]") {
    // A plain offer is accepted with the defaults and answered in kind.
    {
        auto cfg = ws_parse_deflate_offer("permessage-deflate");
        REQUIRE(cfg.has_value());
        CHECK(cfg->enabled);
        CHECK_FALSE(cfg->server_no_context_takeover);
        CHECK_FALSE(cfg->client_no_context_takeover);
        CHECK(cfg->server_window_bits == 15);
        CHECK(cfg->client_window_bits == 15);
        CHECK(ws_deflate_response_value(*cfg) == "permessage-deflate");
    }
    // No-context-takeover both directions is honoured and echoed.
    {
        auto cfg = ws_parse_deflate_offer("permessage-deflate; server_no_context_takeover; client_no_context_takeover");
        REQUIRE(cfg.has_value());
        CHECK(cfg->server_no_context_takeover);
        CHECK(cfg->client_no_context_takeover);
        CHECK(ws_deflate_response_value(*cfg) ==
              "permessage-deflate; server_no_context_takeover; client_no_context_takeover");
    }
    // Window-bit offers bind the streams and are echoed (RFC 7692 §7.1.2).
    {
        auto cfg = ws_parse_deflate_offer("permessage-deflate; server_max_window_bits=12; client_max_window_bits=10");
        REQUIRE(cfg.has_value());
        CHECK(cfg->server_window_bits == 12);
        CHECK(cfg->client_window_bits == 10);
        CHECK(cfg->client_offered_server_window_bits);
        CHECK(cfg->echo_client_window_bits);
        CHECK(ws_deflate_response_value(*cfg) ==
              "permessage-deflate; server_max_window_bits=12; client_max_window_bits=10");
    }
    // A bare client_max_window_bits (no limit) is accepted without an echo.
    {
        auto cfg = ws_parse_deflate_offer("permessage-deflate; client_max_window_bits");
        REQUIRE(cfg.has_value());
        CHECK(cfg->client_window_bits == 15);
        CHECK_FALSE(cfg->echo_client_window_bits);
        CHECK(ws_deflate_response_value(*cfg) == "permessage-deflate");
    }
    // Whitespace and mixed case are tolerated.
    {
        auto cfg = ws_parse_deflate_offer(" Permessage-Deflate ; Client_Max_Window_Bits = 8 ");
        REQUIRE(cfg.has_value());
        CHECK(cfg->client_window_bits == 8);
        CHECK(cfg->echo_client_window_bits);
        CHECK(ws_deflate_response_value(*cfg) == "permessage-deflate; client_max_window_bits=8");
    }
    // Offers that must be declined: no offer, only foreign extensions, an
    // unknown parameter, a value on a valueless parameter, out-of-range or
    // duplicated window bits, a duplicated extension.
    CHECK_FALSE(ws_parse_deflate_offer("").has_value());
    CHECK_FALSE(ws_parse_deflate_offer("x-webkit-deflate-frame").has_value());
    CHECK_FALSE(ws_parse_deflate_offer("permessage-deflate; unknown_param").has_value());
    CHECK_FALSE(ws_parse_deflate_offer("permessage-deflate; server_no_context_takeover=1").has_value());
    CHECK_FALSE(ws_parse_deflate_offer("permessage-deflate; server_max_window_bits=7").has_value());
    CHECK_FALSE(ws_parse_deflate_offer("permessage-deflate; server_max_window_bits=16").has_value());
    CHECK_FALSE(
        ws_parse_deflate_offer("permessage-deflate; server_max_window_bits=12; server_max_window_bits=12").has_value());
    CHECK_FALSE(ws_parse_deflate_offer("permessage-deflate; permessage-deflate").has_value());
    // A foreign extension alongside ours is ignored, not fatal.
    {
        auto cfg = ws_parse_deflate_offer("x-webkit-deflate-frame, permessage-deflate; client_no_context_takeover");
        REQUIRE(cfg.has_value());
        CHECK(cfg->client_no_context_takeover);
    }
    // Several *alternative* permessage-deflate offers in one header (RFC 6455
    // §9.1 — Autobahn 13.7 sends exactly this): the first acceptable one wins.
    {
        auto cfg = ws_parse_deflate_offer("permessage-deflate; client_no_context_takeover; client_max_window_bits;"
                                          " server_no_context_takeover; server_max_window_bits=9,"
                                          " permessage-deflate; client_no_context_takeover; client_max_window_bits,"
                                          " permessage-deflate");
        REQUIRE(cfg.has_value());
        CHECK(cfg->client_no_context_takeover);
        CHECK(cfg->server_no_context_takeover);
        CHECK(cfg->server_window_bits == 9);
        CHECK(cfg->client_offered_server_window_bits);
        CHECK(ws_deflate_response_value(*cfg) ==
              "permessage-deflate; server_no_context_takeover; client_no_context_takeover; server_max_window_bits=9");
    }
    // A malformed first alternative falls through to a later valid one.
    {
        auto cfg =
            ws_parse_deflate_offer("permessage-deflate; nonsense, permessage-deflate; client_no_context_takeover");
        REQUIRE(cfg.has_value());
        CHECK(cfg->client_no_context_takeover);
    }
}

TEST_CASE("ws/deflate: the codec round-trips through zlib", "[ws][ws-deflate]") {
    WsDeflater deflater(15);
    WsInflater inflater(15);
    REQUIRE(deflater.ok());
    REQUIRE(inflater.ok());

    const std::vector<std::string> payloads{
        std::string{"hello"},                 // tiny, compressible
        std::string{},                        // empty message
        std::string(1024, 'x'),               // compressible run
        std::string{"\xC3\xA9 \xE2\x82\xAC"}, // multibyte UTF-8
        std::string(1024, '\x01'),            // binary-ish, no NUL
    };
    for (const std::string &payload : payloads) {
        SECTION(("payload " + std::to_string(payload.size()) + " bytes").c_str()) {
            std::string compressed;
            bool compressed_flag = false;
            REQUIRE(deflater.compress(payload, compressed, /*reset=*/false, compressed_flag));
            std::string decoded;
            if (compressed_flag) {
                // Only a compressed message reaches the inflater; an empty
                // message is sent uncompressed (RFC 7692 §7.2.3.5) and its
                // decoder is untouched, keeping the streams in step.
                REQUIRE(inflater.feed(compressed, decoded, 65536) == WsInflateStatus::Ok);
                REQUIRE(inflater.finish(decoded, 65536, /*reset=*/false) == WsInflateStatus::Ok);
            }
            CHECK(decoded == payload);
        }
    }

    // Feeding in one-byte chunks (a fragmented message on the wire) decodes
    // exactly the same.
    {
        std::string compressed;
        REQUIRE(deflater.compress("chunked feed", compressed, false));
        std::string decoded;
        for (std::size_t i = 0; i < compressed.size(); ++i) {
            REQUIRE(inflater.feed(std::string_view{compressed}.substr(i, 1), decoded, 65536) == WsInflateStatus::Ok);
        }
        REQUIRE(inflater.finish(decoded, 65536, false) == WsInflateStatus::Ok);
        CHECK(decoded == "chunked feed");
    }

    // Context takeover: a second message may reference the first's window.
    {
        WsDeflater shared(15);
        WsInflater shared_in(15);
        std::string c1, c2;
        REQUIRE(shared.compress(std::string(512, 'a'), c1, /*reset=*/false));
        REQUIRE(shared.compress(std::string(512, 'a'), c2, /*reset=*/false));
        // The second message re-uses the window: strictly smaller than the first.
        CHECK(c2.size() < c1.size());

        std::string d1, d2;
        REQUIRE(shared_in.feed(c1, d1, 65536) == WsInflateStatus::Ok);
        REQUIRE(shared_in.finish(d1, 65536, /*reset=*/false) == WsInflateStatus::Ok);
        REQUIRE(shared_in.feed(c2, d2, 65536) == WsInflateStatus::Ok);
        REQUIRE(shared_in.finish(d2, 65536, /*reset=*/false) == WsInflateStatus::Ok);
        CHECK(d1 == std::string(512, 'a'));
        CHECK(d2 == std::string(512, 'a'));
    }

    // No context takeover: each message stands alone, so sizes do not shrink.
    {
        WsDeflater nct(15);
        WsInflater nct_in(15);
        std::string c1, c2;
        REQUIRE(nct.compress(std::string(512, 'a'), c1, /*reset=*/true));
        REQUIRE(nct.compress(std::string(512, 'a'), c2, /*reset=*/true));
        CHECK(c1.size() == c2.size());
        std::string d1, d2;
        REQUIRE(nct_in.feed(c1, d1, 65536) == WsInflateStatus::Ok);
        REQUIRE(nct_in.finish(d1, 65536, /*reset=*/true) == WsInflateStatus::Ok);
        REQUIRE(nct_in.feed(c2, d2, 65536) == WsInflateStatus::Ok);
        REQUIRE(nct_in.finish(d2, 65536, /*reset=*/true) == WsInflateStatus::Ok);
        CHECK(d1 == std::string(512, 'a'));
        CHECK(d2 == std::string(512, 'a'));
    }

    // Context takeover with an empty message interleaved: an empty message is
    // sent uncompressed (rsv1 clear), so neither direction sees a deflate
    // stream for it — and a subsequent message must still decode using the
    // window carried over from before the empty one. (A regression: the
    // compressor used to fail after an empty message, closing the connection.)
    {
        WsDeflater e(15);
        WsInflater d(15);
        std::string c1, c2, c3, d1, d3;
        bool compressed_flag = false;
        REQUIRE(e.compress("hello deflate", c1, /*reset=*/false, compressed_flag));
        REQUIRE(compressed_flag);
        REQUIRE(e.compress("", c2, /*reset=*/false, compressed_flag));
        CHECK_FALSE(compressed_flag); // empty -> uncompressed
        CHECK(c2.empty());
        REQUIRE(e.compress(std::string(256, 'a'), c3, /*reset=*/false, compressed_flag));
        REQUIRE(compressed_flag);
        // The second compressed message references the first's window even
        // though an empty message sat between them.
        CHECK(c3.size() < 32);

        REQUIRE(d.feed(c1, d1, 65536) == WsInflateStatus::Ok);
        REQUIRE(d.finish(d1, 65536, /*reset=*/false) == WsInflateStatus::Ok);
        CHECK(d1 == "hello deflate");
        // No feed/finish for the empty message (it never touched the wire's
        // deflate stream); the decoder stays in step with the encoder.
        REQUIRE(d.feed(c3, d3, 65536) == WsInflateStatus::Ok);
        REQUIRE(d.finish(d3, 65536, /*reset=*/false) == WsInflateStatus::Ok);
        CHECK(d3 == std::string(256, 'a'));
    }

    // Garbage decodes to an error, not to data.
    {
        std::string decoded;
        REQUIRE(inflater.feed(std::string{"\xDE\xAD\xBE\xEF"}, decoded, 65536) == WsInflateStatus::Error);
    }
}

TEST_CASE("ws/deflate: the backend round-trips compressed messages", "[ws][ws-deflate]") {
    asio::io_context ctx;
    auto transport = std::make_shared<MockTransport>(ctx.get_executor());
    WsDeflateConfig deflate = *ws_parse_deflate_offer("permessage-deflate");
    auto backend = std::make_shared<WsBackendImpl<MockTransport>>(transport, /*max_payload=*/65536,
                                                                  std::chrono::seconds(5), /*expect_masked=*/true,
                                                                  /*inbox_limit=*/4u * 1024 * 1024, deflate);
    auto ws = std::make_shared<WebSocket>(backend);
    asio::co_spawn(ctx, ws->run_writer(), asio::detached);
    drain(ctx);

    const unsigned char mask[4] = {0x37, 0xfa, 0x21, 0x3d};

    SECTION("a compressed text message is decompressed to the original") {
        WsDeflater d(15);
        std::string compressed;
        REQUIRE(d.compress("hello compressed world", compressed, false));
        transport->push(ws_client_frame(WsOpcode::Text, compressed, /*fin=*/true, mask, /*rsv1=*/true));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE(message->has_value());
        CHECK((*message)->data == "hello compressed world");
        CHECK((*message)->text);
    }

    SECTION("a fragmented compressed message reassembles across fragments") {
        WsDeflater d(15);
        std::string compressed;
        REQUIRE(d.compress(std::string(1024, 'q') + "tail", compressed, false));
        const std::size_t half = compressed.size() / 2;
        transport->push(
            ws_client_frame(WsOpcode::Text, compressed.substr(0, half), /*fin=*/false, mask, /*rsv1=*/true));
        transport->push(ws_client_frame(WsOpcode::Continuation, compressed.substr(half), /*fin=*/true, mask));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE(message->has_value());
        CHECK((*message)->data == std::string(1024, 'q') + "tail");
    }

    SECTION("uncompressed messages still pass through once compression is on") {
        transport->push(ws_client_frame(WsOpcode::Text, "plain", /*fin=*/true, mask, /*rsv1=*/false));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE(message->has_value());
        CHECK((*message)->data == "plain");
    }

    SECTION("a compressed write hits the wire with RSV1 and decodes back") {
        REQUIRE(run_until(ctx, ws->write("outbound compressed", /*text=*/true)));
        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(written.size() >= 3);
        CHECK(static_cast<unsigned char>(written[0]) == 0xC1); // RSV1 | FIN | Text
        // server frames are unmasked: header + compressed payload
        const std::size_t len = static_cast<unsigned char>(written[1]);
        REQUIRE(written.size() >= len + 2);
        WsInflater in(15);
        std::string decoded;
        const auto st = in.feed(std::string_view{written}.substr(2, len), decoded, 65536);
        REQUIRE(st == WsInflateStatus::Ok);
        REQUIRE(in.finish(decoded, 65536, false) == WsInflateStatus::Ok);
        CHECK(decoded == "outbound compressed");
    }

    SECTION("a continuation frame carrying RSV1 is a protocol error") {
        WsDeflater d(15);
        std::string compressed;
        REQUIRE(d.compress("two", compressed, false));
        transport->push(ws_client_frame(WsOpcode::Text, "one", /*fin=*/false, mask, /*rsv1=*/false));
        transport->push(ws_client_frame(WsOpcode::Continuation, compressed, /*fin=*/true, mask, /*rsv1=*/true));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE_FALSE(message->has_value());
        CHECK(message->error() == asio::error::invalid_argument);
    }

    SECTION("a compressed frame split across reads is validated only after decompression") {
        // The first read hands out header + a few payload octets; the
        // incremental UTF-8 pass must NOT inspect them (they are still
        // deflated) — that used to fail the connection with 1007. The check
        // is deferred to the decompressed whole message.
        WsDeflater d(15);
        std::string compressed;
        REQUIRE(d.compress("split-in-flights", compressed, false));
        const std::string frame = ws_client_frame(WsOpcode::Text, compressed, /*fin=*/true, mask, /*rsv1=*/true);
        const std::size_t mid = 6; // header + mask + 2 payload octets
        transport->push(frame.substr(0, mid));
        drain(ctx); // the reader must see the partial frame
        transport->push(frame.substr(mid));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE(message->has_value());
        CHECK((*message)->data == "split-in-flights");
    }

    SECTION("corrupt deflate data is a protocol error, not a crash") {
        transport->push(ws_client_frame(WsOpcode::Text, std::string{"\xDE\xAD\xBE\xEF\x00\x01\x02\x03"}, /*fin=*/true,
                                        mask, /*rsv1=*/true));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE_FALSE(message->has_value());
        CHECK(message->error() == asio::error::invalid_argument);
    }

    SECTION("invalid UTF-8 inside a compressed text message fails with 1007") {
        WsDeflater d(15);
        std::string compressed;
        REQUIRE(d.compress(std::string{"\x80\x81"}, compressed, false));
        transport->push(ws_client_frame(WsOpcode::Text, compressed, /*fin=*/true, mask, /*rsv1=*/true));
        auto message = run_until(ctx, ws->read());
        REQUIRE(message.has_value());
        REQUIRE_FALSE(message->has_value());
        CHECK(message->error() == asio::error::invalid_argument);
    }
}

TEST_CASE("ws/deflate: per-connection compression controls", "[ws][ws-deflate]") {
    asio::io_context ctx;

    SECTION("a negotiated connection compresses by default and reports it") {
        auto transport = std::make_shared<MockTransport>(ctx.get_executor());
        WsDeflateConfig deflate = *ws_parse_deflate_offer("permessage-deflate");
        auto backend = std::make_shared<WsBackendImpl<MockTransport>>(transport, /*max_payload=*/65536,
                                                                      std::chrono::seconds(5), /*expect_masked=*/true,
                                                                      /*inbox_limit=*/4u * 1024 * 1024, deflate);
        auto ws = std::make_shared<WebSocket>(backend);
        asio::co_spawn(ctx, ws->run_writer(), asio::detached);
        drain(ctx);

        CHECK(ws->compression_negotiated());
        REQUIRE(run_until(ctx, ws->write("compressed by default", /*text=*/true)));
        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(!written.empty());
        CHECK(static_cast<unsigned char>(written[0]) == 0xC1); // RSV1 | FIN | Text
    }

    SECTION("enable_write_compression(false) turns RSV1 off for later messages") {
        auto transport = std::make_shared<MockTransport>(ctx.get_executor());
        WsDeflateConfig deflate = *ws_parse_deflate_offer("permessage-deflate");
        auto backend = std::make_shared<WsBackendImpl<MockTransport>>(transport, /*max_payload=*/65536,
                                                                      std::chrono::seconds(5), /*expect_masked=*/true,
                                                                      /*inbox_limit=*/4u * 1024 * 1024, deflate);
        auto ws = std::make_shared<WebSocket>(backend);
        asio::co_spawn(ctx, ws->run_writer(), asio::detached);
        drain(ctx);

        REQUIRE(run_until(ctx, ws->write("first compressed", /*text=*/true)));
        REQUIRE(run_until(ctx, ws->enable_write_compression(false)));
        REQUIRE(run_until(ctx, ws->write("then plain", /*text=*/true)));
        REQUIRE(run_until(ctx, ws->enable_write_compression(true)));
        REQUIRE(run_until(ctx, ws->write("compressed again", /*text=*/true)));
        drain(ctx);

        // Each write is one queued frame: walk them and check the RSV1 bit.
        const auto &writes = transport->writes();
        REQUIRE(writes.size() == 3);
        CHECK(static_cast<unsigned char>(writes[0][0]) == 0xC1); // compressed
        CHECK(static_cast<unsigned char>(writes[1][0]) == 0x81); // plain
        CHECK(static_cast<unsigned char>(writes[2][0]) == 0xC1); // compressed again
    }

    SECTION("set_compression_level accepts 0..9 and refuses the rest") {
        auto transport = std::make_shared<MockTransport>(ctx.get_executor());
        WsDeflateConfig deflate = *ws_parse_deflate_offer("permessage-deflate");
        auto backend = std::make_shared<WsBackendImpl<MockTransport>>(transport, /*max_payload=*/65536,
                                                                      std::chrono::seconds(5), /*expect_masked=*/true,
                                                                      /*inbox_limit=*/4u * 1024 * 1024, deflate);
        auto ws = std::make_shared<WebSocket>(backend);
        asio::co_spawn(ctx, ws->run_writer(), asio::detached);
        drain(ctx);

        for (int good : {0, 1, 6, 9}) {
            const auto ec = run_until(ctx, ws->set_compression_level(good));
            REQUIRE(ec.has_value());
            CHECK_FALSE(*ec);
        }
        for (int bad : {-1, 10, 100}) {
            const auto ec = run_until(ctx, ws->set_compression_level(bad));
            REQUIRE(ec.has_value());
            CHECK(*ec == asio::error::invalid_argument);
        }
        // The level change takes effect: compressed output still round-trips.
        REQUIRE(run_until(ctx, ws->set_compression_level(1)));
        REQUIRE(run_until(ctx, ws->write("level one", /*text=*/true)));
        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(written.size() >= 3);
        CHECK(static_cast<unsigned char>(written[0]) == 0xC1);
    }

    SECTION("a connection without the extension is inert") {
        auto transport = std::make_shared<MockTransport>(ctx.get_executor());
        auto backend =
            std::make_shared<WsBackendImpl<MockTransport>>(transport, /*max_payload=*/65536, std::chrono::seconds(5));
        auto ws = std::make_shared<WebSocket>(backend);
        asio::co_spawn(ctx, ws->run_writer(), asio::detached);
        drain(ctx);

        CHECK_FALSE(ws->compression_negotiated());
        REQUIRE(run_until(ctx, ws->enable_write_compression(true))); // accepted, no effect
        REQUIRE(run_until(ctx, ws->write("plain", /*text=*/true)));
        drain(ctx);
        const std::string written = transport->written();
        REQUIRE(!written.empty());
        CHECK(static_cast<unsigned char>(written[0]) == 0x81); // never compressed
    }
}
