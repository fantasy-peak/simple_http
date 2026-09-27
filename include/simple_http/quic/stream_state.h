#pragma once

// The per-stream state a QUIC connection holds, and the wake-up plumbing a
// reader on that stream needs.
//
// This is the seam between the connection (which owns the map of streams, feeds
// them from STREAM frames, and drains them into packets) and the transport that
// turns one stream into a byte stream for a protocol engine. It exists as its
// own header so neither side has to know the other: the connection holds
// `shared_ptr<QuicStreamState>`, the transport holds the same pointer, and the
// only things that cross between them are two callbacks and a channel.
//
// The channel is the same "capacity 1, coalescing try_send" idiom the HTTP/2
// engine uses for its write loop: a wake-up that arrives when the reader is
// already runnable is not information, so it may be dropped, and the reader
// re-checks its own state after every wake.

#include <cstdint>
#include <functional>
#include <memory>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "../core/types.h"
#include "stream.h"

namespace simple_http::quic {

namespace asio = boost::asio;

// One stream's shared state. Everything here is touched only on the connection's
// executor (concurrency model A), so there are no locks — the `shared_ptr` is
// for lifetime, not for sharing across threads.
struct QuicStreamState {
    std::uint64_t id{0};
    SendStream send;
    RecvStream recv;

    // Signals that the connection has something to put on the wire for this
    // stream. Set by the connection; called by the transport after queueing
    // bytes.
    std::function<void()> wake_send;
    // Signals that the receive half changed: data arrived, the FIN landed, or
    // the peer reset the stream. Closed when the connection is torn down, which
    // is what unparks a reader waiting on a stream that will never advance.
    std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> notify;

    // The same idiom for outbound backpressure: a producer parks here once the
    // send buffer is above the high watermark, and the connection wakes it when
    // acknowledgements have drained the buffer back to the low mark. Without it
    // a streaming response would accumulate in the send buffer as fast as the
    // handler could produce it, however slow the peer was.
    std::shared_ptr<asio::experimental::concurrent_channel<void(error_code)>> write_space;

    // How much of the received data the reader has consumed, so the connection
    // knows when to raise MAX_STREAM_DATA. Counts octets the application has
    // taken, not octets that arrived — crediting arrival would let a peer fill
    // the window with data nobody has read.
    std::uint64_t consumed{0};
    std::uint64_t consumed_credited{0};
    // Our receive window when the stream was created, which is the reference the
    // half-window replenishment threshold is measured against.
    std::uint64_t recv_start{0};
    // The highest offset the peer has sent on this stream. Tracked separately
    // from `recv.highest_offset()` because that one is also advanced by a
    // RESET_STREAM's final size, and connection-level flow control counts only
    // octets that actually arrived.
    std::uint64_t recv_highest{0};

    // The peer's flow-control limit for this stream's send half, in absolute
    // offsets. Raised by MAX_STREAM_DATA and by the peer's initial limits once
    // the handshake completes — whichever is later, which is why it is a max.
    std::uint64_t send_max_data{0};

    // The peer has finished sending (FIN) or has given up (RESET_STREAM).
    bool remote_closed{false};

    [[nodiscard]] bool readable() const noexcept { return recv.readable() > 0; }
};

}  // namespace simple_http::quic
