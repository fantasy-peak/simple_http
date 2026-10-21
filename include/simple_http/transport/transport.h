#pragma once

// Transport: the byte-stream abstraction protocol engines run on.
//
// A protocol engine (HTTP/1.x, HTTP/2, HTTP/3) is templated on a Transport and
// only performs async reads/writes of bytes, plus close/peer/tls_handle. It
// never names a concrete socket type. TCP (plain/TLS) satisfy this today; a
// QUIC transport will satisfy it later, so no engine code changes when HTTP/3
// lands.
//
// The Transport concept documents the required surface. Concrete transports are
// defined in tcp_transport.h / tls_transport.h.

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

#include "../core/types.h"

namespace simple_http {

namespace asio = boost::asio;

// Native TLS handle exposed to TLS-aware handlers (client-cert inspection etc).
// std::nullopt for plaintext transports.
using SslHandle = std::optional<SSL *>;

using ByteSpan = std::span<std::byte>;
using ConstByteSpan = std::span<const std::byte>;

// The result of an async I/O op: an error_code and the number of bytes moved.
using IoResult = std::pair<error_code, std::size_t>;

namespace detail {

// Most buffers one scatter-gather write accepts; see stream_write_seq.
inline constexpr std::size_t kMaxWriteBuffers = 8;

// The I/O primitives every stream-based transport needs. Plain sockets and
// asio::ssl::stream share the same composed-operation shapes; keeping the four
// bodies here means a change to error/partial-write handling cannot drift
// between TcpTransport and TlsTransport.
template <typename Stream> asio::awaitable<IoResult> stream_read_some(Stream &stream, ByteSpan buffer) {
    auto [ec, n] = co_await stream.async_read_some(asio::buffer(buffer.data(), buffer.size()),
                                                   asio::as_tuple(asio::use_awaitable));
    co_return IoResult{ec, n};
}

// Reads exactly `buffer.size()` bytes. Composed operation: it completes when
// the buffer is full, or with an error (EOF included) after a partial read -
// the result still reports the bytes read.
template <typename Stream> asio::awaitable<IoResult> stream_read_exact(Stream &stream, ByteSpan buffer) {
    auto [ec, n] = co_await asio::async_read(stream, asio::buffer(buffer.data(), buffer.size()),
                                             asio::as_tuple(asio::use_awaitable));
    co_return IoResult{ec, n};
}

template <typename Stream> asio::awaitable<IoResult> stream_write_all(Stream &stream, ConstByteSpan buffer) {
    auto [ec, n] = co_await asio::async_write(stream, asio::buffer(buffer.data(), buffer.size()),
                                              asio::as_tuple(asio::use_awaitable));
    co_return IoResult{ec, n};
}

// Writes several buffers as one operation (one writev / TLS record batch), so a
// frame header and its payload need not be concatenated first. Composed: it
// completes only once every byte has been written.
template <typename Stream>
asio::awaitable<IoResult> stream_write_seq(Stream &stream, std::span<const ConstByteSpan> buffers) {
    // Bounded by the iovec this builds. Callers pass a handful at most, so the
    // cap is a backstop rather than a real limit — dropping buffers past it
    // would write a *shorter* message and still report success, which is silent
    // truncation.
    assert(buffers.size() <= kMaxWriteBuffers);
    const std::size_t count = std::min<std::size_t>(buffers.size(), kMaxWriteBuffers);
    std::array<asio::const_buffer, kMaxWriteBuffers> bufs{};
    for (std::size_t i = 0; i < count; ++i)
        bufs[i] = asio::buffer(buffers[i].data(), buffers[i].size());
    auto [ec, n] = co_await asio::async_write(stream, std::span<const asio::const_buffer>{bufs.data(), count},
                                              asio::as_tuple(asio::use_awaitable));
    co_return IoResult{ec, n};
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

template <typename T>
concept TransportLike = requires(T t, ByteSpan mut, ConstByteSpan buf, std::span<const ConstByteSpan> seq) {
    // Read some bytes into `mut`; resolves with (ec, bytes_read).
    { t.async_read_some(mut) } -> std::same_as<asio::awaitable<IoResult>>;
    // Read exactly `mut.size()` bytes (composed); on error it reports how many
    // bytes were read, so a caller can still act on a partial read.
    { t.async_read(mut) } -> std::same_as<asio::awaitable<IoResult>>;
    // Write all of `buf`; resolves with (ec, bytes_written).
    { t.async_write(buf) } -> std::same_as<asio::awaitable<IoResult>>;
    // Write every buffer as one operation — a single writev where the platform
    // has one — so a head and its body, or a frame and its payload, need not be
    // concatenated into a scratch buffer first. Also composed: it completes only
    // once every byte has been written.
    { t.async_write_seq(seq) } -> std::same_as<asio::awaitable<IoResult>>;
    // The executor this transport (and its connection) is bound to.
    { t.get_executor() };
    // Remote peer endpoint (default-constructed for peerless transports).
    { t.peer() } -> std::convertible_to<asio::ip::tcp::endpoint>;
    // Native TLS handle, or nullopt for plaintext.
    { t.tls_handle() } -> std::convertible_to<SslHandle>;
    // Best-effort orderly shutdown + close.
    { t.close() } -> std::same_as<void>;
};

} // namespace simple_http
