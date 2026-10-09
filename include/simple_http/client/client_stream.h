#pragma once

// The client's request/response model: what to send (Request), what came
// back (ResponseHead) and the two handles the caller drives — ClientStream for
// one exchange, ClientSession for the connection under it.
//
// The response side is deliberately shaped like the server's: read_head() gives
// the status and headers, then read() yields body chunks until one reports
// end-of-body (the same ReadResult the server's Body uses). Whichever of the
// two runs first parses and caches the head, so a caller may ask for the head
// explicitly or go straight to the body and consult head()/status() afterwards
// — nothing is silently dropped either way. Failures are error codes on the
// expected: a truncated body, a reset stream, a stalled peer.
//
// The request side mirrors Response as well: write() sends a body chunk and
// finish() ends the body (chunked on HTTP/1.1, DATA frames on HTTP/2).
//
// Version collapsing works exactly as it does on the server: the caller never
// branches on HTTP/1.1 vs HTTP/2. HTTP/1.1 sessions serve one exchange at a
// time, HTTP/2 sessions multiplex — a second open_stream() on an h1 session
// fails with client_errc::session_busy, while an h2 session happily returns
// more streams.
//
// Every operation that touches the transport or the session - the reads, the
// writes and cancel() - is an awaitable that hops onto the session's executor
// first (concurrency model A), so a stream may be driven from another thread
// just like a server-side Response. The remaining accessors (head(), status(),
// finished(), id(), version()) are synchronous snapshots of the last value
// published on that executor: safe to read, but not to call concurrently with
// an operation on the same stream.

#include <atomic>
#include <boost/asio.hpp>
#include <cstdint>
#include <expected>
#include <functional> // set_on_destroy / session hooks
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../core/base64.h" // ClientStream::write path (Request carries basic_auth)
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/types.h"
#include "../proto/body.h" // ReadResult
#include "../proto/headers.h"
#include "../proto/request.h" // Request — the shared request message type
#include "client_config.h"

namespace simple_http::detail {

namespace asio = boost::asio;

// The request message type is `simple_http::Request` (proto/request.h), shared
// with the server: a caller builds one (method/target/headers/body/close) and a
// session sends it. Framing headers (Host, Content-Length, Transfer-Encoding,
// connection-specific fields) are the session's business — anything set on the
// Request that the protocol forbids is dropped at send time.

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
    virtual ~ClientStream() {
        // One-shot lifecycle hook fired when the caller drops the stream: the
        // engine uses it to release its single HTTP/1.1 connection's one-
        // exchange gate (so the next request can proceed) and to close a
        // throwaway connection for a different origin. Must not touch the
        // stream — the derived members are already gone when this runs.
        if (m_on_destroy)
            m_on_destroy();
    }

    // Installs the destruction hook (see the destructor). The engine sets it on
    // every stream it hands out.
    void set_on_destroy(std::function<void()> cb) { m_on_destroy = std::move(cb); }

    // The engine's single-connection h1 gate holder for this exchange: the
    // on-destroy hook uses it to hand the gate back (idempotently — see
    // ExchangeGate::release) when the caller drops the stream.
    void set_gate_token(std::shared_ptr<void> tok) { m_gate_token = std::move(tok); }
    const std::shared_ptr<void> &gate_token() const { return m_gate_token; }

    // --- request side ---
    // Sends a body chunk / finishes the body. Only valid for a request opened
    // with Request::stream_body; otherwise body_not_streaming.
    [[nodiscard]] virtual asio::awaitable<error_code> write(std::string data) = 0;
    [[nodiscard]] virtual asio::awaitable<error_code> finish(std::string data) = 0;

    // --- response side ---
    // The response head: waits for it (and parses it) if that has not happened
    // yet. Idempotent — the head is cached, so calling it after read() has
    // already delivered body bytes works too.
    [[nodiscard]] asio::awaitable<std::expected<ResponseHead, error_code>> read_head() {
        if (!m_head_ready) {
            if (auto ec = co_await await_head(); ec)
                co_return std::unexpected{ec};
        }
        co_return *m_head;
    }

    // The head, once read_head() (or read()) has completed. Before that it is a
    // default-constructed head (status 0).
    const ResponseHead &head() const { return m_head ? *m_head : m_no_head; }
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
            if (!chunk)
                co_return std::unexpected{chunk.error()};
            if (chunk->eof)
                co_return out;
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
        if (!m_head)
            m_head = std::make_shared<ResponseHead>();
        *m_head = std::move(head);
        m_head_ready = true;
    }

    // Invalidates the cached head so the next read_head() re-parses. Used by the
    // HTTP/1.1 session after it publishes an interim 100-continue: that head is
    // handed to the caller (so it can send the body), but the *real* response
    // head still follows and must not be masked by the cached interim.
    void reset_head() { m_head_ready = false; }

    // Waits until the response head is available (and publishes it via
    // set_head()), or fails. Implemented by the session: only it knows how the
    // head arrives on its wire.
    [[nodiscard]] virtual asio::awaitable<error_code> await_head() = 0;

  private:
    std::shared_ptr<ResponseHead> m_head;
    bool m_head_ready{false};
    ResponseHead m_no_head{};
    std::function<void()> m_on_destroy;
    std::shared_ptr<void> m_gate_token;
};

// A connection to one target, serving h1 or h2 exchanges.
class ClientSession {
  public:
    virtual ~ClientSession() = default;

    // Starts an exchange. HTTP/1.1: one at a time. HTTP/2: any number, each
    // multiplexed on the same connection. The request to send is a shared_ptr
    // to a `Request`, the same type the server hands its handlers — built the
    // same way (method, target, headers, body) and read back by the engine at
    // send time.
    [[nodiscard]] virtual asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>>
    open_stream(std::shared_ptr<Request> request) = 0;

    // The transport is usable and no fatal error has been seen.
    virtual bool alive() const = 0;
    // True when the session is positioned at a request boundary (HTTP/1.x: the
    // last exchange ended with an explicit body length and no `Connection:
    // close`; HTTP/2: no in-flight streams).
    virtual bool reusable() const = 0;
    // True when the session can serve another request after the current one —
    // alive and neither side asked to close. Unlike reusable() this ignores
    // whether an exchange is currently in flight, which is exactly what the
    // single-connection engine's reuse check needs: a busy-but-keepable HTTP/1.1
    // connection is a *reuse* (the next request queues on the gate), while a
    // close-requested one must be dropped and re-dialed. Read from foreign
    // threads (shared Client), so its inputs are atomics.
    virtual bool keepable() const = 0;
    virtual Version version() const = 0;
    // "host:port" this session is connected to, for diagnostics.
    virtual std::string_view authority() const = 0;
    // The executor every operation of this session runs on (model A). The
    // engine uses it to release its kept connection: dropping the last
    // reference to a session frees an object graph (Body channels, timers)
    // that was created on this executor, so the destructor must run on it even
    // when the drop happens on another thread.
    virtual asio::any_io_executor executor() const = 0;
    virtual void close() = 0;

    // One-shot hook fired when an exchange completes and the connection can
    // serve another. The engine uses it on its single HTTP/1.1 connection to
    // release the one-exchange gate on *completion*, independent of how long
    // the caller keeps the stream handle. No-op except on HTTP/1.1 sessions.
    virtual void set_on_exchange_done(std::function<void()> cb) { (void)cb; }

    // The completion flag of the exchange currently in flight, shared with the
    // engine's stream-destruction hook so it can tell an *abandoned* exchange
    // (dropped before it ever reached its end — a half-read message, which must
    // close the connection) from one that completed and merely left a stream
    // handle alive (whose connection the session itself decides to keep or
    // close). HTTP/1.1 only; a null shared_ptr elsewhere.
    virtual std::shared_ptr<std::atomic<bool>> exchange_done_flag() const { return nullptr; }
};

} // namespace simple_http::detail
