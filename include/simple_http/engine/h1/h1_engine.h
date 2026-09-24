#pragma once

// HTTP/1.x engine — beast-free.
//
// Http1Engine::run drives one connection: read bytes off the Transport into an
// H1Parser (engine/h1/h1_parser.h, ported from paozhu's byte-level request-line
// / header parsing) until a full head is parsed, frame the request body
// (chunked / Content-Length / none) and feed it into Request::body(), then hand
// the request to the dispatcher (Router) as (Request&, Response&, SslHandle).
// The loop repeats while the connection is keep-alive. Responses go through
// Http1ResponseWriter, which hand-serializes either a Content-Length reply
// (one-shot) or a chunked stream directly onto the transport.
//
// Upgrades: an Upgrade: websocket request (matched by a registered ws route) is
// handed to the WebSocket layer; an Upgrade: h2c request carrying an
// HTTP2-Settings header is answered with 101 and handed to Http2Engine::run_h2c,
// which replays the original request as HTTP/2 stream 1 and continues as h2.
//
// Thread safety (model A): every write first hops onto the connection executor
// before touching the transport, so a Response captured by the handler is safe
// to use from any thread.

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include "../../core/http_method.h"
#include "../../core/http_status.h"
#include "../../core/limits.h"
#include "../../core/logging.h"
#include "../../core/types.h"
#include "../../core/version.h"
#include "../../proto/request.h"
#include "../../proto/response.h"
#include "../../proto/response_writer.h"
#include "../../proto/websocket.h"
#include "../../transport/transport.h"
#include "../dispatcher.h"
#include "../h2/h2_engine.h"
#include "h1_parser.h"
#include "ws_proxy.h"

namespace simple_http {

namespace asio = boost::asio;

// A connection-level idle deadline shared between the engine and the response
// writer(s) it creates, so that both reads and writes can refresh it and the
// watchdog reaps a connection only when it is genuinely idle in both directions.
using SharedDeadline = std::shared_ptr<std::chrono::steady_clock::time_point>;

// ResponseWriter for HTTP/1.x. Hand-serializes directly onto the transport.
template <TransportLike Transport>
class Http1ResponseWriter : public ResponseWriter {
  public:
    // `deadline` and `idle_timeout` let a write refresh the connection's idle
    // deadline (shared with the engine's read path and watchdog).
    Http1ResponseWriter(std::shared_ptr<Transport> transport, Version version, SharedDeadline deadline,
                        std::chrono::steady_clock::duration idle_timeout)
        : m_transport(std::move(transport)),
          m_executor(m_transport->get_executor()),
          m_version(version),
          m_deadline(std::move(deadline)),
          m_idle_timeout(idle_timeout) {}

    asio::awaitable<error_code> send(int status, Headers headers, std::string body) override {
        co_await hop();
        if (!m_open) co_return make_error_code(asio::error::not_connected);

        headers.add_lower("content-length", std::to_string(body.size()));
        std::string out = status_line(status);
        append_headers(out, headers);
        out.append("\r\n");
        out.append(body);

        auto ec = co_await write_raw(out);
        if (ec) m_open = false;
        co_return ec;
    }

    asio::awaitable<error_code> send_headers(int status, Headers headers) override {
        co_await hop();
        if (!m_open) co_return make_error_code(asio::error::not_connected);

        headers.add_lower("transfer-encoding", "chunked");
        std::string out = status_line(status);
        append_headers(out, headers);
        out.append("\r\n");

        auto ec = co_await write_raw(out);
        if (ec) m_open = false;
        co_return ec;
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        co_await hop();
        if (!m_open) co_return make_error_code(asio::error::not_connected);
        auto ec = co_await write_raw(encode_chunk(data));
        if (ec) m_open = false;
        co_return ec;
    }

    asio::awaitable<error_code> send_last(std::string data) override {
        co_await hop();
        if (!m_open) co_return make_error_code(asio::error::not_connected);
        std::string out;
        if (!data.empty()) out = encode_chunk(data);
        out.append("0\r\n\r\n");  // last-chunk + trailer-less terminator
        auto ec = co_await write_raw(out);
        if (ec) m_open = false;
        co_return ec;
    }

    bool connected() const override { return m_open; }
    void close() override {
        m_open = false;
        m_transport->close();
    }
    Version version() const override { return m_version; }

    // Set by the engine from the parsed request's Connection header so the
    // response line matches what the engine decided about keep-alive.
    void set_keep_alive(bool ka) { m_keep_alive_out = ka; }
    bool keep_alive_out() const { return m_keep_alive_out; }

  private:
    std::string status_line(int status) const {
        return std::format("{} {} {}\r\n",
                            m_version == Version::Http1 ? "HTTP/1.0" : "HTTP/1.1", 
                            status,
                            reason_phrase(status));
    }

    void append_headers(std::string& out, const Headers& headers) const {
        bool saw_connection = false;
        for (const auto& [name, value] : headers) {
            if (name == "connection") saw_connection = true;
            out.append(name);
            out.append(": ");
            out.append(value);
            out.append("\r\n");
        }
        if (!saw_connection) {
            out.append("connection: ");
            out.append(m_keep_alive_out ? "keep-alive" : "close");
            out.append("\r\n");
        }
    }

    static std::string encode_chunk(const std::string& data) {
        std::string out;
        char size_buf[2 * sizeof(std::size_t) + 1];
        int n = std::snprintf(size_buf, sizeof(size_buf), "%zx", data.size());
        out.append(size_buf, static_cast<std::size_t>(n));
        out.append("\r\n");
        out.append(data);
        out.append("\r\n");
        return out;
    }

    asio::awaitable<error_code> write_raw(const std::string& out) {
        // Writing is connection activity: refresh the shared idle deadline so the
        // watchdog does not reap a connection busy streaming a response.
        if (m_deadline) *m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
        // Transport::async_write is a composed operation (asio::async_write): it
        // writes the whole buffer or returns an error, so no partial-write loop
        // is needed here.
        auto [ec, n] = co_await m_transport->async_write(
            std::as_bytes(std::span<const char>{out.data(), out.size()}));
        (void)n;
        co_return ec;
    }

    asio::awaitable<void> hop() {
        co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable));
    }

    std::shared_ptr<Transport> m_transport;
    decltype(std::declval<Transport&>().get_executor()) m_executor;
    Version m_version;
    bool m_open{true};
    bool m_keep_alive_out{true};
    SharedDeadline m_deadline;  // shared with the engine (read path + watchdog)
    std::chrono::steady_clock::duration m_idle_timeout{};
};

template <TransportLike Transport>
class Http1Engine {
  public:
    explicit Http1Engine(std::shared_ptr<Transport> transport, EngineLimits limits = {})
        : m_transport(std::move(transport)), m_executor(m_transport->get_executor()), m_limits(limits) {}

    // Serve requests on this connection until it closes or a request is not
    // keep-alive. `dispatch` runs the handler for each request. `initial` may
    // contain bytes already read from the transport during protocol detection
    // (e.g. by a preceding peek); they are consumed before reading more.
    asio::awaitable<void> run(const Dispatcher& dispatch, std::string initial = {}, WsLookup ws_lookup = {},
                              WsProxyLookup ws_proxy_lookup = {}) {
        using namespace asio::experimental::awaitable_operators;
        m_ws_lookup = std::move(ws_lookup);
        m_ws_proxy_lookup = std::move(ws_proxy_lookup);
        *m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
        co_await (serve_loop(dispatch, std::move(initial)) || watchdog());
        if (!m_upgraded) {
            m_transport->close();
        }
        co_return;
    }

  private:
    // Idle watchdog: closes the transport once no bytes have arrived within the
    // idle timeout, which unblocks any pending read in serve_loop and ends the
    // connection. Mirrors the HTTP/2 engine's watchdog (Slowloris defense).
    asio::awaitable<void> watchdog() {
        asio::steady_timer timer{m_executor};
        for (;;) {
            timer.expires_at(*m_deadline);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            if (m_upgraded) {
                // The connection was handed to the WebSocket layer, which manages
                // its own lifetime; stop watching without closing the transport.
                for (;;) {
                    timer.expires_after(std::chrono::hours(24));
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                }
            }
            if (std::chrono::steady_clock::now() >= *m_deadline) break;  // idle timeout elapsed
        }
        co_return;
    }

    // A read that refreshes the idle deadline on every attempt; all transport
    // reads in the engine go through this so the watchdog covers header reads,
    // body reads and pipelined-request reads alike. Writes refresh the same
    // shared deadline from Http1ResponseWriter::write_raw.
    asio::awaitable<std::pair<error_code, std::size_t>> read_some(std::span<std::byte> buf) {
        *m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
        co_return co_await m_transport->async_read_some(buf);
    }

    asio::awaitable<void> serve_loop(const Dispatcher& dispatch, std::string initial) {
        m_buf = std::move(initial);
        auto exec = m_transport->get_executor();

        for (;;) {
            H1Parser parser;
            parser.feed(m_buf);
            auto state = parser.parse_head();
            while (state == H1Parser::State::NeedMore) {
                if (parser.buffered() > m_limits.max_header_bytes) {
                    co_await send_error_response(431);  // Request Header Fields Too Large
                    co_return;
                }
                std::array<std::byte, 8192> tmp{};
                auto [ec, n] = co_await read_some(std::span<std::byte>{tmp});
                if (ec) {
                    co_return;  // EOF or error before a full head — nothing more to do
                }
                parser.feed(tmp.data(), n);
                state = parser.parse_head();
            }
            if (state == H1Parser::State::Error) {
                co_await send_error_response(400);
                co_return;
            }

            const auto& head = parser.head();
            Version version = head.version;
            bool keep_alive = connection_keep_alive(head, version);

            // WebSocket upgrade: an Upgrade: websocket request with a
            // Sec-WebSocket-Key. A path registered as a proxy route is spliced
            // byte-for-byte to its backend (transparent reverse proxy); otherwise
            // a matched local ws route hands the connection to the WebSocket
            // layer. On success serve_loop returns (the transport is no longer
            // owned by this engine).
            if (is_websocket_upgrade(head)) {
                std::string ws_path = request_path(head.target);

                // 1) Byte-level proxy pass-through takes precedence.
                if (m_ws_proxy_lookup) {
                    if (auto target = m_ws_proxy_lookup(ws_path)) {
                        m_upgraded = true;  // stop the watchdog from closing the transport
                        co_await run_ws_proxy(m_transport, head, std::string{parser.remainder()},
                                              std::move(*target));
                        co_return;  // tunnel finished; run() must not touch the transport
                    }
                }

                // 2) Local WebSocket handler.
                if (m_ws_lookup) {
                    m_buf.assign(parser.remainder());
                    if (co_await try_websocket_upgrade(head)) {
                        co_return;  // connection upgraded; run() must not close it
                    }
                }

                // Upgrade requested but no proxy/route matched (or handshake
                // failed): reply 404 and close.
                co_await send_error_response(404);
                co_return;
            }

            // h2c cleartext upgrade: Upgrade: h2c + HTTP2-Settings. Answer 101 and
            // continue the connection as HTTP/2, replaying this request as
            // stream 1 (RFC 7540 §3.2). The transport is then owned by the h2
            // engine, so serve_loop returns.
            if (is_h2c_upgrade(head)) {
                m_buf.assign(parser.remainder());
                co_await do_h2c_upgrade(dispatch, head);
                co_return;
            }

            // Reject an oversized or malformed Content-Length before running the
            // handler (chunked bodies, which have no declared length, are bounded
            // incrementally in read_chunked_body instead).
            {
                auto te = head.headers.get("transfer-encoding");
                bool chunked = te && icontains(*te, "chunked");
                auto cl = head.headers.get("content-length");
                if (!chunked && cl) {
                    std::uint64_t len = 0;
                    switch (parse_uint(*cl, 10, m_limits.max_body_bytes, len)) {
                        case NumParse::Invalid:
                            co_await send_error_response(400);  // Bad Request
                            co_return;
                        case NumParse::Overflow:
                            co_await send_error_response(413);  // Payload Too Large
                            co_return;
                        case NumParse::Ok:
                            break;
                    }
                }
            }

            auto request = std::make_shared<Request>(version, exec, m_transport->peer());
            request->set_method_token(head.method_token);
            request->set_target(head.target);
            request->mutable_headers() = head.headers;

            // Bytes already read past the head are the start of the body.
            m_buf.assign(parser.remainder());

            // Install pull-mode body framing: the handler reads the body on demand
            // (req.body().read()), which pulls/frames bytes off the socket only
            // when asked. A handler that ignores the body triggers no body reads.
            setup_body_framing(head);
            request->body().set_pull_provider(
                [this]() -> asio::awaitable<std::expected<ReadResult, error_code>> {
                    co_return co_await pull_body_chunk();
                });

            auto writer = std::make_shared<Http1ResponseWriter<Transport>>(m_transport, version, m_deadline,
                                                                           m_limits.idle_timeout);
            writer->set_keep_alive(keep_alive);
            auto response = std::make_shared<Response>(writer);

            // Run the handler to completion (h1 is half-duplex: the reader and the
            // response writer are the same coroutine, no concurrent read/write).
            bool handler_failed = false;
            co_await run_handler(dispatch, request, response, handler_failed);

            // Drain any request body the handler did not read, so the connection
            // is positioned at the next pipelined request. If draining fails or
            // the body was oversized/malformed, we cannot safely reuse the
            // connection.
            bool drained = co_await drain_body();

            if (handler_failed || !keep_alive || !writer->connected() || !drained) {
                break;
            }
        }
        co_return;  // run() closes the transport when serve_loop returns
    }

    // Configures body framing for the current request from its head (called once
    // per request, before dispatch). Chunked takes precedence over Content-Length
    // (RFC 7230 §3.3.3); a request with neither has no body.
    void setup_body_framing(const ParsedHead& head) {
        m_body_done = false;
        m_body_remaining = 0;
        m_body_total = 0;
        auto te = head.headers.get("transfer-encoding");
        if (te && icontains(*te, "chunked")) {
            m_body_mode = BodyMode::Chunked;
            return;
        }
        auto cl = head.headers.get("content-length");
        if (cl) {
            std::uint64_t len = 0;
            // serve_loop already validated this; Ok is guaranteed here.
            (void)parse_uint(*cl, 10, m_limits.max_body_bytes, len);
            m_body_mode = BodyMode::Fixed;
            m_body_remaining = len;
            if (len == 0) m_body_done = true;
            return;
        }
        m_body_mode = BodyMode::None;
        m_body_done = true;  // no body
    }

    // Pull one body chunk on demand (Body::read calls this via the provider).
    // Returns a data chunk, end-of-body, or an error_code.
    asio::awaitable<std::expected<ReadResult, error_code>> pull_body_chunk() {
        if (m_body_done) co_return ReadResult::end();
        switch (m_body_mode) {
            case BodyMode::None:
                m_body_done = true;
                co_return ReadResult::end();
            case BodyMode::Fixed:
                co_return co_await pull_fixed_chunk();
            case BodyMode::Chunked:
                co_return co_await pull_chunked_chunk();
        }
        co_return ReadResult::end();
    }

    // Fixed-length body: hand back up to one buffer's worth per call.
    asio::awaitable<std::expected<ReadResult, error_code>> pull_fixed_chunk() {
        if (m_body_remaining == 0) {
            m_body_done = true;
            co_return ReadResult::end();
        }
        if (m_buf.empty()) {
            std::array<std::byte, 8192> tmp{};
            auto [ec, n] = co_await read_some(std::span<std::byte>{tmp});
            if (ec) {
                m_body_done = true;
                co_return std::unexpected(ec);
            }
            m_buf.append(reinterpret_cast<const char*>(tmp.data()), n);
        }
        std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(m_body_remaining, m_buf.size()));
        std::string chunk = m_buf.substr(0, take);
        m_buf.erase(0, take);
        m_body_remaining -= take;
        if (m_body_remaining == 0) m_body_done = true;
        co_return ReadResult::chunk(std::move(chunk));
    }

    // Chunked body: decode and return the next chunk's data (RFC 7230 §4.1).
    asio::awaitable<std::expected<ReadResult, error_code>> pull_chunked_chunk() {
        std::string size_line;
        if (!co_await read_line(size_line)) {
            m_body_done = true;
            co_return std::unexpected(make_error_code(asio::error::eof));
        }
        std::string_view size_tok{size_line};
        if (auto semi = size_tok.find(';'); semi != std::string_view::npos) {
            size_tok = size_tok.substr(0, semi);
        }
        std::uint64_t chunk_len = 0;
        if (parse_uint(size_tok, 16, m_limits.max_body_bytes, chunk_len) != NumParse::Ok) {
            m_body_done = true;
            co_return std::unexpected(make_error_code(asio::error::invalid_argument));
        }
        if (chunk_len == 0) {
            // Last chunk: consume trailers up to the blank line, then EOF.
            std::string trailer;
            while (co_await read_line(trailer) && !trailer.empty()) {
            }
            m_body_done = true;
            co_return ReadResult::end();
        }
        m_body_total += chunk_len;
        if (m_body_total > m_limits.max_body_bytes) {
            m_body_done = true;
            co_return std::unexpected(make_error_code(asio::error::message_size));
        }
        std::string chunk;
        if (!co_await read_exact(static_cast<std::size_t>(chunk_len), chunk)) {
            m_body_done = true;
            co_return std::unexpected(make_error_code(asio::error::eof));
        }
        std::string crlf;
        if (!co_await read_exact(2, crlf)) {  // trailing CRLF after chunk data
            m_body_done = true;
            co_return std::unexpected(make_error_code(asio::error::eof));
        }
        co_return ReadResult::chunk(std::move(chunk));
    }

    // Reads and discards any unread request body so the connection is aligned to
    // the next request. Returns false if the body could not be fully drained
    // (error/EOF) — the caller then closes the connection instead of reusing it.
    asio::awaitable<bool> drain_body() {
        while (!m_body_done) {
            auto r = co_await pull_body_chunk();
            if (!r) co_return false;  // error mid-body: cannot reuse the connection
        }
        co_return true;
    }

    // Parses an unsigned integer (base 10 or 16) with explicit overflow and
    // upper-bound checks. `max` is the largest accepted value; anything larger
    // (or that would overflow) is reported as Overflow rather than wrapping.
    enum class NumParse { Ok, Invalid, Overflow };
    static NumParse parse_uint(std::string_view s, unsigned base, std::uint64_t max, std::uint64_t& out) {
        if (s.empty()) return NumParse::Invalid;
        std::uint64_t v = 0;
        bool any = false;
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
            if (v > (max - d) / base) return NumParse::Overflow;  // would exceed max
            v = v * base + d;
            any = true;
        }
        if (!any) return NumParse::Invalid;
        out = v;
        return NumParse::Ok;
    }

    static bool connection_keep_alive(const ParsedHead& head, Version version) {
        auto conn = head.headers.get("connection");
        if (conn) {
            if (icontains(*conn, "close")) return false;
            if (icontains(*conn, "keep-alive")) return true;
        }
        return version != Version::Http1;  // HTTP/1.1 defaults to keep-alive, 1.0 to close
    }

    // Allocation-free ASCII case-insensitive substring test. Called per request
    // for keep-alive detection and body framing, so it avoids the temporary
    // lowercased copies the previous implementation made.
    static bool icontains(std::string_view haystack, std::string_view needle) {
        if (needle.empty()) return true;
        if (needle.size() > haystack.size()) return false;
        auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        const std::size_t last = haystack.size() - needle.size();
        for (std::size_t i = 0; i <= last; ++i) {
            std::size_t j = 0;
            for (; j < needle.size(); ++j) {
                if (lower(haystack[i + j]) != lower(needle[j])) break;
            }
            if (j == needle.size()) return true;
        }
        return false;
    }

    // Reads one CRLF- or LF-terminated line from m_buf (topping up from the
    // transport as needed), stripping the terminator. Returns false on EOF.
    asio::awaitable<bool> read_line(std::string& out) {
        for (;;) {
            std::size_t nl = m_buf.find('\n');
            if (nl != std::string::npos) {
                std::size_t end = (nl > 0 && m_buf[nl - 1] == '\r') ? nl - 1 : nl;
                out.assign(m_buf, 0, end);
                m_buf.erase(0, nl + 1);
                co_return true;
            }
            std::array<std::byte, 8192> tmp{};
            auto [ec, n] = co_await read_some(std::span<std::byte>{tmp});
            if (ec) co_return false;
            m_buf.append(reinterpret_cast<const char*>(tmp.data()), n);
        }
    }

    // Reads exactly `len` bytes from m_buf (topping up from the transport).
    asio::awaitable<bool> read_exact(std::size_t len, std::string& out) {
        while (m_buf.size() < len) {
            std::array<std::byte, 8192> tmp{};
            auto [ec, n] = co_await read_some(std::span<std::byte>{tmp});
            if (ec) co_return false;
            m_buf.append(reinterpret_cast<const char*>(tmp.data()), n);
        }
        out.assign(m_buf, 0, len);
        m_buf.erase(0, len);
        co_return true;
    }

    // Runs the handler; captures failure so run() can close the connection.
    asio::awaitable<void> run_handler(const Dispatcher& dispatch, std::shared_ptr<Request> request,
                                      std::shared_ptr<Response> response, bool& failed) {
        try {
            co_await dispatch(std::move(request), std::move(response), m_transport->tls_handle());
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("h1 handler threw: {}", e.what());
            failed = true;
        } catch (...) {
            SIMPLE_HTTP_ERROR_LOG("h1 handler threw unknown exception");
            failed = true;
        }
        co_return;
    }

    asio::awaitable<void> send_error_response(int status) {
        auto writer = std::make_shared<Http1ResponseWriter<Transport>>(m_transport, Version::Http11, m_deadline,
                                                                        m_limits.idle_timeout);
        writer->set_keep_alive(false);
        Headers headers;
        std::string body{reason_phrase(status)};
        (void)co_await writer->send(status, std::move(headers), std::move(body));
    }

    // Detects a WebSocket upgrade handshake: a GET carrying
    // `Upgrade: websocket`, `Connection: Upgrade` and a Sec-WebSocket-Key.
    static bool is_websocket_upgrade(const ParsedHead& head) {
        auto upgrade = head.headers.get("upgrade");
        if (!upgrade || !icontains(*upgrade, "websocket")) return false;
        auto connection = head.headers.get("connection");
        if (!connection || !icontains(*connection, "upgrade")) return false;
        return head.headers.get("sec-websocket-key").has_value();
    }

    // Performs the WebSocket handshake for a matched route: sends the 101
    // response, builds a WebSocket over this transport, then runs the ws handler
    // and its write pump concurrently. Returns true if the connection was
    // upgraded (the caller must not close it), false if no route matched.
    // Strips the query string from a request target, yielding the path used for
    // route/proxy lookup.
    static std::string request_path(std::string_view target) {
        std::string path{target};
        if (auto q = path.find('?'); q != std::string::npos) {
            path.resize(q);
        }
        return path;
    }

    asio::awaitable<bool> try_websocket_upgrade(const ParsedHead& head) {
        std::string path = request_path(head.target);
        auto handler = m_ws_lookup(path);
        if (!handler) {
            co_return false;
        }

        // Build and send the 101 Switching Protocols handshake response.
        auto key = head.headers.get("sec-websocket-key");
        std::string accept = ws_accept_key(*key);
        std::string resp = std::format(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: {}\r\n\r\n",
            accept
        );
        if (auto ec = co_await write_all(resp); ec) {
            co_return false;  // could not send handshake; connection is unusable
        }

        m_upgraded = true;

        // Build a transport-agnostic WebSocket handle and the Request view.
        auto request = std::make_shared<Request>(Version::Http11, m_executor, m_transport->peer());
        request->set_method_token(head.method_token);
        request->set_target(head.target);
        request->mutable_headers() = head.headers;

        // The backend is shared-owned: the detached write pump keeps a reference
        // to it (and hence to the transport) while a write is in flight, even
        // after this WebSocket handle is gone.
        auto backend = std::make_shared<WsBackendImpl<Transport>>(m_transport, m_limits.max_body_bytes,
                                                                  m_limits.idle_timeout);
        auto ws = std::make_shared<WebSocket>(std::move(backend));

        // Start the write pump as an independent coroutine on this executor so it
        // outlives the handler: after the handler returns we still need the pump
        // running to flush a graceful Close frame (ws->close()).
        asio::co_spawn(m_executor, ws->run_writer(), asio::detached);

        // Run the handler to completion.
        try {
            co_await run_ws_handler(*handler, request, ws);
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("ws handler threw: {}", e.what());
        } catch (...) {
            SIMPLE_HTTP_ERROR_LOG("ws handler threw unknown exception");
        }

        // Graceful shutdown: close() enqueues a Close frame, waits for the pump
        // to write it (if the handler did not already close), then stops the pump
        // and tears down the transport.
        (void)co_await ws->close();
        co_return true;
    }

    asio::awaitable<void> run_ws_handler(const WsHandlerFn& handler, std::shared_ptr<Request> request,
                                         std::shared_ptr<WebSocket> ws) {
        co_await handler(std::move(request), std::move(ws));
        co_return;
    }

    // Detects an h2c cleartext upgrade: Upgrade: h2c, Connection listing both
    // "Upgrade" and "HTTP2-Settings", and the HTTP2-Settings header itself
    // (RFC 7540 §3.2).
    static bool is_h2c_upgrade(const ParsedHead& head) {
        auto upgrade = head.headers.get("upgrade");
        if (!upgrade || !icontains(*upgrade, "h2c")) return false;
        auto connection = head.headers.get("connection");
        if (!connection || !icontains(*connection, "upgrade")) return false;
        return head.headers.get("http2-settings").has_value();
    }

    // Performs the h2c upgrade: reads any request body, sends 101, then hands the
    // connection to an Http2Engine that replays this request as stream 1.
    asio::awaitable<void> do_h2c_upgrade(const Dispatcher& dispatch, const ParsedHead& head) {
        // Drain the (small) upgrade-request body so it can be replayed on stream 1.
        setup_body_framing(head);
        std::string body;
        while (!m_body_done) {
            auto r = co_await pull_body_chunk();
            if (!r) break;  // body error: proceed with whatever we have
            if (!r->eof) body.append(r->data);
        }

        // 101 Switching Protocols handshake response.
        std::string resp;
        resp.append("HTTP/1.1 101 Switching Protocols\r\n");
        resp.append("Connection: Upgrade\r\n");
        resp.append("Upgrade: h2c\r\n\r\n");
        if (auto ec = co_await write_all(resp); ec) {
            m_transport->close();
            co_return;
        }

        m_upgraded = true;
        std::string settings_b64{*head.headers.get("http2-settings")};

        auto engine = std::make_shared<Http2Engine<Transport>>(m_transport, m_limits);
        co_await engine->run_h2c(dispatch, settings_b64, head.method, std::string{head.target}, head.headers,
                                 std::move(body));
        co_return;
    }

    // Writes all of `out` to the transport (partial-write loop).
    asio::awaitable<error_code> write_all(const std::string& out) {
        // Composed async_write: whole buffer or error, no partial-write loop.
        auto [ec, n] = co_await m_transport->async_write(
            std::as_bytes(std::span<const char>{out.data(), out.size()}));
        (void)n;
        co_return ec;
    }

    // --- pull-mode request-body state (one request at a time; h1 is serial) ---
    enum class BodyMode { None, Fixed, Chunked };
    BodyMode m_body_mode = BodyMode::None;
    std::uint64_t m_body_remaining = 0;  // Fixed: bytes left to read
    std::uint64_t m_body_total = 0;      // Chunked: running total (bounded by kMaxBodyBytes)
    bool m_body_done = false;            // body fully delivered (EOF reached)

    std::shared_ptr<Transport> m_transport;
    decltype(std::declval<Transport&>().get_executor()) m_executor;
    EngineLimits m_limits;
    // Shared so response writers can refresh it on writes too (see SharedDeadline).
    SharedDeadline m_deadline{std::make_shared<std::chrono::steady_clock::time_point>()};
    std::string m_buf;  // bytes read past the most recently parsed head
    WsLookup m_ws_lookup;  // WebSocket route lookup (empty if ws disabled)
    WsProxyLookup m_ws_proxy_lookup;  // WebSocket proxy-route lookup (empty if none)
    bool m_upgraded{false};  // connection handed off to the WebSocket / proxy layer
};

}  // namespace simple_http
