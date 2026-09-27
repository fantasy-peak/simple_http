#pragma once

// What a sender remembers about a packet it has sent.
//
// QUIC tracks every ack-eliciting packet until it is acknowledged or declared
// lost, because two separate things hang off it: whether the bytes still count
// against the congestion window, and what has to be sent again if the packet
// never arrives.
//
// This type is its own header because both halves of that — the loss detector in
// recovery.h and the congestion controller in congestion.h — need the complete
// type, and neither is a sensible place to define it. In particular the
// congestion controller's per-packet loop reads the fields directly, so a
// forward declaration will not do.
//
// Note the deliberate absence of any dependency on the packet-protection layer:
// a CRYPTO range is identified by its *packet number space*, which is exactly
// one crypto stream each (RFC 9000 §19.6), and 0-RTT packets never carry CRYPTO
// frames — so three spaces name every crypto stream there is, and no encryption
// level has to be dragged in here.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace simple_http::quic {

// RFC 9000 §12.3. Initial and Handshake each have their own; 0-RTT and 1-RTT
// share the application space, because they are numbered by the same sequence.
enum class PacketNumberSpace : std::uint8_t {
    Initial = 0,
    Handshake = 1,
    Application = 2,
};

inline constexpr std::size_t kPacketNumberSpaceCount = 3;

inline constexpr std::size_t space_index(PacketNumberSpace space) noexcept {
    return static_cast<std::size_t>(space);
}

// A byte range a sent packet carried, so that losing the packet can put the data
// back on the send queue.
//
// Offsets and lengths rather than a copy of the bytes: the sender keeps its
// unacknowledged data anyway (it may still have to retransmit it), so a record
// that points into that buffer costs 32 octets where a copy would cost the
// payload again, in every packet in flight at once.
struct StreamRange {
    // True for one of the three CRYPTO streams, false for a data stream. CRYPTO
    // is singled out because its offsets are a separate space per packet number
    // space, not per stream id.
    bool crypto{false};
    PacketNumberSpace space{PacketNumberSpace::Application};
    std::uint64_t stream_id{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

// Sentinel for SentPacket::acked_peer_largest: no ACK frame went in this packet.
// The largest packet number a QUIC endpoint may use is 2^62 - 1 (RFC 9000
// §17.1), so this cannot collide with a real one.
inline constexpr std::uint64_t kNoPeerAck = ~0ULL;

struct SentPacket {
    std::uint64_t packet_number{0};
    PacketNumberSpace space{PacketNumberSpace::Initial};
    std::chrono::steady_clock::time_point time_sent{};

    // Octets on the wire, excluding IP and UDP overhead but including the QUIC
    // header and the AEAD tag — which is what the congestion window is measured
    // in (RFC 9002 §B.2).
    std::size_t sent_bytes{0};

    bool ack_eliciting{false};
    // Whether the packet counts toward bytes in flight. Not the same as
    // ack_eliciting: RFC 9002 §B.2 excludes packets that carry only ACK frames
    // and nothing else, so that congestion control never impedes the
    // acknowledgement feedback it depends on.
    bool in_flight{false};

    // Data to be re-sent from its own buffer if this packet is lost.
    std::vector<StreamRange> ranges;

    // Control frames this packet carried, by the sender's own sequence number
    // for them. Their bytes are kept by the connection until acknowledged, so a
    // loss only has to name them — which is what makes RESET_STREAM and
    // STOP_SENDING, whose retransmission is mandatory until acknowledged,
    // need no special case here.
    std::vector<std::uint64_t> control_ids;

    // The largest *peer* packet number this packet's ACK frame acknowledged, when
    // it carried one. It is what lets the received-packet ranges be forgotten
    // once the peer confirms it has the acknowledgement.
    //
    // It has to be recorded here, per packet, because it belongs to the peer's
    // packet number space: this connection's own largest-acked is a number in a
    // space of its own, and the two are unrelated — treating one as the other
    // drops ranges the peer was never told about. `kNoPeerAck` means the packet
    // carried no ACK frame.
    std::uint64_t acked_peer_largest{kNoPeerAck};
};

}  // namespace simple_http::quic
