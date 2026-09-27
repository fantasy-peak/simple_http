#pragma once

// One QUIC stream, presented as the byte-stream transport the protocol engines
// are written against.
//
// This is the piece that lets the HTTP/3 engine reuse the shape of the h1 and h2
// engines unchanged: it satisfies `TransportLike` (transport.h), so an engine
// that only knows how to `async_read_some` and `async_write` runs on it exactly
// as it runs on a socket. The multiplexing, framing and flow control all live
// below, in the connection.
//
// Two deliberate departures from a socket, both of which the engine has to be
// written knowing about:
//
//   * `async_write` completing means the bytes are *queued*, not that the peer
//     has them. QUIC retransmits on its own, so a writer never waits for an
//     acknowledgement — it waits for room, which is the write_space channel.
//   * `close()` is a stream reset, not a connection close. A cancelled request
//     must not take the other streams with it, which is the whole point of
//     HTTP/3 running over QUIC.
//
// The error codes are the ones the engines already translate: `eof` for a clean
// FIN, `connection_reset` for a RESET_STREAM, `operation_aborted` for a
// connection that went away underneath.

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include <boost/asio.hpp>

#include "../core/types.h"
#include "../transport/transport.h"  // SslHandle, ByteSpan, ConstByteSpan, IoResult
#include "stream_state.h"
#include "wire.h"

namespace simple_http::quic {

// Outbound backpressure watermarks, matching the h2 engine's: park the producer
// at the high mark, release it at the low one. The buffer cannot be shed until
// the peer acknowledges, so this bounds per-stream memory to roughly the high
// mark no matter how large the response is.
inline constexpr std::size_t kStreamOutHighWatermark = 1u << 20;   // 1 MiB
inline constexpr std::size_t kStreamOutLowWatermark = 256u << 10;  // 256 KiB

template <typename Executor>
class QuicStreamTransport {
  public:
    // The connection's side of the contract: how to wake its send loop, how to
    // hear that the application consumed bytes (so the window can be raised),
    // and how to hear that the stream was abandoned.
    struct Hooks {
        std::function<void()> wake_send;
        std::function<void(std::uint64_t consumed)> on_consumed;
        std::function<void(std::uint64_t error_code)> on_reset;
    };

    QuicStreamTransport(std::shared_ptr<QuicStreamState> state, Executor exec, asio::ip::tcp::endpoint peer,
                        SslHandle ssl, Hooks hooks)
        : m_state(std::move(state)), m_executor(std::move(exec)), m_peer(std::move(peer)), m_ssl(ssl),
          m_hooks(std::move(hooks)) {}

    using IoResult = std::pair<error_code, std::size_t>;

    // Read whatever is available. `async_read_some` is the engine-facing
    // primitive; a zero-length result with no error cannot happen, because
    // `eof` is what an empty stream reports.
    asio::awaitable<IoResult> async_read_some(ByteSpan buffer) {
        for (;;) {
            if (m_state->recv.readable() > 0) {
                const std::size_t n = m_state->recv.drain_into(buffer);
                if (m_hooks.on_consumed) m_hooks.on_consumed(n);
                co_return IoResult{error_code{}, n};
            }
            // Terminal states are checked after the data, never before: a FIN
            // that arrives in the same datagram as the last bytes must not
            // discard them.
            if (m_state->recv.reset_received()) {
                co_return IoResult{make_error_code(asio::error::connection_reset), 0};
            }
            if (m_state->recv.finished()) {
                co_return IoResult{make_error_code(asio::error::eof), 0};
            }
            auto [ec] = co_await m_state->notify->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                // The channel was closed: the connection is gone, so this stream
                // can only be reported as interrupted.
                co_return IoResult{make_error_code(asio::error::operation_aborted), 0};
            }
        }
    }

    // Fill `buffer` completely, or fail with however much was read — the same
    // contract as a socket's composed read, and for the same reason: a caller
    // that asked for N octets of a fixed-size header has to know that it did not
    // get them.
    asio::awaitable<IoResult> async_read(ByteSpan buffer) {
        std::size_t total = 0;
        while (total < buffer.size()) {
            auto [ec, n] = co_await async_read_some(buffer.subspan(total));
            total += n;
            if (ec) co_return IoResult{ec, total};
        }
        co_return IoResult{error_code{}, total};
    }

    asio::awaitable<IoResult> async_write(ConstByteSpan buffer) {
        co_return co_await write_bytes(std::string_view{reinterpret_cast<const char*>(buffer.data()),
                                                        buffer.size()});
    }

    asio::awaitable<IoResult> async_write_seq(std::span<const ConstByteSpan> buffers) {
        // A QUIC stream frame carries a byte range, so there is nothing to gain
        // from keeping the pieces apart the way a writev does: the peer sees one
        // stream either way. Concatenating is also what lets the whole thing go
        // in a single STREAM frame when it fits.
        std::string joined;
        std::size_t total = 0;
        for (const ConstByteSpan& b : buffers) total += b.size();
        joined.reserve(total);
        for (const ConstByteSpan& b : buffers) {
            joined.append(reinterpret_cast<const char*>(b.data()), b.size());
        }
        auto [ec, n] = co_await write_bytes(joined);
        (void)n;
        co_return IoResult{ec, ec ? 0 : total};
    }

    // Queue the end of the stream. Separate from close() because a clean finish
    // and an abandonment are different wire events: FIN versus RESET_STREAM.
    asio::awaitable<error_code> async_shutdown() {
        if (m_shutdown_sent || m_state->send.finished()) co_return error_code{};
        m_shutdown_sent = true;
        m_state->send.write({}, /*fin=*/true);
        if (m_hooks.wake_send) m_hooks.wake_send();
        co_return error_code{};
    }

    // Abandon the stream: the peer is told with RESET_STREAM, and the send half
    // forgets everything queued. Returns immediately; there is nothing to wait
    // for, because a reset is fire-and-forget by design.
    void close() {
        if (m_closed) return;
        m_closed = true;
        if (m_hooks.on_reset) {
            m_hooks.on_reset(0);
        } else {
            m_state->send.reset(0);
        }
        // Unpark anyone waiting on either side.
        if (m_state->notify) m_state->notify->close();
        if (m_state->write_space) m_state->write_space->close();
    }

    [[nodiscard]] Executor get_executor() const { return m_executor; }
    [[nodiscard]] asio::ip::tcp::endpoint peer() const { return m_peer; }
    [[nodiscard]] SslHandle tls_handle() const { return m_ssl; }

    // Not part of TransportLike, but what an HTTP/3 engine needs to know.
    [[nodiscard]] std::uint64_t stream_id() const noexcept { return m_state->id; }
    [[nodiscard]] bool fin_received() const noexcept { return m_state->recv.fin_received(); }
    [[nodiscard]] std::shared_ptr<QuicStreamState> state() const noexcept { return m_state; }

  private:
    asio::awaitable<IoResult> write_bytes(std::string_view data) {
        if (m_closed || m_state->send.reset_sent()) {
            co_return IoResult{make_error_code(asio::error::operation_aborted), 0};
        }
        // Park while the buffer is over the high mark. Checked before writing so
        // the buffer can overshoot by at most one write, and re-checked after
        // waking because the wake is only a hint.
        for (;;) {
            if (m_state->send.buffered() <= kStreamOutHighWatermark || data.empty()) break;
            if (!m_state->write_space) {
                m_state->write_space =
                    std::make_shared<asio::experimental::concurrent_channel<void(error_code)>>(m_executor, 1);
            }
            auto space = m_state->write_space;
            auto [ec] = co_await space->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) co_return IoResult{make_error_code(asio::error::operation_aborted), 0};
        }
        if (!m_state->send.write(data, /*fin=*/false)) {
            co_return IoResult{make_error_code(asio::error::operation_aborted), 0};
        }
        if (m_hooks.wake_send) m_hooks.wake_send();
        co_return IoResult{error_code{}, data.size()};
    }

    std::shared_ptr<QuicStreamState> m_state;
    Executor m_executor;
    asio::ip::tcp::endpoint m_peer;
    SslHandle m_ssl;
    Hooks m_hooks;
    bool m_shutdown_sent{false};
    bool m_closed{false};
};

}  // namespace simple_http::quic
