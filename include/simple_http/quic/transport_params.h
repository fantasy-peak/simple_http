#pragma once

// QUIC transport parameters (RFC 9000 §18) — the extension carried in the TLS
// handshake that tells each side the other's limits.
//
// The encoding is uniform: a variable-length parameter id, a variable-length
// length, then the value. The *validation* is not: which parameters may appear
// depends on which endpoint sent them (a client sending
// original_destination_connection_id is a protocol error), several carry a floor
// or ceiling of their own, and every one may appear at most once. All of that
// lives here because the alternative — trusting the peer and clamping later —
// is how a connection ends up with a zero-length connection ID or an
// acknowledgement delay exponent that silently overflows its shift.
//
// Unknown parameters are ignored, as §18.1 requires: they are how the protocol
// grows, and a peer is entitled to advertise one we have never heard of.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "wire.h"

namespace simple_http::quic {

// Parameter identifiers (RFC 9000 §18.2). Named because the decoder's switch and
// the encoder's emit list both need them and a bare hex literal in two places is
// a place to typo.
enum class TransportParam : std::uint64_t {
    OriginalDestinationConnectionId = 0x00,
    MaxIdleTimeout = 0x01,
    StatelessResetToken = 0x02,
    MaxUdpPayloadSize = 0x03,
    InitialMaxData = 0x04,
    InitialMaxStreamDataBidiLocal = 0x05,
    InitialMaxStreamDataBidiRemote = 0x06,
    InitialMaxStreamDataUni = 0x07,
    InitialMaxStreamsBidi = 0x08,
    InitialMaxStreamsUni = 0x09,
    AckDelayExponent = 0x0a,
    MaxAckDelay = 0x0b,
    DisableActiveMigration = 0x0c,
    PreferredAddress = 0x0d,
    ActiveConnectionIdLimit = 0x0e,
    InitialSourceConnectionId = 0x0f,
    RetrySourceConnectionId = 0x10,
};

// §18.2 defaults, which apply when the parameter is absent.
inline constexpr std::uint64_t kDefaultMaxUdpPayloadSize = 65527;
inline constexpr std::uint64_t kDefaultAckDelayExponent = 3;
inline constexpr std::uint64_t kDefaultMaxAckDelayMs = 25;
inline constexpr std::uint64_t kDefaultActiveConnectionIdLimit = 2;
// The smallest max_udp_payload_size a peer may advertise (§18.2).
inline constexpr std::uint64_t kMinMaxUdpPayloadSize = 1200;

struct TransportParams {
    // Server-only, and required of a server: the DCID the client used in its
    // first Initial. It is what proves the server is not replaying someone
    // else's connection (RFC 9000 §7.3).
    bool has_original_dcid{false};
    std::string original_dcid;

    // Milliseconds; zero disables the idle timeout.
    std::uint64_t max_idle_timeout{0};
    // Server-only. Sent so the peer can recognise a stateless reset.
    bool has_stateless_reset_token{false};
    std::array<std::uint8_t, 16> stateless_reset_token{};

    std::uint64_t max_udp_payload_size{kDefaultMaxUdpPayloadSize};
    std::uint64_t initial_max_data{0};
    std::uint64_t initial_max_stream_data_bidi_local{0};
    std::uint64_t initial_max_stream_data_bidi_remote{0};
    std::uint64_t initial_max_stream_data_uni{0};
    std::uint64_t initial_max_streams_bidi{0};
    std::uint64_t initial_max_streams_uni{0};
    std::uint64_t ack_delay_exponent{kDefaultAckDelayExponent};
    std::uint64_t max_ack_delay{kDefaultMaxAckDelayMs};
    bool disable_active_migration{false};
    std::uint64_t active_connection_id_limit{kDefaultActiveConnectionIdLimit};

    // Both endpoints send this, and both must check it against the source
    // connection ID they actually observed (RFC 9000 §7.3).
    std::string initial_source_connection_id;
    bool has_initial_scid{false};
    // Server-only, and present only if the server sent a Retry.
    std::string retry_source_connection_id;
    bool has_retry_scid{false};
};

// Encode `p` for sending. `server` selects which of the endpoint-specific
// parameters are emitted at all — a client must not send a stateless reset
// token, and emitting one would be a protocol error at the peer, not a harmless
// extra.
Bytes encode_transport_params(const TransportParams& p, bool server);

// Decode the extension body. `from_client` is true when we are the server (so
// the peer is a client), which is what decides whether an endpoint-specific
// parameter is legal.
//
// Returns false when the parameters are unusable — a duplicate, an illegal
// direction, or a value outside its range. The caller closes the connection with
// TRANSPORT_PARAMETER_ERROR; there is no partial success.
bool decode_transport_params(std::span<const std::uint8_t> data, bool from_client, TransportParams& out);

// --- implementation -------------------------------------------------------

namespace detail {

// The largest connection ID either endpoint may use (RFC 9000 §17.2). Repeating
// packet.h's kMaxConnectionIdLen would be a second place to change it, and the
// two files do not include each other.
inline constexpr std::uint64_t kMaxConnectionIdLen = 20;

// A parameter value that has to be exactly one variable-length integer. The
// declared length must equal the integer's *own* width: a longer or shorter
// length field would leave octets on either side of the value, and the RFC pairs
// a parameter's length with its value (RFC 9000 §18).
//
// Note this is not the shortest-encoding rule — §16 is explicit that a value
// need not use the minimum number of octets, so a self-consistent two-octet
// encoding of 1 is legal here and must be accepted.
inline bool read_int_param(Reader& r, std::uint64_t length, std::uint64_t& out) {
    if (r.peek_varint_width() != length) return false;
    out = r.varint();
    return !r.failed();
}

inline bool read_cid_param(Reader& r, std::uint64_t length, std::string& out) {
    if (length > kMaxConnectionIdLen) return false;
    out = r.copy(static_cast<std::size_t>(length));
    return !r.failed();
}

}  // namespace detail

inline Bytes encode_transport_params(const TransportParams& p, bool server) {
    Bytes out;
    // Every value here is short enough that its encoded size is known before it
    // is written, so the length can be emitted straight through rather than
    // reserved and patched. Anything longer (a connection ID, a token) is a byte
    // string whose size is `size()`.
    const auto emit = [&out](TransportParam id, std::size_t value_len) {
        append_varint(out, static_cast<std::uint64_t>(id));
        append_varint(out, value_len);
    };
    const auto emit_int = [&out, &emit](TransportParam id, std::uint64_t value) {
        emit(id, varint_len(value));
        append_varint(out, value);
    };
    const auto emit_cid = [&out, &emit](TransportParam id, std::string_view cid) {
        emit(id, cid.size());
        out.append(cid);
    };
    const auto emit_empty = [&out, &emit](TransportParam id) { emit(id, 0); };

    if (server && p.has_original_dcid) {
        emit_cid(TransportParam::OriginalDestinationConnectionId, p.original_dcid);
    }
    if (p.max_idle_timeout != 0) emit_int(TransportParam::MaxIdleTimeout, p.max_idle_timeout);
    if (server && p.has_stateless_reset_token) {
        emit(TransportParam::StatelessResetToken, p.stateless_reset_token.size());
        out.append(reinterpret_cast<const char*>(p.stateless_reset_token.data()),
                   p.stateless_reset_token.size());
    }
    emit_int(TransportParam::MaxUdpPayloadSize, p.max_udp_payload_size);
    emit_int(TransportParam::InitialMaxData, p.initial_max_data);
    emit_int(TransportParam::InitialMaxStreamDataBidiLocal, p.initial_max_stream_data_bidi_local);
    emit_int(TransportParam::InitialMaxStreamDataBidiRemote, p.initial_max_stream_data_bidi_remote);
    emit_int(TransportParam::InitialMaxStreamDataUni, p.initial_max_stream_data_uni);
    emit_int(TransportParam::InitialMaxStreamsBidi, p.initial_max_streams_bidi);
    emit_int(TransportParam::InitialMaxStreamsUni, p.initial_max_streams_uni);
    emit_int(TransportParam::AckDelayExponent, p.ack_delay_exponent);
    emit_int(TransportParam::MaxAckDelay, p.max_ack_delay);
    if (p.disable_active_migration) emit_empty(TransportParam::DisableActiveMigration);
    emit_int(TransportParam::ActiveConnectionIdLimit, p.active_connection_id_limit);
    emit_cid(TransportParam::InitialSourceConnectionId, p.initial_source_connection_id);
    if (server && p.has_retry_scid) {
        emit_cid(TransportParam::RetrySourceConnectionId, p.retry_source_connection_id);
    }
    return out;
}

inline bool decode_transport_params(std::span<const std::uint8_t> data, bool from_client, TransportParams& out) {
    out = TransportParams{};
    Reader r{data};
    // Every parameter may appear at most once (§7.4). Tracked as a bitmask over
    // the ids we know: fit in a 64-bit word with room to spare, and unknown ids
    // are ignored rather than tracked, since a repeat of one we do not read is
    // not a repeat of anything.
    std::uint64_t seen = 0;

    while (!r.empty()) {
        const std::uint64_t id = r.varint();
        const std::uint64_t len = r.varint();
        if (r.failed() || r.remaining() < len) return false;
        const auto value = r.bytes(static_cast<std::size_t>(len));
        if (r.failed()) return false;

        if (id < 64) {
            const std::uint64_t bit = 1ULL << id;
            if (seen & bit) return false;  // duplicate
            seen |= bit;
        }

        // Server-only parameters. A client sending one is a protocol error
        // (§18.2), not something to ignore: it would mean the client believes it
        // is a server.
        const bool server_only = id == static_cast<std::uint64_t>(TransportParam::OriginalDestinationConnectionId) ||
                                 id == static_cast<std::uint64_t>(TransportParam::StatelessResetToken) ||
                                 id == static_cast<std::uint64_t>(TransportParam::PreferredAddress) ||
                                 id == static_cast<std::uint64_t>(TransportParam::RetrySourceConnectionId);
        if (server_only && from_client) return false;

        Reader v{value};
        switch (static_cast<TransportParam>(id)) {
            case TransportParam::OriginalDestinationConnectionId:
                if (!detail::read_cid_param(v, len, out.original_dcid)) return false;
                out.has_original_dcid = true;
                break;

            case TransportParam::MaxIdleTimeout:
                if (!detail::read_int_param(v, len, out.max_idle_timeout)) return false;
                break;

            case TransportParam::StatelessResetToken:
                // Exactly 16 octets (§18.2): it is compared byte-for-byte against
                // the tail of a datagram, so a different length is not a smaller
                // token, it is a token that can never match.
                if (len != out.stateless_reset_token.size()) return false;
                for (std::size_t i = 0; i < out.stateless_reset_token.size(); ++i) {
                    out.stateless_reset_token[i] = value[i];
                }
                out.has_stateless_reset_token = true;
                break;

            case TransportParam::MaxUdpPayloadSize: {
                std::uint64_t v64 = 0;
                if (!detail::read_int_param(v, len, v64)) return false;
                // §18.2: at least 1200, so that a datagram carrying a full
                // Initial always fits.
                if (v64 < kMinMaxUdpPayloadSize) return false;
                out.max_udp_payload_size = v64;
                break;
            }

            case TransportParam::InitialMaxData:
                if (!detail::read_int_param(v, len, out.initial_max_data)) return false;
                break;
            case TransportParam::InitialMaxStreamDataBidiLocal:
                if (!detail::read_int_param(v, len, out.initial_max_stream_data_bidi_local)) return false;
                break;
            case TransportParam::InitialMaxStreamDataBidiRemote:
                if (!detail::read_int_param(v, len, out.initial_max_stream_data_bidi_remote)) return false;
                break;
            case TransportParam::InitialMaxStreamDataUni:
                if (!detail::read_int_param(v, len, out.initial_max_stream_data_uni)) return false;
                break;

            case TransportParam::InitialMaxStreamsBidi: {
                std::uint64_t v64 = 0;
                if (!detail::read_int_param(v, len, v64)) return false;
                // §18.2: capped at 2^60 so stream IDs stay in range.
                if (v64 > (1ULL << 60)) return false;
                out.initial_max_streams_bidi = v64;
                break;
            }
            case TransportParam::InitialMaxStreamsUni: {
                std::uint64_t v64 = 0;
                if (!detail::read_int_param(v, len, v64)) return false;
                if (v64 > (1ULL << 60)) return false;
                out.initial_max_streams_uni = v64;
                break;
            }

            case TransportParam::AckDelayExponent: {
                std::uint64_t v64 = 0;
                if (!detail::read_int_param(v, len, v64)) return false;
                // §18.2: at most 20, because the value is used as a left shift.
                if (v64 > 20) return false;
                out.ack_delay_exponent = v64;
                break;
            }

            case TransportParam::MaxAckDelay: {
                std::uint64_t v64 = 0;
                if (!detail::read_int_param(v, len, v64)) return false;
                // §18.2: below 2^14, so the peer's max_ack_delay always fits the
                // ACK frame's varint after being scaled.
                if (v64 >= (1ULL << 14)) return false;
                out.max_ack_delay = v64;
                break;
            }

            case TransportParam::DisableActiveMigration:
                if (len != 0) return false;
                out.disable_active_migration = true;
                break;

            case TransportParam::PreferredAddress:
                // A server's alternate address. We advertise none, and a client
                // sending one was rejected above; a well-formed one from a
                // server is accepted but unused, so all that is checked is the
                // fixed part's size (RFC 9000 §18.2).
                if (len < 41) return false;
                break;

            case TransportParam::ActiveConnectionIdLimit: {
                std::uint64_t v64 = 0;
                if (!detail::read_int_param(v, len, v64)) return false;
                // §18.2: at least 2 — an endpoint has to be able to hold the
                // initial connection ID plus one more.
                if (v64 < 2) return false;
                out.active_connection_id_limit = v64;
                break;
            }

            case TransportParam::InitialSourceConnectionId:
                if (!detail::read_cid_param(v, len, out.initial_source_connection_id)) return false;
                out.has_initial_scid = true;
                break;

            case TransportParam::RetrySourceConnectionId:
                if (!detail::read_cid_param(v, len, out.retry_source_connection_id)) return false;
                out.has_retry_scid = true;
                break;

            default:
                // Unknown: ignore the value, keep the connection (§18.1).
                break;
        }
        if (v.failed()) return false;
    }

    // §7.3: both endpoints MUST send initial_source_connection_id, and a server
    // MUST send original_destination_connection_id. Without the first there is
    // no way to detect a connection ID that was rewritten in flight; without the
    // second, no way to tell a fresh connection from a replayed one.
    if (!out.has_initial_scid) return false;
    if (!from_client && !out.has_original_dcid) return false;
    return true;
}

}  // namespace simple_http::quic
