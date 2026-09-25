// engine/h2: the frame codec, HPACK (encode + decode, dynamic table) and the
// Huffman codec. These are the pieces both roles share, and where a silent
// mismatch corrupts every later header — hence the regression cases below.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "simple_http.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::codec;
using namespace simple_http::test;

namespace {

std::string hex_of(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

std::string bytes_of(std::initializer_list<int> values) {
    std::string out;
    for (int v : values)
        out.push_back(static_cast<char>(v));
    return out;
}

}  // namespace

// --- frames ------------------------------------------------------------------

TEST_CASE("h2/frame: header round trip", "[h2]") {
    std::string out;
    serialize_frame_header(out, 16384, static_cast<std::uint8_t>(H2FrameType::Data), H2_FLAG_END_STREAM, 7);
    REQUIRE(out.size() == kH2FrameHeaderSize);

    H2FrameHeader hdr;
    REQUIRE(parse_frame_header(out, hdr));
    CHECK(hdr.length == 16384);
    CHECK(hdr.type == static_cast<std::uint8_t>(H2FrameType::Data));
    CHECK(hdr.flags == H2_FLAG_END_STREAM);
    CHECK(hdr.has_flag(H2_FLAG_END_STREAM));
    CHECK_FALSE(hdr.has_flag(H2_FLAG_END_HEADERS));
    CHECK(hdr.stream_id == 7);
}

TEST_CASE("h2/frame: length and stream id bounds", "[h2]") {
    {
        std::string out;
        serialize_frame_header(out, 0xFFFFFF, static_cast<std::uint8_t>(H2FrameType::Goaway), 0, 0x7FFFFFFF);
        H2FrameHeader hdr;
        REQUIRE(parse_frame_header(out, hdr));
        CHECK(hdr.length == 0xFFFFFF);       // the 24-bit maximum
        CHECK(hdr.stream_id == 0x7FFFFFFF);  // the 31-bit maximum
    }
    {
        // The reserved bit (0x80 of the first stream-id octet) must be cleared.
        std::string raw = bytes_of({0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x00, 0x00, 0x2A});
        H2FrameHeader hdr;
        REQUIRE(parse_frame_header(raw, hdr));
        CHECK(hdr.stream_id == 42);
        CHECK(hdr.length == 1);
    }
    {
        H2FrameHeader hdr;
        CHECK_FALSE(parse_frame_header(bytes_of({0x00, 0x00, 0x01}), hdr));  // shorter than a header
        CHECK_FALSE(parse_frame_header("", hdr));
    }
}

TEST_CASE("h2/frame: big-endian integers and constants", "[h2]") {
    CHECK(read_u32(bytes_of({0x01, 0x02, 0x03, 0x04}), 0) == 0x01020304u);
    CHECK(read_u32(bytes_of({0x00, 0x00, 0x00, 0x01}), 0) == 1u);

    CHECK(kH2FrameHeaderSize == 9);
    CHECK(kH2ClientPreface == "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
    CHECK(kH2ClientPreface.size() == 24);
    // Settings and error identifiers must stay distinct (a duplicated constant
    // would mis-frame or silently ignore a peer's setting).
    CHECK(H2_SETTINGS_HEADER_TABLE_SIZE != H2_SETTINGS_ENABLE_PUSH);
    CHECK(H2_SETTINGS_INITIAL_WINDOW_SIZE != H2_SETTINGS_MAX_FRAME_SIZE);
    CHECK(H2_NO_ERROR != H2_PROTOCOL_ERROR);
    CHECK(H2_FLOW_CONTROL_ERROR != H2_PROTOCOL_ERROR);
    CHECK(H2_FLAG_END_STREAM == 0x01);
    CHECK(H2_FLAG_END_HEADERS == 0x04);
}

// --- HPACK encoding ----------------------------------------------------------

TEST_CASE("h2/hpack: integer encoding with the prefix rule", "[h2]") {
    // 7-bit prefix: values below 127 fit inline, 127 and above use continuation bytes.
    auto encode7 = [](std::uint64_t value) {
        std::string out;
        hpack_append_integer(out, 0x80, 7, value);
        return out;
    };
    CHECK(hex_of(encode7(0)) == "80");
    CHECK(hex_of(encode7(2)) == "82");
    CHECK(hex_of(encode7(126)) == "fe");
    CHECK(hex_of(encode7(127)) == "ff00");  // exactly the prefix maximum: one continuation byte
    CHECK(hex_of(encode7(128)) == "ff01");
    CHECK(hex_of(encode7(1337)) == "ffba09");  // 1337 - 127 = 1210 = 0b10010111010
    CHECK(hex_of(encode7(255)) == "ff8001");   // 128 -> 0x80|0, then 1

    // 5-bit prefix (as a dynamic table size update uses).
    std::string out;
    hpack_append_integer(out, 0x20, 5, 4096);
    CHECK(out.front() == static_cast<char>(0x3F));  // 0x20 | 31: the prefix is saturated
}

TEST_CASE("h2/hpack: string encoding is Huffman and never expands ASCII", "[h2]") {
    std::string out;
    hpack_append_string(out, "www.example.com");
    CHECK(static_cast<unsigned char>(out[0]) == (0x80 | 12));  // H bit set, 12 octets (RFC 7541 C.4.1)
    CHECK(hex_of(out.substr(1)) == "f1e3c2e5f23a6ba0ab90f4ff");
    CHECK(out.size() <= 1 + std::string_view{"www.example.com"}.size());

    // Empty string: a zero length with the Huffman bit set.
    std::string empty;
    hpack_append_string(empty, "");
    CHECK(hex_of(empty) == "80");

    // A long value needs the integer continuation form for its length.
    const std::string long_value(300, 'a');
    std::string long_out;
    hpack_append_string(long_out, long_value);
    CHECK((static_cast<unsigned char>(long_out[0]) & 0x80) != 0);
    CHECK((static_cast<unsigned char>(long_out[0]) & 0x7F) == 0x7F);  // saturated prefix
}

TEST_CASE("h2/hpack: literals, indexed fields and :status shortcuts", "[h2]") {
    CHECK(hex_of(bytes_of({0x00})) == "00");  // sanity for the hex helper

    std::string indexed;
    hpack_append_indexed(indexed, 2);  // static table: :method GET
    CHECK(hex_of(indexed) == "82");

    std::string named;
    hpack_append_literal_indexed_name(named, 1, "example.com");  // :authority
    CHECK((static_cast<unsigned char>(named[0]) & 0xF0) == 0x00);
    CHECK((static_cast<unsigned char>(named[0]) & 0x0F) == 1);

    std::string status;
    hpack_append_status(status, 200);
    CHECK(hex_of(status) == "88");  // one byte from the static table
    hpack_append_status(status, 204);
    CHECK(hex_of(status) == "8889");

    std::string uncommon;
    hpack_append_status(uncommon, 418);  // not in the table: a literal
    CHECK(uncommon.size() > 1);
    std::vector<HpackHeader> decoded;
    HpackDecoder decoder;
    REQUIRE(decoder.decode(uncommon, decoded));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0].name == ":status");
    CHECK(decoded[0].value == "418");
}

// --- HPACK decoding ----------------------------------------------------------

TEST_CASE("h2/hpack: static table references", "[h2]") {
    HpackDecoder decoder;
    std::vector<HpackHeader> fields;

    REQUIRE(decoder.decode(bytes_of({0x82}), fields));  // index 2: :method GET
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].name == ":method");
    CHECK(fields[0].value == "GET");

    fields.clear();
    REQUIRE(decoder.decode(bytes_of({0x86}), fields));  // index 6: :scheme http
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].name == ":scheme");
    CHECK(fields[0].value == "http");
}

TEST_CASE("h2/hpack: literal fields, with and without indexing", "[h2]") {
    HpackDecoder decoder;
    std::vector<HpackHeader> fields;

    std::string literal;
    hpack_append_literal(literal, "x-custom", "value with spaces");
    REQUIRE(decoder.decode(literal, fields));
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].name == "x-custom");
    CHECK(fields[0].value == "value with spaces");
    CHECK(decoder.dynamic_table_size() == 0);  // "without indexing" must not touch the table

    // Incremental indexing: 0x40 | index 0, then name and value.
    std::string indexed;
    indexed.push_back('\x40');
    hpack_append_string(indexed, "x-trace");
    hpack_append_string(indexed, "abc123");
    fields.clear();
    REQUIRE(decoder.decode(indexed, fields));
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].name == "x-trace");
    // RFC 7541 §4.1: name + value + 32 per entry ("x-trace" + "abc123" + 32).
    CHECK(decoder.dynamic_table_size() == 7 + 6 + 32);
}

TEST_CASE("h2/hpack: dynamic table entries resolve by index 62+", "[h2]") {
    HpackDecoder decoder;
    std::string insert;
    insert.push_back('\x40');
    hpack_append_string(insert, "x-one");
    hpack_append_string(insert, "1");

    std::vector<HpackHeader> fields;
    REQUIRE(decoder.decode(insert, fields));

    std::string reference;
    hpack_append_indexed(reference, 62);  // the first dynamic entry
    fields.clear();
    REQUIRE(decoder.decode(reference, fields));
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].name == "x-one");
    CHECK(fields[0].value == "1");
}

TEST_CASE("h2/hpack: an entry larger than the table empties it and is not inserted", "[h2]") {
    // RFC 7541 §4.4. Skipping this shifts every later dynamic index, so a peer's
    // headers would silently decode to the wrong values (regression).
    HpackDecoder decoder;
    std::vector<HpackHeader> fields;

    std::string small;
    small.push_back('\x40');
    hpack_append_string(small, "x-one");
    hpack_append_string(small, "1");
    REQUIRE(decoder.decode(small, fields));
    REQUIRE(decoder.dynamic_table_size() > 0);

    std::string oversized;
    oversized.push_back('\x40');
    hpack_append_string(oversized, "x-big");
    hpack_append_string(oversized, std::string(5000, 'a'));  // > the 4096-byte default table
    fields.clear();
    REQUIRE(decoder.decode(oversized, fields));  // the field itself is delivered…
    CHECK(fields[0].name == "x-big");
    CHECK(decoder.dynamic_table_size() == 0);  // …but the table was emptied and left empty

    std::string stale;
    hpack_append_indexed(stale, 62);  // the entry that used to be index 62 is gone
    fields.clear();
    CHECK_FALSE(decoder.decode(stale, fields));
    CHECK(decoder.last_error() != 0);
}

TEST_CASE("h2/hpack: the table is bounded by bytes, newest first", "[h2]") {
    HpackDecoder decoder;
    std::vector<HpackHeader> fields;

    // Fill it with ~1 KiB entries: the oldest must be evicted, the newest kept.
    for (int i = 0; i < 6; ++i) {
        std::string insert;
        insert.push_back('\x40');
        hpack_append_string(insert, "x-" + std::to_string(i));
        hpack_append_string(insert, std::string(1000, 'v'));
        fields.clear();
        REQUIRE(decoder.decode(insert, fields));
    }
    CHECK(decoder.dynamic_table_size() <= decoder.max_table_size());
    CHECK(decoder.dynamic_table_size() <= 4096);

    std::string newest;
    hpack_append_indexed(newest, 62);
    fields.clear();
    REQUIRE(decoder.decode(newest, fields));
    CHECK(fields[0].name == "x-5");
}

TEST_CASE("h2/hpack: dynamic table size updates", "[h2]") {
    {
        HpackDecoder decoder;
        std::string insert;
        insert.push_back('\x40');
        hpack_append_string(insert, "x-one");
        hpack_append_string(insert, "1");
        std::vector<HpackHeader> fields;
        REQUIRE(decoder.decode(insert, fields));
        REQUIRE(decoder.dynamic_table_size() > 0);

        std::string shrink;                        // RFC 7541 §6.3
        hpack_append_integer(shrink, 0x20, 5, 0);  // set the table size to zero
        fields.clear();
        REQUIRE(decoder.decode(shrink, fields));
        CHECK(decoder.dynamic_table_size() == 0);  // evicted immediately

        std::string stale;
        hpack_append_indexed(stale, 62);
        fields.clear();
        CHECK_FALSE(decoder.decode(stale, fields));
    }
    {
        // A size update above what we advertised is a compression error (§4.2).
        HpackDecoder decoder;
        decoder.set_max_table_size(4096);
        std::string grow;
        hpack_append_integer(grow, 0x20, 5, 8192);
        std::vector<HpackHeader> fields;
        CHECK_FALSE(decoder.decode(grow, fields));
        CHECK(decoder.last_error() != 0);
    }
}

TEST_CASE("h2/hpack: malformed blocks are rejected, not guessed", "[h2]") {
    HpackDecoder decoder;
    std::vector<HpackHeader> fields;
    {
        // A string length that runs past the end of the block.
        std::string truncated;
        truncated.push_back('\x00');
        truncated.push_back(static_cast<char>(0x80 | 10));  // says ten bytes follow
        truncated.append("abc");
        CHECK_FALSE(decoder.decode(truncated, fields));
        CHECK(decoder.last_error() != 0);
    }
    {
        // An indexed reference past the end of the table.
        HpackDecoder fresh;
        std::string bad_index;
        hpack_append_integer(bad_index, 0x80, 7, 200);
        CHECK_FALSE(fresh.decode(bad_index, fields));
    }
    {
        // A lone prefix byte with nothing behind it.
        HpackDecoder fresh;
        CHECK_FALSE(fresh.decode(bytes_of({0x40}), fields));
    }
}

TEST_CASE("h2/hpack: encode/decode round trip", "[h2]") {
    const std::vector<std::pair<std::string, std::string>> original = {
        {"content-type", "application/json"},
        {"x-empty", ""},
        {"x-long", std::string(200, 'z')},
        {"x-mixed", "MiXeD case / symbols !@#$%^&*()"},
        {":status", "200"},
    };

    std::string block;
    for (const auto& [name, value] : original)
        hpack_append_literal(block, name, value);

    HpackDecoder decoder;
    std::vector<HpackHeader> decoded;
    REQUIRE(decoder.decode(block, decoded));
    REQUIRE(decoded.size() == original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        CHECK(decoded[i].name == original[i].first);
        CHECK(decoded[i].value == original[i].second);
    }
}

// --- Huffman -----------------------------------------------------------------

TEST_CASE("h2/huffman: RFC 7541 vectors and round trips", "[h2]") {
    {
        // RFC 7541 Appendix C.4.1.
        std::string encoded;
        const std::string value = "www.example.com";
        CHECK(http_huffman_encode(reinterpret_cast<unsigned char*>(const_cast<char*>(value.data())),
                                  static_cast<unsigned int>(value.size()),
                                  encoded) == HUFFMAN_OK);
        CHECK(hex_of(encoded) == "f1e3c2e5f23a6ba0ab90f4ff");

        unsigned char state = 0;
        std::string decoded;
        CHECK(http_huffman_decode(&state,
                                  reinterpret_cast<unsigned char*>(const_cast<char*>(encoded.data())),
                                  encoded.size(),
                                  decoded,
                                  1) == HUFFMAN_OK);
        CHECK(decoded == value);
    }
    {
        // Every printable ASCII byte survives a round trip.
        std::string value;
        for (int c = 0x20; c <= 0x7E; ++c)
            value.push_back(static_cast<char>(c));
        std::string encoded;
        REQUIRE(http_huffman_encode(reinterpret_cast<unsigned char*>(value.data()),
                                    static_cast<unsigned int>(value.size()),
                                    encoded) == HUFFMAN_OK);
        CHECK(encoded.size() <= value.size());  // the table never expands ASCII

        unsigned char state = 0;
        std::string decoded;
        REQUIRE(
            http_huffman_decode(&state, reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), decoded, 1) ==
            HUFFMAN_OK);
        CHECK(decoded == value);
    }
}
