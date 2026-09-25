#pragma once

// HTTP/3 engine (reserved extension point).
//
// The entire file is gated behind SIMPLE_HTTP_ENABLE_HTTP3 (default off), so the
// standard build needs no ngtcp2 / nghttp3 / QUIC-TLS dependency. The skeleton
// below is written against the SAME seams as the HTTP/1.x and HTTP/2 engines —
// a TransportLike transport (which for HTTP/3 will be a QUIC stream transport),
// the ResponseWriter interface, and the Dispatcher. Consequently, wiring a real
// HTTP/3 engine later requires NO changes above the engine layer.
//
// A working implementation needs:
//   * QuicTransport satisfying TransportLike over a QUIC/UDP stream (ngtcp2)
//   * nghttp3 for HTTP/3 frame/QPACK handling
//   * a QUIC-capable TLS backend (OpenSSL 3.5+ QUIC API or quictls/boringssl)

#ifdef SIMPLE_HTTP_ENABLE_HTTP3

#include <memory>
#include <string>

#include <boost/asio/awaitable.hpp>

#include "../../core/types.h"
#include "../../proto/response_writer.h"
#include "../dispatcher.h"

namespace simple_http {

namespace asio = boost::asio;

// ResponseWriter for an HTTP/3 stream (nghttp3). Skeleton: reports
// not-implemented until the QUIC/nghttp3 backend is wired up.
template <typename Transport>
class Http3ResponseWriter : public ResponseWriter {
  public:
    explicit Http3ResponseWriter(std::shared_ptr<Transport> transport) : m_transport(std::move(transport)) {}

    asio::awaitable<error_code> send(int, Headers, std::string) override { co_return not_implemented(); }
    asio::awaitable<error_code> send_headers(int, Headers) override { co_return not_implemented(); }
    asio::awaitable<error_code> send_chunk(std::string) override { co_return not_implemented(); }
    asio::awaitable<error_code> send_last(std::string) override { co_return not_implemented(); }
    // The interface hops before touching connection state; this skeleton has no
    // executor of its own yet, so there is nothing to hop to. A real QUIC
    // implementation will need the same hop the h1/h2 writers do.
    asio::awaitable<bool> connected() const override { co_return false; }
    asio::awaitable<void> close() override {
        if (m_transport) {
            m_transport->close();
        }
        co_return;
    }
    Version version() const override { return Version::Http3; }

  private:
    static error_code not_implemented() { return make_error_code(std::errc::not_supported); }
    std::shared_ptr<Transport> m_transport;
};

// HTTP/3 engine skeleton. Runs against a (future) QUIC transport and dispatches
// requests through the same Dispatcher as the other engines.
template <typename Transport>
class Http3Engine {
  public:
    explicit Http3Engine(std::shared_ptr<Transport> transport) : m_transport(std::move(transport)) {}

    asio::awaitable<void> run(Dispatcher /*dispatch*/) {
        // TODO(http3): drive ngtcp2/nghttp3 over the QUIC transport, building a
        // Request per stream and replying via Http3ResponseWriter, mirroring the
        // read_loop/write_loop structure of Http2Engine.
        if (m_transport) m_transport->close();
        co_return;
    }

  private:
    std::shared_ptr<Transport> m_transport;
};

}  // namespace simple_http

#endif  // SIMPLE_HTTP_ENABLE_HTTP3
