#pragma once

// ResponseWriter: the polymorphic, version-collapsing response backend.
//
// Every protocol engine supplies a concrete writer. All write operations are
// awaitable so they perform real asynchronous I/O and, per the concurrency
// model, hop onto the owning connection's executor before touching the
// transport/session — making Response safe to use from any thread.
//
// Write operations report success/failure through a [[nodiscard]]
// awaitable<error_code> (a default-constructed error_code means success).
// Multi-state reads live on Body, which uses std::expected.

#include <string>

#include <boost/asio/awaitable.hpp>

#include "../core/types.h"
#include "headers.h"

namespace simple_http {

namespace asio = boost::asio;

class ResponseWriter {
  public:
    virtual ~ResponseWriter() = default;

    // One-shot: status + headers + full body, framed optimally per protocol
    // (e.g. HTTP/1.x sets Content-Length).
    [[nodiscard]] virtual asio::awaitable<error_code> send(int status, Headers headers, std::string body) = 0;

    // --- streaming ---
    // Begin a streamed response (HTTP/1.x uses chunked; HTTP/2 emits HEADERS).
    [[nodiscard]] virtual asio::awaitable<error_code> send_headers(int status, Headers headers) = 0;
    // A body chunk.
    [[nodiscard]] virtual asio::awaitable<error_code> send_chunk(std::string data) = 0;
    // The final body chunk; ends the response.
    [[nodiscard]] virtual asio::awaitable<error_code> send_last(std::string data) = 0;

    // Status + headers for a response with no body and no body framing at all:
    // 204/304 and every response to HEAD. Nothing follows the header block, so the
    // client sees the response end where the headers end (RFC 9110 §6.3). Emitting a
    // chunked terminator or a Content-Length here desynchronizes a keep-alive
    // client, which then reads the remainder as the next response.
    [[nodiscard]] virtual asio::awaitable<error_code> send_bodyless(int status, Headers headers) = 0;

    // Informational response (e.g. 100 Continue), when the client asked for one with
    // `Expect: 100-continue`. Protocols without the concept ignore it.
    [[nodiscard]] virtual asio::awaitable<error_code> send_continue() { co_return error_code{}; }

    virtual bool connected() const = 0;
    virtual void close() = 0;
    virtual Version version() const = 0;
};

}  // namespace simple_http
