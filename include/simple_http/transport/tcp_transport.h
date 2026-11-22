#pragma once

// TcpTransport: a plaintext byte-stream transport over a stream socket
// (C++17 token-only port).
//
// Templated on the underlying Asio socket so plain TCP and Unix-domain sockets
// share one implementation. Every operation is an initiating function; the
// completion signature is void(error_code, std::size_t).

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>

#include "../core/asio_compat.h"
#include "../core/span.h"
#include "../core/types.h"
#include "transport.h"

namespace simple_http {

template <typename Socket> class TcpTransport {
  public:
    explicit TcpTransport(std::shared_ptr<Socket> socket, asio::ip::tcp::endpoint peer = {})
        : m_socket(std::move(socket)), m_peer(std::move(peer)) {}

    template <typename Token> auto async_read_some(ByteSpan buffer, Token &&token) {
        return detail::async_stream_read_some(*m_socket, buffer, std::forward<Token>(token));
    }

    template <typename Token> auto async_read(ByteSpan buffer, Token &&token) {
        return detail::async_stream_read_exact(*m_socket, buffer, std::forward<Token>(token));
    }

    template <typename Token> auto async_write(ConstByteSpan buffer, Token &&token) {
        return detail::async_stream_write_all(*m_socket, buffer, std::forward<Token>(token));
    }

    template <typename Token> auto async_write_seq(span<const ConstByteSpan> buffers, Token &&token) {
        return detail::async_stream_write_seq(*m_socket, buffers, std::forward<Token>(token));
    }

    auto get_executor() { return m_socket->get_executor(); }

    asio::ip::tcp::endpoint peer() const { return m_peer; }

    SslHandle tls_handle() const { return std::nullopt; }

    // TCP's FIN is its close notification; nothing to flush first. The
    // operation is still submitted to this transport's executor rather than
    // completing inside the initiating function, so a caller's callback never
    // re-enters before async_shutdown returns.
    template <typename Token> auto async_shutdown(Token &&token) {
        auto ex = get_executor();
        return asio::async_initiate<Token, void(error_code)>(
            detail::make_executor_initiation(
                [ex](auto handler) mutable {
                    asio::post(ex, [handler = std::move(handler)]() mutable { std::move(handler)(error_code{}); });
                },
                ex),
            token);
    }

    void close() { shutdown_socket(*m_socket); }

    Socket &socket() { return *m_socket; }
    const std::shared_ptr<Socket> &socket_ptr() const { return m_socket; }

  private:
    std::shared_ptr<Socket> m_socket;
    asio::ip::tcp::endpoint m_peer;
};

using TcpStreamTransport = TcpTransport<asio::ip::tcp::socket>;
#ifdef SIMPLE_HTTP_HAS_LOCAL_SOCKETS
using UnixStreamTransport = TcpTransport<asio::local::stream_protocol::socket>;
#endif

} // namespace simple_http
