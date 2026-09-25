#pragma once

// TlsTransport: a TLS byte-stream transport over asio::ssl::stream<Socket>.
//
// Performs the server handshake and exposes the ALPN-negotiated protocol so the
// connection layer can pick the right engine (h2 vs http/1.1). Satisfies the
// TransportLike concept.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>

#include "../core/types.h"
#include "transport.h"

namespace simple_http {

namespace asio = boost::asio;

template <typename Socket>
class TlsTransport {
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
        const unsigned char* proto = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(m_stream->native_handle(), &proto, &len);
        return std::string_view{reinterpret_cast<const char*>(proto), len};
    }

    asio::awaitable<IoResult> async_read_some(ByteSpan buffer) {
        auto [ec, n] = co_await m_stream->async_read_some(asio::buffer(buffer.data(), buffer.size()),
                                                          asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }

    // Reads exactly `buffer.size()` bytes. Composed operation: it completes when
    // the buffer is full, or with an error (EOF included) after a partial read -
    // the result still reports the bytes read.
    asio::awaitable<IoResult> async_read(ByteSpan buffer) {
        auto [ec, n] = co_await asio::async_read(*m_stream, asio::buffer(buffer.data(), buffer.size()),
                                                asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }

    asio::awaitable<IoResult> async_write(ConstByteSpan buffer) {
        auto [ec, n] = co_await asio::async_write(*m_stream, asio::buffer(buffer.data(), buffer.size()),
                                                  asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }
    // Writes several buffers as one operation (one TLS record batch), so a frame header and
    // its payload need not be concatenated first. Like async_write this is a
    // composed operation: it completes only once every byte has been written.
    asio::awaitable<IoResult> async_write_seq(std::span<const ConstByteSpan> buffers) {
        const std::size_t count = std::min<std::size_t>(buffers.size(), 8);
        std::array<asio::const_buffer, 8> bufs{};
        for (std::size_t i = 0; i < count; ++i) {
            bufs[i] = asio::buffer(buffers[i].data(), buffers[i].size());
        }
        auto [ec, n] = co_await asio::async_write(
            *m_stream, std::span<const asio::const_buffer>{bufs.data(), count}, asio::as_tuple(asio::use_awaitable));
        co_return IoResult{ec, n};
    }


    auto get_executor() { return m_stream->get_executor(); }

    asio::ip::tcp::endpoint peer() const { return m_peer; }

    SslHandle tls_handle() const { return m_stream->native_handle(); }

    void close() {
        auto& lowest = m_stream->lowest_layer();
        if (lowest.is_open()) {
            error_code ec;
            lowest.shutdown(asio::socket_base::shutdown_both, ec);
            lowest.close(ec);
        }
    }

    Stream& stream() { return *m_stream; }
    const std::shared_ptr<Stream>& stream_ptr() const { return m_stream; }

  private:
    std::shared_ptr<Stream> m_stream;
    asio::ip::tcp::endpoint m_peer;
};

using TlsStreamTransport = TlsTransport<asio::ip::tcp::socket>;
#ifdef BOOST_ASIO_HAS_LOCAL_SOCKETS
using TlsUnixTransport = TlsTransport<asio::local::stream_protocol::socket>;
#endif

}  // namespace simple_http
