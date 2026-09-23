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
// ws_frame.h (ported from paozhu's websockets_parse.cpp) and reads/writes bytes
// straight through the simple_http Transport concept (async_read_some /
// async_write). No boost::beast::websocket::stream.
//
// Concurrency (model A): a connection is pinned to one single-threaded
// io_context. Every public operation (read/write/close) first hops onto that
// connection's executor before touching connection state (m_open, the parser,
// the transport), so the handle is safe to use from any coroutine or thread —
// matching the guarantee the HTTP/2 ResponseWriter already gives. In addition,
// all writes are serialized through an internal write pump: write_* enqueue a
// frame and await their own completion while a single pump coroutine
// (run_writer, started by the engine) performs the async_writes one at a time.
//
// Fragmentation and control frames are handled inside read(): continuation
// frames are reassembled into one message (bounded by max_payload), Ping is
// answered with Pong, and a received Close is answered with a Close before the
// read reports end-of-stream.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "../core/types.h"
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
// to it, so handlers never see the transport type.
class WsBackend {
  public:
    virtual ~WsBackend() = default;
    virtual asio::awaitable<std::expected<WsMessage, error_code>> read() = 0;
    virtual asio::awaitable<error_code> write(std::string data, bool text) = 0;
    virtual asio::awaitable<error_code> close() = 0;
    virtual asio::awaitable<void> run_writer() = 0;  // the serializing write pump
    virtual bool is_open() const = 0;
};

// Concrete backend over a simple_http Transport (TCP plain or TLS). Reads whole
// messages via WsFrameParser and serializes all writes through a write pump.
template <typename Transport>
class WsBackendImpl final : public WsBackend {
    using Executor = decltype(std::declval<Transport&>().get_executor());

    // One queued outbound frame + a channel to deliver its write result.
    using ResultChannel = asio::experimental::concurrent_channel<void(error_code)>;
    struct WriteReq {
        std::string frame;                     // already-serialized WebSocket frame bytes
        std::shared_ptr<ResultChannel> done;   // null for fire-and-forget (auto Pong)
        bool close_after = false;              // stop the pump once this frame is written
    };
    using WriteQueue = asio::experimental::concurrent_channel<void(error_code, WriteReq)>;

  public:
    explicit WsBackendImpl(std::shared_ptr<Transport> transport, std::uint64_t max_payload = 16u * 1024 * 1024)
        : m_transport(std::move(transport)),
          m_executor(m_transport->get_executor()),
          m_parser(max_payload),
          m_max_payload(max_payload),
          m_write_q(m_executor, 1024) {}

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
                std::array<std::byte, 8192> tmp{};
                auto [ec, n] = co_await m_transport->async_read_some(std::span<std::byte>{tmp});
                if (ec) {
                    m_open = false;
                    co_return std::unexpected(ec);
                }
                m_parser.append(tmp.data(), n);
                continue;
            }

            // A complete frame was decoded.
            switch (frame.opcode) {
                case WsOpcode::Close:
                    // Answer with a Close (echoing status if present) and stop.
                    m_open = false;
                    co_await enqueue_control(ws_encode_close(), /*close_after=*/true);
                    co_return std::unexpected(make_error_code(asio::error::eof));

                case WsOpcode::Ping:
                    co_await enqueue_control(ws_encode_pong(frame.payload));
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
        std::string frame = ws_encode_frame(text ? WsOpcode::Text : WsOpcode::Binary, data);
        auto done = std::make_shared<ResultChannel>(m_executor, 1);
        WriteReq req{std::move(frame), done, false};
        auto [send_ec] =
            co_await m_write_q.async_send(error_code{}, std::move(req), asio::as_tuple(asio::use_awaitable));
        if (send_ec) {
            m_open = false;
            co_return make_error_code(asio::error::not_connected);
        }
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
            WriteReq req{ws_encode_close(), done, /*close_after=*/true};
            auto [send_ec] =
                co_await m_write_q.async_send(error_code{}, std::move(req), asio::as_tuple(asio::use_awaitable));
            if (!send_ec) {
                // Wait for the pump to actually write the Close frame.
                co_await done->async_receive(asio::as_tuple(asio::use_awaitable));
            }
        }
        m_write_q.close();  // stop the write pump if it is still running
        m_transport->close();
        co_return error_code{};
    }

    // Single write pump: serializes all async_writes for this connection. Runs
    // on the connection executor (started by the engine).
    asio::awaitable<void> run_writer() override {
        for (;;) {
            auto [qec, req] = co_await m_write_q.async_receive(asio::as_tuple(asio::use_awaitable));
            if (qec) {
                break;  // queue closed -> connection is going away
            }
            auto wec = co_await write_all(req.frame);
            if (wec) {
                m_open = false;
            }
            if (req.done) {
                (void)req.done->try_send(wec);  // deliver result to the waiter
            }
            if (wec || req.close_after) {
                break;  // transport error, or a Close frame just went out
            }
        }
        co_return;
    }

    bool is_open() const override { return m_open; }

  private:
    // Re-enter the connection's executor regardless of the caller's context, so
    // touching m_open / m_parser / m_transport is always single-threaded.
    asio::awaitable<void> hop() {
        co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable));
    }

    // Enqueue a control frame (Pong/Close). Fire-and-forget for Pong; the caller
    // is already on the connection executor (read() hopped). If the queue is
    // closed/full the connection is going away, so mark it not-open.
    asio::awaitable<void> enqueue_control(std::string frame, bool close_after = false) {
        WriteReq req{std::move(frame), nullptr, close_after};
        auto [ec] =
            co_await m_write_q.async_send(error_code{}, std::move(req), asio::as_tuple(asio::use_awaitable));
        if (ec) {
            m_open = false;
        }
        co_return;
    }

    // Writes all of `frame` to the transport, looping over partial writes.
    asio::awaitable<error_code> write_all(const std::string& frame) {
        std::size_t sent = 0;
        auto bytes = std::as_bytes(std::span<const char>{frame.data(), frame.size()});
        while (sent < frame.size()) {
            auto [ec, n] = co_await m_transport->async_write(bytes.subspan(sent));
            if (ec) co_return ec;
            sent += n;
        }
        co_return error_code{};
    }

    std::shared_ptr<Transport> m_transport;
    Executor m_executor;
    WsFrameParser m_parser;
    std::uint64_t m_max_payload;
    WriteQueue m_write_q;
    bool m_open{true};
};

// User-facing WebSocket handle. Forwards to the type-erased backend.
class WebSocket {
  public:
    explicit WebSocket(std::unique_ptr<WsBackend> backend) : m_backend(std::move(backend)) {}

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

    bool is_open() const { return m_backend->is_open(); }

    // Runs the serializing write pump (started by the engine on the connection
    // executor; keep the WebSocket alive for its duration).
    asio::awaitable<void> run_writer() { return m_backend->run_writer(); }

  private:
    std::unique_ptr<WsBackend> m_backend;
};

}  // namespace simple_http
