#pragma once

// Adapter that makes one HTTP/2 extended-CONNECT stream look like a byte-stream
// Transport to WsBackendImpl (RFC 8441). The TLS connection is already
// established, so this only moves bytes: reads come from the stream's inbound
// DATA queue, writes become outbound DATA frames.
//
// The adapter is token-based like every other transport in the C++17 port: the
// engine's read/write helpers take an any_completion_handler, and the adapter's
// async_read_some / async_write_seq / async_write forward the caller's token to
// them, preserving its associated executor and cancellation slot.

#include <array>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <cstddef>
#include <memory>
#include <utility>

#include "../../core/compat.h"
#include "../../core/types.h"
#include "../../transport/transport.h"

namespace simple_http {

namespace asio = boost::asio;

class H2WsStream {
  public:
    using IoCompletion = asio::any_completion_handler<void(error_code, std::size_t)>;

    virtual ~H2WsStream() = default;
    virtual void read_some(ByteSpan out, IoCompletion done) = 0;
    virtual void write_seq(boost::span<const ConstByteSpan> buffers, IoCompletion done) = 0;
    virtual void close() = 0;
    virtual asio::any_io_executor get_executor() = 0;
};

template <typename Adapter> class Http2WsTransport {
  public:
    explicit Http2WsTransport(std::shared_ptr<Adapter> adapter) : m_adapter(std::move(adapter)) {}

    // A peer's END_STREAM on an extended-CONNECT stream ends only its sending
    // direction; writes stay legal until the application closes.
    static constexpr bool kReadEofIsHalfClose = true;

    asio::any_io_executor get_executor() { return m_adapter->get_executor(); }

    template <typename Token> auto async_read_some(ByteSpan out, Token &&token) {
        auto adapter = m_adapter;
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            [adapter, out](auto handler) mutable { adapter->read_some(out, std::move(handler)); }, token);
    }

    template <typename Token> auto async_write_seq(boost::span<const ConstByteSpan> buffers, Token &&token) {
        auto adapter = m_adapter;
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            [adapter, buffers](auto handler) mutable { adapter->write_seq(buffers, std::move(handler)); }, token);
    }

    template <typename Token> auto async_write(ConstByteSpan buffer, Token &&token) {
        auto adapter = m_adapter;
        return asio::async_initiate<Token, void(error_code, std::size_t)>(
            [adapter, buffer](auto handler) mutable {
                const std::array<ConstByteSpan, 1> buffers{buffer};
                adapter->write_seq(boost::span<const ConstByteSpan>{buffers.data(), buffers.size()},
                                   std::move(handler));
            },
            token);
    }

    template <typename Token> auto async_read(ByteSpan buffer, Token &&token) {
        return async_read_some(buffer, std::forward<Token>(token));
    }

    template <typename Token> auto async_shutdown(Token &&token) {
        return asio::async_initiate<Token, void(error_code)>([](auto handler) { std::move(handler)(error_code{}); },
                                                             token);
    }

    void close() { m_adapter->close(); }

    asio::ip::tcp::endpoint peer() const { return {}; }
    SslHandle tls_handle() const { return std::nullopt; }

  private:
    std::shared_ptr<Adapter> m_adapter;
};

} // namespace simple_http
