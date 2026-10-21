#pragma once

// TcpTransport: a plaintext byte-stream transport over a stream socket.
//
// Templated on the underlying Asio socket so plain TCP and Unix-domain sockets
// share one implementation. Satisfies the TransportLike concept.

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "../core/types.h"
#include "transport.h"

namespace simple_http {

namespace asio = boost::asio;

template <typename Socket> class TcpTransport {
  public:
    explicit TcpTransport(std::shared_ptr<Socket> socket, asio::ip::tcp::endpoint peer = {})
        : m_socket(std::move(socket)), m_peer(std::move(peer)) {}

    asio::awaitable<IoResult> async_read_some(ByteSpan buffer) {
        co_return co_await detail::stream_read_some(*m_socket, buffer);
    }

    asio::awaitable<IoResult> async_read(ByteSpan buffer) {
        co_return co_await detail::stream_read_exact(*m_socket, buffer);
    }

    asio::awaitable<IoResult> async_write(ConstByteSpan buffer) {
        co_return co_await detail::stream_write_all(*m_socket, buffer);
    }

    asio::awaitable<IoResult> async_write_seq(std::span<const ConstByteSpan> buffers) {
        co_return co_await detail::stream_write_seq(*m_socket, buffers);
    }

    auto get_executor() { return m_socket->get_executor(); }

    asio::ip::tcp::endpoint peer() const { return m_peer; }

    SslHandle tls_handle() const { return std::nullopt; }

    void close() { shutdown_socket(*m_socket); }

    Socket &socket() { return *m_socket; }
    const std::shared_ptr<Socket> &socket_ptr() const { return m_socket; }

  private:
    std::shared_ptr<Socket> m_socket;
    asio::ip::tcp::endpoint m_peer;
};

using TcpStreamTransport = TcpTransport<asio::ip::tcp::socket>;
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
using UnixStreamTransport = TcpTransport<asio::local::stream_protocol::socket>;
#endif

} // namespace simple_http
