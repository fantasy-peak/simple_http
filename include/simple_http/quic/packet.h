#pragma once

// QUIC packet headers (RFC 9000 §17) and the parsing order header protection
// forces on us.
//
// Header protection encrypts the *low bits of the first octet and the whole
// packet-number field* (RFC 9001 §5.4). One of those low bits is the packet
// number's own length, so a protected packet cannot be fully parsed in one pass:
// the header up to the packet number is cleartext and can be parsed directly,
// but the packet number's width — and its value — only exist after the first
// octet has been unmasked. So parsing here stops at `pn_offset` and says so;
// `read_packet_number` finishes the job once crypto.h has unmasked the first
// octet.
//
// The split is not cosmetic. Everything before the packet number (connection
// IDs, the Length field, the Retry token) is what the endpoint needs to *find*
// the connection in the first place, and it is available without any keys at
// all — including for a version we do not speak, where the response is a Version
// Negotiation packet rather than a parse error.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "wire.h"

namespace simple_http::quic {

// The only QUIC version this implementation speaks (RFC 9000).
inline constexpr std::uint32_t kQuicVersion1 = 0x00000001;

// The smallest datagram a client's Initial may occupy, and the value a server
// pads its own datagrams to while the handshake is unconfirmed: it is the
// amplification limit's unit (RFC 9000 §14.1).
inline constexpr std::size_t kMinInitialDatagramSize = 1200;

// RFC 8999 §5.2: a QUIC packet's first octet has bit 0x40 set. A packet that
// does not is not QUIC and must be dropped without a response.
inline constexpr std::uint8_t kFixedBit = 0x40;

// Long header: form bit, then the two-bit type (RFC 9000 §17.2).
inline constexpr std::uint8_t kHeaderFormLong = 0x80;
inline constexpr std::uint8_t kLongTypeMask = 0x30;
inline constexpr std::uint8_t kLongTypeShift = 4;

// Short header (RFC 9000 §17.3.1): spin bit, reserved, key phase, PN length.
inline constexpr std::uint8_t kSpinBit = 0x20;
inline constexpr std::uint8_t kKeyPhaseBit = 0x04;
inline constexpr std::uint8_t kPnLenMask = 0x03;

enum class LongHeaderType : std::uint8_t {
    Initial = 0,
    ZeroRtt = 1,
    Handshake = 2,
    Retry = 3,
};

// Header protection takes its sample four octets past the start of the packet
// number field, and needs sixteen of them (RFC 9001 §5.4.2).
inline constexpr std::size_t kHeaderProtectionSampleOffset = 4;
inline constexpr std::size_t kHeaderProtectionSampleLen = 16;

// The largest connection ID either side may use (RFC 9000 §17.2).
inline constexpr std::size_t kMaxConnectionIdLen = 20;

struct PacketHeader {
    bool long_header{false};
    LongHeaderType type{LongHeaderType::Initial};
    std::uint32_t version{0};
    std::string dcid;
    std::string scid;   // long headers only
    std::string token;  // Initial only

    // Long headers carry an explicit Length covering the packet number and the
    // payload; short headers run to the end of the datagram.
    std::uint64_t length{0};

    // Where the (still protected) packet-number field starts. Everything before
    // it is authenticated but not encrypted, which is what makes it the AEAD's
    // associated data.
    std::size_t pn_offset{0};

    // Filled in by read_packet_number(), after header protection is removed.
    std::size_t pn_len{0};
    std::uint64_t packet_number{0};

    // Short header only.
    bool key_phase{false};
    bool spin{false};

    // The octets the AEAD authenticates: the whole header up to and including
    // the packet number.
    [[nodiscard]] std::size_t header_len() const noexcept { return pn_offset + pn_len; }
};

enum class PacketParseStatus {
    Ok,
    // Fixed bit clear, or too short to be a header at all. Not an error and not
    // answerable — the RFC requires dropping these without a reply.
    NotQuic,
    // A Version Negotiation packet: a long header whose version field is zero.
    // Only a client receives one, so a server drops it; it is distinct from
    // NotQuic because it *is* QUIC, and a client would have to act on it.
    VersionNegotiation,
    // A long header naming a version we do not implement. The right answer is a
    // Version Negotiation packet, so the DCID/SCID are still filled in.
    UnsupportedVersion,
    Malformed,
    // A Retry packet. A server never receives one, but it must be recognised so
    // it can be rejected as a protocol violation rather than parsed as Initial.
    Retry,
};

// Parse the invariant and header fields, stopping at the packet number.
//
// `local_cid_len` is the length of the connection IDs this endpoint issues: a
// short header's DCID has no length field, so the only way to know where it ends
// is to already know how long it is.
PacketParseStatus parse_packet_header(Reader& r, std::size_t local_cid_len, PacketHeader& out);

// Read the packet number out of `r`, which must be positioned at `out.pn_offset`
// and must already have had header protection removed. `pn_len` is read from the
// unmasked first octet, so `packet[0]` has to be the datagram's first octet —
// callers pass the whole datagram's reader, not a slice.
//
// `largest_received` is this packet number space's largest packet number so far,
// which is what makes the truncated number unambiguous.
bool read_packet_number(Reader& r, PacketHeader& out, std::uint64_t largest_received);

// --- senders --------------------------------------------------------------

// A long-header packet's bytes up to (not including) the packet number.
// `length` is the value written to the Length field: the packet number's octets
// plus the payload's (RFC 9000 §17.2). `pn_offset` receives the offset of the
// packet-number field within `out`.
void append_long_header(Bytes& out, LongHeaderType type, std::uint32_t version, std::string_view dcid,
                        std::string_view scid, std::string_view token, std::uint64_t length,
                        std::size_t pn_len, std::size_t& pn_offset);

// A short-header (1-RTT) packet's bytes up to (not including) the packet number.
void append_short_header(Bytes& out, std::string_view dcid, bool key_phase, std::size_t pn_len,
                         std::size_t& pn_offset);

// Append `pn` truncated to `pn_len` octets at the current end of `out`.
void append_packet_number(Bytes& out, std::uint64_t pn, std::size_t pn_len);

// A Version Negotiation packet (RFC 9000 §17.2.1). Its version field is zero,
// which is what distinguishes it.
void append_version_negotiation(Bytes& out, std::string_view dcid, std::string_view scid,
                                std::span<const std::uint32_t> supported_versions);

// A Retry packet (RFC 9000 §17.2.5). `integrity_tag` is the 16-octet tag from
// crypto.h's retry_integrity_tag(), computed over the pseudo-packet below.
void append_retry(Bytes& out, std::string_view dcid, std::string_view scid, std::string_view token,
                  std::string_view integrity_tag);

// The Retry Pseudo-Packet (RFC 9001 §5.8): the original destination connection
// ID, then the Retry packet with its integrity tag removed.
void append_retry_pseudo_packet(Bytes& out, std::string_view original_dcid,
                                std::string_view retry_without_tag);

// --- implementation -------------------------------------------------------

namespace detail {

// A connection ID's length-prefixed wire form (RFC 9000 §17.2).
inline void append_cid(Bytes& out, std::string_view cid) {
    append_u8(out, static_cast<std::uint8_t>(cid.size()));
    out.append(cid);
}

inline bool read_cid(Reader& r, std::string& out) {
    const std::uint64_t len = r.u8();
    if (r.failed() || len > kMaxConnectionIdLen) return false;
    out = r.copy(static_cast<std::size_t>(len));
    return !r.failed();
}

}  // namespace detail

inline PacketParseStatus parse_packet_header(Reader& r, std::size_t local_cid_len, PacketHeader& out) {
    out = PacketHeader{};
    if (r.remaining() < 1) return PacketParseStatus::NotQuic;

    const std::uint8_t first = r.u8();
    out.long_header = (first & kHeaderFormLong) != 0;

    if (!out.long_header) {
        if ((first & kFixedBit) == 0) return PacketParseStatus::NotQuic;
        // Short header: DCID of the endpoint's own length, then the packet
        // number. No length field — the packet always ends the datagram, which
        // is why a coalesced datagram puts it last.
        out.dcid = r.copy(local_cid_len);
        if (r.failed()) return PacketParseStatus::Malformed;
        out.spin = (first & kSpinBit) != 0;
        out.key_phase = (first & kKeyPhaseBit) != 0;
        out.pn_offset = r.offset();
        return PacketParseStatus::Ok;
    }

    out.version = r.u32();
    if (r.failed()) return PacketParseStatus::Malformed;

    // Version Negotiation is the one QUIC packet whose fixed bit is *clear*
    // (RFC 8999 §5.2). It is therefore recognised by its zero version before the
    // bit is judged, or the bit test would file it as "not QUIC" — which is
    // exactly the packet a client must not ignore.
    if (out.version == 0) {
        if (!detail::read_cid(r, out.dcid)) return PacketParseStatus::Malformed;
        if (!detail::read_cid(r, out.scid)) return PacketParseStatus::Malformed;
        return PacketParseStatus::VersionNegotiation;
    }
    if ((first & kFixedBit) == 0) return PacketParseStatus::NotQuic;

    out.type = static_cast<LongHeaderType>((first & kLongTypeMask) >> kLongTypeShift);
    // The connection IDs are parsed before the version is judged: a Version
    // Negotiation reply has to echo both of them, and a version we do not speak
    // is exactly when that reply is owed. The CID layout (RFC 8999) is common to
    // every version precisely so this is possible.
    if (!detail::read_cid(r, out.dcid)) return PacketParseStatus::Malformed;
    if (!detail::read_cid(r, out.scid)) return PacketParseStatus::Malformed;

    if (out.version != kQuicVersion1) return PacketParseStatus::UnsupportedVersion;

    if (out.type == LongHeaderType::Retry) {
        // The token is whatever is left after the 16-octet integrity tag.
        if (r.remaining() < 16) return PacketParseStatus::Malformed;
        out.token = r.copy(r.remaining() - 16);
        return r.failed() ? PacketParseStatus::Malformed : PacketParseStatus::Retry;
    }

    if (out.type == LongHeaderType::Initial) {
        const std::uint64_t token_len = r.varint();
        if (r.failed() || r.remaining() < token_len) return PacketParseStatus::Malformed;
        out.token = r.copy(static_cast<std::size_t>(token_len));
        if (r.failed()) return PacketParseStatus::Malformed;
    }

    out.length = r.varint();
    if (r.failed()) return PacketParseStatus::Malformed;
    out.pn_offset = r.offset();
    // Length covers the packet number and the payload. At least one octet of
    // packet number has to be accounted for, so a Length below that is
    // self-contradictory.
    if (out.length < 1) return PacketParseStatus::Malformed;
    if (r.remaining() < out.length) return PacketParseStatus::Malformed;
    return PacketParseStatus::Ok;
}

inline bool read_packet_number(Reader& r, PacketHeader& out, std::uint64_t largest_received) {
    if (r.offset() > out.pn_offset) return false;
    if (r.size() < out.pn_offset + 1) return false;
    // The width lives in the low two bits of the first octet — the ones header
    // protection masks, so this only reads correctly post-unmasking. `r` must
    // span the whole datagram for that octet to be the one being described;
    // `parse_packet_header` produces `pn_offset` in exactly that frame.
    out.pn_len = (r.data()[0] & kPnLenMask) + 1;
    if (r.size() < out.pn_offset + out.pn_len) return false;
    if (!r.skip(out.pn_offset - r.offset())) return false;
    const std::uint64_t truncated = r.packet_number(out.pn_len);
    if (r.failed()) return false;
    out.packet_number = decode_packet_number(largest_received, truncated, out.pn_len * 8);
    return true;
}

inline void append_long_header(Bytes& out, LongHeaderType type, std::uint32_t version,
                               std::string_view dcid, std::string_view scid, std::string_view token,
                               std::uint64_t length, std::size_t pn_len, std::size_t& pn_offset) {
    const auto type_bits = static_cast<std::uint8_t>(static_cast<std::uint8_t>(type) << kLongTypeShift);
    append_u8(out, static_cast<std::uint8_t>(kHeaderFormLong | kFixedBit | type_bits |
                                             static_cast<std::uint8_t>(pn_len - 1)));
    append_u32(out, version);
    detail::append_cid(out, dcid);
    detail::append_cid(out, scid);
    if (type == LongHeaderType::Initial) {
        append_varint(out, token.size());
        out.append(token);
    }
    append_varint(out, length);
    pn_offset = out.size();
}

inline void append_short_header(Bytes& out, std::string_view dcid, bool key_phase, std::size_t pn_len,
                                std::size_t& pn_offset) {
    std::uint8_t first = static_cast<std::uint8_t>(kFixedBit | (pn_len - 1));
    if (key_phase) first = static_cast<std::uint8_t>(first | kKeyPhaseBit);
    append_u8(out, first);
    out.append(dcid);
    pn_offset = out.size();
}

inline void append_packet_number(Bytes& out, std::uint64_t pn, std::size_t pn_len) {
    for (std::size_t i = pn_len; i-- > 0;) {
        out.push_back(static_cast<char>((pn >> (8 * i)) & 0xff));
    }
}

inline void append_version_negotiation(Bytes& out, std::string_view dcid, std::string_view scid,
                                       std::span<const std::uint32_t> supported_versions) {
    // Version Negotiation is a long header with a zero version and *no* length
    // field or packet number: it is not protected and carries no frames.
    // The form bit is set and the fixed bit is *not* — RFC 9000 §17.2.1 makes
    // this the one packet where 0x40 must be clear, so a peer cannot mistake a
    // negotiation for a real Initial.
    append_u8(out, kHeaderFormLong);
    append_u32(out, 0);
    detail::append_cid(out, dcid);
    detail::append_cid(out, scid);
    for (const std::uint32_t v : supported_versions) append_u32(out, v);
}

inline void append_retry(Bytes& out, std::string_view dcid, std::string_view scid, std::string_view token,
                         std::string_view integrity_tag) {
    const auto type_bits = static_cast<std::uint8_t>(static_cast<std::uint8_t>(LongHeaderType::Retry)
                                                     << kLongTypeShift);
    append_u8(out, static_cast<std::uint8_t>(kHeaderFormLong | kFixedBit | type_bits));
    append_u32(out, kQuicVersion1);
    detail::append_cid(out, dcid);
    detail::append_cid(out, scid);
    out.append(token);
    out.append(integrity_tag);
}

inline void append_retry_pseudo_packet(Bytes& out, std::string_view original_dcid,
                                       std::string_view retry_without_tag) {
    detail::append_cid(out, original_dcid);
    out.append(retry_without_tag);
}

}  // namespace simple_http::quic
