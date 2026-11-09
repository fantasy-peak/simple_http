#pragma once

// TlsTransport: a TLS byte-stream transport over asio::ssl::stream<Socket>
// (C++17 token-only port).
//
// Performs the server handshake and exposes the ALPN-negotiated protocol so the
// connection layer can pick the right engine (h2 vs http/1.1).

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "../core/types.h"
#include "transport.h"

namespace simple_http {

namespace asio = boost::asio;

template <typename Socket> class TlsTransport {
  public:
    using Stream = asio::ssl::stream<Socket>;

    explicit TlsTransport(std::shared_ptr<Stream> stream, asio::ip::tcp::endpoint peer = {})
        : m_stream(std::move(stream)), m_peer(std::move(peer)) {}

    // Performs the server-side TLS handshake. Completion: void(error_code).
    template <typename Token> auto async_handshake(Token &&token) {
        return m_stream->async_handshake(asio::ssl::stream_base::server, std::forward<Token>(token));
    }

    // The ALPN protocol negotiated during the handshake ("h2", "http/1.1", ...),
    // or empty if none was selected.
    std::string_view alpn_selected() const {
        const unsigned char *proto = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(m_stream->native_handle(), &proto, &len);
        return std::string_view{reinterpret_cast<const char *>(proto), len};
    }

    template <typename Token> auto async_read_some(ByteSpan buffer, Token &&token) {
        return detail::async_stream_read_some(*m_stream, buffer, std::forward<Token>(token));
    }

    template <typename Token> auto async_read(ByteSpan buffer, Token &&token) {
        return detail::async_stream_read_exact(*m_stream, buffer, std::forward<Token>(token));
    }

    template <typename Token> auto async_write(ConstByteSpan buffer, Token &&token) {
        return detail::async_stream_write_all(*m_stream, buffer, std::forward<Token>(token));
    }

    // Writes several buffers as one TLS record batch, so a frame header and its
    // payload need not be concatenated first.
    template <typename Token> auto async_write_seq(boost::span<const ConstByteSpan> buffers, Token &&token) {
        return detail::async_stream_write_seq(*m_stream, buffers, std::forward<Token>(token));
    }

    auto get_executor() { return m_stream->get_executor(); }

    asio::ip::tcp::endpoint peer() const { return m_peer; }

    SslHandle tls_handle() const { return m_stream->native_handle(); }

    // Send close_notify before the TCP socket goes away. Without it the peer
    // sees a bare FIN and reports a truncated TLS stream instead of a clean end
    // of data.
    //
    // Bounded because asio's async_shutdown also waits for the peer's
    // close_notify; cancel_after turns a peer that never sends one into an
    // operation_aborted rather than a delayed teardown.
    template <typename Token> auto async_shutdown(std::chrono::milliseconds limit, Token &&token) {
        return m_stream->async_shutdown(asio::cancel_after(limit, std::forward<Token>(token)));
    }

    template <typename Token> auto async_shutdown(Token &&token) {
        return async_shutdown(std::chrono::milliseconds{250}, std::forward<Token>(token));
    }

    void close() { shutdown_socket(m_stream->lowest_layer()); }

    Stream &stream() { return *m_stream; }
    const std::shared_ptr<Stream> &stream_ptr() const { return m_stream; }

  private:
    std::shared_ptr<Stream> m_stream;
    asio::ip::tcp::endpoint m_peer;
};

using TlsStreamTransport = TlsTransport<asio::ip::tcp::socket>;

// TLS over a TLS connection: the client's origin TLS runs inside the TLS
// connection it already established to an HTTPS proxy.
using NestedTlsTransport = TlsTransport<asio::ssl::stream<asio::ip::tcp::socket>>;
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
using TlsUnixTransport = TlsTransport<asio::local::stream_protocol::socket>;
#endif

} // namespace simple_http
