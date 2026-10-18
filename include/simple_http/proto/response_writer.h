#pragma once

// ResponseSink: the polymorphic, version-collapsing response backend.
//
// Every protocol engine supplies a concrete writer. Every operation that
// touches the transport or the engine - the writes below and the connection
// state - is awaitable and hops onto the owning connection's executor before
// touching them, which is what makes Response safe to use from any thread or
// coroutine. `version()` is the exception: it returns a plain value and needs
// no hop.
//
// Write operations report success/failure through a [[nodiscard]]
// awaitable<error_code> (a default-constructed error_code means success).
// Multi-state reads live on Body, which uses std::expected.

#include <boost/asio/awaitable.hpp>
#include <string>

#include "../core/types.h"
#include "headers.h"

namespace simple_http {

namespace asio = boost::asio;

class ResponseSink {
  public:
    virtual ~ResponseSink() = default;

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

    // Sends a trailing field section and ends the response (RFC 9110 §6.5).
    // This replaces the final send_last() rather than following it: the trailer
    // fields are what carries the end-of-stream marker on every version (the
    // terminating chunk on HTTP/1.x, a trailing HEADERS frame with END_STREAM on
    // HTTP/2 and HTTP/3). Only meaningful for a streamed response (send_headers
    // has run); Response::send() with trailers switches to that framing itself.
    //
    // HTTP/1.x needs chunked framing for this; a close-delimited response (an
    // HTTP/1.0 client) or a response to HEAD has nowhere to put a trailer
    // section, so the fields are dropped there. Implementers that carry no
    // trailers at all may keep the default, which ignores them.
    [[nodiscard]] virtual asio::awaitable<error_code> send_trailers(Headers trailers) {
        (void)trailers;
        co_return error_code{};
    }

    // Status + headers for a response with no body and no body framing at all:
    // 204/304 and every response to HEAD. Nothing follows the header block, so
    // the client sees the response end where the headers end (RFC 9110 §6.3).
    // Emitting a chunked terminator or a Content-Length here desynchronizes a
    // keep-alive client, which then reads the remainder as the next response.
    [[nodiscard]] virtual asio::awaitable<error_code> send_bodyless(int status, Headers headers) = 0;

    // Informational response (e.g. 100 Continue), when the client asked for one
    // with `Expect: 100-continue`. Protocols without the concept ignore it.
    [[nodiscard]] virtual asio::awaitable<error_code> send_continue() { co_return error_code{}; }

    // Connection state. These hop onto the connection executor first, exactly
    // like the writes above, because they read or change state the engine also
    // touches from there - HTTP/2's stream table, HTTP/1.x's open flag. Without
    // the hop, calling them from another thread would race with the engine
    // rather than merely return a stale answer.
    [[nodiscard]] virtual asio::awaitable<bool> connected() const = 0;
    [[nodiscard]] virtual asio::awaitable<void> close() = 0;

    // A plain value: no hop, safe from any thread.
    virtual Version version() const = 0;

    // Whether this request was a HEAD. A HEAD response has no body and no
    // trailer section, but it still carries the Content-Length a GET would have
    // produced - so the one-shot send() path is the right one for it, even when
    // the handler accumulated trailers. Known before the response starts (the
    // engine sets it from the parsed request), unlike the stream-table lookups
    // the write methods use.
    [[nodiscard]] virtual bool head_request() const noexcept { return false; }
};

} // namespace simple_http
