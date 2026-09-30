#pragma once

// One HTTP/3 request stream's state.
//
// The split between this and the old engine's StreamState is that the
// *protocol* half is gone: nghttp3 owns whether a field section is complete,
// whether the stream is blocked on QPACK, whether it is writable, and what the
// peer's flow-control limit is. What is left here is the two queues this layer
// is uniquely responsible for, and the wake-ups that connect them to the
// handler coroutine:
//
//   * **in_q** — body bytes nghttp3 decoded but the handler has not read. They
//     are credited to the peer only as the handler consumes them, so an
//     un-reading handler throttles the sender instead of buffering without
//     bound.
//
//   * **out_q** — body bytes the handler produced but QUIC has not
//   acknowledged.
//     nghttp3 hands slices out of this queue, and a slice must stay put until
//     the peer acknowledges it, because QUIC retransmits from the same memory.
//     That is why `out_ack` — not the queue length — is the watermark: bytes
//     handed to nghttp3 are not gone, they are in flight.
//
// Both wake-ups are capacity-1 coalescing channels, the idiom the rest of the
// library uses: a nudge that arrives when the waiter is already runnable is not
// information, so dropping it is correct.

#include <boost/asio/experimental/channel.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "../../core/types.h"
#include "../../proto/request.h"
#include "../../proto/response.h"

namespace simple_http::h3 {

namespace asio = boost::asio;

// A one-shot wake-up with a coalescing send. `try_send` posts rather than
// inlining the wake, which is what makes it safe to call from inside an ngtcp2
// or nghttp3 callback. Lock-free `channel` (not `concurrent_channel`): those
// callbacks run on the connection's executor (model A, one thread) and so do
// the consumers who await the signal, so no lock is needed.
using Channel = asio::experimental::channel<void(error_code)>;
using Signal = std::shared_ptr<Channel>;

inline void wake(const Signal &signal) noexcept {
    if (signal)
        (void)signal->try_send(error_code{});
}

// The largest slice of a response body handed to nghttp3 in one go. nghttp3
// puts whatever it is given into a single DATA frame, so this caps frame size —
// and it also bounds how much of the queue one write loop pass can visit.
inline constexpr std::size_t kMaxBodyPerRead = 32 * 1024;

// Outbound backpressure watermarks, matching the h2 engine's and the previous
// QUIC stack's. The measured quantity is *unacknowledged* bytes, so this bounds
// per-stream memory to roughly the high mark however fast the handler produces.
inline constexpr std::uint64_t kOutHighWatermark = 1u << 20;  // 1 MiB
inline constexpr std::uint64_t kOutLowWatermark = 256u << 10; // 256 KiB

struct H3Stream {
    std::int64_t id{-1};

    // --- request ------------------------------------------------------------
    std::shared_ptr<Request> request;
    std::shared_ptr<Response> response;
    // Collected by the header callbacks, applied in `finish_request()`: a real
    // Host field has to win over the one synthesised from :authority, and the
    // fields arrive in wire order, so which comes first is the peer's choice.
    std::string authority;
    std::string scheme;
    bool is_head{false};
    bool seen_authority{false};
    bool headers_done{false};
    bool dispatched{false};
    bool request_complete{false};
    bool reset_by_peer{false};

    // --- inbound body -------------------------------------------------------
    std::deque<std::string> in_q;
    std::size_t in_bytes{0};
    bool in_fin{false};
    Signal in_space;

    // --- outbound body ------------------------------------------------------
    std::deque<std::string> out_q;
    // The stream offset of `out_q.front()`'s first byte.
    std::uint64_t out_base{0};
    // Sum of the sizes in `out_q`; kept beside the queue so the "is everything
    // handed over" test does not walk it.
    std::uint64_t out_bytes{0};
    // How much of the body has been handed to nghttp3 (advanced by
    // `nghttp3_conn_add_write_offset`) and how much the peer has acknowledged.
    std::uint64_t out_write{0};
    std::uint64_t out_ack{0};
    bool out_eof{false};
    // nghttp3 parked this stream because its body was not ready; it has to be
    // resumed when it is, or the stream never sends again.
    bool data_blocked{false};
    Signal out_space;

    // --- response -----------------------------------------------------------
    bool response_started{false};
    bool end_stream_sent{false};

    [[nodiscard]] std::uint64_t out_end() const noexcept { return out_base + out_bytes; }
    [[nodiscard]] bool stream_ended() const noexcept { return request_complete && end_stream_sent; }
};

} // namespace simple_http::h3
