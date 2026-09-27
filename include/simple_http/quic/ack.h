#pragma once

// The receive side of acknowledgement: what has been received, in the range
// form an ACK frame needs.
//
// Ranges are kept in descending order with the highest first, which is exactly
// the order the wire format wants them in — so encoding is a walk, not a sort.
// Recording a packet is the only mutation and it can only ever touch the two
// ranges on either side of the insertion point: a received packet either lands
// inside an existing range (a duplicate), extends one, or bridges two. That
// locality is why a vector beats a tree here; the ranges stay few because a
// well-behaved peer sends them in order.
//
// Keeping every range forever is not an option — a long-lived connection would
// accumulate one per gap and never shed them, and the ACK frame would grow
// without bound. Two things bound it: `drop_below`, which the connection calls
// once its *own* ACK has been acknowledged (at which point the peer has the
// information and repeating it is pointless), and the range cap the connection
// passes when it encodes.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "frame.h"

namespace simple_http::quic {

// How many ranges a single ACK frame carries. RFC 9000 §13.2.4 leaves the cap to
// the implementation; the oldest ranges beyond it are simply not acknowledged,
// which is legal — an ACK frame is a hint, and losing one costs a retransmission
// of data we already have.
inline constexpr std::size_t kMaxAckRanges = 32;

class AckTracker {
  public:
    using Clock = std::chrono::steady_clock;

    // Record that packet number `pn` arrived at `now`. Returns false when it was
    // already covered — a duplicate, or a late arrival inside a range we have.
    bool record(std::uint64_t pn, Clock::time_point now) {
        // Find the first range whose bottom is at or below `pn`: the one that
        // can contain it, or (failing that) the one just below it.
        std::size_t i = 0;
        while (i < m_ranges.size() && m_ranges[i].smallest > pn) ++i;
        if (i < m_ranges.size() && m_ranges[i].largest >= pn) return false;
        return insert_at(i, pn, now);
    }

    [[nodiscard]] bool empty() const noexcept { return m_ranges.empty(); }
    [[nodiscard]] bool has_any() const noexcept { return m_has_any; }
    [[nodiscard]] std::uint64_t largest() const noexcept { return m_largest; }
    [[nodiscard]] Clock::time_point largest_time() const noexcept { return m_largest_time; }

    // The ranges an ACK frame should carry, most recent first, capped at `max`.
    [[nodiscard]] std::vector<AckRange> ranges(std::size_t max = kMaxAckRanges) const {
        const std::size_t count = m_ranges.size() < max ? m_ranges.size() : max;
        return {m_ranges.begin(), m_ranges.begin() + static_cast<std::ptrdiff_t>(count)};
    }

    // Forget every range that lies strictly below `pn`. Called when the peer
    // acknowledges a packet that carried our ACK: it has that information, so
    // repeating it would only keep the frame growing.
    //
    // "Below" is the operative word, and the ranges are descending — so the ones
    // to forget are at the *back*, and the newest is never dropped. Dropping from
    // the front instead, which is what this did, discards the recent ranges and
    // keeps the ancient ones; then `ranges()[0].largest` no longer matches
    // `largest()`, and an ACK frame built from the two disagrees with itself:
    // RFC 9000 §19.3.1 puts the first range immediately below the Largest
    // Acknowledged field, so the peer decodes a range it never sent and answers
    // with PROTOCOL_VIOLATION. Whether that happens depends on whether a drop has
    // landed before the next ACK is built, which is what made it intermittent.
    void drop_below(std::uint64_t pn) {
        while (!m_ranges.empty() && m_ranges.back().largest < pn) {
            m_ranges.pop_back();
        }
    }

    // Number of disjoint ranges currently tracked. For tests and diagnostics.
    [[nodiscard]] std::size_t range_count() const noexcept { return m_ranges.size(); }

  private:
    bool insert_at(std::size_t i, std::uint64_t pn, Clock::time_point now) {
        // The two neighbours the new packet can join: the range below it (index
        // i) and the range above it (index i - 1). A packet can bridge both at
        // once, which is the only case that removes a range.
        const bool joins_below = i < m_ranges.size() && m_ranges[i].largest + 1 == pn;
        const bool joins_above = i > 0 && m_ranges[i - 1].smallest == pn + 1;

        if (joins_below && joins_above) {
            m_ranges[i].largest = m_ranges[i - 1].largest;
            m_ranges.erase(m_ranges.begin() + static_cast<std::ptrdiff_t>(i) - 1);
        } else if (joins_below) {
            m_ranges[i].largest = pn;
        } else if (joins_above) {
            m_ranges[i - 1].smallest = pn;
        } else {
            m_ranges.insert(m_ranges.begin() + static_cast<std::ptrdiff_t>(i), AckRange{pn, pn});
        }

        // The ACK frame's largest_acked and its delay are those of the highest
        // packet ever received, not of the most recent one: a reordered packet
        // arriving late must not move the delay backwards.
        if (!m_has_any || pn > m_largest) {
            m_largest = pn;
            m_largest_time = now;
        }
        m_has_any = true;
        return true;
    }

    std::vector<AckRange> m_ranges;  // descending: m_ranges[0] is the highest
    std::uint64_t m_largest{0};
    Clock::time_point m_largest_time{};
    bool m_has_any{false};
};

}  // namespace simple_http::quic
