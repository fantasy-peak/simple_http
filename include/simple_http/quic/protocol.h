#pragma once

// The narrow seam between the QUIC connection and the protocol engine that
// rides on it.
//
// This replaces the byte-pipe seam the hand-written stack used
// (`QuicStreamTransport`, which presented one QUIC stream as a Transport the
// HTTP/3 engine read and wrote like a socket). That seam cannot survive a move
// to nghttp3: nghttp3 does not read and write byte streams, it *asks* for the
// next slice of a response and hands back the bytes it decoded, so the
// connection has to pump it rather than serve it.
//
// The two directions, and why each lives where it does:
//
//   * **Up** (this interface): QUIC events, all forwarded from ngtcp2
//     callbacks, plus `next_stream_data()` — the pull that lets the connection
//     fill a packet without knowing what a HEADERS frame is. Every method is
//     `noexcept` and must not block: they run inside an ngtcp2 callback, where
//     re-entering ngtcp2 is undefined behaviour.
//
//   * **Down**: the engine holds its `QuicConnection` and calls the handful of
//     narrow methods it needs (extend a flow-control window, reset a stream,
//     open a unidirectional stream). Those are the connection's own vocabulary,
//     not ngtcp2's — which is what keeps ngtcp2 headers out of `engine/h3/`.
//
// Deliberately absent: any ngtcp2 or nghttp3 type. `ByteVec` exists so the
// engine can hand the connection a scatter list without including
// `<ngtcp2/ngtcp2.h>`; the connection translates it to `ngtcp2_vec`, which is
// layout-compatible but a different type with a non-const base pointer.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace simple_http::quic {

// Set in `on_stream_data`'s flags when the slice ends the stream. This layer's
// own bit rather than ngtcp2's, so the engine needs no ngtcp2 header; the
// connection translates on the way in.
inline constexpr std::uint32_t kStreamDataFin = 0x01;

// A piece of a scatter list. `base` is borrowed: the bytes must stay alive
// until the connection reports them acknowledged (or the stream is gone),
// because QUIC retransmits from the same memory.
struct ByteVec {
    const std::uint8_t *base{nullptr};
    std::size_t len{0};
};

// What the connection should try to put on the wire next.
struct StreamData {
    // The stream to write, or negative for "nothing to send".
    std::int64_t stream_id{-1};
    // Non-zero when this slice ends the stream (the FIN rides with it).
    int fin{0};
    // The bytes. Empty with a negative `stream_id` is the common case.
    std::span<const ByteVec> vec{};
    // Non-zero: the engine hit a connection-level error and the connection must
    // be closed with this HTTP/3 error code. Returning the code here rather
    // than throwing keeps the pull path allocation- and exception-free, which
    // matters because it runs inside ngtcp2's write loop.
    std::uint64_t error{0};
};

class Protocol {
  public:
    Protocol() = default;
    virtual ~Protocol() = default;
    Protocol(const Protocol &) = delete;
    Protocol &operator=(const Protocol &) = delete;

    // --- write side: driven by the connection's send loop -------------------

    // The next slice to send, or a `stream_id` below zero for "idle". Called
    // before every packet; must not block, and must not call back into ngtcp2.
    virtual StreamData next_stream_data() noexcept = 0;

    // ngtcp2 accepted `datalen` of the last slice. The engine must advance its
    // send cursor by exactly this much — missing a call re-sends the same bytes
    // forever, calling early drops them.
    virtual void on_stream_data_written(std::int64_t stream_id, std::size_t datalen) noexcept = 0;

    // ngtcp2 refused the slice because the peer's stream flow-control window is
    // full. The engine should stop offering this stream until the window opens
    // (the connection will call `on_extend_max_stream_data`).
    virtual void on_stream_blocked(std::int64_t stream_id) noexcept = 0;

    // The send half is shut: the peer reset the stream, or we did. The engine
    // stops producing for it.
    virtual void on_stream_shut_wr(std::int64_t stream_id) noexcept = 0;

    // --- read side and events: forwarded from ngtcp2 callbacks --------------

    // STREAM frame payload. `fin` is carried in `flags` as `kStreamDataFin`,
    // which the engine masks itself.
    virtual void on_stream_data(std::uint32_t flags, std::int64_t stream_id,
                                std::span<const std::uint8_t> data) noexcept = 0;

    // The peer acknowledged `datalen` bytes of this stream's send half. This is
    // what lets the engine release the buffer it lent to the connection, and
    // what unparks a producer waiting on backpressure.
    virtual void on_acked_stream_data(std::int64_t stream_id, std::uint64_t datalen) noexcept = 0;

    // The stream is finished on both sides. Either error code is absent when
    // that direction ended without an application error.
    virtual void on_stream_close(std::int64_t stream_id, std::optional<std::uint64_t> rx_error,
                                 std::optional<std::uint64_t> tx_error) noexcept = 0;

    // The peer sent RESET_STREAM.
    virtual void on_stream_reset(std::int64_t stream_id, std::uint64_t app_error_code) noexcept = 0;

    // The peer sent STOP_SENDING: it is no longer interested in what we send.
    virtual void on_stream_stop_sending(std::int64_t stream_id, std::uint64_t app_error_code) noexcept = 0;

    // The peer raised this stream's flow-control limit.
    virtual void on_extend_max_stream_data(std::int64_t stream_id, std::uint64_t max_data) noexcept = 0;

    // The peer raised the number of bidirectional streams we may open.
    virtual void on_extend_max_remote_streams_bidi(std::uint64_t max_streams) noexcept = 0;

    // The 1-RTT transmit keys are installed. This is the first moment the
    // engine may open its unidirectional streams: nghttp3's control and QPACK
    // streams have to be sent under application keys, and ngtcp2 has not handed
    // out the stream credit for them before this point. Returning normally is
    // the only option — a failure here means the connection dies, which the
    // engine does by closing it directly.
    virtual void on_tx_keys_ready() noexcept = 0;

    // The connection is finished and nothing will feed the engine again. Called
    // exactly once, from the connection's own teardown, so that a parked
    // handler coroutine observes the end instead of waiting for a stream that
    // will never advance.
    virtual void on_connection_closed() noexcept = 0;
};

} // namespace simple_http::quic
