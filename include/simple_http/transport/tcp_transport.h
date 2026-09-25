#pragma once

// TcpTransport: a plaintext byte-stream transport over a stream socket.
//
// Templated on the underlying Asio socket so plain TCP and Unix-domain sockets
// share one implementation. Satisfies the TransportLike concept.

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include <boost/asio.hpp>

#include "../core/types.h"
#include "transport.h"

namespace simple_http {

namespace asio = boost::asio;

// Orderly shutdown+close, selected by socket type via overloading (no
// if-constexpr chains).
inline void shutdown_socket(asio::ip::tcp::socket& s) {
    if (s.is_open()) {
        error_code ec;
        s.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        s.close(ec);
    }
}

// Compiles in wherever the platform has AF_UNIX. asio is the one that knows, so
// there is no switch for a consumer to set (and none to forget).
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
inline void shutdown_socket(asio::local::stream_protocol::socket& s) {
    if (s.is_open()) {
        error_code ec;
        s.shutdown(asio::local::stream_protocol::socket::shutdown_both, ec);
        s.close(ec);
    }
}
#endif

template <typename Socket>
class TcpTransport {
  public:
    explicit TcpTransport(std::shared_ptr<Socket> socket, asio::ip::tcp::endpoint peer = {})
        : m_socket(std::move(socket)), m_peer(std::move(peer)) {}

    asio::awaitable<IoResult> async_read_some(ByteSpan buffer) {
        auto [ec, n] = co_await m_socket->async_read_some(asio::buffer(buffer.data(), buffer.size()),
                                                          asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }

    // Reads exactly `buffer.size()` bytes. Composed operation: it completes when
    // the buffer is full, or with an error (EOF included) after a partial read -
    // the result still reports the bytes read.
    asio::awaitable<IoResult> async_read(ByteSpan buffer) {
        auto [ec, n] = co_await asio::async_read(*m_socket, asio::buffer(buffer.data(), buffer.size()),
                                                asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }

    asio::awaitable<IoResult> async_write(ConstByteSpan buffer) {
        auto [ec, n] = co_await asio::async_write(*m_socket, asio::buffer(buffer.data(), buffer.size()),
                                                  asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }
    // Writes several buffers as one operation (one writev), so a frame header and
    // its payload need not be concatenated first. Like async_write this is a
    // composed operation: it completes only once every byte has been written.
    asio::awaitable<IoResult> async_write_seq(std::span<const ConstByteSpan> buffers) {
        const std::size_t count = std::min<std::size_t>(buffers.size(), 8);
        std::array<asio::const_buffer, 8> bufs{};
        for (std::size_t i = 0; i < count; ++i) {
            bufs[i] = asio::buffer(buffers[i].data(), buffers[i].size());
        }
        auto [ec, n] = co_await asio::async_write(
            *m_socket, std::span<const asio::const_buffer>{bufs.data(), count}, asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }


    auto get_executor() { return m_socket->get_executor(); }

    asio::ip::tcp::endpoint peer() const { return m_peer; }

    SslHandle tls_handle() const { return std::nullopt; }

    void close() { shutdown_socket(*m_socket); }

    Socket& socket() { return *m_socket; }
    const std::shared_ptr<Socket>& socket_ptr() const { return m_socket; }

  private:
    std::shared_ptr<Socket> m_socket;
    asio::ip::tcp::endpoint m_peer;
};

using TcpStreamTransport = TcpTransport<asio::ip::tcp::socket>;
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
using UnixStreamTransport = TcpTransport<asio::local::stream_protocol::socket>;
#endif

}  // namespace simple_http
