#pragma once

// HTTP/3 frame codec (RFC 9114 §6-§8), framework-free.
//
// Compared with HTTP/2's frame layer this one is thin: every field is a QUIC
// variable-length integer (RFC 9000 §16), so a frame header is two varints and
// there is not a single flag bit to interpret. What HTTP/2 spends flags on,
// HTTP/3 spends on *placement*: whether a frame is legal depends on which kind
// of stream carries it and on what came before it there (RFC 9114 §7, Table 1).
// Those rules need per-stream state and therefore live in the engine; what lives
// here is the wire format plus the constant tables both sides have to agree on
// before either can parse a byte of the other.
//
// Reads go through quic::Reader rather than a second bounds-checked cursor: it
// already carries the bounds and the sticky failure flag that turn a truncated
// frame into one check at the end instead of one per field.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "../../quic/wire.h"

namespace simple_http::h3codec {

using quic::append_varint;
using quic::Bytes;
using quic::Reader;
using quic::reader_of;

// Unidirectional stream types (RFC 9114 §6.2, extended by RFC 9204 §4.2). The
// type is a varint at the very start of the stream; everything after it is
// interpreted according to which of these it is — which is also the only way the
// connection layer, which knows nothing of HTTP/3, learns what a stream is for.
inline constexpr std::uint64_t H3_STREAM_CONTROL = 0x00;
inline constexpr std::uint64_t H3_STREAM_PUSH = 0x01;
inline constexpr std::uint64_t H3_STREAM_QPACK_ENCODER = 0x02;
inline constexpr std::uint64_t H3_STREAM_QPACK_DECODER = 0x03;

// Frame types (RFC 9114 §7.2, Table 2). The gaps are deliberate: 0x02, 0x06,
// 0x08 and 0x09 are reserved HTTP/2 codes that HTTP/3 forbids on the wire
// (§7.2.8), and are rejected rather than ignored — see H3_H2_RESERVED_FRAME.
enum class H3FrameType : std::uint64_t {
    Data = 0x00,          // request/push stream content (§7.2.1)
    Headers = 0x01,       // a QPACK-encoded field section (§7.2.2)
    CancelPush = 0x03,    // control stream (§7.2.3)
    Settings = 0x04,      // control stream, first frame only (§7.2.4)
    PushPromise = 0x05,   // request stream (§7.2.5)
    Goaway = 0x07,        // control stream (§7.2.6)
    MaxPushId = 0x0d,     // control stream, client to server only (§7.2.7)
};

// The frame types HTTP/2 defined that HTTP/3 has no equivalent for — 0x02
// (PRIORITY), 0x06 (PING), 0x08 (WINDOW_UPDATE), 0x09 (CONTINUATION). They are
// the one class of "unknown" frame a receiver must *not* ignore: §7.2.8 makes
// their receipt a connection error of type H3_FRAME_UNEXPECTED.
constexpr bool h3_h2_reserved_frame(std::uint64_t type) noexcept {
    return type == 0x02 || type == 0x06 || type == 0x08 || type == 0x09;
}

// SETTINGS identifiers this endpoint understands (RFC 9114 §7.2.4.1, RFC 9204
// §5). The whole set is three values; every other identifier, known or not, is
// accepted and ignored (§7.2.4).
inline constexpr std::uint64_t H3_SETTINGS_QPACK_MAX_TABLE_CAPACITY = 0x01;
inline constexpr std::uint64_t H3_SETTINGS_MAX_FIELD_SECTION_SIZE = 0x06;
inline constexpr std::uint64_t H3_SETTINGS_QPACK_BLOCKED_STREAMS = 0x07;

// The identifiers HTTP/2 used that HTTP/3 reserves instead of redefining:
// 0x00, 0x02 (ENABLE_PUSH), 0x03 (MAX_CONCURRENT_STREAMS), 0x04
// (INITIAL_WINDOW_SIZE), 0x05 (MAX_FRAME_SIZE). §7.2.4.1 forbids sending them
// and makes their receipt H3_SETTINGS_ERROR — a peer that sends one is speaking
// the wrong protocol, not a peer with a setting we happen not to implement.
constexpr bool h3_h2_reserved_setting(std::uint64_t id) noexcept {
    return id == 0x00 || (id >= 0x02 && id <= 0x05);
}

// Error codes (RFC 9114 §8.1, RFC 9204 §6). All of them, because the engine
// chooses between them by RFC clause and a local re-derivation of the numbers
// would be a place for the two to disagree.
inline constexpr std::uint64_t H3_NO_ERROR = 0x0100;
inline constexpr std::uint64_t H3_GENERAL_PROTOCOL_ERROR = 0x0101;
inline constexpr std::uint64_t H3_INTERNAL_ERROR = 0x0102;
inline constexpr std::uint64_t H3_STREAM_CREATION_ERROR = 0x0103;
inline constexpr std::uint64_t H3_CLOSED_CRITICAL_STREAM = 0x0104;
inline constexpr std::uint64_t H3_FRAME_UNEXPECTED = 0x0105;
inline constexpr std::uint64_t H3_FRAME_ERROR = 0x0106;
inline constexpr std::uint64_t H3_EXCESSIVE_LOAD = 0x0107;
inline constexpr std::uint64_t H3_ID_ERROR = 0x0108;
inline constexpr std::uint64_t H3_SETTINGS_ERROR = 0x0109;
inline constexpr std::uint64_t H3_MISSING_SETTINGS = 0x010a;
inline constexpr std::uint64_t H3_REQUEST_REJECTED = 0x010b;
inline constexpr std::uint64_t H3_REQUEST_CANCELLED = 0x010c;
inline constexpr std::uint64_t H3_REQUEST_INCOMPLETE = 0x010d;
inline constexpr std::uint64_t H3_MESSAGE_ERROR = 0x010e;
inline constexpr std::uint64_t H3_CONNECT_ERROR = 0x010f;
inline constexpr std::uint64_t H3_VERSION_FALLBACK = 0x0110;
inline constexpr std::uint64_t QPACK_DECOMPRESSION_FAILED = 0x0200;
inline constexpr std::uint64_t QPACK_ENCODER_STREAM_ERROR = 0x0201;
inline constexpr std::uint64_t QPACK_DECODER_STREAM_ERROR = 0x0202;

// The GREASE range (§7.2.8, §6.2.3, §8.1): 0x1f * N + 0x21. Frame types, stream
// types, settings identifiers and error codes all reserve this one pattern, and
// all four rules are "accept it, give it no meaning". A single predicate keeps
// the four call sites from drifting apart.
constexpr bool h3_reserved_code(std::uint64_t v) noexcept {
    return v >= 0x21 && (v - 0x21) % 0x1f == 0;
}

// A parsed frame header. The payload is not part of it: a frame's bytes arrive
// over a stream, and who owns them — the frame parser's buffer or the request
// body — differs per frame type.
struct H3FrameHeader {
    std::uint64_t type = 0;
    std::uint64_t length = 0;
};

// The size of the frame header at the front of `r`, or 0 when it is not all
// there yet.
//
// The engine needs the size *before* it commits to a parse, because a frame
// header has no alignment and may be split anywhere across QUIC STREAM frames. A
// parser that consumed a partial header could not rewind the cursor, so it would
// have to buffer the bytes forever or lose them.
inline std::size_t frame_header_len(Reader r) noexcept {
    const std::size_t type_len = r.peek_varint_width();
    if (type_len == 0 || r.remaining() < type_len) return 0;
    (void)r.varint();
    const std::size_t length_len = r.peek_varint_width();
    if (length_len == 0 || r.remaining() < length_len) return 0;
    return type_len + length_len;
}

// Parses the frame header at `r`'s cursor, which frame_header_len() has already
// shown to be complete.
inline H3FrameHeader parse_frame_header(Reader& r) noexcept {
    H3FrameHeader hdr;
    hdr.type = r.varint();
    hdr.length = r.varint();
    return hdr;
}

// Whether a frame body of `length` octets is wholly present in `r`. A declared
// length that runs past the end of the buffered bytes is not a truncated frame —
// the rest may simply not have arrived (§7.1).
inline bool frame_payload_available(Reader r, const H3FrameHeader& hdr) noexcept {
    return hdr.length <= r.remaining();
}

// A whole frame, header and payload together. `length` is what the frame header
// will say, so the two cannot disagree — the mistake §10.8 warns about, where a
// nested length is derived from something other than the bytes actually written.
inline void append_frame(Bytes& out, std::uint64_t type, std::string_view payload) {
    append_varint(out, type);
    append_varint(out, payload.size());
    out.append(payload);
}

inline void append_frame(Bytes& out, H3FrameType type, std::string_view payload) {
    append_frame(out, static_cast<std::uint64_t>(type), payload);
}

// One SETTINGS parameter (§7.2.4).
struct H3Setting {
    std::uint64_t id = 0;
    std::uint64_t value = 0;
};

// Encodes the payload of a SETTINGS frame: a flat sequence of identifier/value
// varint pairs with no count and no framing of its own, so the frame length is
// the only thing that says where the list ends.
inline void append_settings(Bytes& out, std::span<const H3Setting> settings) {
    for (const H3Setting& s : settings) {
        append_varint(out, s.id);
        append_varint(out, s.value);
    }
}

// Decodes a SETTINGS payload, enforcing only the two rules that make the frame
// structurally malformed: an identifier may not repeat (§7.2.4), and the
// identifiers HTTP/2 owned and HTTP/3 reserved may not appear at all (§7.2.4.1).
//
// Everything else is kept and handed to the caller, *including* identifiers
// nobody understands and the reserved 0x1f*N+0x21 range. §7.2.4 requires those to
// be accepted and ignored, and "ignored" is a decision only the caller can make:
// this function does not know which extensions the engine implements.
//
// Returns H3_NO_ERROR, or H3_SETTINGS_ERROR for a duplicate or a reserved
// identifier. A truncated parameter is H3_SETTINGS_ERROR too — the frame
// declared a length its contents do not fill (§10.8).
inline std::uint64_t decode_settings(Reader r, std::vector<H3Setting>& out) {
    while (!r.empty()) {
        H3Setting s;
        s.id = r.varint();
        s.value = r.varint();
        if (r.failed()) return H3_SETTINGS_ERROR;
        if (h3_h2_reserved_setting(s.id)) return H3_SETTINGS_ERROR;
        for (const H3Setting& seen : out) {
            if (seen.id == s.id) return H3_SETTINGS_ERROR;
        }
        out.push_back(s);
    }
    return H3_NO_ERROR;
}

// Looks one setting up in a decoded SETTINGS payload, or nullopt when the peer
// did not send it (in which case the protocol default applies, not zero — the
// caller is responsible for knowing which default that is).
inline const std::uint64_t* find_setting(const std::vector<H3Setting>& settings, std::uint64_t id) noexcept {
    for (const H3Setting& s : settings) {
        if (s.id == id) return &s.value;
    }
    return nullptr;
}

// Encodes the payload of a GOAWAY frame (§7.2.6). Server to client it carries
// the ID of a client-initiated bidirectional stream: requests at or above it
// were not processed. The reason phrase is a local extension an implementation
// may append and a peer must ignore; this engine sends none, but the parameter
// keeps the field's sender-side definition in one place.
inline Bytes goaway_payload(std::uint64_t stream_id, std::string_view reason = {}) {
    Bytes payload;
    append_varint(payload, stream_id);
    payload.append(reason);
    return payload;
}

}  // namespace simple_http::h3codec
