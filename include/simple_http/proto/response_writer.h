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

    virtual bool connected() const = 0;
    virtual void close() = 0;
    virtual Version version() const = 0;
};

}  // namespace simple_http
