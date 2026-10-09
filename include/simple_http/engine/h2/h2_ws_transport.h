#pragma once

// HTTP/2 WebSocket stream transport (RFC 8441).
//
// An extended CONNECT stream (`:method = CONNECT`, `:protocol = websocket`) is
// a bidirectional byte pipe carried in DATA frames on one HTTP/2 stream. The
// WebSocket frame codec is protocol-agnostic and lives behind the `Transport`
// seam (see proto/websocket.h), so rather than duplicate it for HTTP/2 the
// engine hands `WsBackendImpl` a transport backed by that single stream.
//
// `H2WsStream` is the engine-side half: the engine implements it per stream and
// performs the DATA framing, flow control and read wake-ups. `Http2WsTransport`
// is the concrete Transport-like adapter WsBackendImpl is templated on.
//
// The real transport was called out in the RFC 8441 gap; keeping this adapter a
// plain byte stream means masking, fragmentation, Ping/Pong, Close ordering and
// permessage-deflate all keep working unchanged over HTTP/2.

#include <boost/asio.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "../../transport/transport.h" // IoResult, ConstByteSpan

namespace simple_http {

namespace asio = boost::asio;

// Engine-side view of one extended-CONNECT stream, implemented by the HTTP/2
// engine. All three operations run on the connection executor.
class H2WsStream {
  public:
    virtual ~H2WsStream() = default;

    // Reads up to out.size() inbound WebSocket bytes. Completes with
    // (eof/aborted, 0) once the peer ended its half or the stream is gone.
    virtual asio::awaitable<IoResult> read_some(std::span<std::byte> out) = 0;
    // Sends every buffer, concatenated, as outgoing WebSocket bytes, honoring
    // the peer's flow-control window.
    virtual asio::awaitable<IoResult> write_seq(std::span<const ConstByteSpan> bufs) = 0;
    // Ends the stream (END_STREAM) and wakes any parked reader. Idempotent; does
    // not touch the rest of the HTTP/2 connection.
    virtual void close() = 0;
    virtual asio::any_io_executor get_executor() = 0;
};

// A Transport-like byte stream over one HTTP/2 stream, handed to WsBackendImpl.
// It only needs the surface WsBackendImpl actually calls: get_executor,
// async_read_some, async_write_seq and close.
class Http2WsTransport {
  public:
    // An inbound EOF here is the client's END_STREAM (half-close): the outbound
    // direction stays open for the handler's reply. See websocket.h.
    static constexpr bool kReadEofIsHalfClose = true;

    explicit Http2WsTransport(std::shared_ptr<H2WsStream> stream)
        : m_stream(std::move(stream)), m_executor(m_stream->get_executor()) {}

    [[nodiscard]] asio::any_io_executor get_executor() const { return m_executor; }

    asio::awaitable<IoResult> async_read_some(std::span<std::byte> out) { co_return co_await m_stream->read_some(out); }

    template <typename Buffers> asio::awaitable<IoResult> async_write_seq(const Buffers &bufs) {
        co_return co_await m_stream->write_seq(std::span<const ConstByteSpan>{bufs});
    }

    void close() { m_stream->close(); }

  private:
    std::shared_ptr<H2WsStream> m_stream;
    asio::any_io_executor m_executor;
};

} // namespace simple_http
