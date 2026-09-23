#pragma once

// HTTP/2 frame constants and the 9-octet frame header (RFC 7540 §4.1),
// framework-free. The frame-type / flag / settings / error enumerations and the
// big-endian header layout mirror paozhu (vendor/httpserver/http2_parse.h and
// http2_frame.h); here they are expressed as plain constants plus a small
// parse/serialize pair over std::string_view, with no Asio or httppeer coupling.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace simple_http::codec {

// Frame types (RFC 7540 §6).
enum class H2FrameType : uint8_t {
    Data = 0x0,
    Headers = 0x1,
    Priority = 0x2,
    RstStream = 0x3,
    Settings = 0x4,
    PushPromise = 0x5,
    Ping = 0x6,
    Goaway = 0x7,
    WindowUpdate = 0x8,
    Continuation = 0x9,
};

// Frame flags (bit values are shared across several frame types).
inline constexpr uint8_t H2_FLAG_END_STREAM = 0x01;   // DATA, HEADERS
inline constexpr uint8_t H2_FLAG_ACK = 0x01;          // SETTINGS, PING
inline constexpr uint8_t H2_FLAG_END_HEADERS = 0x04;  // HEADERS, CONTINUATION, PUSH_PROMISE
inline constexpr uint8_t H2_FLAG_PADDED = 0x08;       // DATA, HEADERS, PUSH_PROMISE
inline constexpr uint8_t H2_FLAG_PRIORITY = 0x20;     // HEADERS

// SETTINGS parameter identifiers (RFC 7540 §6.5.2).
inline constexpr uint16_t H2_SETTINGS_HEADER_TABLE_SIZE = 0x1;
inline constexpr uint16_t H2_SETTINGS_ENABLE_PUSH = 0x2;
inline constexpr uint16_t H2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3;
inline constexpr uint16_t H2_SETTINGS_INITIAL_WINDOW_SIZE = 0x4;
inline constexpr uint16_t H2_SETTINGS_MAX_FRAME_SIZE = 0x5;
inline constexpr uint16_t H2_SETTINGS_MAX_HEADER_LIST_SIZE = 0x6;

// Error codes (RFC 7540 §7).
inline constexpr uint32_t H2_NO_ERROR = 0x0;
inline constexpr uint32_t H2_PROTOCOL_ERROR = 0x1;
inline constexpr uint32_t H2_INTERNAL_ERROR = 0x2;
inline constexpr uint32_t H2_FLOW_CONTROL_ERROR = 0x3;
inline constexpr uint32_t H2_SETTINGS_TIMEOUT = 0x4;
inline constexpr uint32_t H2_STREAM_CLOSED = 0x5;
inline constexpr uint32_t H2_FRAME_SIZE_ERROR = 0x6;
inline constexpr uint32_t H2_REFUSED_STREAM = 0x7;
inline constexpr uint32_t H2_CANCEL = 0x8;
inline constexpr uint32_t H2_COMPRESSION_ERROR = 0x9;
inline constexpr uint32_t H2_ENHANCE_YOUR_CALM = 0xb;

// The 9-octet frame header: 24-bit length, 8-bit type, 8-bit flags,
// 1-bit reserved + 31-bit stream id.
inline constexpr std::size_t kH2FrameHeaderSize = 9;

// The client connection preface (RFC 7540 §3.5).
inline constexpr std::string_view kH2ClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

struct H2FrameHeader {
    uint32_t length = 0;    // payload length (24-bit)
    uint8_t type = 0;       // H2FrameType
    uint8_t flags = 0;
    uint32_t stream_id = 0;  // 31-bit (reserved bit cleared)

    bool has_flag(uint8_t f) const { return (flags & f) != 0; }
};

// Parses a 9-octet frame header from the front of `buf`. Returns false if
// fewer than 9 bytes are available.
inline bool parse_frame_header(std::string_view buf, H2FrameHeader& out) {
    if (buf.size() < kH2FrameHeaderSize) return false;
    auto b = [&](std::size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(buf[i])); };
    out.length = (b(0) << 16) | (b(1) << 8) | b(2);
    out.type = static_cast<uint8_t>(b(3));
    out.flags = static_cast<uint8_t>(b(4));
    out.stream_id = ((b(5) & 0x7F) << 24) | (b(6) << 16) | (b(7) << 8) | b(8);
    return true;
}

// Appends a 9-octet frame header to `out`.
inline void serialize_frame_header(std::string& out, uint32_t length, uint8_t type, uint8_t flags,
                                   uint32_t stream_id) {
    out.push_back(static_cast<char>((length >> 16) & 0xFF));
    out.push_back(static_cast<char>((length >> 8) & 0xFF));
    out.push_back(static_cast<char>(length & 0xFF));
    out.push_back(static_cast<char>(type));
    out.push_back(static_cast<char>(flags));
    out.push_back(static_cast<char>((stream_id >> 24) & 0x7F));
    out.push_back(static_cast<char>((stream_id >> 16) & 0xFF));
    out.push_back(static_cast<char>((stream_id >> 8) & 0xFF));
    out.push_back(static_cast<char>(stream_id & 0xFF));
}

// Reads a 32-bit big-endian integer at `buf[off]` (caller ensures bounds).
inline uint32_t read_u32(std::string_view buf, std::size_t off) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(buf[off])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 3]));
}

}  // namespace simple_http::codec
