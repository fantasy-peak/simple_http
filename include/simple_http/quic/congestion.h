#pragma once

// NewReno congestion control (RFC 9002 §7, pseudocode in Appendix B).
//
// The window is the only thing this class decides; what counts as in flight, and
// which packets are acked or lost, comes from recovery.h. Splitting them that way
// keeps this file's whole state — a window, a threshold, bytes in flight and the
// start of the current recovery period — readable at a glance, which matters
// because every one of those numbers is a place a subtle bug turns into a
// connection that stalls rather than one that misbehaves visibly.
//
// The recovery period is what stops one loss event from collapsing the window
// several times over: a packet sent before the period began is already accounted
// for, so its loss or its late acknowledgement must not move the window again
// (RFC 9002 §7.3.2).

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include "sent_packet.h"
#include "wire.h"

namespace simple_http::quic {

// RFC 9002 §7.2 / Appendix B.1.
inline constexpr double kLossReductionFactor = 0.5;
// §7.6: how many PTOs of silence constitute persistent congestion.
inline constexpr std::size_t kPersistentCongestionThreshold = 3;

// The largest datagram this implementation will send. QUIC's floor is 1200
// (§14.1 of the transport RFC) and probing for a larger path MTU is a separate
// feature; a fixed 1200 is correct everywhere and is what the initial window is
// computed from.
inline constexpr std::size_t kMaxDatagramSize = 1200;

class NewReno {
  public:
    using Clock = std::chrono::steady_clock;

    explicit NewReno(std::size_t max_datagram_size = kMaxDatagramSize)
        : m_max_datagram_size(max_datagram_size), m_cwnd(initial_window(max_datagram_size)),
          m_ssthresh(kInfiniteWindow) {}

    [[nodiscard]] std::size_t congestion_window() const noexcept { return m_cwnd; }
    [[nodiscard]] std::size_t bytes_in_flight() const noexcept { return m_bytes_in_flight; }
    [[nodiscard]] std::size_t max_datagram_size() const noexcept { return m_max_datagram_size; }

    // How many more bytes may be sent right now.
    [[nodiscard]] std::size_t send_allowance() const noexcept {
        return m_bytes_in_flight >= m_cwnd ? 0 : m_cwnd - m_bytes_in_flight;
    }

    [[nodiscard]] bool in_recovery(Clock::time_point sent_time) const noexcept {
        return sent_time <= m_recovery_start_time;
    }

    void on_packet_sent(std::size_t sent_bytes, bool in_flight) noexcept {
        if (in_flight) m_bytes_in_flight += sent_bytes;
    }

    // Packets the peer acknowledged. `app_limited` and `flow_control_limited`
    // suppress the window growth: a window that is already being under-used
    // cannot be the thing limiting throughput, and growing it on the strength of
    // acknowledgements that came back instantly would let it balloon to a size
    // the path never demonstrated it could carry (RFC 9002 §7.8).
    void on_packets_acked(const std::vector<SentPacket>& acked, std::chrono::steady_clock::time_point now,
                          bool limited) {
        (void)now;
        for (const SentPacket& packet : acked) {
            if (!packet.in_flight) continue;
            m_bytes_in_flight -= std::min(m_bytes_in_flight, packet.sent_bytes);
            if (limited) continue;
            if (in_recovery(packet.time_sent)) continue;
            if (m_cwnd < m_ssthresh) {
                m_cwnd += packet.sent_bytes;  // slow start
            } else {
                // Congestion avoidance: one window's worth of growth per RTT.
                m_cwnd += m_max_datagram_size * packet.sent_bytes / m_cwnd;
            }
        }
    }

    // Packets declared lost. A recovery period is entered once, from the send
    // time of the *last* lost packet, so a burst of losses from a single event
    // costs one halving rather than one per packet.
    void on_packets_lost(const std::vector<SentPacket>& lost, std::chrono::steady_clock::time_point now) {
        Clock::time_point last_loss{};
        bool any = false;
        for (const SentPacket& packet : lost) {
            if (!packet.in_flight) continue;
            m_bytes_in_flight -= std::min(m_bytes_in_flight, packet.sent_bytes);
            if (!any || packet.time_sent > last_loss) last_loss = packet.time_sent;
            any = true;
        }
        if (!any) return;
        on_congestion_event(last_loss, now);
    }

    // Unacknowledged packets in a discarded packet number space stop counting
    // (RFC 9002 §B.9): once the keys are gone they can never be acknowledged,
    // and leaving them in flight would pin the window shut forever.
    void remove_from_flight(std::size_t bytes) noexcept {
        m_bytes_in_flight -= std::min(m_bytes_in_flight, bytes);
    }

    void on_persistent_congestion() noexcept {
        m_cwnd = minimum_window(m_max_datagram_size);
        m_recovery_start_time = Clock::time_point{};
    }

    // RFC 9002 §7.2: the initial window is ten datagrams, floored so that a
    // connection is never slower than it would have been with a larger MTU.
    [[nodiscard]] static std::size_t initial_window(std::size_t max_datagram_size) noexcept {
        return std::min(10 * max_datagram_size, std::max<std::size_t>(14720, 2 * max_datagram_size));
    }

    [[nodiscard]] static std::size_t minimum_window(std::size_t max_datagram_size) noexcept {
        return 2 * max_datagram_size;
    }

  private:
    // Stands in for RFC 9002's "infinite" ssthresh. Not SIZE_MAX: the congestion
    // avoidance branch compares `m_cwnd < m_ssthresh` and then divides by
    // `m_cwnd`, and a threshold that no window can ever reach keeps slow start
    // in charge until the first loss.
    static constexpr std::size_t kInfiniteWindow = std::size_t{1} << 40;

    void on_congestion_event(Clock::time_point sent_time, Clock::time_point now) {
        if (in_recovery(sent_time)) return;
        m_recovery_start_time = now;
        m_ssthresh = static_cast<std::size_t>(static_cast<double>(m_cwnd) * kLossReductionFactor);
        m_cwnd = std::max(m_ssthresh, minimum_window(m_max_datagram_size));
    }

    std::size_t m_max_datagram_size;
    std::size_t m_cwnd;
    std::size_t m_ssthresh;
    std::size_t m_bytes_in_flight{0};
    Clock::time_point m_recovery_start_time{};
};

}  // namespace simple_http::quic
