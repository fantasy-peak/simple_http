#pragma once

// QUIC frame codec (RFC 9000 §19).
//
// Every frame is `type || fields`, the type being a variable-length integer, so
// one flat `Frame` struct beats a variant here: the parser fills the fields the
// type implies, the senders fill the ones they mean, and neither needs a visitor
// to get at them. The alternative — one struct per frame type — costs a
// `std::variant` and a `std::visit` at every use site to buy type safety the
// wire format does not have anyway (a peer can put any frame type in any
// packet).
//
// Two things the flat shape must not lose, and does not:
//   * an unparsed or truncated frame is a *connection error*, never a partial
//     value — `Reader` latches the overrun and `parse_frame` reports it;
//   * an ACK frame's ranges are validated at parse time (RFC 9000 §19.3.1), so
//     the receive path never sees a range that would underflow.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wire.h"

namespace simple_http::quic {

// RFC 9000 §20.1 transport error codes. The application error codes (HTTP/3's
// H3_*) are a separate space and travel in CONNECTION_CLOSE type 0x1d.
enum class TransportError : std::uint64_t {
    NoError = 0x00,
    InternalError = 0x01,
    ConnectionRefused = 0x02,
    FlowControlError = 0x03,
    StreamLimitError = 0x04,
    StreamStateError = 0x05,
    FinalSizeError = 0x06,
    FrameEncodingError = 0x07,
    TransportParameterError = 0x08,
    ConnectionIdLimitError = 0x09,
    ProtocolViolation = 0x0a,
    InvalidToken = 0x0b,
    ApplicationError = 0x0c,
    CryptoBufferExceeded = 0x0d,
    KeyUpdateError = 0x0e,
    AeadLimitReached = 0x0f,
    NoViablePath = 0x10,
};

// RFC 9000 §22.31: the frame type used in a CONNECTION_CLOSE when the error is
// not attributable to a specific frame.
inline constexpr std::uint64_t kNoFrame = 0;

enum class FrameType : std::uint64_t {
    Padding = 0x00,
    Ping = 0x01,
    Ack = 0x02,  // 0x03 is Ack with ECN counts
    ResetStream = 0x04,
    StopSending = 0x05,
    Crypto = 0x06,
    NewToken = 0x07,
    Stream = 0x08,  // 0x08..0x0f: OFF/LEN/FIN are the low three bits
    MaxData = 0x10,
    MaxStreamData = 0x11,
    MaxStreams = 0x12,  // 0x12 bidirectional, 0x13 unidirectional
    DataBlocked = 0x14,
    StreamDataBlocked = 0x15,
    StreamsBlocked = 0x16,  // 0x16 bidirectional, 0x17 unidirectional
    NewConnectionId = 0x18,
    RetireConnectionId = 0x19,
    PathChallenge = 0x1a,
    PathResponse = 0x1b,
    ConnectionClose = 0x1c,   // 0x1c transport, 0x1d application
    HandshakeDone = 0x1e,
};

// The low three bits of a STREAM frame's type byte.
inline constexpr std::uint64_t kStreamFrameFin = 0x01;
inline constexpr std::uint64_t kStreamFrameLen = 0x02;
inline constexpr std::uint64_t kStreamFrameOff = 0x04;

// One acknowledged range, in the decoded form: everything from `smallest` to
// `largest` inclusive was received. Decoding here rather than in the caller is
// what keeps the underflow checks in one place.
struct AckRange {
    std::uint64_t smallest{0};
    std::uint64_t largest{0};
};

struct Frame {
    FrameType type{FrameType::Padding};

    // --- ACK ---
    std::uint64_t largest_ack{0};
    std::uint64_t ack_delay{0};  // in units of 2^ack_delay_exponent microseconds
    std::vector<AckRange> ack_ranges;  // descending, ranges[0] is the one containing largest_ack
    bool has_ecn{false};
    std::uint64_t ect0{0};
    std::uint64_t ect1{0};
    std::uint64_t ecn_ce{0};

    // --- RESET_STREAM / STOP_SENDING / CRYPTO / STREAM / MAX_* / BLOCKED ---
    std::uint64_t stream_id{0};
    std::uint64_t error_code{0};
    std::uint64_t final_size{0};
    std::uint64_t offset{0};
    // The limit carried by MAX_DATA / MAX_STREAM_DATA / MAX_STREAMS, or the
    // limit a *_BLOCKED frame reports being blocked at. One field because the
    // frames never mean both at once and the type says which.
    std::uint64_t limit{0};
    // MAX_STREAMS and STREAMS_BLOCKED share a positive space with two meanings:
    // 0x12/0x16 are the bidirectional count, 0x13/0x17 the unidirectional one.
    // The peer's two limits are tracked separately, so the distinction has to
    // survive parsing.
    bool bidirectional{true};
    bool fin{false};
    std::string data;  // CRYPTO, STREAM and NEW_TOKEN payload

    // --- NEW_CONNECTION_ID ---
    std::uint64_t sequence{0};
    std::uint64_t retire_prior_to{0};
    std::string connection_id;
    std::array<std::uint8_t, 16> stateless_reset_token{};

    // --- PATH_CHALLENGE / PATH_RESPONSE ---
    std::array<std::uint8_t, 8> path_data{};

    // --- CONNECTION_CLOSE ---
    bool application{false};       // true for 0x1d: error_code is an application code
    std::uint64_t frame_type{0};   // the frame that provoked it, transport form only
    std::string reason;

    // Whether the packet carrying this frame counts as ack-eliciting
    // (RFC 9000 §2). ACK, PADDING and CONNECTION_CLOSE are not; everything else
    // is. CONNECTION_CLOSE being non-ack-eliciting is what lets a closing
    // endpoint stop without the peer's acknowledgements keeping it alive.
    [[nodiscard]] bool ack_eliciting() const noexcept {
        switch (type) {
            case FrameType::Padding:
            case FrameType::Ack:
            case FrameType::ConnectionClose:
                return false;
            default:
                return true;
        }
    }
};

enum class FrameParseStatus {
    Ok,
    Malformed,  // truncated or self-inconsistent: FRAME_ENCODING_ERROR
    Unknown,    // a frame type this version does not define: FRAME_ENCODING_ERROR
    // A frame type that is not encoded in the shortest form it has. RFC 9000
    // §12.4 makes the short form a MUST and lets a receiver decide: "An endpoint
    // MAY treat the receipt of a frame type that uses a longer encoding than
    // necessary as a connection error of type PROTOCOL_VIOLATION." Enforcing it
    // is what makes the type byte a reliable one-octet dispatch key, so we do.
    ProtocolViolation,
};

// Parse one frame, advancing `r` past it. On a status other than Ok, `r` is left
// in an unspecified position — the caller is closing the connection either way.
FrameParseStatus parse_frame(Reader& r, Frame& out);

// --- senders -------------------------------------------------------------
//
// Each appends a complete frame. STREAM and CRYPTO take an already-sliced view:
// the caller decides how much fits in the datagram, because only it knows the
// remaining budget (see the length helpers below).

void append_padding(Bytes& out, std::size_t count);
void append_ping(Bytes& out);

// An ACK frame built from the received ranges, most recent first. `ranges` must
// be descending and non-overlapping; the encoder assumes it (see ack.h, which
// is what produces them).
void append_ack(Bytes& out, std::uint64_t largest_ack, std::uint64_t ack_delay,
                const std::vector<AckRange>& ranges);

void append_reset_stream(Bytes& out, std::uint64_t stream_id, std::uint64_t error_code,
                         std::uint64_t final_size);
void append_stop_sending(Bytes& out, std::uint64_t stream_id, std::uint64_t error_code);
void append_crypto(Bytes& out, std::uint64_t offset, std::string_view data);
void append_new_token(Bytes& out, std::string_view token);
void append_stream(Bytes& out, std::uint64_t stream_id, std::uint64_t offset, std::string_view data,
                   bool fin);
void append_max_data(Bytes& out, std::uint64_t maximum);
void append_max_stream_data(Bytes& out, std::uint64_t stream_id, std::uint64_t maximum);
void append_max_streams(Bytes& out, std::uint64_t maximum, bool bidirectional);
void append_data_blocked(Bytes& out, std::uint64_t limit);
void append_stream_data_blocked(Bytes& out, std::uint64_t stream_id, std::uint64_t limit);
void append_streams_blocked(Bytes& out, std::uint64_t limit, bool bidirectional);
void append_new_connection_id(Bytes& out, std::uint64_t sequence, std::uint64_t retire_prior_to,
                              std::string_view connection_id,
                              const std::array<std::uint8_t, 16>& reset_token);
void append_retire_connection_id(Bytes& out, std::uint64_t sequence);
void append_path_challenge(Bytes& out, const std::array<std::uint8_t, 8>& data);
void append_path_response(Bytes& out, const std::array<std::uint8_t, 8>& data);
// `frame_type` is only meaningful for the transport form; the application form
// (HTTP/3) has no frame type field and omits it from the wire.
void append_connection_close(Bytes& out, std::uint64_t error_code, std::string_view reason,
                             bool application, std::uint64_t frame_type = kNoFrame);
void append_handshake_done(Bytes& out);

// --- length helpers -------------------------------------------------------
//
// The connection builds one datagram up to a byte budget, so it has to know a
// frame's size *before* writing it — appending and rolling back would mean
// keeping the previous size around at every call site. STREAM frames always set
// the LEN bit even though it is optional, which is what makes them measurable
// independently of what follows them in the packet.

inline std::size_t crypto_frame_len(std::uint64_t offset, std::size_t payload) {
    return 1 + varint_len(offset) + varint_len(payload) + payload;
}

inline std::size_t stream_frame_len(std::uint64_t stream_id, std::uint64_t offset, std::size_t payload,
                                    bool fin) {
    std::size_t len = 1 + varint_len(stream_id) + varint_len(payload) + payload;
    if (offset != 0) len += varint_len(offset);
    (void)fin;  // FIN rides in the type octet, which is already counted
    return len;
}

// --- implementation -------------------------------------------------------

namespace detail {

inline std::string_view as_view(const std::array<std::uint8_t, 16>& a) noexcept {
    return {reinterpret_cast<const char*>(a.data()), a.size()};
}

inline std::string_view as_view(const std::array<std::uint8_t, 8>& a) noexcept {
    return {reinterpret_cast<const char*>(a.data()), a.size()};
}

}  // namespace detail

inline FrameParseStatus parse_frame(Reader& r, Frame& out) {
    out = Frame{};
    const std::size_t type_width = r.peek_varint_width();
    const std::uint64_t type = r.varint();
    if (r.failed()) return FrameParseStatus::Malformed;
    // RFC 9000 §12.4: the frame type must use the shortest encoding — a MUST,
    // unlike every other integer in the protocol, which §16 leaves free.
    if (type_width != varint_len(type)) return FrameParseStatus::ProtocolViolation;

    // PADDING is a run of zero octets, not one frame per octet: a 1200-octet
    // Initial is mostly padding, and one frame per octet would cost a thousand
    // turns of the packet loop to see what a single skip sees.
    if (type == 0x00) {
        out.type = FrameType::Padding;
        std::size_t n = 0;
        while (n < r.remaining() && r.data()[r.offset() + n] == 0x00) ++n;
        r.skip(n);
        return FrameParseStatus::Ok;
    }

    switch (type) {
        case 0x01:
            out.type = FrameType::Ping;
            return FrameParseStatus::Ok;

        case 0x02:
        case 0x03: {
            out.type = FrameType::Ack;
            out.has_ecn = type == 0x03;
            out.largest_ack = r.varint();
            out.ack_delay = r.varint();
            const std::uint64_t range_count = r.varint();
            const std::uint64_t first_range = r.varint();
            if (r.failed()) return FrameParseStatus::Malformed;
            // A count is only believable if the octets for it could be there at
            // all — every range costs at least two one-octet integers. Without
            // this, a 30-octet packet could ask for 2^62 ranges.
            if (range_count > r.remaining() / 2) return FrameParseStatus::Malformed;
            if (first_range > out.largest_ack) return FrameParseStatus::Malformed;

            out.ack_ranges.reserve(static_cast<std::size_t>(range_count) + 1);
            std::uint64_t largest = out.largest_ack;
            std::uint64_t smallest = largest - first_range;
            out.ack_ranges.push_back(AckRange{smallest, largest});
            for (std::uint64_t i = 0; i < range_count; ++i) {
                const std::uint64_t gap = r.varint();
                const std::uint64_t len = r.varint();
                if (r.failed()) return FrameParseStatus::Malformed;
                // The new largest sits `gap + 2` below the last range's smallest
                // (the +2 skips the two numbers the RFC reserves). Both
                // subtractions must land, or the frame is describing ranges that
                // cannot exist.
                if (smallest < gap + 2) return FrameParseStatus::Malformed;
                largest = smallest - gap - 2;
                if (len > largest) return FrameParseStatus::Malformed;
                smallest = largest - len;
                out.ack_ranges.push_back(AckRange{smallest, largest});
            }
            if (out.has_ecn) {
                out.ect0 = r.varint();
                out.ect1 = r.varint();
                out.ecn_ce = r.varint();
                if (r.failed()) return FrameParseStatus::Malformed;
            }
            return FrameParseStatus::Ok;
        }

        case 0x04:
            out.type = FrameType::ResetStream;
            out.stream_id = r.varint();
            out.error_code = r.varint();
            out.final_size = r.varint();
            break;

        case 0x05:
            out.type = FrameType::StopSending;
            out.stream_id = r.varint();
            out.error_code = r.varint();
            break;

        case 0x06:
            out.type = FrameType::Crypto;
            out.offset = r.varint();
            out.data = r.copy(static_cast<std::size_t>(r.varint()));
            break;

        case 0x07:
            out.type = FrameType::NewToken;
            out.data = r.copy(static_cast<std::size_t>(r.varint()));
            break;

        case 0x10:
            out.type = FrameType::MaxData;
            out.limit = r.varint();
            break;

        case 0x11:
            out.type = FrameType::MaxStreamData;
            out.stream_id = r.varint();
            out.limit = r.varint();
            break;

        case 0x12:
        case 0x13:
            out.type = FrameType::MaxStreams;
            out.bidirectional = type == 0x12;
            out.limit = r.varint();
            break;

        case 0x14:
            out.type = FrameType::DataBlocked;
            out.limit = r.varint();
            break;

        case 0x15:
            out.type = FrameType::StreamDataBlocked;
            out.stream_id = r.varint();
            out.limit = r.varint();
            break;

        case 0x16:
        case 0x17:
            out.type = FrameType::StreamsBlocked;
            out.bidirectional = type == 0x16;
            out.limit = r.varint();
            break;

        case 0x18: {
            out.type = FrameType::NewConnectionId;
            out.sequence = r.varint();
            out.retire_prior_to = r.varint();
            const std::uint64_t cid_len = r.u8();
            if (r.failed()) return FrameParseStatus::Malformed;
            // RFC 9000 §19.15: 1..20 octets, and a peer cannot retire a sequence
            // number it has not issued yet.
            if (cid_len == 0 || cid_len > 20) return FrameParseStatus::Malformed;
            if (out.retire_prior_to > out.sequence) return FrameParseStatus::Malformed;
            const auto cid = r.bytes(static_cast<std::size_t>(cid_len));
            const auto token = r.bytes(16);
            if (r.failed()) return FrameParseStatus::Malformed;
            out.connection_id.assign(reinterpret_cast<const char*>(cid.data()), cid.size());
            for (std::size_t i = 0; i < 16; ++i) out.stateless_reset_token[i] = token[i];
            return FrameParseStatus::Ok;
        }

        case 0x19:
            out.type = FrameType::RetireConnectionId;
            out.sequence = r.varint();
            break;

        case 0x1a: {
            out.type = FrameType::PathChallenge;
            const auto d = r.bytes(8);
            if (r.failed()) return FrameParseStatus::Malformed;
            for (std::size_t i = 0; i < 8; ++i) out.path_data[i] = d[i];
            return FrameParseStatus::Ok;
        }

        case 0x1b: {
            out.type = FrameType::PathResponse;
            const auto d = r.bytes(8);
            if (r.failed()) return FrameParseStatus::Malformed;
            for (std::size_t i = 0; i < 8; ++i) out.path_data[i] = d[i];
            return FrameParseStatus::Ok;
        }

        case 0x1c:
        case 0x1d:
            out.type = FrameType::ConnectionClose;
            out.application = type == 0x1d;
            out.error_code = r.varint();
            // The application form has no frame-type field: the error came from
            // the application, which does not speak in QUIC frame types.
            if (!out.application) out.frame_type = r.varint();
            out.reason = r.copy(static_cast<std::size_t>(r.varint()));
            break;

        case 0x1e:
            out.type = FrameType::HandshakeDone;
            return FrameParseStatus::Ok;

        default:
            // STREAM frames are 0x08..0x0f: the low three bits are flags.
            if (type >= 0x08 && type <= 0x0f) {
                out.type = FrameType::Stream;
                out.fin = (type & kStreamFrameFin) != 0;
                out.stream_id = r.varint();
                if ((type & kStreamFrameOff) != 0) out.offset = r.varint();
                if ((type & kStreamFrameLen) != 0) {
                    out.data = r.copy(static_cast<std::size_t>(r.varint()));
                } else {
                    // No LEN: the frame runs to the end of the packet, which is
                    // also everything the reader has left.
                    out.data.assign(reinterpret_cast<const char*>(r.rest().data()), r.rest().size());
                    r.skip(r.remaining());
                }
                break;
            }
            // RFC 9000 §12.4: an unknown frame type is a connection error, not
            // something to skip — extension frames must be registered, and a
            // peer that does not know one cannot know its length either.
            return FrameParseStatus::Unknown;
    }

    if (r.failed()) return FrameParseStatus::Malformed;
    return FrameParseStatus::Ok;
}

inline void append_padding(Bytes& out, std::size_t count) { out.append(count, '\0'); }

inline void append_ping(Bytes& out) { append_varint(out, 0x01); }

inline void append_ack(Bytes& out, std::uint64_t largest_ack, std::uint64_t ack_delay,
                       const std::vector<AckRange>& ranges) {
    append_varint(out, 0x02);
    append_varint(out, largest_ack);
    append_varint(out, ack_delay);
    if (ranges.empty()) {
        // Unreachable from ack.h (which only builds an ACK with at least one
        // range), but an ACK claiming zero ranges and a largest_ack it does not
        // cover is malformed, so the count must not be fabricated here.
        append_varint(out, 0);
        append_varint(out, 0);
        return;
    }
    append_varint(out, ranges.size() - 1);
    append_varint(out, ranges[0].largest - ranges[0].smallest);
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        // gap counts the numbers not covered between two ranges, and the RFC's
        // encoding leaves two of them implicit.
        append_varint(out, ranges[i - 1].smallest - ranges[i].largest - 2);
        append_varint(out, ranges[i].largest - ranges[i].smallest);
    }
}

inline void append_reset_stream(Bytes& out, std::uint64_t stream_id, std::uint64_t error_code,
                                std::uint64_t final_size) {
    append_varint(out, 0x04);
    append_varint(out, stream_id);
    append_varint(out, error_code);
    append_varint(out, final_size);
}

inline void append_stop_sending(Bytes& out, std::uint64_t stream_id, std::uint64_t error_code) {
    append_varint(out, 0x05);
    append_varint(out, stream_id);
    append_varint(out, error_code);
}

inline void append_crypto(Bytes& out, std::uint64_t offset, std::string_view data) {
    append_varint(out, 0x06);
    append_varint(out, offset);
    append_varint(out, data.size());
    out.append(data);
}

inline void append_new_token(Bytes& out, std::string_view token) {
    append_varint(out, 0x07);
    append_varint(out, token.size());
    out.append(token);
}

inline void append_stream(Bytes& out, std::uint64_t stream_id, std::uint64_t offset, std::string_view data,
                          bool fin) {
    // LEN is always set: an unset LEN means "to the end of the packet", which
    // forbids any frame after this one — and a datagram is exactly where several
    // frames belong together.
    std::uint64_t type = 0x08 | kStreamFrameLen;
    if (offset != 0) type |= kStreamFrameOff;
    if (fin) type |= kStreamFrameFin;
    append_varint(out, type);
    append_varint(out, stream_id);
    if (offset != 0) append_varint(out, offset);
    append_varint(out, data.size());
    out.append(data);
}

inline void append_max_data(Bytes& out, std::uint64_t maximum) {
    append_varint(out, 0x10);
    append_varint(out, maximum);
}

inline void append_max_stream_data(Bytes& out, std::uint64_t stream_id, std::uint64_t maximum) {
    append_varint(out, 0x11);
    append_varint(out, stream_id);
    append_varint(out, maximum);
}

inline void append_max_streams(Bytes& out, std::uint64_t maximum, bool bidirectional) {
    append_varint(out, bidirectional ? 0x12 : 0x13);
    append_varint(out, maximum);
}

inline void append_data_blocked(Bytes& out, std::uint64_t limit) {
    append_varint(out, 0x14);
    append_varint(out, limit);
}

inline void append_stream_data_blocked(Bytes& out, std::uint64_t stream_id, std::uint64_t limit) {
    append_varint(out, 0x15);
    append_varint(out, stream_id);
    append_varint(out, limit);
}

inline void append_streams_blocked(Bytes& out, std::uint64_t limit, bool bidirectional) {
    append_varint(out, bidirectional ? 0x16 : 0x17);
    append_varint(out, limit);
}

inline void append_new_connection_id(Bytes& out, std::uint64_t sequence, std::uint64_t retire_prior_to,
                                     std::string_view connection_id,
                                     const std::array<std::uint8_t, 16>& reset_token) {
    append_varint(out, 0x18);
    append_varint(out, sequence);
    append_varint(out, retire_prior_to);
    append_u8(out, static_cast<std::uint8_t>(connection_id.size()));
    out.append(connection_id);
    out.append(detail::as_view(reset_token));
}

inline void append_retire_connection_id(Bytes& out, std::uint64_t sequence) {
    append_varint(out, 0x19);
    append_varint(out, sequence);
}

inline void append_path_challenge(Bytes& out, const std::array<std::uint8_t, 8>& data) {
    append_varint(out, 0x1a);
    out.append(detail::as_view(data));
}

inline void append_path_response(Bytes& out, const std::array<std::uint8_t, 8>& data) {
    append_varint(out, 0x1b);
    out.append(detail::as_view(data));
}

inline void append_connection_close(Bytes& out, std::uint64_t error_code, std::string_view reason,
                                    bool application, std::uint64_t frame_type) {
    append_varint(out, application ? 0x1d : 0x1c);
    append_varint(out, error_code);
    if (!application) append_varint(out, frame_type);
    append_varint(out, reason.size());
    out.append(reason);
}

inline void append_handshake_done(Bytes& out) { append_varint(out, 0x1e); }

}  // namespace simple_http::quic
