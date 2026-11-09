#pragma once

// ResponseSink: the polymorphic, version-collapsing response backend (C++17).
//
// Every protocol engine supplies a concrete writer. Virtual functions cannot be
// templates, so the token boundary is type-erased here: each operation takes an
// asio::any_completion_handler with the Asio-conventional completion signature
// void(error_code). any_completion_handler propagates the caller's associated
// executor, allocator and cancellation slot, so wrapping a token on the way in
// does not lose any of them.
//
// The public, token-shaped API lives on ResponseWriter (response.h); this
// interface is what h1/h2/h3 implement and what the facade forwards to.

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <string>

#include "../core/types.h"
#include "headers.h"

namespace simple_http {

namespace asio = boost::asio;

using WriteCompletion = asio::any_completion_handler<void(error_code)>;
using ConnectedCompletion = asio::any_completion_handler<void(error_code, bool)>;

class ResponseSink {
  public:
    virtual ~ResponseSink() = default;

    // One-shot: status + headers + full body, framed optimally per protocol
    // (e.g. HTTP/1.x sets Content-Length).
    virtual void send(int status, Headers headers, std::string body, WriteCompletion done) = 0;

    // --- streaming ---
    // Begin a streamed response (HTTP/1.x uses chunked; HTTP/2 emits HEADERS).
    virtual void send_headers(int status, Headers headers, WriteCompletion done) = 0;
    // A body chunk.
    virtual void send_chunk(std::string data, WriteCompletion done) = 0;
    // The final body chunk; ends the response.
    virtual void send_last(std::string data, WriteCompletion done) = 0;

    // Sends a trailing field section and ends the response (RFC 9110 §6.5).
    // This replaces the final send_last() rather than following it. Protocols
    // without trailers keep this default, which reports success.
    virtual void send_trailers(Headers trailers, WriteCompletion done) {
        (void)trailers;
        std::move(done)(error_code{});
    }

    // Status + headers for a response with no body and no body framing at all:
    // 204/304 and every response to HEAD.
    virtual void send_bodyless(int status, Headers headers, WriteCompletion done) = 0;

    // Informational response (e.g. 100 Continue), when the client asked for one
    // with `Expect: 100-continue`. Protocols without the concept ignore it.
    virtual void send_continue(WriteCompletion done) { std::move(done)(error_code{}); }

    // Connection state. Implementations hop onto the connection executor before
    // touching engine state, exactly like the writes.
    virtual void connected(ConnectedCompletion done) const = 0;
    virtual void close(WriteCompletion done) = 0;

    // A pool the facade uses as async_compose's I/O object. It is the executor
    // the sink's operations complete on, not necessarily the caller's.
    virtual asio::any_io_executor get_executor() const = 0;

    // A plain value: no hop, safe from any thread.
    virtual Version version() const = 0;

    // Whether this request was a HEAD. A HEAD response has no body and no
    // trailer section, but it still carries the Content-Length a GET would have
    // produced - so the one-shot send() path is the right one for it.
    virtual bool head_request() const noexcept { return false; }
};

} // namespace simple_http
