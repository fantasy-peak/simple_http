#pragma once

// HTTP/1.x engine (C++17 token-only port).
//
// Http1Engine drives one connection: it reads bytes into an H1Parser until a
// full head is parsed, frames the request body (chunked / Content-Length /
// none), hands the request to the dispatcher, and repeats while the connection
// is keep-alive. Responses go through Http1ResponseSink, which frames either a
// Content-Length reply or a chunked stream and hands the bytes to the
// connection's SerializedWriter (one transport write in flight).
//
// This first port supports HTTP/1.x and local WebSocket upgrades. The h2c
// upgrade and the byte-level WebSocket reverse proxy are deferred with the
// HTTP/2 phase and the proxy phase respectively.
//
// Thread affinity (model A): the sink and the ResponseWriter built on it belong
// to the connection thread. Writes are serialized by SerializedWriter (two
// overlapping async_write calls on one socket are undefined even on the same
// thread); using either object from another thread aborts via
// require_owner_thread.

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../core/http_field.h"
#include "../../core/http_method.h"
#include "../../core/http_status.h"
#include "../../core/limits.h"
#include "../../core/logging.h"
#include "../../core/types.h"
#include "../../core/version.h"
#include "../../proto/compressing_writer.h"
#include "../../proto/request.h"
#include "../../proto/response.h"
#include "../../proto/response_writer.h"
#include "../../proto/websocket.h"
#include "../../transport/serialized_writer.h"
#include "../../transport/transport.h"
#include "../dispatcher.h"
#include "../h2/h2_engine.h"
#include "../ws_proxy_common.h"
#include "h1_parser.h"
#include "ws_proxy.h"

namespace simple_http {

namespace asio = boost::asio;

// A connection-level idle deadline shared between the engine and the response
// writer(s) it creates.
using SharedDeadline = std::shared_ptr<std::chrono::steady_clock::time_point>;

// ResponseSink for HTTP/1.x. Owns the response framing; the bytes go through
// one SerializedWriter so the connection never has two writes in flight.
template <typename Transport>
class Http1ResponseSink final : public ResponseSink, public std::enable_shared_from_this<Http1ResponseSink<Transport>> {
  public:
    Http1ResponseSink(std::shared_ptr<Transport> transport, Version version, SharedDeadline deadline,
                      std::chrono::steady_clock::duration idle_timeout, std::string alt_svc = {},
                      std::shared_ptr<std::atomic<bool>> continue_sent = {})
        : m_transport(std::move(transport)), m_executor(m_transport->get_executor()), m_version(version),
          m_alt_svc(std::move(alt_svc)), m_continue_sent(std::move(continue_sent)),
          m_writer(std::make_shared<SerializedWriter<Transport>>(m_transport, std::move(deadline), idle_timeout)) {}

    void set_head_request(bool head) { m_head_request = head; }
    void set_keep_alive(bool ka) { m_keep_alive_out = ka; }
    bool keep_alive_out() const { return m_keep_alive_out; }

    void send(int status, Headers headers, std::string body, WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), status, headers = std::move(headers), body = std::move(body),
                     done = std::move(done)]() mutable {
            self->enqueue_send(status, std::move(headers), std::move(body), std::move(done));
        });
    }

    void send_bodyless(int status, Headers headers, WriteCompletion done) override {
        on_executor(
            [self = this->shared_from_this(), status, headers = std::move(headers), done = std::move(done)]() mutable {
                self->enqueue_bodyless(status, std::move(headers), std::move(done));
            });
    }

    void send_continue(WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), done = std::move(done)]() mutable {
            self->enqueue_continue(std::move(done));
        });
    }

    void send_headers(int status, Headers headers, WriteCompletion done) override {
        on_executor(
            [self = this->shared_from_this(), status, headers = std::move(headers), done = std::move(done)]() mutable {
                self->enqueue_headers(status, std::move(headers), std::move(done));
            });
    }

    void send_chunk(std::string data, WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), data = std::move(data), done = std::move(done)]() mutable {
            self->enqueue_chunk(std::move(data), std::move(done));
        });
    }

    void send_last(std::string data, WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), data = std::move(data), done = std::move(done)]() mutable {
            self->enqueue_last(std::move(data), std::move(done));
        });
    }

    void send_trailers(Headers trailers, WriteCompletion done) override {
        on_executor(
            [self = this->shared_from_this(), trailers = std::move(trailers), done = std::move(done)]() mutable {
                self->enqueue_trailers(std::move(trailers), std::move(done));
            });
    }

    void connected(ConnectedCompletion done) const override {
        on_executor([self = this->shared_from_this(), done = std::move(done)]() mutable {
            // Pending writes mean the response is not on the wire yet; the
            // connection must not be reused for another request.
            std::move(done)(error_code{}, self->m_open && self->m_writer->idle());
        });
    }

    void close(WriteCompletion done) override {
        on_executor([self = this->shared_from_this(), done = std::move(done)]() mutable {
            self->m_open = false;
            self->m_writer->close(make_error_code(asio::error::not_connected));
            self->m_transport->close();
            std::move(done)(error_code{});
        });
    }

    Version version() const override { return m_version; }
    asio::any_io_executor get_executor() const override { return m_executor; }
    bool head_request() const noexcept override { return m_head_request; }

  private:
    template <typename Fn> void on_executor(Fn &&fn) const {
        asio::dispatch(asio::bind_executor(asio::any_io_executor{m_executor}, std::forward<Fn>(fn)));
    }

    static constexpr std::size_t kResponseOverhead = 96;
    static constexpr std::string_view kCrLf = "\r\n";
    static constexpr std::string_view kLastChunk = "0\r\n\r\n";
    // CRLF of the last data chunk followed by the terminating chunk, written as
    // one static tail so the final chunk needs no concatenation.
    static constexpr std::string_view kChunkEnd = "\r\n0\r\n\r\n";
    static constexpr std::string_view kContinueResponse = "HTTP/1.1 100 Continue\r\n\r\n";
    static constexpr std::string_view kAltSvcPrefix = "alt-svc: ";
    static constexpr std::string_view kKeepAliveConnection = "connection: keep-alive\r\n";
    static constexpr std::string_view kCloseConnection = "connection: close\r\n";

    static ConstByteSpan as_bytes(std::string_view bytes) {
        return ConstByteSpan{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()};
    }

    static std::size_t headers_bytes(const Headers &headers) {
        std::size_t total = 0;
        for (const auto &[name, value] : headers) {
            total += name.size() + value.size() + 4;
        }
        return total;
    }

    [[nodiscard]] std::size_t headers_bytes_with_alt_svc(const Headers &headers) const {
        std::size_t total = headers_bytes(headers);
        if (!m_alt_svc.empty())
            total += kAltSvcPrefix.size() + m_alt_svc.size() + 2;
        return total;
    }

    void append_status_line(std::string &out, int status) const {
        out.append(m_version == Version::Http1 ? "HTTP/1.0 " : "HTTP/1.1 ");
        append_int(out, status);
        out.push_back(' ');
        out.append(reason_phrase(status));
        out.append("\r\n");
    }

    static void append_int(std::string &out, int value) {
        char buf[std::numeric_limits<int>::digits10 + 2];
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
        (void)ec;
        out.append(buf, static_cast<std::size_t>(end - buf));
    }

    void append_headers(std::string &out, const Headers &headers) const {
        bool saw_connection = false;
        for (const auto &[name, value] : headers) {
            if (name == "connection")
                saw_connection = true;
            out.append(name);
            out.append(": ");
            out.append(value);
            out.append("\r\n");
        }
        if (!m_alt_svc.empty()) {
            out.append(kAltSvcPrefix);
            out.append(m_alt_svc);
            out.append("\r\n");
        }
        if (!saw_connection) {
            out.append(m_keep_alive_out ? kKeepAliveConnection : kCloseConnection);
        }
    }

    void build_head(std::string &out, int status, const Headers &headers) const {
        out.reserve(kResponseOverhead + headers_bytes_with_alt_svc(headers));
        append_status_line(out, status);
        append_headers(out, headers);
        out.append("\r\n");
    }

    static void strip_hop_by_hop(Headers &headers) {
        for (const std::string_view name :
             {"connection", "keep-alive", "transfer-encoding", "proxy-connection", "upgrade"}) {
            headers.erase(name);
        }
    }

    // One framed write as it goes to the connection writer. Framing (status
    // line, headers, chunk prefix, terminator) is decided here; the bytes are
    // owned by the queue entry until the transport write completes.
    //
    // `done` is wrapped so an error also closes this sink. The writer is the
    // only place that issues transport writes, so "one write in flight per
    // socket" holds even when several coroutines share the response.
    WriteCompletion wrap(WriteCompletion done) {
        // Weak by design: the sink owns the writer and the writer's queue owns
        // this completion, so a strong capture here would close the loop
        // (sink -> writer -> entry -> sink) and keep both alive until the
        // transport happened to complete. The engine (m_active_writer + the
        // finish/after_handler lambdas) and the ResponseWriter keep the sink
        // alive for the whole request/response cycle; the lock can only fail
        // once the connection is already being torn down, and the caller's
        // completion still has to run.
        std::weak_ptr<Http1ResponseSink> guard = this->weak_from_this();
        return WriteCompletion([guard, done = std::move(done)](error_code ec) mutable {
            if (auto self = guard.lock()) {
                self->complete_write(ec, std::move(done));
                return;
            }
            if (done)
                std::move(done)(ec ? ec : make_error_code(asio::error::not_connected));
        });
    }

    void enqueue_send(int status, Headers headers, std::string body, WriteCompletion done) {
        if (!m_open) {
            std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        if (status == status::no_content || status == status::not_modified) {
            if (status == status::no_content) {
                headers.erase("content-length");
            }
            enqueue_bodyless(status, std::move(headers), std::move(done));
            return;
        }
        strip_hop_by_hop(headers);
        ensure_date(headers);
        headers.erase("content-length");
        headers.add_lower("content-length", std::to_string(body.size()));
        std::string head;
        build_head(head, status, headers);
        if (m_head_request) {
            m_writer->enqueue(WriteEntry::owned(std::move(head)), wrap(std::move(done)));
            return;
        }
        m_writer->enqueue(WriteEntry::owned(std::move(head), std::move(body)), wrap(std::move(done)));
    }

    void enqueue_bodyless(int status, Headers headers, WriteCompletion done) {
        if (!m_open) {
            std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        strip_hop_by_hop(headers);
        if (status == status::no_content || (status >= 100 && status < 200)) {
            headers.erase("content-length");
        }
        ensure_date(headers);
        std::string out;
        build_head(out, status, headers);
        m_writer->enqueue(WriteEntry::owned(std::move(out)), wrap(std::move(done)));
    }

    void enqueue_continue(WriteCompletion done) {
        if (!m_open) {
            std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        // At most one interim response per request, shared with the body
        // puller through the engine's flag.
        if (m_continue_sent && m_continue_sent->exchange(true, std::memory_order_relaxed)) {
            std::move(done)(error_code{});
            return;
        }
        m_writer->enqueue(WriteEntry::owned(std::string{kContinueResponse}), wrap(std::move(done)));
    }

    void enqueue_headers(int status, Headers headers, WriteCompletion done) {
        if (!m_open) {
            std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        strip_hop_by_hop(headers);
        ensure_date(headers);
        headers.erase("content-length");
        if (m_version == Version::Http1 || m_head_request) {
            m_keep_alive_out = false;
        } else {
            headers.add_lower("transfer-encoding", "chunked");
            m_chunked = true;
        }
        std::string out;
        build_head(out, status, headers);
        m_writer->enqueue(WriteEntry::owned(std::move(out)), wrap(std::move(done)));
    }

    void enqueue_chunk(std::string data, WriteCompletion done) {
        if (m_head_request) {
            std::move(done)(error_code{});
            return;
        }
        if (!m_open) {
            std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        if (!m_chunked) {
            if (data.empty()) {
                std::move(done)(error_code{});
                return;
            }
            m_writer->enqueue(WriteEntry::owned(std::string{}, std::move(data)), wrap(std::move(done)));
            return;
        }
        std::string head = chunk_prefix(data.size());
        m_writer->enqueue(WriteEntry::owned(std::move(head), std::move(data), as_bytes(kCrLf)), wrap(std::move(done)));
    }

    void enqueue_last(std::string data, WriteCompletion done) {
        if (m_head_request) {
            std::move(done)(error_code{});
            return;
        }
        if (!m_open) {
            std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        if (!m_chunked) {
            if (data.empty()) {
                std::move(done)(error_code{});
                return;
            }
            m_writer->enqueue(WriteEntry::owned(std::string{}, std::move(data)), wrap(std::move(done)));
            return;
        }
        if (data.empty()) {
            m_writer->enqueue(WriteEntry::owned(std::string{kLastChunk}), wrap(std::move(done)));
            return;
        }
        std::string head = chunk_prefix(data.size());
        m_writer->enqueue(WriteEntry::owned(std::move(head), std::move(data), as_bytes(kChunkEnd)),
                          wrap(std::move(done)));
    }

    void enqueue_trailers(Headers trailers, WriteCompletion done) {
        if (m_head_request) {
            std::move(done)(error_code{});
            return;
        }
        if (!m_open) {
            std::move(done)(make_error_code(asio::error::not_connected));
            return;
        }
        if (!m_chunked) {
            std::move(done)(error_code{});
            return;
        }
        std::string out;
        out.reserve(3 + headers_bytes(trailers) + 2);
        out.append("0\r\n");
        for (const auto &[name, value] : trailers) {
            if (is_forbidden_trailer_field(name))
                continue;
            out.append(name);
            out.append(": ");
            out.append(value);
            out.append("\r\n");
        }
        out.append("\r\n");
        m_writer->enqueue(WriteEntry::owned(std::move(out)), wrap(std::move(done)));
    }

    // "<hex size>\r\n" for one chunk. The result lives in the queue entry, so
    // it needs no separate allocation of its own.
    static std::string chunk_prefix(std::size_t size) {
        char hex[2 * sizeof(std::size_t) + 2];
        auto [end, ec] = std::to_chars(hex, hex + sizeof(hex) - 2, size, 16);
        (void)ec;
        std::string prefix(hex, static_cast<std::size_t>(end - hex));
        prefix.append("\r\n");
        return prefix;
    }

    void complete_write(error_code ec, WriteCompletion done) {
        if (ec)
            m_open = false;
        std::move(done)(ec);
    }

    std::shared_ptr<Transport> m_transport;
    asio::any_io_executor m_executor;
    Version m_version;
    bool m_open{true};
    bool m_keep_alive_out{true};
    bool m_head_request{false};
    bool m_chunked{false};
    std::string m_alt_svc;
    std::shared_ptr<std::atomic<bool>> m_continue_sent;
    // Sole owner of the transport write direction; see serialized_writer.h.
    std::shared_ptr<SerializedWriter<Transport>> m_writer;
};

template <typename Transport> class Http1Engine : public std::enable_shared_from_this<Http1Engine<Transport>> {
  public:
    explicit Http1Engine(std::shared_ptr<Transport> transport, EngineLimits limits = {})
        : m_transport(std::move(transport)), m_executor(m_transport->get_executor()), m_limits(limits),
          m_watchdog(std::make_shared<asio::steady_timer>(m_executor)) {}

    // Whether an Upgrade: h2c request may switch this connection to HTTP/2.
    // TLS listeners must leave this off: on TLS the protocol is chosen by ALPN,
    // so honouring a cleartext upgrade there would let a client switch to h2
    // without negotiating it.
    void allow_h2c_upgrade(bool allow) { m_allow_h2c = allow; }

    // Serves requests until the connection closes or a request is not
    // keep-alive. `on_done` runs exactly once, after the transport has been
    // shut down and closed.
    void run(Dispatcher dispatch, std::string initial, WsLookup ws_lookup, WsProxyLookup ws_proxy_lookup,
             WsLookup ws_regex_lookup, std::function<void()> on_done) {
        m_dispatch = std::move(dispatch);
        m_ws_lookup = std::move(ws_lookup);
        m_ws_proxy_lookup = std::move(ws_proxy_lookup);
        m_ws_regex_lookup = std::move(ws_regex_lookup);
        m_on_done = std::move(on_done);
        m_in.assign(initial);
        *m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
        if (m_limits.idle_timeout.count() > 0) {
            arm_watchdog();
        }
        parse_head();
    }

  private:
    using LineCompletion = asio::any_completion_handler<void(error_code, std::string)>;
    using PullCompletion = Body::ReadCompletion;
    using PullAttention = std::function<void(bool)>;

    // --- connection lifetime -------------------------------------------------

    void finish() {
        if (m_finished)
            return;
        m_finished = true;
        m_active_writer.reset();
        m_watchdog->cancel();
        auto self = this->shared_from_this();
        if (m_owner != TransportOwner::Engine) {
            complete_done();
            return;
        }
        m_transport->async_shutdown([self](error_code) {
            self->m_transport->close();
            self->complete_done();
        });
    }

    void complete_done() {
        if (m_done_called)
            return;
        m_done_called = true;
        auto on_done = std::move(m_on_done);
        if (on_done) {
            on_done();
        }
    }

    void arm_watchdog() {
        if (m_limits.idle_timeout.count() <= 0)
            return;
        auto self = this->shared_from_this();
        m_watchdog->expires_at(*m_deadline);
        m_watchdog->async_wait([self](error_code ec) {
            if (ec == asio::error::operation_aborted)
                return;
            if (self->m_owner != TransportOwner::Engine)
                return;
            if (std::chrono::steady_clock::now() >= *self->m_deadline) {
                self->m_transport->close(); // unblocks any pending read; finish() follows
                return;
            }
            self->arm_watchdog();
        });
    }

    void touch_deadline() { *m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout; }

    // One transport read into the scan buffer, refreshing the shared deadline.
    template <typename Handler> void read_scan(Handler &&handler) {
        touch_deadline();
        m_transport->async_read_some(ByteSpan{m_scan.data(), m_scan.size()}, std::forward<Handler>(handler));
    }

    // --- head parsing --------------------------------------------------------

    void parse_head() {
        m_parser.reset_for_next_head();
        parse_head_step();
    }

    void parse_head_step() {
        auto state = m_parser.parse_head(m_in.view());
        if (state == H1Parser::State::NeedMore) {
            if (m_in.size() > m_limits.max_header_bytes) {
                send_error_response(status::request_header_fields_too_large);
                return;
            }
            auto self = this->shared_from_this();
            read_scan([self](error_code ec, std::size_t n) {
                if (ec) {
                    self->finish();
                    return;
                }
                self->m_in.append(self->m_scan.data(), n);
                self->parse_head_step();
            });
            return;
        }
        if (state == H1Parser::State::Error) {
            send_error_response(status::bad_request);
            return;
        }
        if (m_parser.consumed() > m_limits.max_header_bytes) {
            send_error_response(status::request_header_fields_too_large);
            return;
        }
        m_in.consume(m_parser.consumed());

        auto &head = m_parser.head();
        Version version = head.version;
        if (head.absolute_form && !head.authority.empty()) {
            head.headers.erase("host");
            head.headers.add_lower("host", head.authority);
        }
        {
            const std::size_t hosts = head.headers.count("host");
            if (hosts > 1 || (hosts == 0 && version == Version::Http11)) {
                send_error_response(status::bad_request, {}, version);
                return;
            }
        }
        const auto framing = parse_body_framing(head);
        if (framing.error_status != 0) {
            send_error_response(framing.error_status, {}, version);
            return;
        }
        const bool request_keep_alive = connection_keep_alive(head, version);

        if (is_websocket_upgrade(head)) {
            if (framing.has_body) {
                send_error_response(status::bad_request, {}, version);
                return;
            }
            handle_websocket(head, version);
            return;
        }

        // h2c cleartext upgrade (RFC 9113 §3.2): answer 101 and continue the
        // connection as HTTP/2, replaying this request as stream 1.
        if (m_allow_h2c && version == Version::Http11 && is_h2c_upgrade(head)) {
            do_h2c_upgrade(head, framing);
            return;
        }

        auto request = std::make_shared<Request>(version, m_executor, m_transport->peer());
        request->set_method_token(std::move(head.method_token));
        request->set_target(std::move(head.target));

        // RFC 9110 §10.1.1: a server MUST NOT send 100 Continue to an HTTP/1.0
        // client; ignoring the expectation makes the peer send the body
        // immediately, which is the 1.0 behaviour.
        const bool expect_continue =
            version != Version::Http1 && framing.has_body && header_has_token(head.headers, "expect", "100-continue");
        setup_body_framing(framing.chunked, framing.content_length);
        m_body_state.expect_continue = expect_continue;
        m_continue_sent->store(false, std::memory_order_relaxed);
        request->mutable_headers() = std::move(head.headers);
        m_body_request = request;

        auto self = this->shared_from_this();
        request->body().set_pull_provider([self](PullCompletion done) { self->pull_body_chunk(std::move(done)); });

        auto writer = std::make_shared<Http1ResponseSink<Transport>>(
            m_transport, version, m_deadline, m_limits.idle_timeout, m_limits.alt_svc_value(), m_continue_sent);
        m_active_writer = writer;
        writer->set_keep_alive(request_keep_alive && !expect_continue);
        const bool head_request = m_parser.head().method == Method::Head;
        writer->set_head_request(head_request);
        auto response =
            std::make_shared<ResponseWriter>(maybe_compress_writer(writer, m_executor, request, head_request));

        // A handler that signals completion twice must not drive after_handler
        // twice: the second pass would start a competing head read on the same
        // parser. h2 already guards this; h1 previously did not.
        auto handler_failed = std::make_shared<bool>(false);
        auto completed = std::make_shared<bool>(false);
        auto finish = [self, request, response, writer, request_keep_alive, expect_continue, head_request,
                       handler_failed, completed]() mutable {
            if (*completed)
                return;
            *completed = true;
            self->after_handler(request, writer, request_keep_alive, expect_continue, head_request, *handler_failed);
        };
        try {
            m_dispatch(request, response, m_transport->tls_handle(), finish);
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("h1 handler threw: {}", e.what());
            *handler_failed = true;
            finish();
        } catch (...) {
            SIMPLE_HTTP_ERROR_LOG("h1 handler threw unknown exception");
            *handler_failed = true;
            finish();
        }
    }

    void after_handler(const std::shared_ptr<Request> &request,
                       const std::shared_ptr<Http1ResponseSink<Transport>> &writer, bool request_keep_alive,
                       bool expect_continue, bool head_request, bool handler_failed) {
        const bool skip_drain = expect_continue && !m_body_state.done;
        auto self = this->shared_from_this();
        auto proceed = [self, writer, request_keep_alive, handler_failed](bool drained) {
            writer->connected([self, writer, request_keep_alive, handler_failed, drained](error_code, bool still_open) {
                // The request/response cycle is over; the engine no longer needs
                // the sink alive for the lazy 100-continue path.
                self->m_active_writer.reset();
                if (!may_reuse(!handler_failed, request_keep_alive, writer->keep_alive_out(), still_open, drained)) {
                    self->finish();
                    return;
                }
                self->parse_head();
            });
        };
        (void)request;
        (void)head_request;
        if (skip_drain) {
            proceed(false);
            return;
        }
        drain_body(std::move(proceed));
    }

    // --- body framing --------------------------------------------------------

    // EngineLimits::max_body_bytes == 0 means "no limit", matching h2/h3.
    std::uint64_t body_limit() const noexcept {
        return m_limits.max_body_bytes == 0 ? std::numeric_limits<std::uint64_t>::max()
                                            : static_cast<std::uint64_t>(m_limits.max_body_bytes);
    }

    void setup_body_framing(bool chunked, std::optional<std::string_view> content_length) {
        m_body_state.reset();
        // Trailer bytes are budgeted per message. Without this reset a keep-alive
        // connection accumulates them across requests and a later, perfectly
        // normal request is failed with message_size.
        m_trailer_bytes = 0;
        if (chunked) {
            m_body_state.mode = BodyState::Mode::Chunked;
            return;
        }
        if (content_length) {
            std::uint64_t len = 0;
            (void)parse_uint(*content_length, 10, body_limit(), len);
            m_body_state.mode = BodyState::Mode::Fixed;
            m_body_state.remaining = len;
            if (len == 0)
                m_body_state.done = true;
            return;
        }
        m_body_state.mode = BodyState::Mode::None;
        m_body_state.done = true;
    }

    // Serializes body pulls. The pull path owns m_in/m_scan/m_body_state, so a
    // handler that leaves a read outstanding (a callback read or a detached
    // coroutine) and then returns must not have after_handler() start a second
    // drain pull over the same state. The second request is queued and starts
    // when the first completion runs.
    void pull_body_chunk(PullCompletion done) {
        if (m_pull_active) {
            m_pending_pulls.push_back(std::move(done));
            return;
        }
        m_pull_active = true;
        // weak: a pending pull must not keep the engine alive through a
        // completion it may never deliver.
        auto weak = this->weak_from_this();
        auto finished = std::make_shared<bool>(false);
        auto completion = [weak, done = std::move(done), finished](error_code ec, ReadResult r) mutable {
            if (std::exchange(*finished, true))
                return;
            auto self = weak.lock();
            if (self)
                self->m_pull_active = false;
            std::move(done)(ec, std::move(r));
            if (self)
                self->drain_pending_pulls();
        };
        pull_body_chunk_impl(PullCompletion{std::move(completion)});
    }

    void drain_pending_pulls() {
        if (m_pull_active || m_pending_pulls.empty())
            return;
        auto next = std::move(m_pending_pulls.front());
        m_pending_pulls.pop_front();
        pull_body_chunk(std::move(next));
    }

    void pull_body_chunk_impl(PullCompletion done) {
        if (m_body_state.done) {
            std::move(done)(error_code{}, ReadResult::end());
            return;
        }
        auto self = this->shared_from_this();
        if (m_body_state.expect_continue) {
            if (m_active_writer) {
                // A handler may be writing a response while it reads the body.
                // Route the interim response through the sink's writer instead
                // of racing it on the transport; the sink's shared flag keeps
                // it at-most-once even if the handler also called send_continue.
                //
                // Weak on purpose: the completion lives in the writer's queue,
                // and the engine owns the sink, so a strong capture would form
                // engine -> sink -> writer -> entry -> engine. The request (and
                // its body pull provider) keeps the engine alive for the whole
                // read, so the lock only fails after teardown.
                std::weak_ptr<Http1Engine> guard = self;
                WriteCompletion after = [guard, done = std::move(done)](error_code ec) mutable {
                    auto engine = guard.lock();
                    if (!engine) {
                        if (done)
                            std::move(done)(ec ? ec : make_error_code(asio::error::not_connected), ReadResult{});
                        return;
                    }
                    if (ec) {
                        engine->body_failed(ec, std::move(done));
                        return;
                    }
                    engine->pull_mode(std::move(done));
                };
                m_active_writer->send_continue(std::move(after));
                return;
            }
            if (m_continue_sent->exchange(true, std::memory_order_relaxed)) {
                pull_mode(std::move(done));
                return;
            }
            auto keep = std::make_shared<std::string>(kContinueResponse);
            const std::array<ConstByteSpan, 1> buffers{
                ConstByteSpan{reinterpret_cast<const std::byte *>(keep->data()), keep->size()}};
            touch_deadline();
            m_transport->async_write_seq(boost::span<const ConstByteSpan>{buffers.data(), buffers.size()},
                                         [self, keep, done = std::move(done)](error_code ec, std::size_t) mutable {
                                             if (ec) {
                                                 self->body_failed(ec, std::move(done));
                                                 return;
                                             }
                                             self->pull_mode(std::move(done));
                                         });
            return;
        }
        pull_mode(std::move(done));
    }

    void pull_mode(PullCompletion done) {
        switch (m_body_state.mode) {
        case BodyState::Mode::None:
            m_body_state.done = true;
            std::move(done)(error_code{}, ReadResult::end());
            return;
        case BodyState::Mode::Fixed:
            pull_fixed(std::move(done));
            return;
        case BodyState::Mode::Chunked:
            pull_chunked(std::move(done));
            return;
        }
        std::move(done)(error_code{}, ReadResult::end());
    }

    void body_failed(error_code ec, PullCompletion done) {
        m_body_state.error = true;
        m_body_state.done = true;
        std::move(done)(ec, ReadResult{});
    }

    void pull_fixed(PullCompletion done) {
        if (m_body_state.remaining == 0) {
            m_body_state.done = true;
            std::move(done)(error_code{}, ReadResult::end());
            return;
        }
        if (!m_in.empty()) {
            const std::size_t take =
                static_cast<std::size_t>(std::min<std::uint64_t>(m_body_state.remaining, m_in.size()));
            std::string chunk{m_in.view().substr(0, take)};
            m_in.consume(take);
            m_body_state.remaining -= take;
            if (m_body_state.remaining == 0)
                m_body_state.done = true;
            std::move(done)(error_code{}, ReadResult::chunk(std::move(chunk)));
            return;
        }
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(m_body_state.remaining, kReadChunkBytes));
        auto chunk = std::make_shared<std::string>(want, '\0');
        touch_deadline();
        auto self = this->shared_from_this();
        m_transport->async_read_some(ByteSpan{reinterpret_cast<std::byte *>(chunk->data()), chunk->size()},
                                     [self, chunk, done = std::move(done)](error_code ec, std::size_t n) mutable {
                                         if (ec) {
                                             self->body_failed(ec, std::move(done));
                                             return;
                                         }
                                         chunk->resize(n);
                                         self->m_body_state.remaining -= n;
                                         if (self->m_body_state.remaining == 0)
                                             self->m_body_state.done = true;
                                         std::move(done)(error_code{}, ReadResult::chunk(std::move(*chunk)));
                                     });
    }

    void pull_chunked(PullCompletion done) {
        auto self = this->shared_from_this();
        // Finish the CRLF that terminates the previous chunk's data.
        if (m_body_state.chunk_crlf_pending) {
            read_exact(2, [self, done = std::move(done)](error_code ec, std::string crlf) mutable {
                if (ec) {
                    self->body_failed(ec, std::move(done));
                    return;
                }
                if (crlf != "\r\n") {
                    self->body_failed(make_error_code(asio::error::invalid_argument), std::move(done));
                    return;
                }
                self->m_body_state.chunk_crlf_pending = false;
                self->pull_chunked(std::move(done));
            });
            return;
        }
        // Deliver a bounded slice of the current chunk. The chunk length is
        // attacker-controlled; allocating it whole let one unauthenticated
        // request reserve up to max_body_bytes (64 MiB by default) without
        // sending a byte, and with max_body_bytes=0 a ~2^63 length threw
        // length_error out of the io_context (std::terminate).
        if (m_body_state.chunk_remaining > 0) {
            const auto want =
                static_cast<std::size_t>(std::min<std::uint64_t>(m_body_state.chunk_remaining, kReadChunkBytes));
            read_exact(want, [self, done = std::move(done)](error_code ec, std::string chunk) mutable {
                if (ec) {
                    self->body_failed(ec, std::move(done));
                    return;
                }
                self->m_body_state.chunk_remaining -= chunk.size();
                if (self->m_body_state.chunk_remaining == 0)
                    self->m_body_state.chunk_crlf_pending = true;
                std::move(done)(error_code{}, ReadResult::chunk(std::move(chunk)));
            });
            return;
        }
        read_line([self, done = std::move(done)](error_code ec, std::string size_line) mutable {
            if (ec) {
                self->body_failed(ec, std::move(done));
                return;
            }
            std::string_view size_tok{size_line};
            if (auto semi = size_tok.find(';'); semi != std::string_view::npos) {
                size_tok = size_tok.substr(0, semi);
            }
            std::uint64_t chunk_len = 0;
            if (parse_uint(size_tok, 16, self->body_limit(), chunk_len) != NumParse::Ok) {
                self->body_failed(make_error_code(asio::error::invalid_argument), std::move(done));
                return;
            }
            if (chunk_len == 0) {
                self->read_trailers(std::move(done));
                return;
            }
            // Checked add: with max_body_bytes=0 body_limit() is UINT64_MAX, and
            // an unchecked total += chunk_len wraps and disables the limit.
            const auto budget = self->body_limit() - self->m_body_state.total;
            if (chunk_len > budget) {
                self->body_failed(make_error_code(asio::error::message_size), std::move(done));
                return;
            }
            self->m_body_state.total += chunk_len;
            self->m_body_state.chunk_remaining = chunk_len;
            self->pull_chunked(std::move(done));
        });
    }

    void read_trailers(PullCompletion done) {
        auto self = this->shared_from_this();
        read_line([self, done = std::move(done)](error_code ec, std::string trailer) mutable {
            if (ec) {
                self->body_failed(ec, std::move(done));
                return;
            }
            if (trailer.empty()) {
                self->m_body_state.done = true;
                std::move(done)(error_code{}, ReadResult::end());
                return;
            }
            self->m_trailer_bytes += trailer.size() + 2;
            if (self->m_trailer_bytes > self->m_limits.max_header_bytes) {
                self->body_failed(make_error_code(asio::error::message_size), std::move(done));
                return;
            }
            if (auto request = self->m_body_request.lock()) {
                const auto colon = trailer.find(':');
                if (colon != std::string::npos) {
                    const std::string_view name = trim_ows(std::string_view{trailer}.substr(0, colon));
                    const std::string_view value = trim_ows(std::string_view{trailer}.substr(colon + 1));
                    const std::string lower_name = ascii_lower_copy(name);
                    if (is_http_token(name) && !contains_ctl(value) && !is_forbidden_trailer_field(lower_name)) {
                        request->mutable_trailers().add_lower(lower_name, std::string{value});
                    }
                }
            }
            self->read_trailers(std::move(done));
        });
    }

    void drain_body(std::function<void(bool)> done) {
        if (m_body_state.done) {
            done(!m_body_state.error);
            return;
        }
        auto self = this->shared_from_this();
        pull_body_chunk([self, done = std::move(done)](error_code ec, ReadResult r) mutable {
            if (ec) {
                done(false);
                return;
            }
            if (r.eof) {
                done(!self->m_body_state.error);
                return;
            }
            self->drain_body(std::move(done));
        });
    }

    // --- framed reads --------------------------------------------------------

    // Reads one CRLF- or LF-terminated line, topping up from the transport.
    void read_line(LineCompletion done) {
        auto state = std::make_shared<std::string>();
        m_body_state.line_overflow = false;
        loop_read_line(std::move(state), std::move(done));
    }

    void loop_read_line(std::shared_ptr<std::string> out, LineCompletion done) {
        const std::string_view buffered = m_in.view();
        const std::size_t nl = buffered.find('\n');
        if (nl != std::string_view::npos) {
            const std::size_t end = (nl > 0 && buffered[nl - 1] == '\r') ? nl - 1 : nl;
            out->assign(buffered.data(), end);
            m_in.consume(nl + 1);
            std::move(done)(error_code{}, std::move(*out));
            return;
        }
        if (m_in.size() >= m_limits.max_header_bytes) {
            m_body_state.line_overflow = true;
            std::move(done)(make_error_code(asio::error::message_size), std::string{});
            return;
        }
        auto self = this->shared_from_this();
        read_scan([self, out, done = std::move(done)](error_code ec, std::size_t n) mutable {
            if (ec) {
                std::move(done)(ec, std::string{});
                return;
            }
            self->m_in.append(self->m_scan.data(), n);
            self->loop_read_line(std::move(out), std::move(done));
        });
    }

    // Reads exactly `len` bytes, copying whatever is buffered first and then
    // reading the rest straight into the output string.
    void read_exact(std::size_t len, LineCompletion done) {
        auto out = std::make_shared<std::string>(len, '\0');
        const std::size_t have = std::min<std::size_t>(len, m_in.size());
        if (have != 0) {
            std::memcpy(out->data(), m_in.view().data(), have);
            m_in.consume(have);
        }
        loop_read_exact(std::move(out), have, len, std::move(done));
    }

    void loop_read_exact(std::shared_ptr<std::string> out, std::size_t have, std::size_t len, LineCompletion done) {
        if (have == len) {
            std::move(done)(error_code{}, std::move(*out));
            return;
        }
        auto self = this->shared_from_this();
        touch_deadline();
        m_transport->async_read_some(
            ByteSpan{reinterpret_cast<std::byte *>(out->data() + have), len - have},
            [self, out, have, len, done = std::move(done)](error_code ec, std::size_t n) mutable {
                if (ec) {
                    std::move(done)(ec, std::string{});
                    return;
                }
                self->loop_read_exact(std::move(out), have + n, len, std::move(done));
            });
    }

    // --- errors and upgrades -------------------------------------------------

    void send_error_response(int status, Headers extra = {}, Version version = Version::Http11) {
        auto writer = std::make_shared<Http1ResponseSink<Transport>>(m_transport, version, m_deadline,
                                                                     m_limits.idle_timeout, m_limits.alt_svc_value());
        writer->set_keep_alive(false);
        std::string body{reason_phrase(status)};
        auto self = this->shared_from_this();
        writer->send(status, std::move(extra), std::move(body), [self](error_code) { self->finish(); });
    }

    static bool is_websocket_upgrade(const ParsedHead &head) {
        if (!header_has_token(head.headers, "upgrade", "websocket")) {
            return false;
        }
        if (!header_has_token(head.headers, "connection", "upgrade")) {
            return false;
        }
        return head.headers.get("sec-websocket-key").has_value();
    }

    static std::string_view request_path(std::string_view target) {
        if (auto q = target.find('?'); q != std::string_view::npos) {
            return target.substr(0, q);
        }
        return target;
    }

    void handle_websocket(const ParsedHead &head, Version version) {
        // RFC 6455 §4.1/§4.2.1: the handshake is a GET over HTTP/1.1 with a
        // 16-byte, base64-encoded key. Anything else is a bad handshake, not a
        // route to fall through to.
        const auto ws_key = head.headers.get("sec-websocket-key");
        if (head.method != Method::Get || head.version != Version::Http11 || !ws_key ||
            base64_decode(*ws_key).size() != 16) {
            send_error_response(status::bad_request, {}, version);
            return;
        }
        // RFC 6455 §4.2.1 version gate.
        const auto ws_version = head.headers.get("sec-websocket-version");
        if (!ws_version) {
            send_error_response(status::bad_request, {}, version);
            return;
        }
        if (*ws_version != "13") {
            Headers extra;
            extra.add_lower("sec-websocket-version", "13");
            send_error_response(status::upgrade_required, std::move(extra), version);
            return;
        }

        auto request = std::make_shared<Request>(version, m_executor, m_transport->peer());
        request->set_method_token(head.method_token);
        request->set_target(head.target);
        request->mutable_headers() = head.headers;
        auto writer = std::make_shared<Http1ResponseSink<Transport>>(m_transport, head.version, m_deadline,
                                                                     m_limits.idle_timeout, m_limits.alt_svc_value());
        auto response = std::make_shared<ResponseWriter>(writer);

        auto self = this->shared_from_this();
        WsUpgrade upgrade = [self](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                                   CallbackWsHandler handler, WsUpgradeCompletion done) mutable {
            self->try_websocket_upgrade(std::move(req), std::move(res), std::move(handler), std::move(done));
        };

        const std::string_view path = request_path(head.target);
        if (m_ws_lookup) {
            if (auto run = m_ws_lookup(path)) {
                (*run)(request, response, m_transport->tls_handle(), std::move(upgrade),
                       [self](error_code, bool) { self->finish(); });
                return;
            }
        }

        // Byte-level proxy pass-through, between the exact and the regex local
        // ws routes (nginx-style precedence).
        if (m_ws_proxy_lookup) {
            if (auto target = m_ws_proxy_lookup(path)) {
                m_owner = TransportOwner::WebSocket; // stop the watchdog from closing the transport
                std::string peer_address = address_string_if_specified(m_transport->peer().address());
                std::string initial{m_in.view()};
                async_run_ws_proxy(m_transport, head, std::move(initial), std::move(*target), m_limits.idle_timeout,
                                   m_limits.max_header_bytes, std::move(peer_address),
                                   m_transport->tls_handle().has_value(), [self](bool) { self->finish(); });
                return;
            }
        }

        if (m_ws_regex_lookup) {
            if (auto run = m_ws_regex_lookup(path)) {
                (*run)(request, response, m_transport->tls_handle(), std::move(upgrade),
                       [self](error_code, bool) { self->finish(); });
                return;
            }
        }
        send_error_response(status::not_found, {}, version);
    }

    void try_websocket_upgrade(std::shared_ptr<Request> request, std::shared_ptr<ResponseWriter> res,
                               CallbackWsHandler handler, WsUpgradeCompletion done) {
        WsDeflateConfig deflate;
        if (m_limits.ws_compression) {
            const auto offered = request->header("sec-websocket-extensions").value_or(std::string_view{});
            if (auto parsed = ws_parse_deflate_offer(offered)) {
                deflate = *parsed;
            }
        }

        auto key = request->header("sec-websocket-key");
        if (!key) {
            std::move(done)(make_error_code(asio::error::invalid_argument), false);
            return;
        }
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: ";
        resp += ws_accept_key(*key);
        resp += "\r\n";
        if (deflate.enabled) {
            resp += "Sec-WebSocket-Extensions: ";
            resp += ws_deflate_response_value(deflate);
            resp += "\r\n";
        }
        const auto &fields = res->headers();
        if (!fields.contains("server")) {
            resp += "Server: ";
            resp += server_product;
            resp += "\r\n";
        }
        for (const auto &[name, value] : fields) {
            resp += name;
            resp += ": ";
            resp += value;
            resp += "\r\n";
        }
        resp += "\r\n";

        auto self = this->shared_from_this();
        auto keep = std::make_shared<std::string>(std::move(resp));
        touch_deadline();
        m_transport->async_write(
            ConstByteSpan{reinterpret_cast<const std::byte *>(keep->data()), keep->size()},
            [self, keep, request, handler, deflate, done = std::move(done)](error_code ec, std::size_t) mutable {
                if (ec) {
                    std::move(done)(ec, false);
                    return;
                }
                self->m_owner = TransportOwner::WebSocket;
                auto backend = std::make_shared<WsBackendImpl<Transport>>(
                    self->m_transport, self->m_limits.max_body_bytes, self->m_limits.idle_timeout,
                    /*expect_masked=*/true, self->m_limits.ws_read_ahead_bytes, deflate);
                backend->feed(self->m_in.view());
                self->m_in.clear();
                auto ws = std::make_shared<WebSocket>(std::move(backend));
                ws->run_writer();
                // Run the ws handler; when it finishes, close gracefully.
                invoke_ws_handler(simple_http::WsHandler(handler), request, ws, [ws, done = std::move(done)]() mutable {
                    ws->async_close(
                        [done = std::move(done)](error_code) mutable { std::move(done)(error_code{}, true); });
                });
            });
    }

    // --- helpers -------------------------------------------------------------

    enum class NumParse { Ok, Invalid, Overflow };
    static NumParse parse_uint(std::string_view s, unsigned base, std::uint64_t max, std::uint64_t &out) {
        if (s.empty())
            return NumParse::Invalid;
        std::uint64_t v = 0;
        for (char c : s) {
            unsigned d;
            if (c >= '0' && c <= '9') {
                d = static_cast<unsigned>(c - '0');
            } else if (base == 16 && c >= 'a' && c <= 'f') {
                d = 10u + static_cast<unsigned>(c - 'a');
            } else if (base == 16 && c >= 'A' && c <= 'F') {
                d = 10u + static_cast<unsigned>(c - 'A');
            } else {
                return NumParse::Invalid;
            }
            // Written this way rather than `v > (max - d) / base`: that form
            // underflows when max < d (a limit of 0, or 1 for base 16), and the
            // underflow silently turns an over-limit value into Ok.
            if (v > max / base || (v == max / base && d > max % base))
                return NumParse::Overflow;
            v = v * base + d;
        }
        out = v;
        return NumParse::Ok;
    }

    static bool connection_keep_alive(const ParsedHead &head, Version version) {
        if (header_has_token(head.headers, "connection", "close")) {
            return false;
        }
        if (header_has_token(head.headers, "connection", "keep-alive")) {
            return true;
        }
        return version != Version::Http1;
    }

    static bool may_reuse(bool handler_ok, bool request_keep_alive, bool response_keep_alive, bool transport_open,
                          bool body_drained) {
        return handler_ok && request_keep_alive && response_keep_alive && transport_open && body_drained;
    }

    struct BodyFraming {
        int error_status{0};
        bool chunked{false};
        bool has_body{false};
        std::optional<std::string_view> content_length;
    };

    BodyFraming parse_body_framing(const ParsedHead &head) const {
        BodyFraming out;
        const auto te = head.headers.get("transfer-encoding");
        const auto cl = head.headers.get("content-length");
        // RFC 9112 §6.1: Transfer-Encoding is an HTTP/1.1 frame; a 1.0 request
        // carrying it must be rejected rather than decoded as chunked (a front
        // end that ignores TE for 1.0 would otherwise disagree about framing).
        if (head.version == Version::Http1 && te) {
            out.error_status = status::bad_request;
            return out;
        }
        if (te) {
            if (!value_has_token(*te, "chunked")) {
                out.error_status = status::not_implemented;
                return out;
            }
            if (cl) {
                out.error_status = status::bad_request; // CL + TE: smuggling shape
                return out;
            }
            out.chunked = true;
            out.has_body = true;
            return out;
        }
        if (cl) {
            if (head.headers.count("content-length") > 1) {
                out.error_status = status::bad_request;
                return out;
            }
            std::uint64_t len = 0;
            const auto parsed = parse_uint(*cl, 10, body_limit(), len);
            if (parsed == NumParse::Invalid) {
                out.error_status = status::bad_request;
                return out;
            }
            if (parsed == NumParse::Overflow) {
                out.error_status = status::payload_too_large;
                return out;
            }
            out.content_length = *cl;
            out.has_body = len > 0;
        }
        return out;
    }
    // Detects an h2c cleartext upgrade: Upgrade: h2c, Connection: Upgrade, and
    // the HTTP2-Settings header itself (RFC 9113 §3.2).
    static bool is_h2c_upgrade(const ParsedHead &head) {
        if (!header_has_token(head.headers, "upgrade", "h2c")) {
            return false;
        }
        if (!header_has_token(head.headers, "connection", "upgrade")) {
            return false;
        }
        return head.headers.get("http2-settings").has_value();
    }

    static constexpr std::string_view kH2cUpgradeResponse = "HTTP/1.1 101 Switching Protocols\r\n"
                                                            "Connection: Upgrade\r\n"
                                                            "Upgrade: h2c\r\n\r\n";

    // Performs the h2c upgrade: reads the upgrade request's body, sends 101, and
    // hands the transport (plus any bytes read past the request) to an
    // Http2Engine that replays this request as stream 1.
    void do_h2c_upgrade(const ParsedHead &head, const BodyFraming &framing) {
        setup_body_framing(framing.chunked, framing.content_length);
        m_body_state.expect_continue = framing.has_body && header_has_token(head.headers, "expect", "100-continue");
        m_continue_sent->store(false, std::memory_order_relaxed);
        auto body = std::make_shared<std::string>();
        auto self = this->shared_from_this();
        drain_h2c_body(head, std::move(body));
    }

    void drain_h2c_body(ParsedHead head, std::shared_ptr<std::string> body) {
        if (m_body_state.done) {
            start_h2c(std::move(head), std::move(*body));
            return;
        }
        auto self = this->shared_from_this();
        pull_body_chunk([self, head = std::move(head), body](error_code ec, ReadResult r) mutable {
            if (ec) {
                self->m_transport->close();
                self->finish();
                return;
            }
            if (r.eof) {
                self->start_h2c(std::move(head), std::move(*body));
                return;
            }
            if (self->m_limits.max_body_bytes != 0 && body->size() + r.data.size() > self->m_limits.max_body_bytes) {
                self->send_error_response(status::payload_too_large, {}, head.version);
                return;
            }
            body->append(r.data);
            self->drain_h2c_body(std::move(head), std::move(body));
        });
    }

    void start_h2c(ParsedHead head, std::string body) {
        auto self = this->shared_from_this();
        // The transport now belongs to the h2 engine. Stop the h1 idle watchdog
        // and keep finish() from shutting the transport down: ownership only
        // moves after the 101 has been written.
        m_finished = true;
        m_watchdog->cancel();
        auto response = std::make_shared<std::string>(kH2cUpgradeResponse);
        auto transport = m_transport;
        detail::async_error_only(
            m_executor,
            [self, head = std::move(head), body = std::move(body)](error_code ec) mutable {
                if (ec) {
                    self->m_transport->close();
                    self->complete_done();
                    return;
                }
                // Bytes already read past the request are the client's
                // connection preface and whatever followed it.
                std::string buffered{self->m_in.view()};
                self->m_in.consume(self->m_in.size());
                std::string settings_b64{*head.headers.get("http2-settings")};
                self->m_owner = TransportOwner::Http2;
                auto engine = std::make_shared<Http2Engine<Transport>>(self->m_transport, self->m_limits);
                engine->run_h2c(self->m_dispatch, std::move(settings_b64), head.method, head.method_token,
                                std::string{head.target}, std::move(head.headers), std::move(body), std::move(buffered),
                                self->m_ws_lookup, self->m_ws_proxy_lookup, self->m_ws_regex_lookup,
                                [self]() { self->complete_done(); });
            },
            [transport, response](asio::any_completion_handler<void(error_code, std::size_t)> handler) {
                const std::array<ConstByteSpan, 1> buffers{
                    ConstByteSpan{reinterpret_cast<const std::byte *>(response->data()), response->size()}};
                transport->async_write_seq(boost::span<const ConstByteSpan>{buffers.data(), buffers.size()},
                                           std::move(handler));
            });
    }

    // Bytes read past the request head (and chunked framing bytes).
    class ReadBuf {
      public:
        void assign(std::string_view bytes) {
            m_data.assign(bytes);
            m_pos = 0;
        }
        void clear() {
            m_data.clear();
            m_pos = 0;
        }
        [[nodiscard]] bool empty() const { return m_pos >= m_data.size(); }
        [[nodiscard]] std::size_t size() const { return m_data.size() - m_pos; }
        [[nodiscard]] std::string_view view() const { return std::string_view{m_data}.substr(m_pos); }
        void append(const std::byte *data, std::size_t n) {
            if (n == 0)
                return;
            if (m_pos >= 4096 && m_pos * 2 >= m_data.size()) {
                m_data.erase(0, m_pos);
                m_pos = 0;
            }
            m_data.append(reinterpret_cast<const char *>(data), n);
        }
        void consume(std::size_t n) {
            m_pos += std::min(n, size());
            if (m_pos >= m_data.size())
                clear();
        }

      private:
        std::string m_data;
        std::size_t m_pos{0};
    };

    struct BodyState {
        enum class Mode { None, Fixed, Chunked };
        Mode mode{Mode::None};
        std::uint64_t remaining{0};
        std::uint64_t total{0};
        // Chunked body progress. A chunk is delivered in bounded pieces rather
        // than allocated whole: a peer may declare a multi-gigabyte chunk and
        // send nothing, and the old code allocated its full size up front.
        std::uint64_t chunk_remaining{0};
        bool chunk_crlf_pending{false};
        bool done{false};
        bool error{false};
        bool line_overflow{false};
        bool expect_continue{false};
        void reset() { *this = BodyState{}; }
    };

    static constexpr std::string_view kContinueResponse = "HTTP/1.1 100 Continue\r\n\r\n";
    static constexpr std::size_t kReadChunkBytes = 8192;

    enum class TransportOwner { Engine, WebSocket, Http2 };
    [[nodiscard]] bool handed_off() const { return m_owner != TransportOwner::Engine; }

    std::shared_ptr<Transport> m_transport;
    asio::any_io_executor m_executor;
    EngineLimits m_limits;
    SharedDeadline m_deadline{std::make_shared<std::chrono::steady_clock::time_point>()};
    ReadBuf m_in;
    std::array<std::byte, kReadChunkBytes> m_scan{};
    H1Parser m_parser;
    BodyState m_body_state;
    std::weak_ptr<Request> m_body_request;
    // The sink handling the current request, used to serialize the lazy
    // 100-continue with any response the handler is writing concurrently.
    std::shared_ptr<Http1ResponseSink<Transport>> m_active_writer;
    std::shared_ptr<std::atomic<bool>> m_continue_sent{std::make_shared<std::atomic<bool>>(false)};
    bool m_pull_active{false};
    std::deque<PullCompletion> m_pending_pulls;
    std::size_t m_trailer_bytes{0};
    Dispatcher m_dispatch;
    WsLookup m_ws_lookup;
    WsProxyLookup m_ws_proxy_lookup; // WebSocket proxy-route lookup (empty if none)
    WsLookup m_ws_regex_lookup;
    std::function<void()> m_on_done;
    std::shared_ptr<asio::steady_timer> m_watchdog;
    TransportOwner m_owner{TransportOwner::Engine};
    bool m_allow_h2c{true};
    bool m_finished{false};
    bool m_done_called{false};
};

} // namespace simple_http
