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

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

#include <boost/asio.hpp>
#include <openssl/ssl.h>

#include "../core/types.h"

namespace simple_http {

namespace asio = boost::asio;

// Native TLS handle exposed to TLS-aware handlers (client-cert inspection etc).
// std::nullopt for plaintext transports.
using SslHandle = std::optional<SSL*>;

using ByteSpan = std::span<std::byte>;
using ConstByteSpan = std::span<const std::byte>;

// The result of an async I/O op: an error_code and the number of bytes moved.
using IoResult = std::pair<error_code, std::size_t>;

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

}  // namespace simple_http
