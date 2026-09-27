#pragma once

// QUIC stream send and receive state (RFC 9000 §2 and §3).
//
// Pure state, deliberately: no asio, no channels, no coroutines. The transport
// that turns a stream into a byte stream (quic_stream_transport.h) owns all of
// that, and keeping it out of here means the reassembly, retransmission and
// flow-control logic can be exercised without an io_context.
//
// --- the send side ---
//
// A send stream doubles as its own retransmission buffer. Rather than copying
// the frames of each sent packet (so they can be replayed on loss), the stream
// keeps every byte from its base to the end unacknowledged, plus the set of
// ranges already acknowledged; a loss only has to name a byte range, and the
// next frame is built by reading it back out. Two consequences worth stating:
//
//   * the buffer cannot be trimmed below the lowest unacknowledged offset, which
//     is why the caller has to bound how far ahead it writes (the transport does,
//     with the same high/low watermark trick the h2 engine uses);
//   * a retransmission is not a copy of the original frame. It is the same bytes
//     at the same offsets, which is all the peer needs — the offset is what
//     identifies stream data, not the frame it arrived in.
//
// --- the receive side ---
//
// In-order delivery with a small out-of-order map. Data is handed to the reader
// only when it is contiguous from the read cursor, which is what lets the whole
// design be "one buffer plus a map of pieces that arrived early" instead of a
// windowed structure.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace simple_http::quic {

// Stream identifiers (RFC 9000 §2.1): the two low bits say who opened the stream
// and whether it is bidirectional. A server's streams have the low bit set; the
// bit above it means unidirectional.
inline constexpr std::uint64_t kStreamTypeBidi = 0x0;
inline constexpr std::uint64_t kStreamTypeUni = 0x2;
inline constexpr std::uint64_t kStreamIdInitiatorBit = 0x1;
inline constexpr std::uint64_t kStreamIdDirectionBit = 0x2;
// The largest stream id the encoding allows (RFC 9000 §2.1).
inline constexpr std::uint64_t kMaxStreamId = (1ULL << 62) - 1;

inline bool stream_is_bidi(std::uint64_t id) noexcept { return (id & kStreamIdDirectionBit) == 0; }
inline bool stream_is_local(std::uint64_t id, bool server) noexcept {
    return (id & kStreamIdInitiatorBit) == (server ? 1u : 0u);
}
// The nth stream of a given kind, counting from zero.
inline std::uint64_t stream_index(std::uint64_t id) noexcept { return id >> 2; }
inline std::uint64_t first_stream_of(bool bidi, bool local, bool server) noexcept {
    return (local ? (server ? 1u : 0u) : (server ? 0u : 1u)) | (bidi ? 0u : 2u);
}
// How many streams of this kind the peer may open given a stream-id limit: the
// limit is a *count*, ids are every fourth number (RFC 9000 §4.6).
inline std::uint64_t stream_count_from_limit(std::uint64_t limit) noexcept { return limit; }

// A byte range, half-open: [offset, offset + length).
struct ByteRange {
    std::uint64_t offset{0};
    std::uint64_t length{0};
    [[nodiscard]] std::uint64_t end() const noexcept { return offset + length; }
};

// The send half of a stream.
class SendStream {
  public:
    // What the next STREAM frame should carry.
    struct Chunk {
        std::uint64_t offset{0};
        std::string_view data{};
        bool fin{false};
    };

    // No cap on how far this call may extend the stream — for the callers that
    // answer to no flow-control limit at all, which is every CRYPTO stream
    // (RFC 9000 §4.1 flow-controls stream data, not handshake data).
    static constexpr std::uint64_t kUnlimitedNewData = ~0ULL;

    // Queue bytes for delivery. Returns false if the stream is already finished
    // or reset — writing after either is a caller bug, not a peer error.
    bool write(std::string_view data, bool fin = false) {
        if (m_finished || m_reset) return false;
        if (!data.empty()) m_buf.append(data);
        if (fin) {
            m_finished = true;
            m_fin_offset = m_base + m_buf.size();
        }
        return true;
    }

    // The next range to put on the wire, bounded by `max_len` octets and by the
    // peer's flow-control limit (`limit` is the highest offset the peer will
    // accept, exclusive).
    //
    // `new_data_limit` bounds how many *new* octets this call may add. It is a
    // separate bound because it answers a separate limit: stream flow control is
    // a high-water mark on one stream, connection flow control is the sum of the
    // high-water marks of all of them, and only the second is what the caller
    // passes here. A retransmission raises no high-water mark, so it is not
    // subject to it — and must not be, because sizing a retransmission to zero
    // would drop it from the lost list without sending it.
    //
    // The returned view points into this stream's buffer and stays valid until
    // the next write() — the caller writes it into a packet and does not write
    // to the stream in between.
    bool next(std::size_t max_len, std::uint64_t limit, std::uint64_t new_data_limit, Chunk& out) {
        if (m_reset) return false;
        const std::uint64_t window_end = std::min<std::uint64_t>(limit, kMaxOffset);

        // Retransmissions first. The peer is missing a hole, and filling it
        // unblocks everything queued behind it; sending new data instead would
        // only widen the gap.
        while (!m_lost.empty()) {
            const ByteRange range = m_lost.front();
            // A range can be stale: acknowledged after it was declared lost, or
            // already trimmed off the front of the buffer.
            if (range.offset + range.length <= m_base || is_acked(range.offset, range.length)) {
                m_lost.erase(m_lost.begin());
                continue;
            }
            if (range.length == 0) {
                // A zero-length range is the FIN, re-sent as an empty frame at
                // the same offset. `m_fin_sent` goes back up so the new-FIN
                // branch below cannot emit it a second time.
                m_lost.erase(m_lost.begin());
                if (range.offset == m_fin_offset) {
                    m_fin_sent = true;
                    out = Chunk{range.offset, {}, true};
                    return true;
                }
                continue;
            }
            if (range.offset >= window_end) return false;  // window is closed

            const std::uint64_t start = std::max(range.offset, m_base);
            const std::uint64_t avail = static_cast<std::uint64_t>(m_buf.size()) - (start - m_base);
            const std::uint64_t take = std::min({range.end() - start, window_end - start,
                                                 static_cast<std::uint64_t>(max_len), avail});
            if (take == 0) {
                m_lost.erase(m_lost.begin());
                continue;
            }
            // Trim rather than remove: the rest of the range still has to go.
            m_lost.front().offset = start + take;
            m_lost.front().length = range.end() - (start + take);
            if (m_lost.front().length == 0) m_lost.erase(m_lost.begin());
            out = Chunk{start, std::string_view{m_buf}.substr(start - m_base, take), false};
            return true;
        }

        // New data, if any is queued and the window allows it. A zero take here
        // is not a return: it means the connection's credit is spent, and the FIN
        // below costs no credit, so it has to be reachable in the same call.
        const std::uint64_t end = m_base + m_buf.size();
        if (m_next_new < end && m_next_new < window_end) {
            const std::uint64_t take = std::min({end - m_next_new, window_end - m_next_new,
                                                 static_cast<std::uint64_t>(max_len), new_data_limit});
            if (take != 0) {
                const std::uint64_t start = m_next_new;
                m_next_new += take;
                out = Chunk{start, std::string_view{m_buf}.substr(start - m_base, take), false};
                return true;
            }
        }

        // The end of the stream: an empty frame carrying FIN, sent once.
        if (m_finished && !m_fin_sent && m_next_new == m_fin_offset && m_fin_offset <= window_end) {
            m_fin_sent = true;
            out = Chunk{m_fin_offset, {}, true};
            return true;
        }
        return false;
    }

    // Whether there is anything at all left to put on the wire.
    [[nodiscard]] bool has_pending() const noexcept {
        if (m_reset) return false;
        if (!m_lost.empty()) return true;
        if (m_next_new < m_base + m_buf.size()) return true;
        return m_finished && !m_fin_sent && m_next_new == m_fin_offset;
    }

    // Everything written has been acknowledged; the stream can be forgotten once
    // the receive half is done too.
    [[nodiscard]] bool all_acked() const noexcept {
        return m_buf.empty() && (!m_finished || m_fin_acked);
    }

    void on_acked(std::uint64_t offset, std::uint64_t length) {
        if (length == 0) {
            // A zero-length ack is the FIN being acknowledged.
            if (m_finished && offset == m_fin_offset) m_fin_acked = true;
            return;
        }
        if (offset + length <= m_base) return;  // already trimmed
        m_acked[offset] = std::max(m_acked[offset], offset + length);
        // Merge adjacent ranges so the map stays as small as the ack pattern
        // really is.
        auto it = m_acked.find(offset);
        while (it != m_acked.end()) {
            auto next = std::next(it);
            if (next == m_acked.end() || next->first > it->second) break;
            it->second = std::max(it->second, next->second);
            m_acked.erase(next);
        }
        prune();
    }

    void on_lost(std::uint64_t offset, std::uint64_t length) {
        if (length == 0) {
            // The final frame is lost: re-arm it, unless the peer has already
            // acknowledged it (a late loss report for an early ack).
            if (m_finished && offset == m_fin_offset && !m_fin_acked) {
                m_fin_sent = false;
                insert_lost(ByteRange{m_fin_offset, 0});
            }
            return;
        }
        if (!is_acked(offset, length)) insert_lost(ByteRange{offset, length});
    }

    // Give up on the stream: nothing more is sent, and the peer is told why with
    // a RESET_STREAM frame.
    void reset(std::uint64_t error_code) {
        m_reset = true;
        m_reset_error = error_code;
        m_buf.clear();
        m_acked.clear();
        m_lost.clear();
        m_base = m_next_new;
    }

    [[nodiscard]] bool reset_sent() const noexcept { return m_reset; }
    [[nodiscard]] std::uint64_t reset_error() const noexcept { return m_reset_error; }
    [[nodiscard]] std::uint64_t next_offset() const noexcept { return m_next_new; }
    [[nodiscard]] std::uint64_t fin_offset() const noexcept { return m_fin_offset; }
    [[nodiscard]] bool finished() const noexcept { return m_finished; }

    // Whether the application has ever put anything on this stream — data or a
    // FIN. A reset is not "writing": it ends the send direction, and a stream it
    // has ended has nothing left to add.
    [[nodiscard]] bool ever_written() const noexcept { return m_next_new != 0 || m_finished; }

    // Whether the send direction is done with everything it will ever send.
    //
    // This is what retirement has to ask, and `all_acked()` cannot answer it: an
    // empty buffer means "nothing outstanding right now", which is also the state
    // of a stream that has written one chunk and is about to write the next. The
    // three ways to be done are the three ways a stream has no more to send — a
    // FIN that has been acknowledged, a reset (after which `write` is refused),
    // or nothing ever written, which is the ordinary state of a peer's
    // unidirectional stream.
    [[nodiscard]] bool send_complete() const noexcept {
        if (!m_buf.empty()) return false;
        if (m_reset) return true;
        if (m_finished) return m_fin_acked;
        return !ever_written();
    }
    // Bytes queued but not yet acknowledged. The transport bounds this; the
    // stream cannot shed them until the peer confirms receipt.
    [[nodiscard]] std::size_t buffered() const noexcept { return m_buf.size(); }

  private:
    // The largest stream offset the wire format allows (RFC 9000 §4.5).
    static constexpr std::uint64_t kMaxOffset = (1ULL << 62) - 1;

    [[nodiscard]] bool is_acked(std::uint64_t offset, std::uint64_t length) const {
        if (length == 0) return false;
        // The ranges are merged and sorted, so the covering range, if any, is
        // the last one starting at or before `offset`.
        auto it = m_acked.upper_bound(offset);
        if (it == m_acked.begin()) return false;
        --it;
        return it->second >= offset + length;
    }

    void insert_lost(ByteRange range) {
        // Keep the list sorted and merged: next() takes from the front, and a
        // fragmented list would re-send the same bytes once per fragment.
        auto it = std::lower_bound(m_lost.begin(), m_lost.end(), range,
                                   [](const ByteRange& a, const ByteRange& b) { return a.offset < b.offset; });
        it = m_lost.insert(it, range);
        // Merge forward.
        while (std::next(it) != m_lost.end() && std::next(it)->offset <= it->end()) {
            it->length = std::max(it->end(), std::next(it)->end()) - it->offset;
            m_lost.erase(std::next(it));
        }
        // Merge backward.
        if (it != m_lost.begin()) {
            auto prev = std::prev(it);
            if (prev->end() >= it->offset) {
                prev->length = std::max(prev->end(), it->end()) - prev->offset;
                m_lost.erase(it);
            }
        }
    }

    // Drop the acknowledged prefix: those bytes can never be needed again. The
    // ranges are merged, so the covered prefix is a single range starting
    // exactly at m_base — if there is one at all.
    void prune() {
        auto it = m_acked.find(m_base);
        if (it == m_acked.end()) return;
        const std::uint64_t advance = it->second - m_base;
        m_acked.erase(it);
        m_buf.erase(0, static_cast<std::size_t>(advance));
        m_base += advance;
    }

    std::string m_buf;   // unacknowledged bytes from m_base
    std::map<std::uint64_t, std::uint64_t> m_acked;  // offset -> end, merged
    std::vector<ByteRange> m_lost;                   // sorted, merged
    std::uint64_t m_base{0};
    std::uint64_t m_next_new{0};
    std::uint64_t m_fin_offset{0};
    std::uint64_t m_reset_error{0};
    bool m_finished{false};
    bool m_fin_sent{false};
    bool m_fin_acked{false};
    bool m_reset{false};
};

// The receive half of a stream.
class RecvStream {
  public:
    // Accept a STREAM frame. Returns false on a protocol violation the caller
    // must close the connection for: data past the final size, or a FIN that
    // contradicts a size already established (RFC 9000 §4.5).
    //
    // Flow control is *not* checked here. Exceeding it is a different error for
    // a different owner — the connection knows both the stream limit and the
    // connection limit, and the two produce different error codes — so the
    // caller checks `end > max_data()` before calling.
    bool push(std::uint64_t offset, std::string_view data, bool fin) {
        // A reset stream rejects everything. A *finished* one does not: the peer
        // may legally retransmit below the final size, and that has to be
        // absorbed rather than answered with a connection error.
        if (m_reset) return false;
        const std::uint64_t end = offset + data.size();

        if (fin) {
            if (m_have_final_size && m_final_size != end) return false;
            if (end < m_highest_offset) return false;  // data already beyond the FIN
            m_have_final_size = true;
            m_final_size = end;
        }
        if (m_have_final_size && end > m_final_size) return false;
        if (end > m_highest_offset) m_highest_offset = end;

        if (data.empty()) {
            if (fin) m_fin_received = true;
            return true;
        }

        if (offset <= m_base + m_buf.size()) {
            // Overlaps or follows what we have: append the part that is new.
            const std::uint64_t have = m_base + m_buf.size();
            if (end > have) {
                const std::uint64_t skip = have > offset ? have - offset : 0;
                m_buf.append(data.substr(static_cast<std::size_t>(skip)));
            }
            absorb_pending();
        } else {
            // A hole: hold it until the gap is filled.
            insert_pending(offset, data);
        }
        if (fin) m_fin_received = true;
        return true;
    }

    // Move up to `max` octets of contiguous data into `out` and advance the
    // cursor. Returns how many were delivered.
    std::size_t drain(std::string& out, std::size_t max = static_cast<std::size_t>(-1)) {
        const std::size_t n = std::min(m_buf.size() - m_read, max);
        if (n == 0) return 0;
        out.append(m_buf, m_read, n);
        m_read += n;
        release_drained();
        return n;
    }

    // The same, into a caller's byte buffer. A transport has a `std::span<std::byte>`
    // to fill, not a string to append to, and routing it through one would mean a
    // copy into the string and a second one out of it.
    std::size_t drain_into(std::span<std::byte> out) {
        const std::size_t n = std::min(m_buf.size() - m_read, out.size());
        if (n == 0) return 0;
        std::ranges::transform(m_buf.substr(m_read, n), out.begin(), [](char c) {
            return static_cast<std::byte>(static_cast<unsigned char>(c));
        });
        m_read += n;
        release_drained();
        return n;
    }

    // Bytes contiguous from the read cursor and not yet delivered.
    [[nodiscard]] std::size_t readable() const noexcept { return m_buf.size() - m_read; }
    // The end of the stream has been reached and everything before it delivered.
    [[nodiscard]] bool finished() const noexcept {
        return m_fin_received && m_base + m_buf.size() == m_final_size && m_read == m_buf.size();
    }
    [[nodiscard]] bool fin_received() const noexcept { return m_fin_received; }

    void reset(std::uint64_t error_code) {
        m_reset = true;
        m_reset_error = error_code;
        m_buf.clear();
        m_pending.clear();
        m_read = 0;
    }
    [[nodiscard]] bool reset_received() const noexcept { return m_reset; }
    [[nodiscard]] std::uint64_t reset_error() const noexcept { return m_reset_error; }

    // Flow control (RFC 9000 §4.1). `m_max_data` is the absolute limit we
    // advertised; the caller raises it with MAX_STREAM_DATA as the handler
    // consumes data, which is what couples the window to real consumption
    // instead of to arrival.
    [[nodiscard]] std::uint64_t highest_offset() const noexcept { return m_highest_offset; }
    [[nodiscard]] std::uint64_t max_data() const noexcept { return m_max_data; }
    void set_max_data(std::uint64_t limit) noexcept { m_max_data = limit; }
    // Total octets delivered to the reader, which is what the window is
    // replenished against.
    [[nodiscard]] std::uint64_t consumed() const noexcept { return m_base + m_read; }

    [[nodiscard]] bool empty() const noexcept { return m_pending.empty() && m_buf.empty(); }

  private:
    // Once the cursor reaches the end, the whole buffer has been delivered and
    // the base moves past *all* of it — not past what the last call delivered,
    // since earlier calls may have drained the front.
    void release_drained() {
        if (m_read != m_buf.size()) return;
        m_base += m_buf.size();
        m_buf.clear();
        m_read = 0;
    }

    void absorb_pending() {
        for (;;) {
            const std::uint64_t have = m_base + m_buf.size();

            // Pieces already covered by the buffer can go: a peer is entitled to
            // retransmit, and that copy is now redundant.
            for (auto it = m_pending.begin();
                 it != m_pending.end() && it->first + it->second.size() <= have;) {
                it = m_pending.erase(it);
            }

            // The piece that closes the gap is the one starting exactly at
            // `have`, or — when a piece was held that begins before the buffer's
            // end and reaches past it — the last one starting before `have`.
            // Anything starting strictly after `have` leaves a hole, and
            // reassembly stops there.
            auto it = m_pending.lower_bound(have);
            if (it != m_pending.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second.size() > have) it = prev;
            }
            if (it == m_pending.end() || it->first > have) return;

            m_buf.append(it->second, static_cast<std::size_t>(have - it->first), std::string::npos);
            m_pending.erase(it);
        }
    }

    void insert_pending(std::uint64_t offset, std::string_view data) {
        std::string piece{data};
        // Trim overlap with what the map already holds.
        auto it = m_pending.lower_bound(offset);
        if (it != m_pending.begin()) {
            auto prev = std::prev(it);
            const std::uint64_t prev_end = prev->first + prev->second.size();
            if (prev_end > offset) {
                if (prev_end >= offset + piece.size()) return;  // fully covered
                piece.erase(0, static_cast<std::size_t>(prev_end - offset));
                offset = prev_end;
            }
        }
        while (it != m_pending.end() && it->first <= offset + piece.size()) {
            if (it->first + it->second.size() > offset + piece.size()) {
                piece.append(it->second, static_cast<std::size_t>(offset + piece.size() - it->first),
                             std::string::npos);
            }
            it = m_pending.erase(it);
        }
        m_pending.emplace(offset, std::move(piece));
    }

    std::string m_buf;   // contiguous bytes from m_base, from m_read onward undelivered
    std::map<std::uint64_t, std::string> m_pending;  // out-of-order pieces
    std::uint64_t m_base{0};       // stream offset of m_buf[0]
    std::size_t m_read{0};         // read cursor within m_buf
    std::uint64_t m_highest_offset{0};
    std::uint64_t m_max_data{(1ULL << 60)};
    std::uint64_t m_final_size{0};
    std::uint64_t m_reset_error{0};
    bool m_have_final_size{false};
    bool m_fin_received{false};
    bool m_reset{false};
};

}  // namespace simple_http::quic
