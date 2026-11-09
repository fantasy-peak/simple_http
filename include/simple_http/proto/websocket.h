#pragma once

// WebSocket: a transport-agnostic, full-duplex WebSocket connection
// (C++17 token-only port).
//
// Usage (handler receives a shared_ptr<WebSocket>):
//   ws->async_read(token);   // completion: void(error_code, WsMessage)
//   ws->async_write_text("hi", token);
//   ws->async_write_binary(bytes, token);
//
// Every operation is an Asio initiating function. The token decides how a
// failure is presented (callback value / as_tuple / use_awaitable exception).
//
// Concurrency (model A): a connection is pinned to one single-threaded
// io_context. Every public operation that touches connection state hops onto
// that executor first, so the handle is safe to use from any thread. is_open()
// is the single synchronous snapshot (an atomic). Writes are serialized by an
// internal write pump; an internal reader keeps draining the transport into a
// bounded inbox so a serialized read/write application cannot deadlock with a
// peer that stops reading.
//
// Fragmentation and control frames are handled inside the reader: continuation
// frames are reassembled, Ping is answered with Pong, and a received Close is
// answered with a Close before the read reports end-of-stream.

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../core/types.h"
#include "../transport/transport.h" // ByteSpan / ConstByteSpan
#include "response_writer.h"        // WriteCompletion
#include "ws_deflate.h"             // permessage-deflate (RFC 7692)
#include "ws_frame.h"

namespace simple_http {

namespace asio = boost::asio;

// Detects a Transport whose read-side EOF means only that this direction ended
// (an HTTP/2 extended-CONNECT stream's END_STREAM), not that the connection is
// gone: writes stay legal until the application closes.
template <typename T, typename = void> struct ws_read_eof_is_half_close : std::false_type {};
template <typename T>
struct ws_read_eof_is_half_close<T, std::void_t<decltype(T::kReadEofIsHalfClose)>> : std::true_type {};

// One received WebSocket message plus its type (text vs binary).
struct WsMessage {
    std::string data;
    bool text = true;
};

using WsReadCompletion = asio::any_completion_handler<void(error_code, WsMessage)>;

// Type-erased backend over a concrete Transport. Owned through a shared_ptr:
// the read and write pumps hold a reference to it, so the transport cannot be
// destroyed while an operation is still in flight.
class WsBackend {
  public:
    virtual ~WsBackend() = default;
    virtual void read(WsReadCompletion done) = 0;
    // Owning send: `data` is moved in and lives in the queue.
    virtual void write(std::string data, bool text, WriteCompletion done) = 0;
    // Borrowing send: `data` must stay valid until `done` runs.
    virtual void write_view(std::string_view data, bool text, WriteCompletion done) = 0;
    virtual void ping(std::string payload, WriteCompletion done) = 0;
    virtual void close(WriteCompletion done) = 0;
    virtual void close(std::uint16_t code, std::string reason, WriteCompletion done) = 0;
    virtual void enable_write_compression(bool enable, WriteCompletion done) = 0;
    virtual void set_compression_level(int level, WriteCompletion done) = 0;
    virtual bool compression_negotiated() const = 0;
    virtual void run_writer() = 0; // starts the serializing write pump + watchdog
    virtual bool is_open() const = 0;
    virtual void abort() = 0;
    virtual void feed(std::string_view bytes) = 0;
    virtual asio::any_io_executor get_executor() const = 0;
};

// Concrete backend over a simple_http Transport (TCP plain or TLS).
template <typename Transport>
class WsBackendImpl final : public WsBackend, public std::enable_shared_from_this<WsBackendImpl<Transport>> {
    struct WriteReq {
        std::string owned;
        std::string_view borrowed;
        bool borrowed_payload = false;
        char header[14]{};
        std::size_t header_len{0};
        std::optional<WriteCompletion> done;
        bool close_after = false;

        std::string_view payload() const { return borrowed_payload ? borrowed : std::string_view{owned}; }
    };
    using WriteQueue = std::deque<WriteReq>;
    using Notify = asio::experimental::channel<void(error_code)>;

    static WriteReq make_req(WsOpcode opcode, std::string payload, std::optional<WriteCompletion> done,
                             bool close_after = false, bool mask = false, bool rsv1 = false) {
        WriteReq req;
        req.header_len = ws_encode_header(req.header, opcode, payload.size(), rsv1);
        if (mask) {
            // Set the MASK bit and insert the 4-byte key between the length and
            // the payload; the header is rebuilt rather than patched because the
            // key lands after the length octets whatever width they are.
            unsigned char mask_key[4];
            if (!ws_fill_mask_key(mask_key)) {
                std::random_device rd;
                for (auto &b : mask_key)
                    b = static_cast<unsigned char>(rd() & 0xFF);
            }
            char rebuilt[14];
            rebuilt[0] = req.header[0];
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

    static WriteReq make_borrowed_req(WsOpcode opcode, std::string_view payload, std::optional<WriteCompletion> done,
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
                m_out.reset();
                m_in.reset();
                m_deflate.enabled = false;
            }
            m_write_compression = m_compress_active;
        }
    }

    void feed(std::string_view bytes) override {
        if (!bytes.empty()) {
            m_parser.append(reinterpret_cast<const std::byte *>(bytes.data()), bytes.size());
        }
    }

    asio::any_io_executor get_executor() const override { return m_executor; }

    // --- reads -------------------------------------------------------------

    void read(WsReadCompletion done) override {
        auto self = this->shared_from_this();
        hop([self, done = std::move(done)]() mutable { self->read_impl(std::move(done)); });
    }

    bool is_open() const override { return m_open; }
    bool compression_negotiated() const override { return m_compress_active; }

    // --- writes ------------------------------------------------------------

    void write(std::string data, bool text, WriteCompletion done) override {
        auto self = this->shared_from_this();
        hop([self, data = std::move(data), text, done = std::move(done)]() mutable {
            self->write_impl(std::move(data), text, std::move(done));
        });
    }

    void write_view(std::string_view data, bool text, WriteCompletion done) override {
        if (!m_expect_masked || (m_compress_active && m_write_compression)) {
            // Masking and compression both rewrite the payload, so own a copy.
            write(std::string(data), text, std::move(done));
            return;
        }
        const WsOpcode opcode = text ? WsOpcode::Text : WsOpcode::Binary;
        auto self = this->shared_from_this();
        hop([self, opcode, data, done = std::move(done)]() mutable {
            if (!self->m_open) {
                std::move(done)(make_error_code(asio::error::not_connected));
                return;
            }
            self->m_pending.push_back(make_borrowed_req(opcode, data, std::move(done)));
            (void)self->m_notify.try_send(error_code{});
        });
    }

    void ping(std::string payload, WriteCompletion done) override {
        auto self = this->shared_from_this();
        hop([self, payload = std::move(payload), done = std::move(done)]() mutable {
            if (!self->m_open) {
                std::move(done)(make_error_code(asio::error::not_connected));
                return;
            }
            if (payload.size() > 125) {
                std::move(done)(make_error_code(asio::error::invalid_argument));
                return;
            }
            const bool mask = !self->m_expect_masked;
            self->m_pending.push_back(make_req(WsOpcode::Ping, std::move(payload), std::move(done), false, mask));
            (void)self->m_notify.try_send(error_code{});
        });
    }

    void close(WriteCompletion done) override { close_with(ws_close_payload(1000), std::move(done)); }

    void close(std::uint16_t code, std::string reason, WriteCompletion done) override {
        if (!ws_valid_close_code(code)) {
            std::move(done)(make_error_code(asio::error::invalid_argument));
            return;
        }
        if (reason.size() > 123) {
            std::move(done)(make_error_code(asio::error::invalid_argument));
            return;
        }
        Utf8Validator reason_utf8;
        if (!reason_utf8.feed(reason) || !reason_utf8.complete()) {
            std::move(done)(make_error_code(asio::error::invalid_argument));
            return;
        }
        std::string payload = ws_close_payload(code);
        payload += reason;
        close_with(std::move(payload), std::move(done));
    }

    void enable_write_compression(bool enable, WriteCompletion done) override {
        auto self = this->shared_from_this();
        hop([self, enable, done = std::move(done)]() mutable {
            if (self->m_compress_active) {
                self->m_write_compression = enable;
            }
            std::move(done)(error_code{});
        });
    }

    void set_compression_level(int level, WriteCompletion done) override {
        if (level != 0 && (level < 1 || level > 9)) {
            std::move(done)(make_error_code(asio::error::invalid_argument));
            return;
        }
        auto self = this->shared_from_this();
        hop([self, level, done = std::move(done)]() mutable {
            if (self->m_compress_active && !self->m_out->set_level(level)) {
                std::move(done)(make_error_code(asio::error::invalid_argument));
                return;
            }
            self->m_compression_level = level;
            std::move(done)(error_code{});
        });
    }

    // --- pumps -------------------------------------------------------------

    void run_writer() override {
        auto self = this->shared_from_this();
        start_watchdog(std::weak_ptr<WsBackendImpl>(self));
        pump(std::move(self));
    }

    void abort() override {
        auto self = this->shared_from_this();
        asio::dispatch(m_executor, [self]() { self->do_abort(); });
    }

  private:
    template <typename Fn> void hop(Fn &&fn) {
        auto self = this->shared_from_this();
        asio::dispatch(asio::bind_executor(asio::any_io_executor{m_executor},
                                           [self, fn = std::forward<Fn>(fn)]() mutable { fn(); }));
    }

    void read_impl(WsReadCompletion done) {
        ensure_reader();
        for (;;) {
            if (!m_inbox.empty()) {
                WsMessage msg = std::move(m_inbox.front());
                m_inbox.pop_front();
                m_inbox_bytes -= kInboxMsgOverhead + msg.data.size();
                (void)m_inbox_space.try_send(error_code{});
                std::move(done)(error_code{}, std::move(msg));
                return;
            }
            if (m_reader_done) {
                finish_read(std::move(done));
                return;
            }
            break;
        }
        auto self = this->shared_from_this();
        m_inbox_ready.async_receive([self, done = std::move(done)](error_code ec) mutable {
            if (ec && self->m_inbox.empty()) {
                self->finish_read(std::move(done));
                return;
            }
            self->read_impl(std::move(done));
        });
    }

    void finish_read(WsReadCompletion done) {
        if (m_close_code != 0) {
            const std::uint16_t code = m_close_code;
            m_close_code = 0;
            auto self = this->shared_from_this();
            close_with(ws_close_payload(code), [self, done = std::move(done)](error_code) mutable {
                std::move(done)(self->reader_error(), WsMessage{});
            });
            return;
        }
        std::move(done)(reader_error(), WsMessage{});
    }

    [[nodiscard]] error_code reader_error() const {
        return m_reader_ec ? m_reader_ec : make_error_code(asio::error::operation_aborted);
    }

    void ensure_reader() {
        if (m_reader_started)
            return;
        m_reader_started = true;
        reader_loop(this->shared_from_this());
    }

    static void reader_loop(std::shared_ptr<WsBackendImpl> self) {
        if (!self->m_open) {
            self->reader_finished();
            return;
        }
        if (self->m_inbox_bytes >= self->m_inbox_limit) {
            auto &ch = self->m_inbox_space;
            ch.async_receive([self](error_code ec) {
                if (ec) {
                    self->reader_finished();
                    return;
                }
                reader_loop(std::move(self));
            });
            return;
        }
        self->read_message([self](error_code ec, WsMessage msg) {
            if (ec) {
                self->m_reader_ec = ec;
                self->reader_finished();
                return;
            }
            self->m_inbox_bytes += kInboxMsgOverhead + msg.data.size();
            self->m_inbox.push_back(std::move(msg));
            (void)self->m_inbox_ready.try_send(error_code{});
            reader_loop(std::move(self));
        });
    }

    void reader_finished() {
        m_reader_done = true;
        (void)m_inbox_ready.try_send(error_code{});
    }

    void stop_inbox() {
        m_inbox_ready.close();
        m_inbox_space.close();
    }

    // Reads/parses one message straight off the transport (reassembling
    // fragments, answering Ping/Close). Runs only inside reader_loop, on the
    // connection executor, and is the single owner of m_parser.
    void read_message(asio::any_completion_handler<void(error_code, WsMessage)> done) {
        m_rm_message.clear();
        m_rm_type = WsOpcode::Text;
        m_rm_assembling = false;
        m_rm_compressed = false;
        m_rm_utf8 = Utf8Validator{};
        read_message_step(std::move(done));
    }

    void read_message_step(asio::any_completion_handler<void(error_code, WsMessage)> done) {
        for (;;) {
            WsFrame frame;
            auto status = m_parser.next(frame);

            if (status == WsFrameParser::Status::Error) {
                m_close_code = 1002;
                std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                return;
            }
            if (status == WsFrameParser::Status::NeedMore) {
                // Validate a text message as it arrives rather than once its
                // frame is whole: a codepoint may straddle a fragment boundary.
                if (!m_rm_compressed && !m_parser.pending_rsv1()) {
                    if (auto in_flight = m_parser.pending_opcode()) {
                        const bool is_text =
                            *in_flight == WsOpcode::Text ||
                            (*in_flight == WsOpcode::Continuation && m_rm_assembling && m_rm_type == WsOpcode::Text);
                        if (is_text) {
                            auto partial = m_parser.take_partial_payload();
                            if (*in_flight == WsOpcode::Text && partial.first) {
                                m_rm_utf8 = Utf8Validator{};
                            }
                            if (!partial.bytes.empty() && !m_rm_utf8.feed(partial.bytes)) {
                                m_close_code = 1007;
                                std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                                return;
                            }
                        }
                    }
                }
                auto self = this->shared_from_this();
                m_transport->async_read_some(ByteSpan{m_read_buf.data(), m_read_buf.size()},
                                             [self, done = std::move(done)](error_code ec, std::size_t n) mutable {
                                                 if (ec) {
                                                     if constexpr (!ws_read_eof_is_half_close<Transport>::value) {
                                                         self->m_open = false;
                                                     }
                                                     std::move(done)(ec, WsMessage{});
                                                     return;
                                                 }
                                                 self->touch_deadline();
                                                 self->m_parser.append(self->m_read_buf.data(), n);
                                                 self->read_message_step(std::move(done));
                                             });
                return;
            }

            switch (frame.opcode) {
            case WsOpcode::Close: {
                const std::string &payload = frame.payload;
                if (payload.size() == 1) {
                    m_close_code = 1002;
                    std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                    return;
                }
                if (payload.size() >= 2) {
                    const auto code = static_cast<std::uint16_t>((static_cast<unsigned char>(payload[0]) << 8) |
                                                                 static_cast<unsigned char>(payload[1]));
                    if (!ws_valid_close_code(code)) {
                        m_close_code = 1002;
                        std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                        return;
                    }
                    Utf8Validator reason;
                    if (!reason.feed(std::string_view{payload}.substr(2)) || !reason.complete()) {
                        m_close_code = 1007;
                        std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                        return;
                    }
                }
                m_close_code = 1000;
                std::move(done)(make_error_code(asio::error::eof), WsMessage{});
                return;
            }

            case WsOpcode::Ping:
                enqueue_control(WsOpcode::Pong, std::move(frame.payload));
                continue;

            case WsOpcode::Pong:
                continue;

            case WsOpcode::Text:
            case WsOpcode::Binary:
                if (m_rm_assembling) {
                    m_close_code = 1002;
                    std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                    return;
                }
                m_rm_type = frame.opcode;
                m_rm_assembling = true;
                m_rm_compressed = m_compress_active && frame.rsv1;
                if (m_rm_compressed) {
                    m_rm_message.clear();
                    if (auto ec = inflate_feed(frame.payload, m_rm_message)) {
                        std::move(done)(*ec, WsMessage{});
                        return;
                    }
                } else {
                    if (frame.rsv1) {
                        m_close_code = 1002;
                        std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                        return;
                    }
                    m_rm_message = std::move(frame.payload);
                    if (m_rm_type == WsOpcode::Text) {
                        if (frame.already_delivered == 0) {
                            m_rm_utf8 = Utf8Validator{};
                        }
                        if (!m_rm_utf8.feed(std::string_view{m_rm_message}.substr(frame.already_delivered))) {
                            m_close_code = 1007;
                            std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                            return;
                        }
                    }
                }
                if (frame.fin) {
                    if (m_rm_compressed) {
                        if (auto ec = inflate_finish(m_rm_message)) {
                            std::move(done)(*ec, WsMessage{});
                            return;
                        }
                    }
                    if (m_rm_type == WsOpcode::Text &&
                        (m_rm_compressed ? !utf8_whole(m_rm_message) : !m_rm_utf8.complete())) {
                        m_close_code = 1007;
                        std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                        return;
                    }
                    std::move(done)(error_code{}, WsMessage{std::move(m_rm_message), m_rm_type == WsOpcode::Text});
                    return;
                }
                continue;

            case WsOpcode::Continuation:
                if (!m_rm_assembling) {
                    m_close_code = 1002;
                    std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                    return;
                }
                if (frame.rsv1) {
                    m_close_code = 1002;
                    std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                    return;
                }
                if (m_rm_compressed) {
                    if (auto ec = inflate_feed(frame.payload, m_rm_message)) {
                        std::move(done)(*ec, WsMessage{});
                        return;
                    }
                } else {
                    if (m_rm_message.size() + frame.payload.size() > m_max_payload) {
                        m_close_code = 1009;
                        std::move(done)(make_error_code(asio::error::message_size), WsMessage{});
                        return;
                    }
                    if (m_rm_type == WsOpcode::Text &&
                        !m_rm_utf8.feed(std::string_view{frame.payload}.substr(frame.already_delivered))) {
                        m_close_code = 1007;
                        std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                        return;
                    }
                    m_rm_message.append(frame.payload);
                }
                if (frame.fin) {
                    if (m_rm_compressed) {
                        if (auto ec = inflate_finish(m_rm_message)) {
                            std::move(done)(*ec, WsMessage{});
                            return;
                        }
                    }
                    if (m_rm_type == WsOpcode::Text &&
                        (m_rm_compressed ? !utf8_whole(m_rm_message) : !m_rm_utf8.complete())) {
                        m_close_code = 1007;
                        std::move(done)(make_error_code(asio::error::invalid_argument), WsMessage{});
                        return;
                    }
                    std::move(done)(error_code{}, WsMessage{std::move(m_rm_message), m_rm_type == WsOpcode::Text});
                    return;
                }
                continue;
            }
        }
    }

    std::optional<error_code> inflate_feed(std::string_view data, std::string &out) {
        switch (m_in->feed(data, out, m_max_payload)) {
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

    static bool utf8_whole(std::string_view s) {
        Utf8Validator v;
        return v.feed(s) && v.complete();
    }

    void write_impl(std::string data, bool text, WriteCompletion done) {
        if (!m_open) {
            std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        bool rsv1 = false;
        if (m_compress_active && m_write_compression) {
            std::string compressed;
            bool compressed_flag = false;
            if (!m_out->compress(data, compressed, m_deflate.server_no_context_takeover, compressed_flag)) {
                std::move(done)(make_error_code(asio::error::invalid_argument));
                return;
            }
            data = std::move(compressed);
            rsv1 = compressed_flag;
        }
        const WsOpcode opcode = text ? WsOpcode::Text : WsOpcode::Binary;
        const bool mask = !m_expect_masked;
        m_pending.push_back(make_req(opcode, std::move(data), std::move(done), false, mask, rsv1));
        (void)m_notify.try_send(error_code{});
    }

    // Sends a Close payload, waits for the pump to put it on the wire, then
    // tears the connection down. The wait is load-bearing: the peer decides how
    // the connection failed from the code it receives.
    void close_with(std::string payload, WriteCompletion done) {
        auto self = this->shared_from_this();
        hop([self, payload = std::move(payload), done = std::move(done)]() mutable {
            if (!self->m_open) {
                self->teardown();
                std::move(done)(error_code{});
                return;
            }
            self->m_open = false;
            const bool mask = !self->m_expect_masked;
            auto handler = [self, done = std::move(done)](error_code) mutable {
                self->teardown();
                std::move(done)(error_code{});
            };
            self->m_pending.push_back(
                make_req(WsOpcode::Close, std::move(payload), WriteCompletion(std::move(handler)), true, mask));
            (void)self->m_notify.try_send(error_code{});
        });
    }

    void teardown() {
        m_open = false;
        m_notify.close();
        stop_inbox();
        fail_pending_writes();
        if (m_watchdog_timer)
            m_watchdog_timer->cancel();
        m_transport->close();
    }

    void do_abort() { teardown(); }

    void fail_pending_writes() {
        while (!m_pending.empty()) {
            WriteReq req = std::move(m_pending.front());
            m_pending.pop_front();
            if (req.done) {
                (*req.done)(make_error_code(asio::error::operation_aborted));
            }
        }
    }

    static void pump(std::shared_ptr<WsBackendImpl> self) {
        if (self->m_pending.empty()) {
            self->m_notify.async_receive([self](error_code ec) {
                if (ec) {
                    self->pump_finished();
                    return;
                }
                pump(std::move(self));
            });
            return;
        }
        auto req = std::make_shared<WriteReq>(std::move(self->m_pending.front()));
        self->m_pending.pop_front();
        self->write_all(req, [self, req](error_code ec) mutable {
            if (ec) {
                self->m_open = false;
            }
            if (req->done) {
                (*req->done)(ec);
            }
            if (ec || req->close_after) {
                self->pump_finished();
                return;
            }
            pump(std::move(self));
        });
    }

    void pump_finished() {
        m_open = false;
        fail_pending_writes();
        m_notify.close();
    }

    // The frame lives in a shared_ptr for the duration of the write: the
    // scatter-gather buffers point into it, so its address must not change.
    void write_all(const std::shared_ptr<WriteReq> &req, WriteCompletion done) {
        touch_deadline();
        const std::string_view payload = req->payload();
        const std::array<ConstByteSpan, 2> bufs{
            ConstByteSpan{reinterpret_cast<const std::byte *>(req->header), req->header_len},
            ConstByteSpan{reinterpret_cast<const std::byte *>(payload.data()), payload.size()},
        };
        auto transport = m_transport;
        // The transport reports void(error_code, size_t); the queued frame's
        // waiter only cares about success/failure.
        detail::async_error_only(
            m_executor, std::move(done),
            [transport, bufs](asio::any_completion_handler<void(error_code, std::size_t)> handler) {
                transport->async_write_seq(boost::span<const ConstByteSpan>{bufs.data(), bufs.size()},
                                           std::move(handler));
            });
    }

    void touch_deadline() { m_deadline = std::chrono::steady_clock::now() + m_idle_timeout; }

    static void start_watchdog(std::weak_ptr<WsBackendImpl> weak) {
        asio::steady_timer *timer = nullptr;
        {
            auto self = weak.lock();
            if (!self || !self->m_open) {
                return;
            }
            timer = self->m_watchdog_timer.get();
            timer->expires_at(self->m_deadline);
        }
        timer->async_wait([weak](error_code ec) {
            if (ec == asio::error::operation_aborted) {
                return;
            }
            auto self = weak.lock();
            if (!self || !self->m_open) {
                return;
            }
            if (std::chrono::steady_clock::now() >= self->m_deadline) {
                self->teardown();
                return;
            }
            start_watchdog(weak);
        });
    }

    void enqueue_control(WsOpcode opcode, std::string payload, bool close_after = false) {
        if (!m_open || m_pending.size() >= kMaxPendingFrames) {
            return;
        }
        m_pending.push_back(make_req(opcode, std::move(payload), std::nullopt, close_after, !m_expect_masked));
        (void)m_notify.try_send(error_code{});
    }

    static constexpr std::size_t kMaxPendingFrames = 64;
    static constexpr std::size_t kInboxMsgOverhead = 128;

    std::shared_ptr<Transport> m_transport;
    asio::any_io_executor m_executor;
    WsFrameParser m_parser;
    std::uint64_t m_max_payload;
    std::chrono::steady_clock::duration m_idle_timeout;
    std::chrono::steady_clock::time_point m_deadline{};
    std::size_t m_inbox_limit;
    bool m_expect_masked{true};
    WsDeflateConfig m_deflate;
    bool m_compress_active{false};
    bool m_write_compression{false};
    int m_compression_level{0};
    std::unique_ptr<WsDeflater> m_out;
    std::unique_ptr<WsInflater> m_in;
    std::shared_ptr<asio::steady_timer> m_watchdog_timer;
    WriteQueue m_pending;
    Notify m_notify;
    std::deque<WsMessage> m_inbox;
    std::size_t m_inbox_bytes{0};
    bool m_reader_started{false};
    bool m_reader_done{false};
    error_code m_reader_ec;
    std::uint16_t m_close_code{0};
    Notify m_inbox_ready;
    Notify m_inbox_space;
    std::atomic<bool> m_open{true};
    // Reader-local per-message state. Only reader_loop calls read_message(),
    // so these do not need to be re-entrant.
    std::array<std::byte, 8192> m_read_buf;
    std::string m_rm_message;
    WsOpcode m_rm_type{WsOpcode::Text};
    bool m_rm_assembling{false};
    bool m_rm_compressed{false};
    Utf8Validator m_rm_utf8;
};

// User-facing WebSocket handle. Forwards to the type-erased backend.
class WebSocket {
  public:
    explicit WebSocket(std::shared_ptr<WsBackend> backend) : m_backend(std::move(backend)) {}

    // Dropping the handle without awaiting close() must still stop the detached
    // write pump, otherwise it would keep the backend alive forever.
    ~WebSocket() { m_backend->abort(); }

    // Reads one complete message. Completion: void(error_code, WsMessage).
    template <typename Token> auto async_read(Token &&token) {
        auto backend = m_backend;
        return asio::async_compose<Token, void(error_code, WsMessage)>(
            ReadOp{[backend](WsReadCompletion done) mutable { backend->read(std::move(done)); },
                   backend->get_executor()},
            token, backend->get_executor());
    }

    template <typename Token> auto async_write_text(std::string data, Token &&token) {
        return async_write(std::move(data), true, std::forward<Token>(token));
    }
    template <typename Token> auto async_write_binary(std::string data, Token &&token) {
        return async_write(std::move(data), false, std::forward<Token>(token));
    }
    template <typename Token> auto async_write(std::string data, bool text, Token &&token) {
        auto backend = m_backend;
        return asio::async_compose<Token, void(error_code)>(
            WriteOp{[backend, data = std::move(data), text](WriteCompletion done) mutable {
                        backend->write(std::move(data), text, std::move(done));
                    },
                    backend->get_executor()},
            token, backend->get_executor());
    }

    template <typename Token> auto async_write_text_view(std::string_view data, Token &&token) {
        return async_write_view(data, true, std::forward<Token>(token));
    }
    template <typename Token> auto async_write_binary_view(std::string_view data, Token &&token) {
        return async_write_view(data, false, std::forward<Token>(token));
    }
    template <typename Token> auto async_write_view(std::string_view data, bool text, Token &&token) {
        auto backend = m_backend;
        return asio::async_compose<Token, void(error_code)>(
            WriteOp{[backend, data, text](WriteCompletion done) mutable {
                        backend->write_view(data, text, std::move(done));
                    },
                    backend->get_executor()},
            token, backend->get_executor());
    }

    template <typename Token> auto async_ping(std::string payload, Token &&token) {
        auto backend = m_backend;
        return asio::async_compose<Token, void(error_code)>(
            WriteOp{[backend, payload = std::move(payload)](WriteCompletion done) mutable {
                        backend->ping(std::move(payload), std::move(done));
                    },
                    backend->get_executor()},
            token, backend->get_executor());
    }

    template <typename Token> auto async_close(Token &&token) {
        auto backend = m_backend;
        return asio::async_compose<Token, void(error_code)>(
            WriteOp{[backend](WriteCompletion done) mutable { backend->close(std::move(done)); },
                    backend->get_executor()},
            token, backend->get_executor());
    }

    template <typename Token> auto async_close(std::uint16_t code, std::string reason, Token &&token) {
        auto backend = m_backend;
        return asio::async_compose<Token, void(error_code)>(
            WriteOp{[backend, code, reason = std::move(reason)](WriteCompletion done) mutable {
                        backend->close(code, std::move(reason), std::move(done));
                    },
                    backend->get_executor()},
            token, backend->get_executor());
    }

    template <typename Token> auto async_enable_write_compression(bool enable, Token &&token) {
        auto backend = m_backend;
        return asio::async_compose<Token, void(error_code)>(WriteOp{[backend, enable](WriteCompletion done) mutable {
                                                                        backend->enable_write_compression(
                                                                            enable, std::move(done));
                                                                    },
                                                                    backend->get_executor()},
                                                            token, backend->get_executor());
    }

    template <typename Token> auto async_set_compression_level(int level, Token &&token) {
        auto backend = m_backend;
        return asio::async_compose<Token, void(error_code)>(WriteOp{[backend, level](WriteCompletion done) mutable {
                                                                        backend->set_compression_level(level,
                                                                                                       std::move(done));
                                                                    },
                                                                    backend->get_executor()},
                                                            token, backend->get_executor());
    }

    bool compression_negotiated() const { return m_backend->compression_negotiated(); }
    bool is_open() const { return m_backend->is_open(); }

    // Starts the serializing write pump and the idle watchdog (the engine calls
    // this on the connection executor right after the upgrade). The pump holds
    // its own reference to the backend, so it may outlive this handle.
    void run_writer() { m_backend->run_writer(); }

  private:
    struct ReadOp {
        std::function<void(WsReadCompletion)> start;
        asio::any_io_executor ex;
        bool started{false};

        asio::any_io_executor get_executor() const noexcept { return ex; }

        template <typename Self> void operator()(Self &self, error_code ec = {}, WsMessage msg = {}) {
            if (!started) {
                started = true;
                // The completion handler construction below moves this
                // composed operation (including `start`) into it; invoke a local
                // copy so the call target is not a moved-from std::function.
                auto action = std::move(start);
                action(WsReadCompletion(std::move(self)));
                return;
            }
            self.complete(ec, std::move(msg));
        }
    };

    struct WriteOp {
        std::function<void(WriteCompletion)> start;
        asio::any_io_executor ex;
        bool started{false};

        asio::any_io_executor get_executor() const noexcept { return ex; }

        template <typename Self> void operator()(Self &self, error_code ec = {}) {
            if (!started) {
                started = true;
                auto action = std::move(start);
                action(WriteCompletion(std::move(self)));
                return;
            }
            self.complete(ec);
        }
    };

    std::shared_ptr<WsBackend> m_backend;
};

} // namespace simple_http
