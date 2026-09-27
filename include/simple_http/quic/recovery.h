#pragma once

// RTT estimation, loss detection and the PTO timer (RFC 9002 §5, §6 and the
// pseudocode in Appendix A), driving the congestion controller in congestion.h.
//
// The three packet number spaces each keep their own sent-packet list, their own
// largest-acked packet number and their own PTO deadline, because a packet can
// only be acknowledged in the space it was sent in. The RTT estimate and the
// congestion window, by contrast, are properties of the *path*, and are shared —
// one of each, no matter how many spaces are live.
//
// Loss detection is deliberately conservative in one direction and generous in
// the other: the packet threshold (three) and the time threshold (9/8 of the
// RTT) are both reordering windows, so a packet is only declared lost when it
// cannot plausibly still be in flight. Erring eager costs throughput through
// spurious retransmission; erring lazy costs latency, which the PTO timer
// bounds.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "congestion.h"
#include "frame.h"
#include "sent_packet.h"

namespace simple_http::quic {

// RFC 9002 §6.1.1: reordering tolerated before a packet is deemed lost.
inline constexpr std::uint64_t kPacketThreshold = 3;
// §6.1.2: the timer granularity, and the floor on the loss delay.
inline constexpr std::chrono::microseconds kGranularity{1000};
// §6.2.2: the RTT assumed until the first sample arrives.
inline constexpr std::chrono::microseconds kInitialRtt{333000};

// The RTT estimator of RFC 9002 §5.
class RttEstimator {
  public:
    [[nodiscard]] std::chrono::microseconds latest() const noexcept { return m_latest; }
    [[nodiscard]] std::chrono::microseconds smoothed() const noexcept { return m_smoothed; }
    [[nodiscard]] std::chrono::microseconds rttvar() const noexcept { return m_rttvar; }
    [[nodiscard]] std::chrono::microseconds min_rtt() const noexcept { return m_min_rtt; }
    [[nodiscard]] bool have_sample() const noexcept { return m_have_sample; }

    // `ack_delay` is what the peer reported, already scaled out of its exponent
    // units. It is only subtracted where it is plausible: a delay larger than
    // the RTT it is meant to explain is an artefact of reordering, not
    // processing time, and believing it would bias the estimate low (§5.3).
    void update(std::chrono::microseconds latest_rtt, std::chrono::microseconds ack_delay,
                bool handshake_confirmed, std::chrono::microseconds max_ack_delay) {
        m_latest = latest_rtt;
        if (!m_have_sample) {
            m_min_rtt = latest_rtt;
            m_smoothed = latest_rtt;
            m_rttvar = latest_rtt / 2;
            m_have_sample = true;
            return;
        }
        m_min_rtt = std::min(m_min_rtt, latest_rtt);

        // Before the handshake is confirmed the peer may still be delaying
        // acknowledgements by more than it admitted, so its report is not
        // trusted at all (§5.3).
        std::chrono::microseconds adjusted = latest_rtt;
        if (handshake_confirmed) {
            const auto capped = std::min(ack_delay, max_ack_delay);
            if (latest_rtt >= m_min_rtt + capped) adjusted = latest_rtt - capped;
        }

        // The 1/4 and 1/8 are applied to *differences* rather than to an
        // absolute value, which keeps the truncation from accumulating the way a
        // scaled multiply would.
        const auto diff = m_smoothed > adjusted ? m_smoothed - adjusted : adjusted - m_smoothed;
        m_rttvar = m_rttvar - m_rttvar / 4 + diff / 4;
        m_smoothed = m_smoothed - m_smoothed / 8 + adjusted / 8;
    }

    // The base of the PTO period (RFC 9002 §6.2.1), before the exponential
    // backoff: smoothed_rtt + max(4 * rttvar, kGranularity).
    [[nodiscard]] std::chrono::microseconds pto_base() const noexcept {
        return m_smoothed + std::max(4 * m_rttvar, kGranularity);
    }

  private:
    std::chrono::microseconds m_latest{0};
    std::chrono::microseconds m_smoothed{kInitialRtt};
    std::chrono::microseconds m_rttvar{kInitialRtt / 2};
    std::chrono::microseconds m_min_rtt{0};
    bool m_have_sample{false};
};

// What processing an ACK frame produced, so the connection can act on it.
struct AckOutcome {
    std::vector<SentPacket> acked;
    std::vector<SentPacket> lost;
    bool updated_rtt{false};
};

class LossRecovery {
  public:
    using Clock = std::chrono::steady_clock;

    explicit LossRecovery(std::size_t max_datagram_size = kMaxDatagramSize) : m_cc(max_datagram_size) {}

    [[nodiscard]] NewReno& congestion() noexcept { return m_cc; }
    [[nodiscard]] const NewReno& congestion() const noexcept { return m_cc; }
    [[nodiscard]] const RttEstimator& rtt() const noexcept { return m_rtt; }

    void set_max_ack_delay(std::chrono::microseconds delay) noexcept { m_max_ack_delay = delay; }
    [[nodiscard]] std::chrono::microseconds max_ack_delay() const noexcept { return m_max_ack_delay; }

    // The exponent the *peer* uses to scale the acknowledgement delays it
    // reports, from its transport parameters. Without it a received ack_delay is
    // off by a power of two.
    void set_peer_ack_delay_exponent(std::uint64_t exponent) noexcept {
        m_peer_ack_delay_exponent = static_cast<unsigned>(std::min<std::uint64_t>(exponent, 20));
    }

    // Handshake confirmation changes three things at once: the peer's reported
    // acknowledgement delay becomes trustworthy, the application space joins the
    // PTO calculation, and pto_count stops being reset on every ACK
    // (RFC 9002 §6.2.2.1).
    void set_handshake_confirmed(bool confirmed) noexcept { m_handshake_confirmed = confirmed; }
    [[nodiscard]] bool handshake_confirmed() const noexcept { return m_handshake_confirmed; }

    // Whether the peer's address is validated. A server validates it with the
    // first protected packet; a server that sent a Retry has it validated from
    // the start. Until then the anti-deadlock PTO keeps probing.
    void set_address_validated(bool validated) noexcept { m_address_validated = validated; }
    [[nodiscard]] bool address_validated() const noexcept { return m_address_validated; }

    void set_handshake_keys_available(bool available) noexcept { m_handshake_keys_available = available; }
    [[nodiscard]] bool handshake_keys_available() const noexcept { return m_handshake_keys_available; }

    void on_packet_sent(const SentPacket& packet) {
        Space& s = m_spaces[space_index(packet.space)];
        s.largest_sent = packet.packet_number;
        s.has_sent = true;
        if (packet.in_flight) {
            m_cc.on_packet_sent(packet.sent_bytes, true);
            if (packet.ack_eliciting) {
                s.time_of_last_ack_eliciting = packet.time_sent;
                s.has_ack_eliciting_in_flight = true;
            }
        }
        s.sent.emplace(packet.packet_number, packet);
    }

    // Process an ACK frame. Returns false only for an ACK that is itself
    // invalid: acknowledging a packet that was never sent is a connection error
    // (RFC 9000 §19.3.1), and the caller closes on it.
    bool on_ack_received(const Frame& ack, PacketNumberSpace space, Clock::time_point now,
                         AckOutcome& outcome) {
        Space& s = m_spaces[space_index(space)];
        if (!s.has_sent || ack.largest_ack > s.largest_sent) return false;

        if (!s.has_largest_acked || ack.largest_ack > s.largest_acked) {
            s.largest_acked = ack.largest_ack;
            s.has_largest_acked = true;
        }

        // The ranges are descending and non-overlapping, so each one is a
        // single contiguous walk of the ordered map.
        bool any_ack_eliciting = false;
        for (const AckRange& range : ack.ack_ranges) {
            auto it = s.sent.lower_bound(range.smallest);
            while (it != s.sent.end() && it->first <= range.largest) {
                if (it->second.ack_eliciting) any_ack_eliciting = true;
                outcome.acked.push_back(std::move(it->second));
                it = s.sent.erase(it);
            }
        }
        if (outcome.acked.empty()) return true;

        // An RTT sample comes only from the largest acknowledged packet, and
        // only when it was acknowledged by *this* frame: sampling from an old
        // send time would inflate the estimate.
        const auto largest = std::max_element(
            outcome.acked.begin(), outcome.acked.end(),
            [](const SentPacket& a, const SentPacket& b) { return a.packet_number < b.packet_number; });
        if (largest->packet_number == ack.largest_ack && any_ack_eliciting) {
            const auto latest = std::chrono::duration_cast<std::chrono::microseconds>(now - largest->time_sent);
            const auto delay = std::chrono::microseconds{ack.ack_delay} * (std::int64_t{1} << m_peer_ack_delay_exponent);
            m_rtt.update(latest, delay, m_handshake_confirmed, m_max_ack_delay);
            outcome.updated_rtt = true;
        }

        m_cc.on_packets_acked(outcome.acked, now, /*limited=*/false);
        detect_lost(space, now, outcome);
        apply_loss(outcome.lost, now);
        refresh_ack_eliciting(space);

        if (peer_completed_address_validation()) m_pto_count = 0;
        return true;
    }

    // Packets that time-threshold loss detection now considers lost, run when
    // the loss timer fires.
    void on_loss_timeout(PacketNumberSpace space, Clock::time_point now, AckOutcome& outcome) {
        detect_lost(space, now, outcome);
        apply_loss(outcome.lost, now);
        refresh_ack_eliciting(space);
    }

    // The earliest time a tracked packet joins the time-threshold loss set, or
    // the epoch when there is none.
    [[nodiscard]] std::pair<Clock::time_point, PacketNumberSpace> loss_time() const noexcept {
        Clock::time_point earliest{};
        PacketNumberSpace best = PacketNumberSpace::Initial;
        for (std::size_t i = 0; i < kPacketNumberSpaceCount; ++i) {
            const Clock::time_point t = m_spaces[i].loss_time;
            if (t == Clock::time_point{}) continue;
            if (earliest == Clock::time_point{} || t < earliest) {
                earliest = t;
                best = static_cast<PacketNumberSpace>(i);
            }
        }
        return {earliest, best};
    }

    // The PTO deadline and the space that should carry the probe (RFC 9002
    // §6.2.1). The epoch means "no timer".
    std::pair<Clock::time_point, PacketNumberSpace> pto(Clock::time_point now) const {
        const auto backoff = std::int64_t{1} << std::min<std::size_t>(m_pto_count, 20);
        const auto duration = m_rtt.pto_base() * backoff;

        if (!has_ack_eliciting_in_flight()) {
            // Anti-deadlock (§6.2.2.1): with nothing in flight and the peer's
            // address unvalidated, the endpoint has to keep probing or the
            // connection stalls with neither side owing the other anything.
            if (m_handshake_keys_available) return {now + duration, PacketNumberSpace::Handshake};
            return {now + duration, PacketNumberSpace::Initial};
        }

        Clock::time_point timeout{};
        PacketNumberSpace best = PacketNumberSpace::Initial;
        for (std::size_t i = 0; i < kPacketNumberSpaceCount; ++i) {
            const Space& s = m_spaces[i];
            if (!s.has_ack_eliciting_in_flight) continue;
            auto space_duration = duration;
            if (static_cast<PacketNumberSpace>(i) == PacketNumberSpace::Application) {
                // The application space waits on the peer's own ack delay, and
                // is skipped entirely until the handshake confirms it (§6.2.1).
                if (!m_handshake_confirmed) continue;
                space_duration += m_max_ack_delay * backoff;
            }
            const Clock::time_point t = s.time_of_last_ack_eliciting + space_duration;
            if (timeout == Clock::time_point{} || t < timeout) {
                timeout = t;
                best = static_cast<PacketNumberSpace>(i);
            }
        }
        return {timeout, best};
    }

    // A PTO probe has to be ack-eliciting, and the reason it is needed is that
    // something the peer depends on never got there. Declaring the oldest
    // unacknowledged packet in the space lost is what turns the probe into a
    // retransmission of it rather than a bare PING.
    //
    // Loss detection cannot reach that packet by itself. Its thresholds measure
    // against an acknowledgement, and a flight that was lost in its entirety —
    // which is exactly this case — never produces one, so the packet stays in
    // flight until the connection gives up. A handshake is where it shows: the
    // server's opening flight is the ServerHello, and a client that never sees
    // it retransmits its ClientHello at a doubling interval while the server
    // answers each copy with an acknowledgement and has nothing to acknowledge.
    // Both ends then back off through a connection neither can leave.
    //
    // No congestion response: a PTO is not a congestion signal (RFC 9002 §7.6),
    // so the octets leave flight without the window being reduced.
    void on_pto_probe(PacketNumberSpace space, AckOutcome& outcome) {
        Space& s = m_spaces[space_index(space)];
        for (auto it = s.sent.begin(); it != s.sent.end(); ++it) {
            if (!it->second.in_flight || !it->second.ack_eliciting) continue;
            m_cc.remove_from_flight(it->second.sent_bytes);
            outcome.lost.push_back(std::move(it->second));
            s.sent.erase(it);
            refresh_ack_eliciting(space);
            return;
        }
    }

    void on_pto_fired() noexcept { ++m_pto_count; }
    [[nodiscard]] std::size_t pto_count() const noexcept { return m_pto_count; }
    void reset_pto_count() noexcept { m_pto_count = 0; }

    [[nodiscard]] bool has_ack_eliciting_in_flight() const noexcept {
        for (const Space& s : m_spaces) {
            if (s.has_ack_eliciting_in_flight) return true;
        }
        return false;
    }

    [[nodiscard]] bool has_ack_eliciting_in_flight(PacketNumberSpace space) const noexcept {
        return m_spaces[space_index(space)].has_ack_eliciting_in_flight;
    }

    [[nodiscard]] std::uint64_t largest_acked(PacketNumberSpace space) const noexcept {
        return m_spaces[space_index(space)].largest_acked;
    }

    [[nodiscard]] bool has_largest_acked(PacketNumberSpace space) const noexcept {
        return m_spaces[space_index(space)].has_largest_acked;
    }

    [[nodiscard]] std::size_t outstanding(PacketNumberSpace space) const noexcept {
        return m_spaces[space_index(space)].sent.size();
    }

    // Every unacknowledged packet in a space, oldest first.
    [[nodiscard]] const std::map<std::uint64_t, SentPacket>& sent(PacketNumberSpace space) const noexcept {
        return m_spaces[space_index(space)].sent;
    }

    // Drop a packet number space. Packets in it can never be acknowledged once
    // the keys are gone, so leaving them in flight would pin the congestion
    // window shut for the rest of the connection (RFC 9002 §A.11, §B.9).
    void discard_space(PacketNumberSpace space) noexcept {
        Space& s = m_spaces[space_index(space)];
        std::size_t bytes = 0;
        for (const auto& [pn, packet] : s.sent) {
            (void)pn;
            if (packet.in_flight) bytes += packet.sent_bytes;
        }
        m_cc.remove_from_flight(bytes);
        s = Space{};
        m_pto_count = 0;
    }

  private:
    struct Space {
        std::map<std::uint64_t, SentPacket> sent;  // ordered by packet number
        std::uint64_t largest_sent{0};
        std::uint64_t largest_acked{0};
        bool has_sent{false};
        bool has_largest_acked{false};
        Clock::time_point time_of_last_ack_eliciting{};
        Clock::time_point loss_time{};
        bool has_ack_eliciting_in_flight{false};
    };

    // A server's address is validated by the first protected packet it receives;
    // a client's is validated implicitly (RFC 9002 §A.8, §6.2.2.1).
    [[nodiscard]] bool peer_completed_address_validation() const noexcept {
        return m_address_validated || m_handshake_confirmed;
    }

    // Apply the congestion response to a set of lost packets. Separate from
    // detection so that a test — or a later change — cannot accidentally apply
    // the same loss twice.
    void apply_loss(const std::vector<SentPacket>& lost, Clock::time_point now) {
        if (lost.empty()) return;
        m_cc.on_packets_lost(lost, now);
        if (in_persistent_congestion(lost)) m_cc.on_persistent_congestion();
    }

    void refresh_ack_eliciting(PacketNumberSpace space) {
        Space& s = m_spaces[space_index(space)];
        s.has_ack_eliciting_in_flight = false;
        for (const auto& [pn, packet] : s.sent) {
            (void)pn;
            if (packet.in_flight && packet.ack_eliciting) {
                s.has_ack_eliciting_in_flight = true;
                break;
            }
        }
    }

    // RFC 9002 §A.10. Fills `outcome.lost` and sets the space's next loss time.
    void detect_lost(PacketNumberSpace space, Clock::time_point now, AckOutcome& outcome) {
        Space& s = m_spaces[space_index(space)];
        s.loss_time = Clock::time_point{};
        if (!s.has_largest_acked) return;

        // 9/8 of the larger of the two RTT estimates, floored at the timer
        // granularity: on a path with sub-millisecond RTTs a reordering window
        // shorter than a clock tick would declare everything lost.
        auto loss_delay = std::max(m_rtt.latest(), m_rtt.smoothed());
        loss_delay = loss_delay * 9 / 8;
        loss_delay = std::max(loss_delay, kGranularity);
        const Clock::time_point lost_send_time = now - loss_delay;

        for (auto it = s.sent.begin(); it != s.sent.end();) {
            const SentPacket& packet = it->second;
            if (packet.packet_number > s.largest_acked) break;  // not yet judged
            // Either older than the reordering window, or enough later packets
            // have been acknowledged that a reordered copy is implausible. The
            // packet threshold assumes no sender-induced gaps, which holds: every
            // packet we send takes the next number.
            if (packet.time_sent <= lost_send_time ||
                s.largest_acked >= packet.packet_number + kPacketThreshold) {
                outcome.lost.push_back(std::move(it->second));
                it = s.sent.erase(it);
            } else {
                const Clock::time_point candidate = packet.time_sent + loss_delay;
                if (s.loss_time == Clock::time_point{} || candidate < s.loss_time) s.loss_time = candidate;
                ++it;
            }
        }
    }

    // RFC 9002 §7.6.2. Persistent congestion is a period so long that it cannot
    // be reordering: every packet sent during it is gone. Recognising it is what
    // lets a connection recover from a path outage without waiting for the
    // window to creep back open one acknowledgement at a time.
    [[nodiscard]] bool in_persistent_congestion(const std::vector<SentPacket>& lost) const {
        if (!m_rtt.have_sample()) return false;

        const auto duration =
            (m_rtt.smoothed() + std::max(4 * m_rtt.rttvar(), kGranularity) + m_max_ack_delay) *
            std::int64_t{kPersistentCongestionThreshold};

        std::vector<const SentPacket*> candidates;
        candidates.reserve(lost.size());
        for (const SentPacket& packet : lost) {
            if (packet.ack_eliciting) candidates.push_back(&packet);
        }
        std::sort(candidates.begin(), candidates.end(), [](const SentPacket* a, const SentPacket* b) {
            return a->packet_number < b->packet_number;
        });

        // A run of *consecutive* packet numbers is a period with nothing
        // acknowledged inside it. A gap means a packet in between survived, which
        // ends the period by definition.
        std::size_t run_start = 0;
        for (std::size_t i = 1; i <= candidates.size(); ++i) {
            const bool consecutive = i < candidates.size() &&
                                     candidates[i]->packet_number == candidates[i - 1]->packet_number + 1;
            if (consecutive) continue;
            if (i - run_start >= 2 &&
                candidates[i - 1]->time_sent - candidates[run_start]->time_sent > duration) {
                return true;
            }
            run_start = i;
        }
        return false;
    }

    std::array<Space, kPacketNumberSpaceCount> m_spaces{};
    RttEstimator m_rtt;
    NewReno m_cc;
    std::chrono::microseconds m_max_ack_delay{25000};
    std::size_t m_pto_count{0};
    unsigned m_peer_ack_delay_exponent{3};
    bool m_handshake_confirmed{false};
    bool m_address_validated{false};
    bool m_handshake_keys_available{false};
};

}  // namespace simple_http::quic
