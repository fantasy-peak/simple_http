#pragma once

// QUIC wire primitives: variable-length integers, a bounds-checked cursor, and
// the big-endian scalar writers everything above them is built from.
//
// RFC 9000 §16 defines QUIC's variable-length integer: the two most significant
// bits of the first byte select a 1, 2, 4 or 8 byte encoding of the remaining 62
// bits. Packet numbers (§17.1) use the same shape with an inverted meaning — the
// low two bits say how many bytes follow. Both are here, plus the plain
// big-endian scalars, because every frame and every packet header is built from
// them and a second copy would only be a place to disagree.
//
// Reading goes through `Reader`, which carries its own bounds. A QUIC parser is
// fed by the network, so a truncated frame has to produce a connection error
// rather than a read past the end of the datagram; the cursor makes every
// overrun a sticky `failed()` that callers check once per frame instead of an
// `if` per field.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../core/types.h"

namespace simple_http::quic {

// Byte buffers are std::string throughout: the codebase already uses it as its
// byte-buffer type (see the h1/h2 engines and the transports), and CRYPTO and
// STREAM data both need an owning, appendable buffer that outlives the datagram
// it arrived in.
using Bytes = std::string;

// The largest value a variable-length integer can hold (RFC 9000 §16).
inline constexpr std::uint64_t kVarintMax = 0x3fffffffffffffffULL;
// The most bytes either encoding of an integer can occupy.
inline constexpr std::size_t kMaxVarintLen = 8;
// Packet numbers are at most 4 octets on the wire (RFC 9000 §17.1).
inline constexpr std::size_t kMaxPacketNumberLen = 4;

// How many octets `v` needs as a variable-length integer. 2 bits of length in
// the first octet, so the boundaries are 2^6, 2^14, 2^30.
constexpr std::size_t varint_len(std::uint64_t v) noexcept {
    if (v <= 63) return 1;
    if (v <= 16383) return 2;
    if (v <= 1073741823) return 4;
    return 8;
}

// How many octets `pn` needs as a truncated packet number, given the largest
// packet number the peer has acknowledged. The encoding only has to be
// unambiguous within a window of twice the distance between the two, so a peer
// that has been keeping up needs a single octet (RFC 9000 §17.1).
constexpr std::size_t packet_number_len(std::uint64_t pn, std::uint64_t largest_acked) noexcept {
    // The window is 2 * (pn - largest_acked); one bit of that window is worth
    // one octet of encoding. Never smaller than 1: a packet number field of
    // zero octets is not a thing.
    const std::uint64_t range = (pn - largest_acked) * 2;
    if (range < (1u << 8)) return 1;
    if (range < (1u << 16)) return 2;
    if (range < (1u << 24)) return 3;
    return 4;
}

// Reconstruct a full packet number from its truncated form (RFC 9000 §17.1,
// reference algorithm in Appendix A.3). `largest_pn` is the largest packet
// number *received* so far in this space; `truncated` is the value read from the
// wire and `pn_nbits` its width in bits.
//
// The candidate closest below `expected` is the right answer as long as it is
// within half a window of it; otherwise the number that fits the window is
// either one window up or one window down. `candidate + half <= expected` is
// `candidate <= expected - half` written so it cannot wrap when `expected` is
// smaller than half a window (i.e. at the very start of a connection).
constexpr std::uint64_t decode_packet_number(std::uint64_t largest_pn, std::uint64_t truncated,
                                             std::size_t pn_nbits) noexcept {
    const std::uint64_t expected = largest_pn + 1;
    const std::uint64_t window = 1ULL << pn_nbits;
    const std::uint64_t half = window / 2;
    const std::uint64_t mask = window - 1;
    std::uint64_t candidate = (expected & ~mask) | truncated;
    if (candidate + half <= expected && candidate < (1ULL << 62) - window) {
        candidate += window;
    } else if (candidate > expected + half && candidate >= window) {
        candidate -= window;
    }
    return candidate;
}

// --- big-endian scalar writers -------------------------------------------

inline void append_u8(Bytes& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }

inline void append_u16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v & 0xff));
}

inline void append_u24(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

inline void append_u32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

inline void append_u64(Bytes& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xff));
    }
}

inline void append_bytes(Bytes& out, std::string_view v) { out.append(v); }

// A variable-length integer, at its shortest encoding — or at `min_len` octets
// when the caller needs a floor (interop tests and some tokens expect a specific
// width).
inline void append_varint(Bytes& out, std::uint64_t v, std::size_t min_len = 1) {
    // A value above 2^62-1 has no encoding at all. Clamping keeps the length
    // selector honest instead of writing a first octet whose top two bits are
    // already occupied by value bits.
    if (v > kVarintMax) v = kVarintMax;
    std::size_t len = varint_len(v);
    if (len < min_len) len = min_len;

    const std::size_t start = out.size();
    for (std::size_t i = len; i-- > 0;) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
    // The two-bit length selector lives in the first octet's high bits. Those
    // bits are free: `len` is the shortest encoding that fits `v`, or longer
    // because the caller asked — and a longer encoding has more room, never
    // less. sel = log2(len): 0, 1, 2, 3 for 1, 2, 4, 8 octets.
    const unsigned sel = (len == 1) ? 0u : (len == 2) ? 1u : (len == 4) ? 2u : 3u;
    const auto first = static_cast<unsigned char>(out[start]);
    out[start] = static_cast<char>(first | static_cast<unsigned char>(sel << 6));
}

// --- Reader ---------------------------------------------------------------

// A bounds-checked forward cursor over a datagram. Every accessor returns 0 and
// latches failure if the data is not there; callers run a whole frame's worth of
// reads and check once.
class Reader {
  public:
    explicit Reader(std::span<const std::byte> data) noexcept
        : m_data(reinterpret_cast<const std::uint8_t*>(data.data())), m_size(data.size()) {}

    // The type a parsed region arrives as: OpenSSL hands back transport
    // parameters as `unsigned char`, and the frame parsers slice datagrams with
    // it. `std::byte` stays for the transport boundary, where asio speaks it.
    explicit Reader(std::span<const std::uint8_t> data) noexcept : m_data(data.data()), m_size(data.size()) {}

    Reader(const std::uint8_t* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] bool empty() const noexcept { return m_pos >= m_size; }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_size - m_pos; }
    [[nodiscard]] std::size_t offset() const noexcept { return m_pos; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }

    // The underlying bytes, for handing a slice to OpenSSL or a hash function.
    [[nodiscard]] const std::uint8_t* data() const noexcept { return m_data; }
    [[nodiscard]] std::span<const std::uint8_t> rest() const noexcept { return {m_data + m_pos, remaining()}; }

    std::uint8_t u8() noexcept {
        if (remaining() < 1) return fail();
        return m_data[m_pos++];
    }

    std::uint16_t u16() noexcept {
        if (remaining() < 2) return fail();
        const std::uint16_t v = static_cast<std::uint16_t>((static_cast<std::uint16_t>(m_data[m_pos]) << 8) |
                                                           m_data[m_pos + 1]);
        m_pos += 2;
        return v;
    }

    std::uint32_t u24() noexcept {
        if (remaining() < 3) return fail();
        const std::uint32_t v = (static_cast<std::uint32_t>(m_data[m_pos]) << 16) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 8) | m_data[m_pos + 2];
        m_pos += 3;
        return v;
    }

    std::uint32_t u32() noexcept {
        if (remaining() < 4) return fail();
        const std::uint32_t v = (static_cast<std::uint32_t>(m_data[m_pos]) << 24) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 16) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 2]) << 8) | m_data[m_pos + 3];
        m_pos += 4;
        return v;
    }

    std::uint64_t u64() noexcept {
        if (remaining() < 8) return fail();
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | m_data[m_pos + static_cast<std::size_t>(i)];
        }
        m_pos += 8;
        return v;
    }

    // How many octets the variable-length integer at the cursor occupies, taken
    // from its own first octet and without consuming it. Two callers need this:
    // the frame parser, because a frame type must use the shortest encoding
    // (RFC 9000 §12.4), and the transport-parameter decoder, because a
    // parameter's declared length has to agree with its value's encoding.
    [[nodiscard]] std::size_t peek_varint_width() const noexcept {
        if (remaining() < 1) return 0;
        return std::size_t{1} << (m_data[m_pos] >> 6);
    }

    // A variable-length integer (RFC 9000 §16).
    std::uint64_t varint() noexcept {
        if (remaining() < 1) return fail();
        const std::uint8_t first = m_data[m_pos];
        const std::size_t len = std::size_t{1} << (first >> 6);
        if (remaining() < len) return fail();
        std::uint64_t v = first & 0x3f;
        for (std::size_t i = 1; i < len; ++i) {
            v = (v << 8) | m_data[m_pos + i];
        }
        m_pos += len;
        return v;
    }

    // A truncated packet number of `len` octets (RFC 9000 §17.1). Unlike the
    // variable-length integer above, the length is *not* on the wire — it is in
    // the packet's first byte, which the header parser has already read.
    std::uint64_t packet_number(std::size_t len) noexcept {
        if (len < 1 || len > kMaxPacketNumberLen || remaining() < len) return fail();
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < len; ++i) {
            v = (v << 8) | m_data[m_pos + i];
        }
        m_pos += len;
        return v;
    }

    // A length-prefixed byte string, as connection IDs and Retry tokens are
    // carried (RFC 9000 §17.2).
    std::string_view varint_string() noexcept {
        const std::uint64_t len = varint();
        if (m_failed || remaining() < len) return std::string_view{};
        const char* p = reinterpret_cast<const char*>(m_data + m_pos);
        m_pos += static_cast<std::size_t>(len);
        return std::string_view{p, static_cast<std::size_t>(len)};
    }

    // `n` raw octets, as a view into the datagram.
    std::span<const std::uint8_t> bytes(std::size_t n) noexcept {
        if (remaining() < n) {
            fail();
            return {};
        }
        const std::span<const std::uint8_t> v{m_data + m_pos, n};
        m_pos += n;
        return v;
    }

    // `n` octets as a copy, for data that must outlive the datagram.
    Bytes copy(std::size_t n) noexcept {
        const auto v = bytes(n);
        if (m_failed) return {};
        return Bytes{reinterpret_cast<const char*>(v.data()), v.size()};
    }

    bool skip(std::size_t n) noexcept {
        if (remaining() < n) return fail_flag();
        m_pos += n;
        return true;
    }

  private:
    std::uint8_t fail() noexcept {
        m_failed = true;
        return 0;
    }

    // skip() reports success as a bool, so it needs its own failing path.
    bool fail_flag() noexcept {
        m_failed = true;
        return false;
    }

    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos{0};
    bool m_failed{false};
};

// A view of `Bytes` as a Reader.
inline Reader reader_of(const Bytes& b) noexcept {
    return Reader{reinterpret_cast<const std::uint8_t*>(b.data()), b.size()};
}

inline Reader reader_of(std::span<const std::byte> b) noexcept { return Reader{b}; }

inline Reader reader_of(std::string_view b) noexcept {
    return Reader{reinterpret_cast<const std::uint8_t*>(b.data()), b.size()};
}

}  // namespace simple_http::quic
