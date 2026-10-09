#pragma once

// WebSocket: a coroutine, transport-agnostic view of an upgraded WebSocket
// connection, safe for full-duplex use.
//
// Usage (handler receives a shared_ptr<WebSocket>):
//   // read loop
//   for (;;) { auto m = co_await ws->read(); if (!m) break; use(m->data,
//   m->text); }
//   // and, concurrently, from any coroutine:
//   co_await ws->write_text("hi");
//   co_await ws->write_binary(bytes);
//
// write_text/write_binary/write take std::string BY VALUE and move it into the
// queue: the operation owns its bytes, so it is safe to build the awaitable,
// let the source die, and co_await it later. write_text_view/write_binary_view/
// write_view (the server-side fast path) instead BORROW the caller's bytes: the
// frame is sent before the returned awaitable completes, so the storage only
// has to stay alive across the co_await (a direct `co_await ...` does). The
// flip side is the usual async-buffer contract: do not store the awaitable and
// await it after the buffer died, and do not mutate the buffer from another
// coroutine while the write is in flight.
//
// This is the beast-free implementation: the wire codec is hand-written in
// ws_frame.h and reads/writes bytes
// straight through the simple_http Transport concept (async_read_some /
// async_write). No boost::beast::websocket::stream.
//
// Concurrency (model A): a connection is pinned to one single-threaded
// io_context. Every public operation that touches connection state
// (read/write/close) first hops onto that connection's executor before doing
// so, which is what makes the handle safe to use from any coroutine or thread —
// matching the guarantee the HTTP/2 ResponseSink gives. Two exceptions:
// is_open() is a synchronous snapshot of a flag (atomic, so reading it from
// anywhere is race-free, though it can be a moment stale), and the destructor,
// which cannot hop but posts instead - so the handle may be destroyed from any
// thread and the teardown simply lands on the executor a moment later. In
// addition, all writes are serialized through an internal write pump: write_*
// enqueue a frame and await their own completion while a single pump coroutine
// (run_writer, started by the engine) performs the async_writes one at a time.
// The pump holds a reference to the backend for its entire run, so it may
// outlive the handle: whenever a write is still in flight the transport (and
// the TLS stream under it) stays alive until that write has completed.
//
// Fragmentation and control frames are handled inside the reader: continuation
// frames are reassembled into one message (bounded by max_payload), Ping is
// answered with Pong, and a received Close is answered with a Close before the
// read reports end-of-stream.
//
// Reads are decoupled from the application on purpose. An internal reader
// coroutine keeps draining the transport into a bounded inbound queue
// (`inbox_limit` bytes, payload + per-message overhead) and read() dequeues from
// it, rather than read() itself pulling from the socket. That matters when the
// application serializes read/write (the common echo shape): if the peer stops
// reading, the application's write blocks, and a read()-driven socket would then
// stop draining too — the two ends park on each other's closed window. The
// independent reader keeps the socket moving regardless of what the application
// coroutine is awaiting. The queue bound is the back-pressure: once it is full the
// reader simply stops reading until read() drains some. So the memory cost is
// bounded per connection, but it is held while the application is not consuming.

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp> // async_initiate
#include <boost/asio/experimental/channel.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../core/types.h"
#include "../transport/transport.h" // ConstByteSpan / kMaxWriteSegments
#include "ws_deflate.h"             // permessage-deflate (RFC 7692)
#include "ws_frame.h"

namespace simple_http {

namespace asio = boost::asio;

// Detects a Transport whose read-side EOF means only that this direction ended
// (an HTTP/2 extended-CONNECT stream's END_STREAM), not that the connection is
// gone: writes stay legal until the application closes. A plain byte-stream
// transport (TCP/TLS) has no half-close notion, so EOF there is the peer going
// away and the backend is marked closed as before. The transport opts in with a
// `static constexpr bool kReadEofIsHalfClose = true;` member.
template <typename T, typename = void> struct ws_read_eof_is_half_close : std::false_type {};
template <typename T>
struct ws_read_eof_is_half_close<T, std::void_t<decltype(T::kReadEofIsHalfClose)>> : std::true_type {};

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
    // Owning send: `data` is moved in (or copied once) and lives in the queue.
    virtual asio::awaitable<error_code> write(std::string data, bool text) = 0;
    // Borrowing send: the server points straight at `data`, which must stay
    // valid until the returned awaitable completes. The client cannot borrow
    // because it masks the payload in place, so it falls back to an owned copy.
    virtual asio::awaitable<error_code> write_view(std::string_view data, bool text) = 0;
    // Sends a Ping frame carrying `payload` (≤125 octets, RFC 6455 §5.5.2) and
    // waits for it to reach the wire. The reader answers a peer's Ping
    // automatically; this is the application's own liveness probe.
    virtual asio::awaitable<error_code> ping(std::string payload) = 0;
    virtual asio::awaitable<error_code> close() = 0;
    // Graceful close carrying an application-level status code and reason, the
    // RFC 6455 §5.5.1 shape (code + optional UTF-8 reason ≤ 123 octets). The
    // code must be one a peer may legitimately receive (ws_valid_close_code).
    virtual asio::awaitable<error_code> close(std::uint16_t code, std::string reason) = 0;
    // Per-connection compression controls (the gorilla Conn shape). Only
    // meaningful once permessage-deflate was negotiated; on a connection that
    // did not negotiate it they are inert. `enable_write_compression` toggles
    // whether outbound messages carry RSV1 (RFC 7692 permits any message to be
    // sent uncompressed); `set_compression_level` sets the outbound DEFLATE
    // level (0 = zlib default, 1..9). `compression_negotiated` is a snapshot,
    // safe to read from any thread like is_open().
    virtual asio::awaitable<error_code> enable_write_compression(bool enable) = 0;
    virtual asio::awaitable<error_code> set_compression_level(int level) = 0;
    virtual bool compression_negotiated() const = 0;
    virtual asio::awaitable<void> run_writer() = 0; // the serializing write pump
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
    using Executor = decltype(std::declval<Transport &>().get_executor());

    // One queued outbound frame plus the completion signal its waiter is parked
    // on (see `done` below).
    //
    // Lock-free `channel` (not `concurrent_channel`) — deliberately. Every
    // operation on the connection-level signal channels below first re-enters
    // the connection's executor: write()/close_with() hop, the pump and the
    // watchdog are co_spawn'd on it, and abort() dispatches to it. Under Model A
    // one thread drives that executor, so all channel access is serialized on
    // that one thread and the thread-safe `concurrent_channel` would only pay a
    // mutex per message for a guarantee nothing here uses. If a future caller
    // ever touches them from a different executor without a hop, this becomes a
    // data race.
    //
    // Per-frame completion is deliberately NOT a channel: each queued frame that
    // someone awaits used to carve a fresh `channel` object plus an asynchronous
    // receive operation out of the heap — two allocations per outbound message,
    // right on the relay hot path. The pump now invokes the awaiting coroutine
    // directly through a type-erased completion handler stored inside the frame
    // (`WriteReq::done`), which `boost::asio::any_completion_handler` holds
    // inline with no allocation. Only the wire-level connection signals stay as
    // channels: the pump wake-up (`m_notify`) and the inbox watermarks.
    struct WriteReq {
        // Owned payload: either moved in (the rvalue/control path) or copied
        // because the client side has to mask the bytes in place.
        std::string owned;
        // Borrowed payload: points at the caller's buffer. It must stay valid
        // until the frame has been written, which a direct co_await guarantees
        // because write() does not return until the pump's async_write finished.
        std::string_view borrowed;
        bool borrowed_payload = false;
        // Serialized frame header. A plain (server) frame is at most 10 bytes
        // (FIN+opcode, len prefix, 64-bit length); a masked (client) frame adds
        // the MASK bit and a 4-byte key, so the worst case is 14.
        char header[14]{};
        std::size_t header_len{0};
        // The completion signal the pump fires once this frame is written (or
        // with an error when teardown abandons the queue). nullopt for
        // fire-and-forget frames (auto Pong), which no-one awaits.
        std::optional<asio::any_completion_handler<void(error_code)>> done;
        bool close_after = false; // stop the pump once this frame is written

        std::string_view payload() const { return borrowed_payload ? borrowed : std::string_view{owned}; }
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
    using Notify = asio::experimental::channel<void(error_code)>;

    // Builds a queued frame without any concatenation buffer: the header is
    // serialized into the request itself (10 bytes inline) and the payload's
    // buffer is moved in. The pump sends the two segments as one write.
    //
    // `mask` — a client MUST mask every frame it sends (RFC 6455 §5.3), so the
    // client-side backend (expect_masked=false) adds the mask bit, a random
    // 4-byte key and the XOR. The server side (expect_masked=true) does not.
    static WriteReq make_req(WsOpcode opcode, std::string payload,
                             std::optional<asio::any_completion_handler<void(error_code)>> done,
                             bool close_after = false, bool mask = false, bool rsv1 = false) {
        WriteReq req;
        req.header_len = ws_encode_header(req.header, opcode, payload.size(), rsv1);
        if (mask) {
            // Set the MASK bit (0x80 on the second header octet — the length
            // field, whose lower 7 bits are the payload length).
            unsigned char mask_key[4];
            for (auto &b : mask_key)
                b = static_cast<unsigned char>(std::rand() & 0xFF);
            // Rebuild the length octet(s) with the mask bit. The header layout
            // from ws_encode_header is: [FIN+opcode][len...]; the mask bit rides
            // on the byte that carries the length.
            const char fin_opcode = req.header[0];
            std::size_t len_bytes = req.header_len - 1; // octets after opcode
            unsigned char len7 = static_cast<unsigned char>(req.header[1]);
            len7 = static_cast<unsigned char>(len7 | 0x80);
            req.header[1] = static_cast<char>(len7);
            // Insert the mask key right after the length prefix + any extended
            // length. Extended length lives in header[2 .. len_bytes].
            std::size_t insert_at = req.header_len;
            // But the ordinary case is a small payload (header_len == 2); for
            // extended lengths the key goes after those bytes. We rebuild the
            // whole header here for clarity.
            char rebuilt[14];
            rebuilt[0] = fin_opcode;
            std::uint64_t l = payload.size();
            std::size_t n = 1;
            if (l <= 125) {
                rebuilt[n++] = static_cast<char>(0x80 | l);
            } else if (l <= 0xFFFF) {
                rebuilt[n++] = static_cast<char>(0x80 | 126);
                rebuilt[n++] = static_cast<char>((l >> 8) & 0xFF);
                rebuilt[n++] = static_cast<char>(l & 0xFF);
            } else {
                rebuilt[n++] = static_cast<char>(0x80 | 127);
                for (int i = 7; i >= 0; --i)
                    rebuilt[n++] = static_cast<char>((l >> (8 * i)) & 0xFF);
            }
            for (auto b : mask_key)
                rebuilt[n++] = static_cast<char>(b);
            std::memcpy(req.header, rebuilt, n);
            req.header_len = n;
            ws_unmask(payload.data(), payload.size(), mask_key);
        }
        req.owned = std::move(payload);
        req.done = std::move(done);
        req.close_after = close_after;
        return req;
    }

    // Builds a queued frame that BORROWS the caller's bytes instead of owning
    // them. Only valid for the server side: a client must mask every frame,
    // which rewrites the payload in place, so the client goes through make_req
    // with an owned copy. Compression also rewrites the payload, so a
    // compressed write always takes the owned path too (rsv1 stays false here).
    static WriteReq make_borrowed_req(WsOpcode opcode, std::string_view payload,
                                      std::optional<asio::any_completion_handler<void(error_code)>> done,
                                      bool close_after = false, bool rsv1 = false) {
        WriteReq req;
        req.header_len = ws_encode_header(req.header, opcode, payload.size(), rsv1);
        req.borrowed = payload;
        req.borrowed_payload = true;
        req.done = std::move(done);
        req.close_after = close_after;
        return req;
    }

  public:
    // `expect_masked`: server side (default) reads masked client frames; the
    // client side (open_websocket) reads unmasked server frames. See
    // WsFrameParser.
    //
    // `inbox_limit`: bytes of undelivered inbound messages the internal reader may
    // buffer before it stops reading the socket. This is the only knob on the
    // read-ahead that keeps a peer which pipelines without reading from
    // deadlocking the connection (see read()); the default is a small multiple of
    // a typical socket buffer, so ordinary request/response use never comes close.
    explicit WsBackendImpl(std::shared_ptr<Transport> transport, std::uint64_t max_payload = 16u * 1024 * 1024,
                           std::chrono::steady_clock::duration idle_timeout = std::chrono::seconds(120),
                           bool expect_masked = true, std::size_t inbox_limit = 4u * 1024 * 1024,
                           WsDeflateConfig deflate = {})
        : m_transport(std::move(transport)), m_executor(m_transport->get_executor()),
          m_parser(max_payload, expect_masked, deflate.enabled), m_max_payload(max_payload),
          m_idle_timeout(idle_timeout), m_inbox_limit(inbox_limit != 0 ? inbox_limit : 4u * 1024 * 1024),
          m_expect_masked(expect_masked), m_deflate(std::move(deflate)),
          m_watchdog_timer(std::make_shared<asio::steady_timer>(m_executor)), m_notify(m_executor, 1),
          m_inbox_ready(m_executor, 1), m_inbox_space(m_executor, 1) {
        m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
        if (m_deflate.enabled) {
            m_out = std::make_unique<WsDeflater>(m_deflate.server_window_bits, m_compression_level);
            m_in = std::make_unique<WsInflater>(m_deflate.client_window_bits);
            m_compress_active = m_out->ok() && m_in->ok();
            if (!m_compress_active) {
                // zlib failed to init a stream: the connection proceeds
                // uncompressed rather than failing the whole upgrade — the peer
                // negotiated best-effort compression, and an uncompressed
                // message is always legal once negotiated.
                m_out.reset();
                m_in.reset();
                m_deflate.enabled = false;
            }
            // Negotiation succeeded: compress outbound by default, exactly like
            // gorilla's EnableWriteCompression default (a live handle can turn
            // it off per connection).
            m_write_compression = m_compress_active;
        }
    }

    // Hands bytes already read past the upgrade request to the parser, so a
    // client that pipelines its first frame behind the handshake does not lose
    // it.
    void feed(std::string_view bytes) override {
        if (!bytes.empty()) {
            m_parser.append(reinterpret_cast<const std::byte *>(bytes.data()), bytes.size());
        }
    }

    // Reads one complete message from the inbound queue. The transport itself is
    // drained by an internal reader coroutine (started on the first read), so an
    // application coroutine blocked writing while the peer has stopped reading no
    // longer deadlocks the connection: the socket keeps draining into a bounded
    // queue regardless of what the application is doing. Reassembling fragments
    // and answering Ping/Close happen in that reader, so the delivered message
    // keeps the same shape as before.
    //
    // Back-pressure is preserved, just moved: the reader stops pulling bytes once
    // `inbox_limit` bytes are buffered and resumes when read() drains some.
    asio::awaitable<std::expected<WsMessage, error_code>> read() override {
        co_await hop(); // the queue is executor-confined
        ensure_reader();
        for (;;) {
            if (!m_inbox.empty()) {
                WsMessage msg = std::move(m_inbox.front());
                m_inbox.pop_front();
                m_inbox_bytes -= kInboxMsgOverhead + msg.data.size();
                (void)m_inbox_space.try_send(error_code{}); // room for the reader
                co_return msg;
            }
            if (m_reader_done) {
                co_await flush_deferred_close();
                co_return std::unexpected(reader_error());
            }
            auto [ec] = co_await m_inbox_ready.async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec && m_inbox.empty()) {
                co_await flush_deferred_close();
                co_return std::unexpected(reader_error());
            }
        }
    }

    // Sends a Close the reader asked for (protocol error, received Close, ...) at
    // the point the application has drained every message that preceded it. Doing
    // it here rather than in the reader is what keeps ordering: the application
    // writes its echoes before it next calls read(), so the Close lands behind
    // them on the wire - exactly where the old (read-driven) implementation put
    // it. Idempotent: once sent, m_close_code is cleared.
    asio::awaitable<void> flush_deferred_close() {
        if (m_close_code != 0) {
            const std::uint16_t code = m_close_code;
            m_close_code = 0;
            co_await close_with(ws_close_payload(code));
        }
    }

    // Starts the reader coroutine once, on the connection executor. It holds a
    // reference to the backend (like the write pump) so it cannot outlive the
    // transport it reads through.
    void ensure_reader() {
        if (m_reader_started)
            return;
        m_reader_started = true;
        asio::co_spawn(m_executor, reader_loop(this->shared_from_this()), asio::detached);
    }

    // The terminal error handed to read(): the reader's own error, or an abort
    // when teardown stopped it without one.
    [[nodiscard]] error_code reader_error() const {
        return m_reader_ec ? m_reader_ec : make_error_code(asio::error::operation_aborted);
    }

    // The single reader: parse messages off the transport into the inbox until the
    // connection ends or the inbox limit is hit. Bounded, so a peer that never
    // reads cannot grow it without limit; once full the reader simply waits for
    // read() to make room.
    static asio::awaitable<void> reader_loop(std::shared_ptr<WsBackendImpl> self) {
        for (;;) {
            while (self->m_open && self->m_inbox_bytes >= self->m_inbox_limit) {
                auto [ec] = co_await self->m_inbox_space.async_receive(asio::as_tuple(asio::use_awaitable));
                if (ec)
                    break; // teardown closed the channel
            }
            if (!self->m_open)
                break;
            auto msg = co_await self->read_message();
            if (!msg) {
                self->m_reader_ec = msg.error();
                break;
            }
            self->m_inbox_bytes += kInboxMsgOverhead + msg->data.size();
            self->m_inbox.push_back(std::move(*msg));
            (void)self->m_inbox_ready.try_send(error_code{}); // wake read(); coalescing is fine
        }
        self->m_reader_done = true;
        (void)self->m_inbox_ready.try_send(error_code{});
        co_return;
    }

    // Wakes both readers out of a waiting state. Idempotent.
    void stop_inbox() {
        m_inbox_ready.close();
        m_inbox_space.close();
    }

    // Internal: reads/parses one message straight off the transport (reassembling
    // fragments, transparently answering Ping with Pong / Close with Close).
    // Returns std::unexpected(ec) on close or transport error. Runs only inside
    // reader_loop, on the connection executor, and is the single owner of
    // m_parser - which is what made moving the read off the application's stack
    // safe.
    asio::awaitable<std::expected<WsMessage, error_code>> read_message() {
        std::string message;
        WsOpcode message_type = WsOpcode::Text;
        bool assembling = false;
        // Whether the message being assembled is a compressed one (its first
        // data frame carried RSV1, RFC 7692): its octets then go through the
        // inflater instead of the message buffer, and UTF-8 is only checkable
        // once the whole thing is decompressed.
        bool compressed = false;
        // Per-message UTF-8 state (RFC 6455 §8.1). It has to outlive a frame
        // because a codepoint may straddle a fragment boundary; binary messages
        // never touch it.
        Utf8Validator utf8;

        for (;;) {
            WsFrame frame;
            auto status = m_parser.next(frame);

            if (status == WsFrameParser::Status::Error) {
                // A malformed frame (bad opcode, RSV2/RSV3 set, unmasked, oversize
                // control frame) is a protocol error: §7.1.7 wants the connection
                // failed with 1002, not dropped without a word.
                m_close_code = 1002;
                co_return std::unexpected(make_error_code(asio::error::invalid_argument));
            }
            if (status == WsFrameParser::Status::NeedMore) {
                // Validate a text message as it arrives rather than once its frame
                // is whole: §8.1 fails the connection when the invalid octet shows
                // up, and one frame can be split across TCP segments — which is
                // exactly what Autobahn's 6.4.3/6.4.4 exercise. A compressed
                // message cannot be inspected this way — its octets are still
                // deflated — so its UTF-8 check happens after decompression.
                if (!compressed && !m_parser.pending_rsv1()) {
                    if (auto in_flight = m_parser.pending_opcode()) {
                        const bool is_text =
                            *in_flight == WsOpcode::Text ||
                            (*in_flight == WsOpcode::Continuation && assembling && message_type == WsOpcode::Text);
                        if (is_text) {
                            auto partial = m_parser.take_partial_payload();
                            if (*in_flight == WsOpcode::Text && partial.first) {
                                utf8 = Utf8Validator{}; // a new message starts here
                            }
                            if (!partial.bytes.empty() && !utf8.feed(partial.bytes)) {
                                m_close_code = 1007;
                                co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                            }
                        }
                    }
                }
                std::array<std::byte, 8192> tmp; // no init: read_some fills [0,n)
                auto [ec, n] = co_await m_transport->async_read_some(std::span<std::byte>{tmp});
                if (ec) {
                    // On a half-closable transport (HTTP/2 RFC 8441 stream) an
                    // inbound EOF only ends the peer's sending direction: the
                    // application still owes a reply, so the backend must stay
                    // writable. Marking it closed here dropped the echo that a
                    // handler writes right after read() returns the last message.
                    if constexpr (!ws_read_eof_is_half_close<Transport>::value) {
                        m_open = false;
                    }
                    co_return std::unexpected(ec);
                }
                touch_deadline(); // inbound bytes: connection is active
                m_parser.append(tmp.data(), n);
                continue;
            }

            // A complete frame was decoded.
            switch (frame.opcode) {
            case WsOpcode::Close: {
                // §7.4: a Close may carry a status code and a reason, and both
                // are checked before anything is echoed back. A 1-octet payload
                // cannot hold a code at all, a code outside §7.4.1 is a
                // protocol error, and the reason has to be valid UTF-8.
                const std::string &payload = frame.payload;
                if (payload.size() == 1) {
                    m_close_code = 1002;
                    co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                }
                if (payload.size() >= 2) {
                    const auto code = static_cast<std::uint16_t>((static_cast<unsigned char>(payload[0]) << 8) |
                                                                 static_cast<unsigned char>(payload[1]));
                    if (!ws_valid_close_code(code)) {
                        m_close_code = 1002;
                        co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                    }
                    Utf8Validator reason;
                    if (!reason.feed(std::string_view{payload}.substr(2)) || !reason.complete()) {
                        m_close_code = 1007;
                        co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                    }
                }
                // Answer with a Close and stop (RFC 6455 §5.5.1). The reply is
                // sent by read() once the application has drained the messages
                // that arrived before it, so the peer's Close is ordered behind
                // every echo the application still owes (see read()).
                m_close_code = 1000;
                co_return std::unexpected(make_error_code(asio::error::eof));
            }

            case WsOpcode::Ping:
                co_await enqueue_control(WsOpcode::Pong, std::move(frame.payload));
                continue;

            case WsOpcode::Pong:
                continue; // ignore, keep reading

            case WsOpcode::Text:
            case WsOpcode::Binary:
                if (assembling) {
                    // A new data frame before the previous message's FIN.
                    m_close_code = 1002;
                    co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                }
                message_type = frame.opcode;
                assembling = true;
                compressed = m_compress_active && frame.rsv1;
                if (compressed) {
                    message.clear();
                    if (auto ec = inflate_feed(frame.payload, message)) {
                        co_return std::unexpected(*ec);
                    }
                } else {
                    if (frame.rsv1) {
                        // RSV1 reached us without a live inflater (compression
                        // failed to initialize, or a peer set it unnegotiated):
                        // RFC 6455 §5.2 makes that a protocol error.
                        m_close_code = 1002;
                        co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                    }
                    message = std::move(frame.payload);
                    if (message_type == WsOpcode::Text) {
                        if (frame.already_delivered == 0) {
                            // Nothing was inspected while the frame was arriving;
                            // start this message from a clean slate.
                            utf8 = Utf8Validator{};
                        }
                        // Whatever the incremental check already saw is skipped,
                        // so no octet goes through the validator twice.
                        if (!utf8.feed(std::string_view{message}.substr(frame.already_delivered))) {
                            m_close_code = 1007;
                            co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                        }
                    }
                }
                if (frame.fin) {
                    if (compressed) {
                        if (auto ec = inflate_finish(message)) {
                            co_return std::unexpected(*ec);
                        }
                    }
                    if (message_type == WsOpcode::Text && (compressed ? !utf8_whole(message) : !utf8.complete())) {
                        // A sequence left half-read: the final codepoint was
                        // truncated by the end of the message (or, compressed,
                        // invalid octets surfaced by decompression).
                        m_close_code = 1007;
                        co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                    }
                    co_return WsMessage{std::move(message), message_type == WsOpcode::Text};
                }
                continue; // more fragments follow

            case WsOpcode::Continuation:
                if (!assembling) {
                    m_close_code = 1002;
                    co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                }
                if (frame.rsv1) {
                    // RFC 7692 §7.2.2: RSV1 is set on the first frame of a
                    // compressed message, never on continuations.
                    m_close_code = 1002;
                    co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                }
                if (compressed) {
                    if (auto ec = inflate_feed(frame.payload, message)) {
                        co_return std::unexpected(*ec);
                    }
                } else {
                    if (message.size() + frame.payload.size() > m_max_payload) {
                        // Bound the reassembled message size (a flood of small
                        // fragments must not exhaust memory). §7.4.1: 1009.
                        m_close_code = 1009;
                        co_return std::unexpected(make_error_code(asio::error::message_size));
                    }
                    if (message_type == WsOpcode::Text &&
                        !utf8.feed(std::string_view{frame.payload}.substr(frame.already_delivered))) {
                        // Fail fast across fragments too — that is the whole point
                        // of carrying the validator between frames.
                        m_close_code = 1007;
                        co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                    }
                    message.append(frame.payload);
                }
                if (frame.fin) {
                    if (compressed) {
                        if (auto ec = inflate_finish(message)) {
                            co_return std::unexpected(*ec);
                        }
                    }
                    if (message_type == WsOpcode::Text && (compressed ? !utf8_whole(message) : !utf8.complete())) {
                        m_close_code = 1007;
                        co_return std::unexpected(make_error_code(asio::error::invalid_argument));
                    }
                    co_return WsMessage{std::move(message), message_type == WsOpcode::Text};
                }
                continue;
            }
        }
    }

    // Feeds one frame's payload into the inflater (RFC 7692; the payload of a
    // compressed message arrives as deflated octets across its fragments).
    // nullopt on success; on failure the returned error_code is the terminal
    // read error and m_close_code carries the Close code the peer should see.
    // Only ever reached when compression was negotiated (m_compress_active).
    std::optional<error_code> inflate_feed(std::string_view data, std::string &out) {
        switch (m_in->feed(data, out, m_max_payload)) {
        case WsInflateStatus::Ok:
            return std::nullopt;
        case WsInflateStatus::Overflow:
            m_close_code = 1009; // §7.4.1: the *decompressed* message is too big
            return make_error_code(asio::error::message_size);
        case WsInflateStatus::Error:
        default:
            m_close_code = 1002; // §7.1.7: failed to decompress
            return make_error_code(asio::error::invalid_argument);
        }
    }

    // Terminates a compressed message: flushes the decoder (feeding back the
    // 4-octet marker the sender stripped) and resets its window when the peer
    // negotiated client_no_context_takeover. See inflate_feed for the result.
    std::optional<error_code> inflate_finish(std::string &out) {
        switch (m_in->finish(out, m_max_payload, m_deflate.client_no_context_takeover)) {
        case WsInflateStatus::Ok:
            return std::nullopt;
        case WsInflateStatus::Overflow:
            m_close_code = 1009;
            return make_error_code(asio::error::message_size);
        case WsInflateStatus::Error:
        default:
            m_close_code = 1002;
            return make_error_code(asio::error::invalid_argument);
        }
    }

    // Whole-message UTF-8 check, used by the compressed path where the octets
    // only exist once decompressed (RFC 6455 §8.1 still applies).
    static bool utf8_whole(std::string_view s) {
        Utf8Validator v;
        return v.feed(s) && v.complete();
    }

    // Enqueue an OWNED message and await its write completion. `data` was moved
    // in (or copied once) by the caller, so the frame owns its bytes. The write
    // pump serializes the actual async_writes. This is safe from any coroutine.
    asio::awaitable<error_code> write(std::string data, bool text) override {
        co_await hop();
        if (!m_open) {
            co_return make_error_code(asio::error::not_connected);
        }
        bool rsv1 = false;
        if (m_compress_active && m_write_compression) {
            // Compress the whole message and drop the flush marker; the first
            // frame then carries RSV1 (RFC 7692 §7.2.1). An empty message stays
            // uncompressed (see WsDeflater::compress), so rsv1 follows the flag.
            std::string compressed;
            bool compressed_flag = false;
            if (!m_out->compress(data, compressed, m_deflate.server_no_context_takeover, compressed_flag)) {
                co_return make_error_code(asio::error::invalid_argument);
            }
            data = std::move(compressed);
            rsv1 = compressed_flag;
        }
        // The completion is delivered through the queued frame itself: the
        // awaitable pulls the awaiting coroutine's continuation out of the
        // pump's wake-up machinery and stores it in the WriteReq, so nothing is
        // allocated for each write beyond the queue node itself. Because the
        // initiation runs synchronously here (on the connection executor, after
        // the hop), the frame is queued before either the pump or a teardown can
        // observe it — no cross-coroutine window.
        const WsOpcode opcode = text ? WsOpcode::Text : WsOpcode::Binary;
        const bool mask = !m_expect_masked;
        auto token = asio::as_tuple(asio::use_awaitable);
        auto [ec] = co_await asio::async_initiate<decltype(token), void(error_code)>(
            [this, opcode, data = std::move(data), mask, rsv1](auto handler) mutable {
                m_pending.push_back(make_req(opcode, std::move(data), std::move(handler), false, mask, rsv1));
                (void)m_notify.try_send(error_code{}); // wake the pump; coalescing is fine
            },
            token);
        co_return ec;
    }

    // Enqueue a BORROWED message (server fast path): the queued frame points
    // straight at `data` instead of copying it, so `data` must stay valid until
    // this coroutine resumes (i.e. across the caller's co_await). On the client
    // the payload must be masked in place, so borrowing is impossible and this
    // delegates to the owning write() with one copy. Compression likewise
    // rewrites the payload, so a compressed write always owns a copy.
    asio::awaitable<error_code> write_view(std::string_view data, bool text) override {
        if (!m_expect_masked) {
            // Client: mask rewrites the bytes, so own a copy.
            co_return co_await write(std::string(data), text);
        }
        if (m_compress_active && m_write_compression) {
            // Compression rewrites the payload: own a copy (write() does).
            co_return co_await write(std::string(data), text);
        }
        co_await hop();
        if (!m_open) {
            co_return make_error_code(asio::error::not_connected);
        }
        const WsOpcode opcode = text ? WsOpcode::Text : WsOpcode::Binary;
        auto token = asio::as_tuple(asio::use_awaitable);
        auto [ec] = co_await asio::async_initiate<decltype(token), void(error_code)>(
            [this, opcode, data](auto handler) mutable {
                m_pending.push_back(make_borrowed_req(opcode, data, std::move(handler)));
                (void)m_notify.try_send(error_code{}); // wake the pump; coalescing is fine
            },
            token);
        co_return ec;
    }

    // Sends a Ping carrying `payload`, waiting for it to reach the wire. The
    // reader answers an inbound Ping automatically; this is the application's
    // own liveness probe (RFC 6455 §5.5.2 caps the payload at 125 octets).
    asio::awaitable<error_code> ping(std::string payload) override {
        co_await hop();
        if (!m_open) {
            co_return make_error_code(asio::error::not_connected);
        }
        if (payload.size() > 125) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        const bool mask = !m_expect_masked;
        auto token = asio::as_tuple(asio::use_awaitable);
        auto [ec] = co_await asio::async_initiate<decltype(token), void(error_code)>(
            [this, payload = std::move(payload), mask](auto handler) mutable {
                m_pending.push_back(make_req(WsOpcode::Ping, std::move(payload), std::move(handler), false, mask));
                (void)m_notify.try_send(error_code{}); // wake the pump; coalescing is fine
            },
            token);
        co_return ec;
    }

    // Graceful close: enqueue a Close frame and wait for the write pump to send
    // it, then tear down the transport. Never closes the transport before the
    // Close frame is on the wire.
    asio::awaitable<error_code> close() override {
        co_await close_with(ws_close_payload(1000));
        co_return error_code{};
    }

    // Graceful close with an application-level code and reason (RFC 6455
    // §5.5.1); see the interface comment for the constraints.
    asio::awaitable<error_code> close(std::uint16_t code, std::string reason) override {
        if (!ws_valid_close_code(code)) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        if (reason.size() > 123) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        Utf8Validator reason_utf8;
        if (!reason_utf8.feed(reason) || !reason_utf8.complete()) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        std::string payload = ws_close_payload(code);
        payload += reason;
        co_await close_with(std::move(payload));
        co_return error_code{};
    }

    // Sends a Close carrying a whole payload (code + reason), waits for it to
    // reach the wire, then tears the transport down. The wait is the
    // load-bearing part: a peer decides whether the connection was failed
    // correctly from the code it receives, so the frame has to be out before the
    // socket goes — the same reason close() waits rather than just marking the
    // connection shut.
    asio::awaitable<void> close_with(std::string payload) {
        co_await hop();
        if (m_open) {
            m_open = false;
            // Queue the Close frame and wait until the pump has actually written
            // it. The wait is the load-bearing part: a peer decides whether the
            // connection was failed correctly from the code it receives, so the
            // frame has to be out before the socket goes — the same reason
            // close() waits rather than just marking the connection shut.
            const bool mask = !m_expect_masked;
            auto token = asio::as_tuple(asio::use_awaitable);
            auto [ec] = co_await asio::async_initiate<decltype(token), void(error_code)>(
                [this, payload = std::move(payload), mask](auto handler) mutable {
                    m_pending.push_back(make_req(WsOpcode::Close, std::move(payload), std::move(handler),
                                                 /*close_after=*/true, mask));
                    (void)m_notify.try_send(error_code{});
                },
                token);
            (void)ec;
        }
        m_notify.close(); // stop the write pump if it is still running
        stop_inbox();     // stop the reader (and any read() parked on the queue)
        fail_pending_writes();
        if (m_watchdog_timer)
            m_watchdog_timer->cancel(); // stop the idle watchdog
        m_transport->close();
        co_return;
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
    // last real owner, releases the backend once any in-flight write has
    // finished. Cancelling the watchdog timer wakes its coroutine immediately; it
    // then sees the closed state (or a failed weak_ptr lock) and exits without
    // waiting out the idle timeout. Callable from any thread. dispatch() gets
    // both cases right: on the connection's executor the teardown runs inline, so
    // destroying a handle there behaves exactly as it always did; from anywhere
    // else it is queued onto that executor - and crucially never run in place,
    // since everything it touches is state the executor also owns.
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
        stop_inbox();
        fail_pending_writes();
        m_watchdog_timer->cancel();
        // The reader may be parked in async_read_some; closing the transport is
        // what unblocks it so the backend (which the reader holds a reference to)
        // can actually die. Idempotent with close()/close_with().
        m_transport->close();
    }

    // Drops every queued frame and fails its waiter. Runs on the connection
    // executor; the deque is ours, so this is ordinary destruction - no asio
    // operation is involved, which is exactly the point. Each abandoned frame's
    // completion handler resumes its waiter with the error, so a writer parked
    // on a frame that will never reach the wire gets an error instead of
    // hanging.
    void fail_pending_writes() {
        while (!m_pending.empty()) {
            WriteReq req = std::move(m_pending.front());
            m_pending.pop_front();
            if (req.done) {
                (*req.done)(make_error_code(asio::error::operation_aborted));
            }
        }
    }

    bool is_open() const override { return m_open; }

    // Per-connection outbound compression toggle. Inert unless the extension
    // was negotiated (an uncompressed message is legal after negotiation, so a
    // toggle to false is always safe; a toggle to true on a connection that
    // never negotiated does nothing). Hops onto the connection executor.
    asio::awaitable<error_code> enable_write_compression(bool enable) override {
        co_await hop();
        if (m_compress_active) {
            m_write_compression = enable;
        }
        co_return error_code{};
    }

    // Per-connection outbound DEFLATE level: 0 = zlib default, 1..9. Applies
    // from the next message; the LZ77 window is preserved. Refused with
    // invalid_argument for an out-of-range level; inert (but accepted) when the
    // extension was not negotiated.
    asio::awaitable<error_code> set_compression_level(int level) override {
        if (level != 0 && (level < 1 || level > 9)) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        co_await hop();
        if (m_compress_active && !m_out->set_level(level)) {
            co_return make_error_code(asio::error::invalid_argument);
        }
        m_compression_level = level;
        co_return error_code{};
    }

    // Snapshot (like is_open), safe from any thread: whether permessage-deflate
    // was negotiated for this connection.
    bool compression_negotiated() const override { return m_compress_active; }

  private:
    // Single write pump: serializes all async_writes for this connection.
    static asio::awaitable<void> pump(std::shared_ptr<WsBackendImpl> self) {
        for (;;) {
            if (self->m_pending.empty()) {
                auto [qec] = co_await self->m_notify.async_receive(asio::as_tuple(asio::use_awaitable));
                if (qec) {
                    break; // closed -> connection is going away
                }
                continue; // drain whatever got queued
            }
            WriteReq req = std::move(self->m_pending.front());
            self->m_pending.pop_front();
            auto wec = co_await self->write_all(req);
            if (wec) {
                self->m_open = false;
            }
            if (req.done) {
                (*req.done)(wec); // deliver the result to the waiter
            }
            if (wec || req.close_after) {
                break; // transport error, or a Close frame just went out
            }
        }
        // Terminal by construction: once this loop is left, no pump will ever run
        // again. Anything still queued would therefore wait forever — a waiter is
        // parked on its frame's completion handler with no timeout, and nothing
        // else signals it — so releasing them here is what turns "the transport
        // died" into an error its callers can actually see. Idempotent: close()
        // may already have done this.
        self->m_open = false;
        self->fail_pending_writes();
        self->m_notify.close();
        co_return;
    }

    // Re-enter the connection's executor regardless of the caller's context, so
    // touching m_open / m_parser / m_transport is always single-threaded.
    asio::awaitable<void> hop() { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    // Bound on the outbound queue, so a peer cannot grow it without limit.
    static constexpr std::size_t kMaxPendingFrames = 64;

    // Charged against the inbound read-ahead budget for every queued message, on
    // top of its payload: the WsMessage struct plus its deque node. Without it a
    // peer could spend a byte-based budget on millions of one-byte messages and
    // make the real footprint many times the limit.
    static constexpr std::size_t kInboxMsgOverhead = 128;

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
        m_pending.push_back(make_req(opcode, std::move(payload), std::nullopt, close_after, /*mask=*/!m_expect_masked));
        (void)m_notify.try_send(error_code{});
        co_return;
    }

    // Writes all of `frame` to the transport, looping over partial writes.
    // Sends one queued frame: the header (serialized into the request) and the
    // payload (the caller's buffer) as a single scatter-gather write, so no
    // per-frame concatenation buffer is ever allocated.
    asio::awaitable<error_code> write_all(const WriteReq &req) {
        touch_deadline(); // outbound bytes: connection is active
        const std::string_view payload = req.payload();
        const std::array<ConstByteSpan, 2> bufs{
            std::as_bytes(std::span<const char>{req.header, req.header_len}),
            std::as_bytes(std::span<const char>{payload.data(), payload.size()}),
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
            asio::steady_timer *timer = nullptr;
            {
                auto self = weak.lock();
                if (!self || !self->m_open) {
                    co_return; // connection gone or already closing: stop watching
                }
                timer = self->m_watchdog_timer.get();
                timer->expires_at(self->m_deadline);
                // `self` is released at the end of this scope, before the await
                // below, so the suspended wait does not keep the backend alive.
            }
            auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
            if (ec == asio::error::operation_aborted) {
                co_return; // timer cancelled or backend destroyed: stop watching
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
                self->stop_inbox();
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
    std::size_t m_inbox_limit;  // read-ahead cap (bytes)
    bool m_expect_masked{true}; // client side (false) masks its writes / reads unmasked frames
    // permessage-deflate (RFC 7692). m_deflate always exists (default disabled);
    // the zlib streams are created only when it is enabled. m_compress_active is
    // the single "is compression live" gate (an init failure degrades to
    // uncompressed rather than failing the upgrade); m_write_compression is the
    // per-connection outbound toggle (gorilla Conn.EnableWriteCompression), and
    // m_compression_level the outbound DEFLATE level (0 = default, 1..9).
    WsDeflateConfig m_deflate;
    bool m_compress_active{false};
    bool m_write_compression{false};
    int m_compression_level{0};
    std::unique_ptr<WsDeflater> m_out;                    // the outbound encoder
    std::unique_ptr<WsInflater> m_in;                     // the inbound decoder
    std::shared_ptr<asio::steady_timer> m_watchdog_timer; // cancelled on teardown
    WriteQueue m_pending;                                 // queued frames: owned by us, cleared on teardown
    Notify m_notify;                                      // pump wake-up signal (carries only a trivial error_code)
    // Inbound read-ahead: reader_loop fills m_inbox from the transport, read()
    // drains it. All executor-confined, like everything else in this class.
    std::deque<WsMessage> m_inbox;
    std::size_t m_inbox_bytes{0};
    bool m_reader_started{false};
    bool m_reader_done{false};
    error_code m_reader_ec;
    // Close code the reader wants sent once the application has drained the
    // messages that preceded it (0 = nothing pending). See flush_deferred_close().
    std::uint16_t m_close_code{0};
    Notify m_inbox_ready; // reader -> read() wake-up
    Notify m_inbox_space; // read() -> reader (room freed)
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
    // serialized internally. The payload is MOVED into the operation, so the
    // awaitable owns its bytes: building it and awaiting it later is safe.
    asio::awaitable<error_code> write_text(std::string data) { return m_backend->write(std::move(data), true); }
    asio::awaitable<error_code> write_binary(std::string data) { return m_backend->write(std::move(data), false); }
    asio::awaitable<error_code> write(std::string data, bool text) { return m_backend->write(std::move(data), text); }

    // Borrowing variants (server fast path): no payload copy. The bytes are
    // borrowed until the operation completes, so `data` must stay valid across
    // the co_await (a direct `co_await ws->write_text_view(buf)` does). Use these
    // when sending a buffer that is already alive for the duration of the write;
    // otherwise prefer the owning write_text/write_binary/write above. On the
    // client these still copy once, because it must mask the payload in place.
    asio::awaitable<error_code> write_text_view(std::string_view data) { return m_backend->write_view(data, true); }
    asio::awaitable<error_code> write_binary_view(std::string_view data) { return m_backend->write_view(data, false); }
    asio::awaitable<error_code> write_view(std::string_view data, bool text) {
        return m_backend->write_view(data, text);
    }

    // Application-level liveness probe: sends a Ping and waits for the pump to
    // put it on the wire (RFC 6455 §5.5.2). The reader answers an inbound Ping
    // with a matching Pong automatically; this is for the side that wants to
    // keep ITS peer honest. `payload` must be ≤ 125 octets.
    asio::awaitable<error_code> ping(std::string payload = {}) { return m_backend->ping(std::move(payload)); }

    // Graceful close: sends a Close frame (default code 1000, no reason), then
    // tears the connection down.
    asio::awaitable<error_code> close() { return m_backend->close(); }

    // Graceful close with an application-level status code and reason (RFC 6455
    // §5.5.1). The code must be one a peer may legitimately receive — use the
    // standard set (1000-1003, 1007-1011, 3000-4999) — and the UTF-8 reason is
    // capped at 123 octets; anything else is refused with invalid_argument.
    asio::awaitable<error_code> close(std::uint16_t code, std::string reason = {}) {
        return m_backend->close(code, std::move(reason));
    }

    // --- per-connection compression controls (gorilla Conn shape) ---
    // These are meaningful only once permessage-deflate was negotiated; on a
    // connection that did not, they are inert. No #ifdef: the API is identical
    // in every build, and the runtime toggle is how a handler customizes one
    // connection after the upgrade.
    //
    // `enable_write_compression(false)` stops compressing this connection's
    // outbound messages (RFC 7692 allows any message uncompressed); true turns
    // it back on. Both hop onto the connection executor.
    asio::awaitable<error_code> enable_write_compression(bool enable) {
        return m_backend->enable_write_compression(enable);
    }
    // The outbound DEFLATE level: 0 = zlib default, 1 (fastest) .. 9 (smallest).
    // Applies from the next message; out-of-range is invalid_argument.
    asio::awaitable<error_code> set_compression_level(int level) { return m_backend->set_compression_level(level); }
    // Whether this connection negotiated permessage-deflate (a snapshot, safe
    // from any thread).
    bool compression_negotiated() const { return m_backend->compression_negotiated(); }

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

} // namespace simple_http
