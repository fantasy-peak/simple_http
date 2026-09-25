#pragma once

// The client's request/response model: what to send (RequestSpec), what came
// back (ResponseHead) and the two handles the caller drives — ClientStream for
// one exchange, ClientSession for the connection under it.
//
// The response side is deliberately shaped like the server's: read_head() gives
// the status and headers, then read() yields body chunks until one reports
// end-of-body (the same ReadResult the server's Body uses). Whichever of the two
// runs first parses and caches the head, so a caller may ask for the head
// explicitly or go straight to the body and consult head()/status() afterwards —
// nothing is silently dropped either way. Failures are error codes on the
// expected: a truncated body, a reset stream, a stalled peer.
//
// The request side mirrors Response as well: write() sends a body chunk and
// finish() ends the body (chunked on HTTP/1.1, DATA frames on HTTP/2).
//
// Version collapsing works exactly as it does on the server: the caller never
// branches on HTTP/1.1 vs HTTP/2. HTTP/1.1 sessions serve one exchange at a
// time, HTTP/2 sessions multiplex — a second open_stream() on an h1 session
// fails with client_errc::session_busy, while an h2 session happily returns more
// streams.
//
// Every operation that touches the transport or the session - the reads, the
// writes and cancel() - is an awaitable that hops onto the session's executor
// first (concurrency model A), so a stream may be driven from another thread
// just like a server-side Response. The remaining accessors (head(), status(),
// finished(), id(), version()) are synchronous snapshots of the last value
// published on that executor: safe to read, but not to call concurrently with an
// operation on the same stream.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>

#include "../core/http_method.h"
#include "../core/types.h"
#include "../proto/body.h"  // ReadResult
#include "../proto/headers.h"
#include "client_config.h"

namespace simple_http {

namespace asio = boost::asio;

// One request to send. `body` is the whole body when it is known up front;
// `stream_body` instead hands the body to ClientStream::write(), which the
// session frames as chunked (HTTP/1.1) or DATA frames (HTTP/2). Framing headers
// (Host, Content-Length, Transfer-Encoding, connection-specific fields) are the
// session's business — anything set here that the protocol forbids is dropped.
struct RequestSpec {
    Method method{Method::Get};
    // Origin-form request target (path + optional query). Empty means "the
    // origin's /", or — for the URL-based facade calls — the URL's own path, so
    // a caller can pass a bare spec and let the URL decide.
    std::string target;
    Headers headers;
    std::string body;
    bool stream_body{false};
    // Ask the peer to close after this exchange (`Connection: close`; on HTTP/2
    // the connection is simply not put back in the pool).
    bool close{false};
};

// The response head, as read_head() returns it.
struct ResponseHead {
    int status{0};
    Version version{Version::Http11};
    Headers headers;
    // True when nothing follows the head: a response to HEAD, or 204/304.
    bool bodyless{false};

    std::optional<std::string_view> header(std::string_view name) const { return headers.get(name); }
};

// One request/response exchange. Lifetime: keep it alive until the exchange is
// done; the session stays alive while any of its streams does.
class ClientStream {
  public:
    virtual ~ClientStream() = default;

    // --- request side ---
    // Sends a body chunk / finishes the body. Only valid for a request opened
    // with RequestSpec::stream_body; otherwise body_not_streaming.
    [[nodiscard]] virtual asio::awaitable<error_code> write(std::string data) = 0;
    [[nodiscard]] virtual asio::awaitable<error_code> finish(std::string data) = 0;

    // --- response side ---
    // The response head: waits for it (and parses it) if that has not happened
    // yet. Idempotent — the head is cached, so calling it after read() has
    // already delivered body bytes works too.
    [[nodiscard]] asio::awaitable<std::expected<ResponseHead, error_code>> read_head() {
        if (!m_head_ready) {
            if (auto ec = co_await await_head(); ec) co_return std::unexpected{ec};
        }
        co_return *m_head;
    }

    // The head, once read_head() (or read()) has completed. Before that it is a
    // default-constructed head (status 0).
    const ResponseHead& head() const { return m_head ? *m_head : m_no_head; }
    int status() const { return head().status; }

    // The next body event: a chunk, or end-of-body. The head is parsed on the
    // way if the caller never asked for it.
    [[nodiscard]] virtual asio::awaitable<std::expected<ReadResult, error_code>> read() = 0;

    // Reads the whole remaining body. `max_bytes` (0 = unlimited) bounds how much
    // is accumulated, so a peer cannot exhaust memory with an endless body; the
    // convenience layer always passes a cap.
    [[nodiscard]] asio::awaitable<std::expected<std::string, error_code>> read_all(std::size_t max_bytes = 0) {
        std::string out;
        for (;;) {
            auto chunk = co_await read();
            if (!chunk) co_return std::unexpected{chunk.error()};
            if (chunk->eof) co_return out;
            if (max_bytes != 0 && out.size() + chunk->data.size() > max_bytes) {
                co_return std::unexpected{make_error_code(client_errc::body_too_large)};
            }
            out.append(chunk->data);
        }
    }

    // --- state ---
    // Both directions complete. The session may then be reused (see
    // ClientSession::reusable()).
    virtual bool finished() const = 0;
    virtual Version version() const = 0;
    // HTTP/2 stream id; 0 on HTTP/1.x.
    virtual std::uint32_t id() const = 0;
    // Abandons the exchange: RST_STREAM on HTTP/2 (the connection survives),
    // connection close on HTTP/1.x (a half-written request leaves the peer at an
    // unknown position). Hops like the operations above, because it changes
    // session state the driving coroutine also touches.
    [[nodiscard]] virtual asio::awaitable<void> cancel() = 0;

  protected:
    // Called by the session once the response head is parsed; `await_head()` must
    // have done so before reporting success.
    void set_head(ResponseHead head) {
        if (!m_head) m_head = std::make_shared<ResponseHead>();
        *m_head = std::move(head);
        m_head_ready = true;
    }

    // Waits until the response head is available (and publishes it via
    // set_head()), or fails. Implemented by the session: only it knows how the
    // head arrives on its wire.
    [[nodiscard]] virtual asio::awaitable<error_code> await_head() = 0;

  private:
    std::shared_ptr<ResponseHead> m_head;
    bool m_head_ready{false};
    ResponseHead m_no_head{};
};

// A connection to one target, serving h1 or h2 exchanges.
class ClientSession {
  public:
    virtual ~ClientSession() = default;

    // Starts an exchange. HTTP/1.1: one at a time. HTTP/2: any number, each
    // multiplexed on the same connection.
    [[nodiscard]] virtual asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> open_stream(
        RequestSpec spec) = 0;

    // The transport is usable and no fatal error has been seen.
    virtual bool alive() const = 0;
    // The session may go back to the pool: alive, idle, and positioned at a
    // request boundary (HTTP/1.x: the last exchange ended with an explicit body
    // length and no `Connection: close`).
    virtual bool reusable() const = 0;
    virtual Version version() const = 0;
    // "host:port" this session is connected to, for diagnostics.
    virtual std::string_view authority() const = 0;
    virtual void close() = 0;

    // --- pooling hooks ---
    // A pooled connection must not hold its socket for the process's lifetime if
    // nothing ever asks for it again: the pool arms a close timer when it puts a
    // session back and cancels it when the session is taken out. Both calls
    // happen on the session's own executor (a session is only ever pooled and
    // taken by the executor that created it), so they are executor-confined.
    virtual void arm_idle_close(std::chrono::milliseconds ttl) = 0;
    virtual void disarm_idle_close() = 0;
};

}  // namespace simple_http
