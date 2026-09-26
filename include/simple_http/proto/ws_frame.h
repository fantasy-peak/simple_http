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

        if (size < pos + payload_len) return Status::NeedMore;

        out.fin = fin;
        out.opcode = static_cast<WsOpcode>(opcode);
        out.payload.assign(reinterpret_cast<const char*>(data + pos), static_cast<std::size_t>(payload_len));
        ws_unmask(out.payload.data(), out.payload.size(), mask_key);  // every frame past the check above is masked

        m_buf.erase(0, pos + static_cast<std::size_t>(payload_len));
        return Status::Frame;
    }

  private:
    std::string m_buf;
    std::uint64_t m_max_payload;
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
