#pragma once

// TcpTransport: a plaintext byte-stream transport over a stream socket.
//
// Templated on the underlying Asio socket so plain TCP and Unix-domain sockets
// share one implementation. Satisfies the TransportLike concept.

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

#ifdef SIMPLE_HTTP_BIND_UNIX_SOCKET
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

    asio::awaitable<IoResult> async_write(ConstByteSpan buffer) {
        auto [ec, n] = co_await asio::async_write(*m_socket, asio::buffer(buffer.data(), buffer.size()),
                                                  asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }

    auto get_executor() { return m_socket->get_executor(); }

    asio::ip::tcp::endpoint peer() const { return m_peer; }

    SslHandle tls_handle() const { return std::nullopt; }

    void close() { shutdown_socket(*m_socket); }

    Socket& socket() { return *m_socket; }
    const std::shared_ptr<Socket>& socket_ptr() const { return m_socket; }

    // The underlying Beast-compatible stream (for the HTTP/1.x engine, which
    // uses Beast's parser/serializer directly). For plaintext this is the socket.
    Socket& beast_stream() { return *m_socket; }

  private:
    std::shared_ptr<Socket> m_socket;
    asio::ip::tcp::endpoint m_peer;
};

using TcpStreamTransport = TcpTransport<asio::ip::tcp::socket>;
#ifdef SIMPLE_HTTP_BIND_UNIX_SOCKET
using UnixStreamTransport = TcpTransport<asio::local::stream_protocol::socket>;
#endif

}  // namespace simple_http
