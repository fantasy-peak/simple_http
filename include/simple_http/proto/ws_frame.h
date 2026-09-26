#pragma once

// WebSocket wire codec (RFC 6455) — hand-written, transport-agnostic.
//
// It provides:
//   * WsOpcode           : frame opcodes.
//   * WsFrameParser      : an incremental parser that turns a byte stream into
//                          decoded frames (fin/opcode/payload), unmasking the
//                          client->server payload as required by the protocol.
//   * ws_accept_key      : the Sec-WebSocket-Accept value (SHA1 + base64).
//   * encode_frame / *   : server->client frame serialization (text/binary/
//                          close/ping/pong), 7/16/64-bit length framing.
//
// Messages stay in memory (nothing spills to a temp file): their size is bounded
// by the engine's limits instead.
//
// It depends only on the standard library, OpenSSL (SHA1 for the handshake) and
// simple_http core base64 — no Beast, no Asio.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/sha.h>

#include "../core/base64.h"

namespace simple_http {

// WebSocket frame opcodes (RFC 6455 §5.2).
enum class WsOpcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

// The RFC 6455 handshake GUID appended to the client key before hashing.
inline constexpr std::string_view ws_handshake_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Computes the Sec-WebSocket-Accept response value: base64(SHA1(key + GUID)).
inline std::string ws_accept_key(std::string_view client_key) {
    std::string material;
    material.reserve(client_key.size() + ws_handshake_guid.size());
    material.append(client_key);
    material.append(ws_handshake_guid);

    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest);
    return base64_encode(std::string_view{reinterpret_cast<const char*>(digest), SHA_DIGEST_LENGTH});
}

// One fully decoded WebSocket frame (payload already unmasked).
struct WsFrame {
    bool fin = true;
    WsOpcode opcode = WsOpcode::Text;
    std::string payload;
    // How many leading octets of `payload` were already handed out by
    // take_partial_payload() while the frame was still arriving, and so have
    // already been inspected. A caller that validates incrementally skips this
    // prefix rather than feeding the same octets through twice.
    std::size_t already_delivered = 0;
};

// Unmask a WebSocket payload in place (RFC 6455 §5.3: data[i] ^= key[i % 4]).
//
// Processes eight bytes per iteration instead of one. Because the payload
// starts at masking offset 0, the per-byte key pattern over any 8-byte block is
// the 4-byte key repeated twice (0 1 2 3 0 1 2 3) — a fixed phase — so a single
// 64-bit key can be XORed against each word. The trailing 0..7 bytes fall back
// to the byte-wise form. memcpy is used for the word loads/stores so no
// alignment or strict-aliasing assumptions are made.
inline void ws_unmask(char* data, std::size_t len, const unsigned char (&key)[4]) {
    // Build the 8-byte key: key repeated twice, matching offsets 0..7.
    unsigned char key8[8] = {key[0], key[1], key[2], key[3], key[0], key[1], key[2], key[3]};
    std::uint64_t mask64;
    std::memcpy(&mask64, key8, sizeof(mask64));

    std::size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        std::uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        word ^= mask64;
        std::memcpy(data + i, &word, sizeof(word));
    }
    // Tail (< 8 bytes): the masking offset here is `i`, and i is a multiple of
    // 8, so i % 4 == 0 — the key index for the remaining bytes is simply their
    // position within the tail modulo 4.
    for (std::size_t j = 0; i < len; ++i, ++j) {
        data[i] = static_cast<char>(static_cast<unsigned char>(data[i]) ^ key[j % 4]);
    }
}

// Incremental frame parser. Feed received bytes with append(); repeatedly call
// next() to pop complete frames. The parser handles the variable-length header
// (7 / 16 / 64-bit payload length) and client masking; it does not reassemble
// fragmented messages (that is the WebSocket layer's job).
class WsFrameParser {
  public:
    // Result of a next() call.
    enum class Status {
        Frame,       // a complete frame is available in `out`
        NeedMore,    // not enough bytes buffered yet; feed more and retry
        Error,       // protocol violation (e.g. reserved opcode, oversize)
    };

    // Payload octets of the in-flight frame, as far as they have arrived.
    struct PartialPayload {
        std::string bytes;   // unmasked octets not yet delivered; may be empty
        bool first = false;  // true on the frame's first delivery
    };

    // Payload size cap; frames larger than this are rejected. The engine sets
    // this from its EngineLimits before serving.
    explicit WsFrameParser(std::uint64_t max_payload = 16u * 1024 * 1024) : m_max_payload(max_payload) {}

    void append(std::string_view bytes) { m_buf.append(bytes); }
    void append(const std::byte* data, std::size_t n) {
        m_buf.append(reinterpret_cast<const char*>(data), n);
    }
    void append(const char* data, std::size_t n) { m_buf.append(data, n); }

    // Attempts to decode the next complete frame from the buffer.
    Status next(WsFrame& out) {
        const auto* data = reinterpret_cast<const unsigned char*>(m_buf.data());
        std::size_t size = m_buf.size();
        if (size < 2) return Status::NeedMore;

        bool fin = (data[0] >> 7) & 0x01;
        std::uint8_t opcode = data[0] & 0x0F;

        // RFC 6455 §5.2: RSV1-3 must be zero unless an extension that defines
        // them was negotiated. No extension is ever accepted here, so a set bit is
        // a protocol error rather than something to mask off and carry on.
        if ((data[0] & 0x70) != 0) return Status::Error;

        // Validate the opcode: only the defined ones are accepted.
        switch (opcode) {
            case 0x0:
            case 0x1:
            case 0x2:
            case 0x8:
            case 0x9:
            case 0xA:
                break;
            default:
                return Status::Error;
        }

        // Control frames (Close/Ping/Pong) must not be fragmented and their
        // payload must be <=125 octets (RFC 6455 §5.5).
        bool is_control = (opcode & 0x08) != 0;
        if (is_control && !fin) {
            return Status::Error;
        }

        bool mask = (data[1] >> 7) & 0x01;
        std::uint8_t len7 = data[1] & 0x7F;
        std::size_t pos = 2;

        if (is_control && len7 > 125) {
            return Status::Error;  // control frame payload capped at 125 octets
        }

        std::uint64_t payload_len = 0;
        if (len7 < 126) {
            payload_len = len7;
        } else if (len7 == 126) {
            if (size < pos + 2) return Status::NeedMore;
            payload_len = (static_cast<std::uint64_t>(data[pos]) << 8) | data[pos + 1];
            pos += 2;
        } else {  // len7 == 127
            if (size < pos + 8) return Status::NeedMore;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | data[pos + i];
            }
            pos += 8;
        }

        if (payload_len > m_max_payload) return Status::Error;

        // RFC 6455 §5.1: every frame a client sends MUST be masked. This parser
        // only ever sees client-to-server frames (WsBackendImpl is its only user),
        // so an unmasked one is a protocol error and not a permissiveness to
        // tolerate — the mask is what stops a cache-poisoning intermediary from
        // replaying a client's bytes verbatim into another connection.
        if (!mask) return Status::Error;

        unsigned char mask_key[4] = {0, 0, 0, 0};
        {
            if (size < pos + 4) return Status::NeedMore;
            mask_key[0] = data[pos];
            mask_key[1] = data[pos + 1];
            mask_key[2] = data[pos + 2];
            mask_key[3] = data[pos + 3];
            pos += 4;
        }

        // Record the in-flight frame before the completeness check, so its payload
        // can be handed out as it arrives — see take_partial_payload().
        m_partial_active = true;
        m_partial_opcode = static_cast<WsOpcode>(opcode);
        m_partial_start = pos;
        m_partial_len = static_cast<std::size_t>(payload_len);
        m_partial_off = 0;
        std::memcpy(m_partial_key, mask_key, sizeof(m_partial_key));

        if (size < pos + payload_len) return Status::NeedMore;

        out.fin = fin;
        out.opcode = static_cast<WsOpcode>(opcode);
        out.payload.assign(reinterpret_cast<const char*>(data + pos), static_cast<std::size_t>(payload_len));
        out.already_delivered = m_partial_off;
        ws_unmask(out.payload.data(), out.payload.size(), mask_key);  // every frame past the check above is masked

        m_partial_active = false;
        m_buf.erase(0, pos + static_cast<std::size_t>(payload_len));
        return Status::Frame;
    }

    // Hands out the payload of the frame currently being accumulated, as it
    // arrives. Without it, a caller that must act on a frame before it is whole —
    // rejecting invalid UTF-8 the moment the offending octet appears, which
    // Autobahn's 6.4.3 pins down — could only look once the frame had completed.
    // Each call drains what has arrived since the last one, so no octet is
    // delivered twice; `first` marks the frame's first delivery.
    PartialPayload take_partial_payload() {
        PartialPayload out;
        if (!m_partial_active) return out;
        const std::size_t arrived = m_buf.size() > m_partial_start ? m_buf.size() - m_partial_start : 0;
        const std::size_t have = std::min(arrived, m_partial_len);
        out.first = m_partial_off == 0;
        if (have <= m_partial_off) return out;
        out.bytes.assign(m_buf, m_partial_start + m_partial_off, have - m_partial_off);
        // Payload octet i is masked with key[i % 4], and this chunk starts at
        // octet m_partial_off, so the key has to be rotated into phase.
        unsigned char rotated[4];
        for (std::size_t i = 0; i < 4; ++i) {
            rotated[i] = m_partial_key[(m_partial_off + i) % 4];
        }
        ws_unmask(out.bytes.data(), out.bytes.size(), rotated);
        m_partial_off = have;
        return out;
    }

    // The opcode of the frame being accumulated, if one is in flight. Lets the
    // caller decide whether the arriving octets are worth inspecting at all.
    std::optional<WsOpcode> pending_opcode() const {
        if (!m_partial_active) return std::nullopt;
        return m_partial_opcode;
    }

  private:
    std::string m_buf;
    std::uint64_t m_max_payload;

    // In-flight frame bookkeeping for take_partial_payload().
    bool m_partial_active = false;
    WsOpcode m_partial_opcode = WsOpcode::Text;
    std::size_t m_partial_start = 0;  // the payload's offset within m_buf
    std::size_t m_partial_len = 0;    // the frame's declared payload length
    std::size_t m_partial_off = 0;    // octets already delivered
    unsigned char m_partial_key[4] = {0, 0, 0, 0};
};

// Serializes a server->client frame header (never masked) into a caller-provided
// buffer, which must hold at least 10 bytes; returns the header length. Writing
// the header into a stack buffer lets a frame go out as "header + payload" in one
// scatter-gather write, with no per-frame buffer to concatenate into.
inline std::size_t ws_encode_header(char* out, WsOpcode opcode, std::uint64_t len) {
    std::size_t n = 0;
    out[n++] = static_cast<char>(0x80 | static_cast<std::uint8_t>(opcode));  // FIN=1 + opcode

    if (len <= 125) {
        out[n++] = static_cast<char>(len);
    } else if (len <= 0xFFFF) {
        out[n++] = static_cast<char>(126);
        out[n++] = static_cast<char>((len >> 8) & 0xFF);
        out[n++] = static_cast<char>(len & 0xFF);
    } else {
        out[n++] = static_cast<char>(127);
        for (int i = 7; i >= 0; --i) {
            out[n++] = static_cast<char>((len >> (8 * i)) & 0xFF);
        }
    }
    return n;
}

inline void ws_append_header(std::string& out, WsOpcode opcode, std::uint64_t len) {
    char buf[10];
    out.append(buf, ws_encode_header(buf, opcode, len));
}

// Builds a complete server->client data frame (header + payload).
inline std::string ws_encode_frame(WsOpcode opcode, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 10);
    ws_append_header(out, opcode, payload.size());
    out.append(payload);
    return out;
}

inline std::string ws_encode_text(std::string_view payload) { return ws_encode_frame(WsOpcode::Text, payload); }
inline std::string ws_encode_binary(std::string_view payload) { return ws_encode_frame(WsOpcode::Binary, payload); }
inline std::string ws_encode_pong(std::string_view payload) { return ws_encode_frame(WsOpcode::Pong, payload); }
inline std::string ws_encode_ping(std::string_view payload) { return ws_encode_frame(WsOpcode::Ping, payload); }

// Whether `code` is one a peer is allowed to put on the wire (RFC 6455 §7.4.1).
// 1004 is reserved, 1005/1006 exist only to be *reported* locally, and 1012-2999
// were unassigned — a Close carrying any of them is a protocol error, not a
// close to echo back.
inline bool ws_valid_close_code(std::uint16_t code) {
    if (code >= 3000 && code <= 4999) return true;
    switch (code) {
        case 1000:
        case 1001:
        case 1002:
        case 1003:
        case 1007:
        case 1008:
        case 1009:
        case 1010:
        case 1011:
            return true;
        default:
            return false;
    }
}

// Incremental UTF-8 validator (RFC 3629), for text messages.
//
// Validation cannot wait for the reassembled message: RFC 6455 §8.1 makes an
// invalid octet a failure the moment it arrives, and a codepoint may straddle a
// fragment boundary — so the state of a half-read sequence has to survive from
// one frame to the next. Hence a feed()/complete() pair rather than a function
// over a finished string.
//
// The byte ranges are the ones that reject every non-shortest form, the
// surrogate range U+D800-DFFF and codepoints past U+10FFFF, which a plain
// "count the continuation bytes" check would let through.
class Utf8Validator {
  public:
    // Feeds octets; false the moment the stream stops being valid UTF-8.
    bool feed(std::string_view bytes) {
        for (char ch : bytes) {
            const auto b = static_cast<unsigned char>(ch);
            if (m_need == 0) {
                if (b < 0x80) continue;  // ASCII
                if (b >= 0xC2 && b <= 0xDF) {
                    start(1, 0x80, 0xBF);
                } else if (b == 0xE0) {
                    start(2, 0xA0, 0xBF);  // excludes overlong
                } else if (b >= 0xE1 && b <= 0xEC) {
                    start(2, 0x80, 0xBF);
                } else if (b == 0xED) {
                    start(2, 0x80, 0x9F);  // excludes U+D800-DFFF
                } else if (b >= 0xEE && b <= 0xEF) {
                    start(2, 0x80, 0xBF);
                } else if (b == 0xF0) {
                    start(3, 0x90, 0xBF);  // excludes overlong
                } else if (b >= 0xF1 && b <= 0xF3) {
                    start(3, 0x80, 0xBF);
                } else if (b == 0xF4) {
                    start(3, 0x80, 0x8F);  // excludes past U+10FFFF
                } else {
                    return false;  // 0x80-0xC1 (stray continuation/overlong), 0xF5-0xFF
                }
                continue;
            }
            if (b < m_lower || b > m_upper) return false;
            m_lower = 0x80;  // later continuation octets take the ordinary range
            m_upper = 0xBF;
            --m_need;
        }
        return true;
    }

    // Whether the stream stopped on a codepoint boundary: false when a sequence
    // was left half-read, which is how a truncated final character is caught.
    bool complete() const { return m_need == 0; }

  private:
    void start(int need, unsigned char lower, unsigned char upper) {
        m_need = need;
        m_lower = lower;
        m_upper = upper;
    }

    int m_need = 0;
    unsigned char m_lower = 0x80;
    unsigned char m_upper = 0xBF;
};

// The payload of a Close frame: an optional status code as a 2-byte prefix.
inline std::string ws_close_payload(std::uint16_t code = 1000) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    return payload;
}

// Builds a Close frame; an optional status code is encoded as a 2-byte prefix.
inline std::string ws_encode_close(std::uint16_t code = 1000) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    return ws_encode_frame(WsOpcode::Close, payload);
}

}  // namespace simple_http
