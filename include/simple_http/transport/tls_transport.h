#pragma once

// TlsTransport: a TLS byte-stream transport over asio::ssl::stream<Socket>.
//
// Performs the server handshake and exposes the ALPN-negotiated protocol so the
// connection layer can pick the right engine (h2 vs http/1.1). Satisfies the
// TransportLike concept.

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
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

    // Performs the server-side TLS handshake. Must be awaited before I/O.
    asio::awaitable<error_code> handshake() {
        auto [ec] =
            co_await m_stream->async_handshake(asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
        co_return ec;
    }

    // The ALPN protocol negotiated during the handshake ("h2", "http/1.1", ...),
    // or empty if none was selected.
    std::string_view alpn_selected() const {
        const unsigned char *proto = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(m_stream->native_handle(), &proto, &len);
        return std::string_view{reinterpret_cast<const char *>(proto), len};
    }

    asio::awaitable<IoResult> async_read_some(ByteSpan buffer) {
        co_return co_await detail::stream_read_some(*m_stream, buffer);
    }

    asio::awaitable<IoResult> async_read(ByteSpan buffer) {
        co_return co_await detail::stream_read_exact(*m_stream, buffer);
    }

    asio::awaitable<IoResult> async_write(ConstByteSpan buffer) {
        co_return co_await detail::stream_write_all(*m_stream, buffer);
    }

    // Writes several buffers as one TLS record batch, so a frame header and its
    // payload need not be concatenated first.
    asio::awaitable<IoResult> async_write_seq(std::span<const ConstByteSpan> buffers) {
        co_return co_await detail::stream_write_seq(*m_stream, buffers);
    }

    auto get_executor() { return m_stream->get_executor(); }

    asio::ip::tcp::endpoint peer() const { return m_peer; }

    SslHandle tls_handle() const { return m_stream->native_handle(); }

    // Send close_notify before the TCP socket goes away. Without it the peer
    // sees a bare FIN and reports a truncated TLS stream (Python: SSLEOFError,
    // OpenSSL: stream_truncated) instead of a clean end of data.
    //
    // Bounded because asio's async_shutdown also waits for the peer's
    // close_notify; a peer that never sends one must not delay teardown.
    asio::awaitable<void> shutdown(std::chrono::milliseconds limit = std::chrono::milliseconds(250)) {
        if (!m_stream)
            co_return;
        using namespace asio::experimental::awaitable_operators;
        auto close_notify = [this]() -> asio::awaitable<void> {
            error_code ec;
            co_await m_stream->async_shutdown(asio::redirect_error(asio::use_awaitable, ec));
        };
        auto deadline = [limit]() -> asio::awaitable<void> {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(limit);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        };
        co_await (close_notify() || deadline());
    }

    void close() { shutdown_socket(m_stream->lowest_layer()); }

    Stream &stream() { return *m_stream; }
    const std::shared_ptr<Stream> &stream_ptr() const { return m_stream; }

  private:
    std::shared_ptr<Stream> m_stream;
    asio::ip::tcp::endpoint m_peer;
};

using TlsStreamTransport = TlsTransport<asio::ip::tcp::socket>;
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
using TlsUnixTransport = TlsTransport<asio::local::stream_protocol::socket>;
#endif

} // namespace simple_http
