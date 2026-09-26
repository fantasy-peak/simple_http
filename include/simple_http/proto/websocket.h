#pragma once

// WebSocket: a coroutine, transport-agnostic view of an upgraded WebSocket
// connection, safe for full-duplex use.
//
// Usage (handler receives a shared_ptr<WebSocket>):
//   // read loop
//   for (;;) { auto m = co_await ws->read(); if (!m) break; use(m->data, m->text); }
//   // and, concurrently, from any coroutine:
//   co_await ws->write_text("hi");
//   co_await ws->write_binary(bytes);
//
// This is the beast-free implementation: the wire codec is hand-written in
// ws_frame.h and reads/writes bytes
// straight through the simple_http Transport concept (async_read_some /
// async_write). No boost::beast::websocket::stream.
//
// Concurrency (model A): a connection is pinned to one single-threaded
// io_context. Every public operation that touches connection state
// (read/write/close) first hops onto that connection's executor before doing so,
// which is what makes the handle safe to use from any coroutine or thread —
// matching the guarantee the HTTP/2 ResponseWriter gives. Two exceptions:
// is_open() is a synchronous snapshot of a flag (atomic, so reading it from
// anywhere is race-free, though it can be a moment stale), and the destructor,
// which cannot hop but posts instead - so the handle may be destroyed from any
// thread and the teardown simply lands on the executor a moment later. In addition,
// all writes are serialized through an internal write pump: write_* enqueue a
// frame and await their own completion while a single pump coroutine
// (run_writer, started by the engine) performs the async_writes one at a time.
// The pump holds a reference to the backend for its entire run, so it may
// outlive the handle: whenever a write is still in flight the transport (and
// the TLS stream under it) stays alive until that write has completed.
//
// Fragmentation and control frames are handled inside read(): continuation
// frames are reassembled into one message (bounded by max_payload), Ping is
// answered with Pong, and a received Close is answered with a Close before the
// read reports end-of-stream.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "../core/types.h"
#include "../transport/transport.h"  // ConstByteSpan / kMaxWriteSegments
#include "ws_frame.h"

namespace simple_http {

namespace asio = boost::asio;

// One received WebSocket message plus its type (text vs binary). Returned by
// read() so callers never need a separate, race-prone got_text() query.
struct WsMessage {
    std::string data;
    bool text = true;
};

// Type-erased backend over a concrete Transport. The WebSocket handle forwards
// to it, so handlers never see the transport type. Owned through a shared_ptr
// (never a unique_ptr): the write pump coroutine holds a reference to itself so
// that it - and the transport it writes through - cannot be destroyed while a
// write is still in flight.
class WsBackend {
  public:
    virtual ~WsBackend() = default;
    virtual asio::awaitable<std::expected<WsMessage, error_code>> read() = 0;
    virtual asio::awaitable<error_code> write(std::string data, bool text) = 0;
    virtual asio::awaitable<error_code> close() = 0;
    virtual asio::awaitable<void> run_writer() = 0;  // the serializing write pump
    virtual bool is_open() const = 0;
    // Non-blocking teardown used by ~WebSocket: stops the write pump so that a
    // detached pump coroutine can finish and drop its self-reference. Safe to
    // call after close() (closing an already closed queue is a no-op).
    virtual void abort() = 0;
    // Hands bytes already read past the upgrade request (the start of the
    // client's first frame) to the frame parser. Must be called on the connection
    // executor before the first read().
    virtual void feed(std::string_view bytes) = 0;
};

// Concrete backend over a simple_http Transport (TCP plain or TLS). Reads whole
// messages via WsFrameParser and serializes all writes through a write pump.
template <typename Transport>
class WsBackendImpl final : public WsBackend, public std::enable_shared_from_this<WsBackendImpl<Transport>> {
    using Executor = decltype(std::declval<Transport&>().get_executor());

    // One queued outbound frame + a channel to deliver its write result.
    using ResultChannel = asio::experimental::concurrent_channel<void(error_code)>;
    struct WriteReq {
        std::string payload;                   // frame payload: the caller's buffer, moved in
        char header[10]{};                     // serialized frame header (at most 10 bytes)
        std::size_t header_len{0};
        std::shared_ptr<ResultChannel> done;   // null for fire-and-forget (auto Pong)
        bool close_after = false;              // stop the pump once this frame is written
    };
    // Queued frames live in our own deque: it is destroyed by ~WsBackendImpl and
    // explicitly cleared on teardown. They are deliberately NOT handed to asio as
    // a channel payload - asio's channel keeps a payload inside its operation
    // objects, and when such an operation is abandoned (the channel is closed
    // while a send is pending, which is exactly what a peer aborting mid-stream
    // does) that payload is released without running its destructor, so every
    // heap member it owns (here the frame payload) leaks. The channel below
    // therefore carries only an error_code, purely as a pump wake-up signal.
    using WriteQueue = std::deque<WriteReq>;
    using Notify = asio::experimental::concurrent_channel<void(error_code)>;

    // Builds a queued frame without any concatenation buffer: the header is
    // serialized into the request itself (10 bytes inline) and the payload's
    // buffer is moved in. The pump sends the two segments as one write.
    static WriteReq make_req(WsOpcode opcode, std::string payload, std::shared_ptr<ResultChannel> done,
                             bool close_after = false) {
        WriteReq req;
        req.header_len = ws_encode_header(req.header, opcode, payload.size());
        req.payload = std::move(payload);
        req.done = std::move(done);
        req.close_after = close_after;
        return req;
    }

  public:
    explicit WsBackendImpl(std::shared_ptr<Transport> transport, std::uint64_t max_payload = 16u * 1024 * 1024,
                           std::chrono::steady_clock::duration idle_timeout = std::chrono::seconds(120))
        : m_transport(std::move(transport)),
          m_executor(m_transport->get_executor()),
          m_parser(max_payload),
          m_max_payload(max_payload),
          m_idle_timeout(idle_timeout),
          m_watchdog_timer(std::make_shared<asio::steady_timer>(m_executor)),
          m_notify(m_executor, 1) {
        m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
    }

    // Hands bytes already read past the upgrade request to the parser, so a
    // client that pipelines its first frame behind the handshake does not lose it.
    void feed(std::string_view bytes) override {
        if (!bytes.empty()) {
            m_parser.append(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        }
    }

    // Reads one complete message, reassembling fragments and transparently
    // answering Ping with Pong / Close with Close. Returns std::unexpected(ec)
    // on close or transport error.
    asio::awaitable<std::expected<WsMessage, error_code>> read() override {
        co_await hop();  // touch parser/transport only on the connection executor

        std::string message;
        WsOpcode message_type = WsOpcode::Text;
        bool assembling = false;

        for (;;) {
            WsFrame frame;
            auto status = m_parser.next(frame);

            if (status == WsFrameParser::Status::Error) {
                m_open = false;
                co_return std::unexpected(make_error_code(asio::error::invalid_argument));
            }
            if (status == WsFrameParser::Status::NeedMore) {
                std::array<std::byte, 8192> tmp;  // no init: read_some fills [0,n)
                auto [ec, n] = co_await m_transport->async_read_some(std::span<std::byte>{tmp});
                if (ec) {
                    m_open = false;
                    co_return std::unexpected(ec);
                }
                touch_deadline();  // inbound bytes: connection is active
                m_parser.append(tmp.data(), n);
                continue;
            }

            // A complete frame was decoded.
            switch (frame.opcode) {
                case WsOpcode::Close:
                    // Answer with a Close and stop (RFC 6455 §5.5.1). The reply
                    // must be on the wire before the engine tears the transport
                    // down, and the engine only waits inside close() - which is a
                    // no-op once m_open is false - so this waits for the pump here.
                    m_open = false;
                    {
                        auto done = std::make_shared<ResultChannel>(m_executor, 1);
                        m_pending.push_back(make_req(WsOpcode::Close, ws_close_payload(), done, /*close_after=*/true));
                        (void)m_notify.try_send(error_code{});
                        co_await done->async_receive(asio::as_tuple(asio::use_awaitable));
                    }
                    co_return std::unexpected(make_error_code(asio::error::eof));

                case WsOpcode::Ping:
                    co_await enqueue_control(WsOpcode::Pong, std::move(frame.payload));
                    continue;

                case WsOpcode::Pong:
                    continue;  // ignore, keep reading

                case WsOpcode::Text:
                case WsOpcode::Binary:
                    if (assembling) {
                        // A new data frame before the previous message's FIN.
                        m_open = false;
                        co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                    }
                    message_type = frame.opcode;
                    message = std::move(frame.payload);
                    assembling = true;
                    if (frame.fin) {
                        co_return WsMessage{std::move(message), message_type == WsOpcode::Text};
                    }
                    continue;  // more fragments follow

                case WsOpcode::Continuation:
                    if (!assembling) {
                        m_open = false;
                        co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                    }
                    if (message.size() + frame.payload.size() > m_max_payload) {
                        // Bound the reassembled message size (a flood of small
                        // fragments must not exhaust memory).
                        m_open = false;
                        co_return std::unexpected(make_error_code(asio::error::message_size));
                    }
                    message.append(frame.payload);
                    if (frame.fin) {
                        co_return WsMessage{std::move(message), message_type == WsOpcode::Text};
                    }
                    continue;
            }
        }
    }

    // Enqueue a message and await its write completion. Safe from any coroutine;
    // the write pump serializes the actual async_writes.
    asio::awaitable<error_code> write(std::string data, bool text) override {
        co_await hop();
        if (!m_open) {
            co_return make_error_code(asio::error::not_connected);
        }
        auto done = std::make_shared<ResultChannel>(m_executor, 1);
        m_pending.push_back(make_req(text ? WsOpcode::Text : WsOpcode::Binary, std::move(data), done, false));
        (void)m_notify.try_send(error_code{});  // wake the pump; coalescing is fine
        auto [ec] = co_await done->async_receive(asio::as_tuple(asio::use_awaitable));
        co_return ec;
    }

    // Graceful close: enqueue a Close frame and wait for the write pump to send
    // it, then tear down the transport. Never closes the transport before the
    // Close frame is on the wire.
    asio::awaitable<error_code> close() override {
        co_await hop();
        if (m_open) {
            m_open = false;
            auto done = std::make_shared<ResultChannel>(m_executor, 1);
            m_pending.push_back(make_req(WsOpcode::Close, ws_close_payload(), done, /*close_after=*/true));
            (void)m_notify.try_send(error_code{});
            // Wait for the pump to actually write the Close frame.
            co_await done->async_receive(asio::as_tuple(asio::use_awaitable));
        }
        m_notify.close();  // stop the write pump if it is still running
        fail_pending_writes();
        if (m_watchdog_timer) m_watchdog_timer->cancel();  // stop the idle watchdog
        m_transport->close();
        co_return error_code{};
    }

    // Runs the write pump on the connection executor (started by the engine).
    // The pump body is a static coroutine taking a shared_ptr to the backend so
    // that its frame keeps the backend - and through it the transport and the
    // TLS stream - alive until the last write has completed. A detached pump
    // outlives the WebSocket handle (the engine destroys it as soon as the
    // handler returns), so holding only `this` would leave it writing through a
    // freed transport.
    asio::awaitable<void> run_writer() override {
        auto self = this->shared_from_this();
        // Start the idle watchdog alongside the pump. It holds only a weak_ptr to
        // the backend (see run_watchdog): it must NOT keep the backend alive, so
        // that a finished connection is freed immediately rather than lingering
        // for the idle timeout.
        asio::co_spawn(m_executor, run_watchdog(std::weak_ptr<WsBackendImpl>(self)), asio::detached);
        return pump(self);
    }

    // Non-blocking teardown for ~WebSocket: mark the connection closed and close
    // the queue. The pump then leaves the loop (its receive fails) and, as the
    // last real owner, releases the backend once any in-flight write has finished.
    // Cancelling the watchdog timer wakes its coroutine immediately; it then sees
    // the closed state (or a failed weak_ptr lock) and exits without waiting out
    // the idle timeout.
    // Callable from any thread. dispatch() gets both cases right: on the
    // connection's executor the teardown runs inline, so destroying a handle
    // there behaves exactly as it always did; from anywhere else it is queued
    // onto that executor - and crucially never run in place, since everything it
    // touches is state the executor also owns.
    //
    // Holding `self` across the dispatch is deliberate: it keeps the backend (and
    // the transport under it) alive until the teardown runs, which is what lets a
    // detached write pump finish and release its own reference.
    void abort() override {
        auto self = this->shared_from_this();
        asio::dispatch(m_executor, [self]() { self->do_abort(); });
    }

    // The executor-confined half of abort(): assumes it is already running on the
    // connection's executor.
    void do_abort() {
        m_open = false;
        m_notify.close();
        fail_pending_writes();
        m_watchdog_timer->cancel();
    }

    // Drops every queued frame and fails its waiter. Runs on the connection
    // executor; the deque is ours, so this is ordinary destruction - no asio
    // operation is involved, which is exactly the point.
    void fail_pending_writes() {
        while (!m_pending.empty()) {
            WriteReq req = std::move(m_pending.front());
            m_pending.pop_front();
            if (req.done) {
                (void)req.done->try_send(make_error_code(asio::error::operation_aborted));
            }
        }
    }

    bool is_open() const override { return m_open; }

  private:
    // Single write pump: serializes all async_writes for this connection.
    static asio::awaitable<void> pump(std::shared_ptr<WsBackendImpl> self) {
        for (;;) {
            if (self->m_pending.empty()) {
                auto [qec] = co_await self->m_notify.async_receive(asio::as_tuple(asio::use_awaitable));
                if (qec) {
                    break;  // closed -> connection is going away
                }
                continue;  // drain whatever got queued
            }
            WriteReq req = std::move(self->m_pending.front());
            self->m_pending.pop_front();
            auto wec = co_await self->write_all(req);
            if (wec) {
                self->m_open = false;
            }
            if (req.done) {
                (void)req.done->try_send(wec);  // deliver result to the waiter
            }
            if (wec || req.close_after) {
                break;  // transport error, or a Close frame just went out
            }
        }
        // Terminal by construction: once this loop is left, no pump will ever run
        // again. Anything still queued would therefore wait forever — a waiter is
        // parked on its `done` channel with no timeout, and nothing else signals
        // it — so releasing them here is what turns "the transport died" into an
        // error its callers can actually see. Idempotent: close() may already
        // have done this.
        self->m_open = false;
        self->fail_pending_writes();
        self->m_notify.close();
        co_return;
    }

    // Re-enter the connection's executor regardless of the caller's context, so
    // touching m_open / m_parser / m_transport is always single-threaded.
    asio::awaitable<void> hop() {
        co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable));
    }

    // Bound on the outbound queue, so a peer cannot grow it without limit.
    static constexpr std::size_t kMaxPendingFrames = 64;

    // Enqueue a control frame (Pong). Fire-and-forget: nobody awaits this frame's
    // result, so it is dropped outright once the connection is going away rather
    // than queued behind a pump that will never run again.
    //
    // The queue is bounded because a peer may ping faster than the pump drains,
    // and dropping a Pong is explicitly allowed (RFC 6455 §5.5.3): a Pong is not
    // required for every Ping. Unbounded growth here would be a remote memory
    // exhaustion vector.
    asio::awaitable<void> enqueue_control(WsOpcode opcode, std::string payload, bool close_after = false) {
        if (!m_open || m_pending.size() >= kMaxPendingFrames) {
            co_return;
        }
        m_pending.push_back(make_req(opcode, std::move(payload), nullptr, close_after));
        (void)m_notify.try_send(error_code{});
        co_return;
    }

    // Writes all of `frame` to the transport, looping over partial writes.
    // Sends one queued frame: the header (serialized into the request) and the
    // payload (the caller's buffer) as a single scatter-gather write, so no
    // per-frame concatenation buffer is ever allocated.
    asio::awaitable<error_code> write_all(const WriteReq& req) {
        touch_deadline();  // outbound bytes: connection is active
        const std::array<ConstByteSpan, 2> bufs{
            std::as_bytes(std::span<const char>{req.header, req.header_len}),
            std::as_bytes(std::span<const char>{req.payload.data(), req.payload.size()}),
        };
        auto [ec, n] = co_await m_transport->async_write_seq(bufs);
        (void)n;
        co_return ec;
    }

    // Push the idle deadline forward; called from both the read and write paths.
    void touch_deadline() { m_deadline = std::chrono::steady_clock::now() + m_idle_timeout; }

    // Idle watchdog: closes the transport once neither a read nor a write has
    // happened within the idle timeout, mirroring the h1/h2 engines.
    //
    // Unlike the pump, the watchdog holds only a WEAK reference to the backend.
    // The pump must keep the backend alive so an in-flight write can finish
    // through a valid transport; the watchdog has no such duty — once every real
    // owner (the pump and the WebSocket handle) is gone the connection is done
    // and the watchdog should just stop. A weak_ptr means the backend (and the
    // transport, TLS stream and parse buffer it owns) is freed the instant those
    // owners drop it, instead of lingering until the idle timeout fires. That is
    // what prevents backend backlog — and the apparent unbounded memory growth —
    // under high connection churn, where run_writer() may spawn this coroutine
    // only after the connection has already closed.
    //
    // The steady_timer lives in the backend (m_watchdog_timer). The coroutine
    // never holds the backend across a suspension: it locks the weak_ptr, arms
    // the timer, then releases the strong ref before awaiting. If the backend is
    // destroyed while suspended, its m_watchdog_timer is destroyed too, which
    // completes the wait with operation_aborted and the coroutine exits.
    static asio::awaitable<void> run_watchdog(std::weak_ptr<WsBackendImpl> weak) {
        for (;;) {
            asio::steady_timer* timer = nullptr;
            {
                auto self = weak.lock();
                if (!self || !self->m_open) {
                    co_return;  // connection gone or already closing: stop watching
                }
                timer = self->m_watchdog_timer.get();
                timer->expires_at(self->m_deadline);
                // `self` is released at the end of this scope, before the await
                // below, so the suspended wait does not keep the backend alive.
            }
            auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
            if (ec == asio::error::operation_aborted) {
                co_return;  // timer cancelled or backend destroyed: stop watching
            }
            auto self = weak.lock();
            if (!self || !self->m_open) {
                co_return;
            }
            if (std::chrono::steady_clock::now() >= self->m_deadline) {
                // Idle: tear down. Marking closed + closing the transport makes the
                // pending read/write fail, ending both loops.
                self->m_open = false;
                self->m_notify.close();
                self->fail_pending_writes();
                self->m_transport->close();
                co_return;
            }
            // Deadline was pushed forward by a read/write: loop and re-arm.
        }
    }

    std::shared_ptr<Transport> m_transport;
    Executor m_executor;
    WsFrameParser m_parser;
    std::uint64_t m_max_payload;
    std::chrono::steady_clock::duration m_idle_timeout;
    std::chrono::steady_clock::time_point m_deadline{};
    std::shared_ptr<asio::steady_timer> m_watchdog_timer;  // cancelled on teardown
    WriteQueue m_pending;  // queued frames: owned by us, cleared on teardown
    Notify m_notify;       // pump wake-up signal (carries only a trivial error_code)
    // Atomic so is_open() can be read from any thread without racing the
    // executor's writes. Everything else in this class is executor-confined.
    std::atomic<bool> m_open{true};
};

// User-facing WebSocket handle. Forwards to the type-erased backend.
class WebSocket {
  public:
    explicit WebSocket(std::shared_ptr<WsBackend> backend) : m_backend(std::move(backend)) {}

    // Dropping the handle without awaiting close() must still stop the detached
    // write pump, otherwise it would keep the backend alive forever.
    //
    // Safe from any thread: abort() runs inline when it is already on the
    // connection's executor and posts itself there otherwise, so a shared_ptr
    // that happens to die on some unrelated thread no longer races the
    // connection. The teardown then lands a moment later, which costs nothing -
    // it is a non-blocking stop, not a synchronisation point.
    ~WebSocket() { m_backend->abort(); }

    // Reads one complete message (with its text/binary type). std::unexpected(ec)
    // on close/error.
    asio::awaitable<std::expected<WsMessage, error_code>> read() { return m_backend->read(); }

    // Send one complete message. All three are safe from any coroutine and are
    // serialized internally.
    asio::awaitable<error_code> write_text(std::string data) { return m_backend->write(std::move(data), true); }
    asio::awaitable<error_code> write_binary(std::string data) { return m_backend->write(std::move(data), false); }
    asio::awaitable<error_code> write(std::string data, bool text) {
        return m_backend->write(std::move(data), text);
    }

    // Graceful close: sends a Close frame, then tears the connection down.
    asio::awaitable<error_code> close() { return m_backend->close(); }

    // A snapshot, not a hopped operation: race-free to call from anywhere, but it
    // can report the state as of a moment ago. Use read()/write() to act on the
    // connection - those hop and so see the current state.
    bool is_open() const { return m_backend->is_open(); }

    // Runs the serializing write pump (started by the engine on the connection
    // executor). The pump holds its own reference to the backend, so it is safe
    // to spawn detached and to drop this handle while it is still draining.
    asio::awaitable<void> run_writer() { return m_backend->run_writer(); }

  private:
    std::shared_ptr<WsBackend> m_backend;
};

}  // namespace simple_http
