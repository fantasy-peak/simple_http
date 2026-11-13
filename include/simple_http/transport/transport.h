#pragma once

// Transport: the byte-stream abstraction protocol engines run on (C++17 port).
//
// A protocol engine (HTTP/1.x, HTTP/2, HTTP/3) is templated on a Transport and
// only performs async reads/writes of bytes, plus close/peer/tls_handle. It
// never names a concrete socket type.
//
// Every operation is an Asio initiating function with the conventional
// completion signature void(error_code, std::size_t). The helpers below wrap
// the plain asio operations on a stream; the concrete transports are in
// tcp_transport.h / tls_transport.h.

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "../core/compat.h"
#include "../core/types.h"

namespace simple_http {

namespace asio = boost::asio;

// Native TLS handle exposed to TLS-aware handlers (client-cert inspection etc).
// std::nullopt for plaintext transports.
using SslHandle = std::optional<SSL *>;

// The result of an async I/O op: an error_code and the number of bytes moved.
// On the wire this is the pair of completion arguments.
using IoResult = std::pair<error_code, std::size_t>;

namespace detail {

// Most buffers one scatter-gather write accepts; see async_stream_write_seq.
inline constexpr std::size_t kMaxWriteBuffers = 8;

// The I/O primitives every stream-based transport needs. Plain sockets and
// asio::ssl::stream share the same composed-operation shapes; keeping the four
// bodies here means a change to error/partial-write handling cannot drift
// between TcpTransport and TlsTransport.
template <typename Stream, typename Token> auto async_stream_read_some(Stream &stream, ByteSpan buffer, Token &&token) {
    return stream.async_read_some(asio::buffer(buffer.data(), buffer.size()), std::forward<Token>(token));
}

// Reads exactly `buffer.size()` bytes (composed). On error it still reports how
// many bytes were read, because asio::async_read's completion carries that.
template <typename Stream, typename Token>
auto async_stream_read_exact(Stream &stream, ByteSpan buffer, Token &&token) {
    return asio::async_read(stream, asio::buffer(buffer.data(), buffer.size()), std::forward<Token>(token));
}

template <typename Stream, typename Token>
auto async_stream_write_all(Stream &stream, ConstByteSpan buffer, Token &&token) {
    return asio::async_write(stream, asio::buffer(buffer.data(), buffer.size()), std::forward<Token>(token));
}

// Writes several buffers as one operation (one writev / TLS record batch), so a
// frame header and its payload need not be concatenated first.
template <typename Stream, typename Token>
auto async_stream_write_seq(Stream &stream, boost::span<const ConstByteSpan> buffers, Token &&token) {
    // Bounded by the iovec this builds. Callers pass a handful at most, so the
    // cap is a backstop rather than a real limit — dropping buffers past it
    // would write a *shorter* message and still report success, which is silent
    // truncation.
    assert(buffers.size() <= kMaxWriteBuffers);
    const std::size_t count = std::min<std::size_t>(buffers.size(), kMaxWriteBuffers);
    std::array<asio::const_buffer, kMaxWriteBuffers> bufs{};
    for (std::size_t i = 0; i < count; ++i)
        bufs[i] = asio::buffer(buffers[i].data(), buffers[i].size());
    // Pass the array itself, not a span over it: asio copies the *sequence
    // object* into the composed operation, and a span copy is only a view.
    // With a span, a write large enough to need a second async_write_some
    // would read the buffer descriptors out of the dead stack frame (observed
    // as a WebSocket echo stalling after exactly 128 KiB). The trailing
    // zero-length entries are valid buffers and are skipped by the
    // scatter-gather machinery.
    return asio::async_write(stream, bufs, std::forward<Token>(token));
}

// Adapts a void(error_code, size_t) I/O operation to a void(error_code)
// completion. Callers that do not care about the byte count (a write that
// already knows it must move every byte, a shutdown) use this rather than
// discarding the count in a lambda, which would strip the associated
// cancellation slot.
template <typename Start> struct ErrorOnlyOp {
    Start start;
    asio::any_io_executor ex;
    bool started{false};

    asio::any_io_executor get_executor() const noexcept { return ex; }

    template <typename Self> void operator()(Self &self, error_code ec = {}, std::size_t = 0) {
        if (!started) {
            started = true;
            // `start` owns whatever the inner operation was handed: the shared
            // string behind a response, and the framed prefix of a chunk. The
            // inner operation only receives raw buffer descriptors, so it must
            // keep that owner alive for as long as it is pending. Moving
            // `start` into a local and returning released it at initiation
            // time, which is a use-after-free the moment the first
            // async_write_some is partial (large body, slow reader, zero
            // window).
            //
            // Hand one copy to the completion handler (it only holds cheap
            // shared_ptr captures) and invoke the other. The handler's copy is
            // what keeps the buffers alive; no additional allocation is made
            // beyond the one `any_completion_handler` already performs.
            static_assert(std::is_copy_constructible_v<Start>, "async_error_only's Start must be copyable");
            auto action = start;
            auto inner = asio::any_completion_handler<void(error_code, std::size_t)>(
                [keep = std::move(start), self = std::move(self)](error_code inner_ec, std::size_t) mutable {
                    (void)keep;
                    self.complete(inner_ec);
                });
            action(std::move(inner));
            return;
        }
        self.complete(ec);
    }
};

template <typename Token, typename Start>
auto async_error_only(asio::any_io_executor ex, Token &&token, Start &&start) {
    // Two copies: the operation takes one, async_compose's I/O object gets the
    // other. Argument evaluation order is unspecified, so passing `ex` twice
    // while moving one of them can leave the I/O object empty.
    auto object = ex;
    return asio::async_compose<Token, void(error_code)>(
        ErrorOnlyOp<std::decay_t<Start>>{std::forward<Start>(start), std::move(ex)}, token, object);
}

} // namespace detail

// Orderly shutdown+close for any asio socket type (TCP, UNIX, or a
// ssl::stream's lowest_layer). Best-effort: both operations report errors only
// through their out-parameter, which the caller has no recovery for.
template <typename Socket> inline void shutdown_socket(Socket &s) {
    if (s.is_open()) {
        error_code ec;
        s.shutdown(Socket::shutdown_both, ec);
        s.close(ec);
    }
}

} // namespace simple_http
