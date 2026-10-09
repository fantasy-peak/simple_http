#pragma once

// HTTP/1.x engine — beast-free.
//
// Http1Engine::run drives one connection: read bytes off the Transport into an
// H1Parser (engine/h1/h1_parser.h) until a full head is parsed, frame the
// request body (chunked / Content-Length / none) and feed it into
// Request::body(), then hand the request to the dispatcher (Router) with the
// transport's TLS handle. The loop repeats while the connection is keep-alive.
// Responses go through Http1ResponseSink, which hand-serializes either a
// Content-Length reply (one-shot) or a chunked stream directly onto the
// transport.
//
// Upgrades: an Upgrade: websocket request (matched by a registered ws route) is
// handed to the WebSocket layer; an Upgrade: h2c request carrying an
// HTTP2-Settings header is answered with 101 and handed to
// Http2Engine::run_h2c, which replays the original request as HTTP/2 stream 1
// and continues as h2.
//
// Thread safety (model A): every write first hops onto the connection executor
// before touching the transport, so a Response captured by the handler is safe
// to use from any thread.

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

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
#include "../../transport/transport.h"
#include "../dispatcher.h"
#include "../h2/h2_engine.h"
#include "h1_parser.h"
#include "ws_proxy.h"

namespace simple_http {

namespace asio = boost::asio;

// A connection-level idle deadline shared between the engine and the response
// writer(s) it creates, so that both reads and writes can refresh it and the
// watchdog reaps a connection only when it is genuinely idle in both
// directions.
using SharedDeadline = std::shared_ptr<std::chrono::steady_clock::time_point>;

// ResponseSink for HTTP/1.x. Hand-serializes directly onto the transport.
template <TransportLike Transport> class Http1ResponseSink : public ResponseSink {
  public:
    // `deadline` and `idle_timeout` let a write refresh the connection's idle
    // deadline (shared with the engine's read path and watchdog). `alt_svc` is
    // the already-rendered value of the Alt-Svc field, or empty for none: the
    // writer has no business knowing about ports or cache lifetimes, only about
    // where the octets go.
    Http1ResponseSink(std::shared_ptr<Transport> transport, Version version, SharedDeadline deadline,
                      std::chrono::steady_clock::duration idle_timeout, std::string alt_svc = {})
        : m_transport(std::move(transport)), m_executor(m_transport->get_executor()), m_version(version),
          m_deadline(std::move(deadline)), m_idle_timeout(idle_timeout), m_alt_svc(std::move(alt_svc)) {}

    // Set by the engine when the request method was HEAD: the response keeps its
    // headers (so the client learns the entity length) but carries no body
    // (RFC 9110 §9.3.2).
    void set_head_request(bool head) { m_head_request = head; }

    asio::awaitable<error_code> send(int status, Headers headers, std::string body) override {
        co_await hop();
        if (!m_open)
            co_return make_error_code(asio::error::not_connected);

        if (status == 204 || status == 304) {
            // Neither a body nor a Content-Length (RFC 9110 §15.3.5/§15.4.5).
            co_return co_await send_bodyless(status, std::move(headers));
        }

        // Erase first: add_lower appends, so a handler that set its own
        // content-length would otherwise put two conflicting ones on the wire —
        // a response-splitting vector for any intermediary, and this library is
        // one. compressing_writer.h already does exactly this.
        headers.erase("content-length");
        headers.add_lower("content-length", std::to_string(body.size()));
        // The head gets its own buffer, and the body is streamed from wherever the
        // caller handed it to us. Concatenating the two first would copy the whole
        // body — plus a second allocation and its page faults — which is what made
        // a large response expensive.
        std::string head;
        head.reserve(kResponseOverhead + headers_bytes_with_alt_svc(headers));
        append_status_line(head, status);
        append_headers(head, headers);
        head.append("\r\n");
        if (m_head_request) { // headers only
            auto ec = co_await write_raw(head);
            if (ec)
                m_open = false;
            co_return ec;
        }

        const std::array<ConstByteSpan, 2> buffers{as_bytes(head), as_bytes(body)};
        auto ec = co_await write_raw_seq(buffers);
        if (ec)
            m_open = false;
        co_return ec;
    }

    asio::awaitable<error_code> send_bodyless(int status, Headers headers) override {
        co_await hop();
        if (!m_open)
            co_return make_error_code(asio::error::not_connected);
        std::string out;
        out.reserve(kResponseOverhead + headers_bytes_with_alt_svc(headers));
        append_status_line(out, status);
        append_headers(out, headers);
        out.append("\r\n");
        auto ec = co_await write_raw(out);
        if (ec)
            m_open = false;
        co_return ec;
    }

    asio::awaitable<error_code> send_continue() override {
        co_await hop();
        if (!m_open)
            co_return make_error_code(asio::error::not_connected);
        auto ec = co_await write_raw("HTTP/1.1 100 Continue\r\n\r\n");
        if (ec)
            m_open = false;
        co_return ec;
    }

    asio::awaitable<error_code> send_headers(int status, Headers headers) override {
        co_await hop();
        if (!m_open)
            co_return make_error_code(asio::error::not_connected);

        // A streamed response is chunked (HTTP/1.1) or close-delimited
        // (HTTP/1.0 / HEAD): a handler-supplied Content-Length must not survive
        // alongside it, or the response carries two conflicting framings — a
        // response-splitting vector for any intermediary (RFC 9112 §6.1). send()
        // already erases it; the streaming path used to miss it.
        headers.erase("content-length");
        // HTTP/1.0 has no chunked framing: such a response is delimited by the
        // connection close, so refuse keep-alive here (append_headers then writes
        // `connection: close` and the engine closes, since keep_alive_out() is
        // false). A response to HEAD is delimited by its headers alone.
        if (m_version == Version::Http1 || m_head_request) {
            m_keep_alive_out = false;
        } else {
            headers.add_lower("transfer-encoding", "chunked");
        }
        std::string out;
        out.reserve(kResponseOverhead + headers_bytes_with_alt_svc(headers));
        append_status_line(out, status);
        append_headers(out, headers);
        out.append("\r\n");

        auto ec = co_await write_raw(out);
        if (ec)
            m_open = false;
        co_return ec;
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        if (m_head_request)
            co_return error_code{}; // HEAD: headers only
        co_await hop();
        if (!m_open)
            co_return make_error_code(asio::error::not_connected);
        auto ec = co_await write_chunked(data, /*last=*/false);
        if (ec)
            m_open = false;
        co_return ec;
    }

    asio::awaitable<error_code> send_last(std::string data) override {
        if (m_head_request)
            co_return error_code{}; // HEAD: headers only
        co_await hop();
        if (!m_open)
            co_return make_error_code(asio::error::not_connected);
        // An empty final chunk is nothing but the terminator; otherwise the data
        // chunk and the terminator go out together.
        error_code ec;
        if (data.empty()) {
            ec = co_await write_raw(kLastChunk);
        } else {
            ec = co_await write_chunked(data, /*last=*/true);
        }
        if (ec)
            m_open = false;
        co_return ec;
    }

    asio::awaitable<bool> connected() const override {
        co_await hop();
        co_return m_open;
    }
    asio::awaitable<void> close() override {
        co_await hop();
        m_open = false;
        m_transport->close();
    }
    Version version() const override { return m_version; }

    // Set by the engine from the parsed request's Connection header so the
    // response line matches what the engine decided about keep-alive.
    void set_keep_alive(bool ka) { m_keep_alive_out = ka; }
    bool keep_alive_out() const { return m_keep_alive_out; }

  private:
    // Headroom for what every response writes and headers_bytes() does not
    // count: the status line, the connection line append_headers() may add, and
    // the blank line closing the head. Sized for the longest of each.
    static constexpr std::size_t kResponseOverhead = 96;

    // Appends the status line ("HTTP/1.1 200 OK\r\n") straight onto `out`.
    // Hand-appended rather than std::format: this runs once per response, and
    // the format-string machinery costs more than the copies it saves.
    void append_status_line(std::string &out, int status) const {
        out.append(m_version == Version::Http1 ? "HTTP/1.0 " : "HTTP/1.1 ");
        append_int(out, status);
        out.push_back(' ');
        out.append(reason_phrase(status));
        out.append("\r\n");
    }

    // Appends a non-negative integer without allocating.
    static void append_int(std::string &out, int value) {
        char buf[std::numeric_limits<int>::digits10 + 2];
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
        out.append(buf, static_cast<std::size_t>(end - buf));
    }

    // Framing pieces the scatter-gather writes below share.
    static constexpr std::string_view kCrLf = "\r\n";
    static constexpr std::string_view kLastChunk = "0\r\n\r\n"; // trailer-less terminator

    // The same bytes, as the transport's buffer type.
    static ConstByteSpan as_bytes(std::string_view bytes) {
        return std::as_bytes(std::span<const char>{bytes.data(), bytes.size()});
    }

    // Wire size of the fields as append_headers() writes them, so a response
    // buffer can be sized once instead of growing and recopying.
    static std::size_t headers_bytes(const Headers &headers) {
        std::size_t total = 0;
        for (const auto &[name, value] : headers) {
            total += name.size() + value.size() + 4; // ": " + CRLF
        }
        return total;
    }

    // The same, plus the Alt-Svc line append_headers() adds on its own, so the
    // reserve stays an upper bound rather than an estimate that reallocates once.
    [[nodiscard]] std::size_t headers_bytes_with_alt_svc(const Headers &headers) const {
        std::size_t total = headers_bytes(headers);
        if (!m_alt_svc.empty())
            total += kAltSvcPrefix.size() + m_alt_svc.size() + 2;
        return total;
    }

    static constexpr std::string_view kAltSvcPrefix = "alt-svc: ";

    // The connection field holds one of exactly two values, so the whole line
    // is a compile-time constant rather than three appends per response.
    static constexpr std::string_view kKeepAliveConnection = "connection: keep-alive\r\n";
    static constexpr std::string_view kCloseConnection = "connection: close\r\n";

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
        // Alt-Svc: how a browser learns this origin also speaks HTTP/3
        // (RFC 7838). Sent on every response rather than once — it is a cache
        // entry with an expiry, and a client that arrives on a fresh profile has
        // seen none of the earlier ones.
        if (!m_alt_svc.empty()) {
            out.append(kAltSvcPrefix);
            out.append(m_alt_svc);
            out.append("\r\n");
        }
        if (!saw_connection) {
            out.append(m_keep_alive_out ? kKeepAliveConnection : kCloseConnection);
        }
    }

    // Writes one chunked frame — "<hex size>\r\n<data>\r\n" — straight from its
    // pieces as a single operation, so a chunk payload is never copied into a
    // framing buffer. With `last` the terminator rides along in the same write.
    asio::awaitable<error_code> write_chunked(std::string_view data, bool last) {
        char hex[2 * sizeof(std::size_t) + 2];
        auto [end, ec] = std::to_chars(hex, hex + sizeof(hex) - 2, data.size(), 16);
        *end++ = '\r';
        *end++ = '\n';
        const std::string_view size{hex, static_cast<std::size_t>(end - hex)};

        const std::array<ConstByteSpan, 4> buffers{as_bytes(size), as_bytes(data), as_bytes(kCrLf),
                                                   as_bytes(last ? kLastChunk : std::string_view{})};
        co_return co_await write_raw_seq(std::span<const ConstByteSpan>{buffers.data(), last ? 4u : 3u});
    }

    // Writes several buffers as one operation (a single writev where the platform
    // has one). Concatenating them first would cost a scratch allocation plus a
    // copy of everything in it — which is exactly what made large responses
    // expensive, since the body is usually the bulk of the bytes.
    asio::awaitable<error_code> write_raw_seq(std::span<const ConstByteSpan> buffers) {
        // Writing is connection activity: refresh the shared idle deadline so the
        // watchdog does not reap a connection busy streaming a response.
        if (m_deadline)
            *m_deadline = std::chrono::steady_clock::now() + m_idle_timeout;
        auto [ec, n] = co_await m_transport->async_write_seq(buffers);
        (void)n;
        co_return ec;
    }

    asio::awaitable<error_code> write_raw(std::string_view out) {
        // Transport::async_write is a composed operation (asio::async_write): it
        // writes the whole buffer or returns an error, so no partial-write loop
        // is needed here.
        const std::array<ConstByteSpan, 1> buffers{as_bytes(out)};
        co_return co_await write_raw_seq(buffers);
    }

    asio::awaitable<void> hop() const { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    std::shared_ptr<Transport> m_transport;
    decltype(std::declval<Transport &>().get_executor()) m_executor;
    Version m_version;
    bool m_open{true};
    bool m_keep_alive_out{true};
    bool m_head_request{false}; // response to HEAD: no body (RFC 9110 §9.3.2)
    SharedDeadline m_deadline;  // shared with the engine (read path + watchdog)
    std::chrono::steady_clock::duration m_idle_timeout{};
    std::string m_alt_svc; // rendered Alt-Svc value; empty for none
};

template <TransportLike Transport> class Http1Engine : public std::enable_shared_from_this<Http1Engine<Transport>> {
  public:
    explicit Http1Engine(std::shared_ptr<Transport> transport, EngineLimits limits = {})
        : m_transport(std::move(transport)), m_executor(m_transport->get_executor()), m_limits(limits) {}

    // Serve requests on this connection until it closes or a request is not
    // keep-alive. `dispatch` runs the handler for each request. `initial` may
    // contain bytes already read from the transport during protocol detection
    // (e.g. by a preceding peek); they are consumed before reading more.
    asio::awaitable<void> run(const Dispatcher &dispatch, std::string initial = {}, WsLookup ws_lookup = {},
                              WsProxyLookup ws_proxy_lookup = {}, WsLookup ws_regex_lookup = {}) {
        using namespace asio::experimental::awaitable_operators;
        m_ws_lookup = std::move(ws_lookup);
        m_ws_proxy_lookup = std::move(ws_proxy_lookup);
        m_ws_regex_lookup = std::move(ws_regex_lookup);
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
            if (std::chrono::steady_clock::now() >= *m_deadline)
                break; // idle timeout elapsed
        }
        co_return;
    }

    // A read that refreshes the idle deadline on every attempt; all transport
    // reads in the engine go through this so the watchdog covers header reads,
    // body reads and pipelined-request reads alike. Writes refresh the same
    // shared deadline from Http1ResponseSink::write_raw.
    asio::awaitable<std::pair<error_code, std::size_t>> read_some(std::span<std::byte> buf) {
        *m_deadline = std::chrono::steady_clock::now() + m_limits.idle_timeout;
        co_return co_await m_transport->async_read_some(buf);
    }

    asio::awaitable<void> serve_loop(const Dispatcher &dispatch, std::string initial) {
        m_buf = std::move(initial);
        auto exec = m_transport->get_executor();

        for (;;) {
            H1Parser parser;
            parser.feed(m_buf);
            auto state = parser.parse_head();
            while (state == H1Parser::State::NeedMore) {
                if (parser.buffered() > m_limits.max_header_bytes) {
                    co_await send_error_response(431); // Request Header Fields Too Large
                    co_return;
                }
                std::array<std::byte, 8192> tmp; // no init: read_some fills [0,n)
                auto [ec, n] = co_await read_some(std::span<std::byte>{tmp});
                if (ec) {
                    co_return; // EOF or error before a full head — nothing more to do
                }
                parser.feed(tmp.data(), n);
                state = parser.parse_head();
            }
            if (state == H1Parser::State::Error) {
                co_await send_error_response(400);
                co_return;
            }

            auto &head = parser.head();
            Version version = head.version;
            // RFC 9112 §3.2: a request carries exactly one Host. Zero leaves no
            // authority to route on; more than one is ambiguous, and two hops
            // reading different copies is the classic smuggling shape. HTTP/1.0
            // made Host optional, so only the "more than one" half applies there.
            //
            // Checked here rather than in H1Parser: "name: value" lines are the
            // parser's business, but which fields must be *present* is HTTP
            // semantics — and the parser's own unit tests are syntax tests that
            // build minimal requests.
            {
                const std::size_t hosts = head.headers.count("host");
                if (hosts > 1 || (hosts == 0 && version == Version::Http11)) {
                    co_await send_error_response(400);
                    co_return;
                }
            }
            bool keep_alive = connection_keep_alive(head, version);
            // The Upgrade field, fetched once: both the WebSocket and the h2c
            // checks below read it, and on a normal request neither applies —
            // so a single lookup (often a full miss) replaces two.
            const auto upgrade = head.headers.get("upgrade");

            // WebSocket upgrade: an Upgrade: websocket request with a
            // Sec-WebSocket-Key. A path registered as a proxy route is spliced
            // byte-for-byte to its backend (transparent reverse proxy); otherwise
            // a matched local ws route hands the connection to the WebSocket
            // layer, through the middleware chain. On success serve_loop returns
            // (the transport is no longer owned by this engine).
            if (upgrade && is_websocket_upgrade(head, *upgrade)) {
                std::string_view ws_path = request_path(head.target);

                // RFC 6455 §4.2.1: the handshake demands Sec-WebSocket-Version:
                // 13. A wrong version is answered 426 naming the supported one; a
                // missing version is a malformed handshake (the RFC's reject and
                // accept paths both key off the version). Checked before routing
                // or middleware: a handshake that cannot proceed must not run
                // user code for it.
                if (!co_await check_ws_version(head)) {
                    co_return;
                }

                // The Request the middleware chain sees, and the ResponseWriter
                // it can short-circuit with — or load headers into, which the
                // terminal then folds into the 101 handshake (request_id's
                // X-Request-Id lands on the wire instead of vanishing).
                auto request = std::make_shared<Request>(version, exec, m_transport->peer());
                request->set_method_token(head.method_token);
                request->set_target(head.target);
                request->mutable_headers() = head.headers;
                auto writer = std::make_shared<Http1ResponseSink<Transport>>(
                    m_transport, head.version, m_deadline, m_limits.idle_timeout, m_limits.alt_svc_value());
                auto response = std::make_shared<ResponseWriter>(writer);
                auto upgrade_fn = [this](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res,
                                         WsHandler handler) -> asio::awaitable<bool> {
                    co_return co_await try_websocket_upgrade(std::move(req), std::move(res), std::move(handler));
                };

                // Nginx-style precedence: a local exact ws route wins over a
                // byte-level proxy route for the same path; the proxy still
                // beats a local regex ws route. An exact route's middleware
                // chain runs through the terminal; if it short-circuits (or the
                // upgrade fails) the connection closes — a failed handshake
                // leaves it unusable.
                if (m_ws_lookup) {
                    if (auto run = m_ws_lookup(ws_path)) {
                        m_buf.assign(parser.remainder());
                        const bool upgraded = co_await (*run)(request, response, m_transport->tls_handle(), upgrade_fn);
                        co_return; // upgraded, or answered by middleware — either way the engine is done
                    }
                }

                // Byte-level proxy pass-through, between the exact and the regex
                // local ws routes.
                if (m_ws_proxy_lookup) {
                    if (auto target = m_ws_proxy_lookup(ws_path)) {
                        m_upgraded = true; // stop the watchdog from closing the transport
                        co_await run_ws_proxy(m_transport, head, std::string{parser.remainder()}, std::move(*target),
                                              m_limits.idle_timeout);
                        co_return; // tunnel finished; run() must not touch the transport
                    }
                }

                // Local regex WebSocket handlers, consulted last.
                if (m_ws_regex_lookup) {
                    if (auto run = m_ws_regex_lookup(ws_path)) {
                        m_buf.assign(parser.remainder());
                        const bool upgraded = co_await (*run)(request, response, m_transport->tls_handle(), upgrade_fn);
                        co_return;
                    }
                }

                // Upgrade requested but no proxy/route matched: reply 404 and
                // close.
                co_await send_error_response(404);
                co_return;
            }

            // h2c cleartext upgrade: Upgrade: h2c + HTTP2-Settings. Answer 101 and
            // continue the connection as HTTP/2, replaying this request as
            // stream 1 (RFC 7540 §3.2). The transport is then owned by the h2
            // engine, so serve_loop returns.
            if (upgrade && is_h2c_upgrade(head, *upgrade)) {
                m_buf.assign(parser.remainder());
                co_await do_h2c_upgrade(dispatch, head);
                co_return;
            }

            // Body framing fields, read once: the validation below and
            // setup_body_framing() need them, and each was looked up separately
            // before (two more full header scans per request on top of this one).
            auto te = head.headers.get("transfer-encoding");
            const bool chunked_body = te && icontains(*te, "chunked");
            auto cl = head.headers.get("content-length");

            // Reject an oversized or malformed Content-Length before running the
            // handler (chunked bodies, which have no declared length, are bounded
            // incrementally in read_chunked_body instead).
            {
                // Duplicate Content-Length is a request-smuggling split (CL.CL):
                // a front-end that keeps the other copy frames a different body
                // than we do, and both sides then disagree about where the next
                // request starts. Rejecting is the only answer that cannot
                // disagree — matching the Host rule elsewhere in this engine.
                if (head.headers.count("content-length") > 1) {
                    co_await send_error_response(400); // Bad Request
                    co_return;
                }
                if (!chunked_body && cl) {
                    std::uint64_t len = 0;
                    switch (parse_uint(*cl, 10, m_limits.max_body_bytes, len)) {
                    case NumParse::Invalid:
                        co_await send_error_response(400); // Bad Request
                        co_return;
                    case NumParse::Overflow:
                        co_await send_error_response(413); // Payload Too Large
                        co_return;
                    case NumParse::Ok:
                        break;
                    }
                }
            }

            auto request = std::make_shared<Request>(version, exec, m_transport->peer());
            request->set_method_token(std::move(head.method_token));
            request->set_target(std::move(head.target));

            // Bytes already read past the head are the start of the body.
            m_buf.assign(parser.remainder());

            // Install pull-mode body framing: the handler reads the body on demand
            // (req.body().read()), which pulls/frames bytes off the socket only
            // when asked. A handler that ignores the body triggers no body reads.
            //
            // The provider must NOT capture `this`: the engine dies with the
            // connection, while the Request may live on — a handler that stashes
            // the Request for a deferred coroutine (delayed processing, a worker
            // pool) and returns would have its later body read call into a freed
            // engine (heap-use-after-free, reproduced under ASan). The weak_ptr
            // is the same pattern Http2ResponseSink uses for the h2 engine: the
            // read succeeds while the engine (and hence the connection) is alive,
            // and reports a clean end-of-life error once it is gone.
            setup_body_framing(chunked_body, cl);
            // Move the parsed header fields into the request: setup_body_framing
            // above was the last thing that read `head.headers`, so from here the
            // per-request copy of every field (name + value string each) is gone.
            request->mutable_headers() = std::move(head.headers);
            std::weak_ptr<Http1Engine> weak = this->weak_from_this();
            request->body().set_pull_provider(
                [weak = std::move(weak)]() mutable -> asio::awaitable<std::expected<ReadResult, error_code>> {
                    auto eng = weak.lock();
                    if (!eng)
                        co_return std::unexpected{make_error_code(asio::error::not_connected)};
                    co_return co_await eng->pull_body_chunk();
                });

            auto writer = std::make_shared<Http1ResponseSink<Transport>>(
                m_transport, version, m_deadline, m_limits.idle_timeout, m_limits.alt_svc_value());
            writer->set_keep_alive(keep_alive);
            const bool head_request = head.method == Method::Head;
            writer->set_head_request(head_request);
            // Compression wraps the writer, not the Response, so that every
            // handler - the reverse proxy included - passes through it without
            // knowing. maybe_compress_writer returns `writer` unchanged when the
            // client accepts none of what we produce; otherwise the per-request
            // policy (a mounted middleware::compress) decides per response.
            auto response = std::make_shared<ResponseWriter>(
                maybe_compress_writer(writer, m_transport->get_executor(), request, head_request));

            // Run the handler to completion (h1 is half-duplex: the reader and the
            // response writer are the same coroutine, no concurrent read/write).
            bool handler_failed = false;
            co_await run_handler(dispatch, request, response, handler_failed);

            // Drain any request body the handler did not read, so the connection
            // is positioned at the next pipelined request. If draining fails or
            // the body was oversized/malformed, we cannot safely reuse the
            // connection.
            bool drained = co_await drain_body();

            const bool still_open = co_await writer->connected();
            if (handler_failed || !keep_alive || !still_open || !drained) {
                break;
            }
        }
        co_return; // run() closes the transport when serve_loop returns
    }

    // Configures body framing for the current request (called once per request,
    // before dispatch). `chunked` and `content_length` are the already-parsed
    // Transfer-Encoding/Content-Length fields: serve_loop read them for its
    // validation and hands them over instead of making this function look them
    // up a second time. Chunked takes precedence over Content-Length (RFC 7230
    // §3.3.3); a request with neither has no body.
    void setup_body_framing(bool chunked, std::optional<std::string_view> content_length) {
        m_body_done = false;
        m_body_remaining = 0;
        m_body_total = 0;
        if (chunked) {
            m_body_mode = BodyMode::Chunked;
            return;
        }
        if (content_length) {
            std::uint64_t len = 0;
            // serve_loop already validated this; Ok is guaranteed here.
            (void)parse_uint(*content_length, 10, m_limits.max_body_bytes, len);
            m_body_mode = BodyMode::Fixed;
            m_body_remaining = len;
            if (len == 0)
                m_body_done = true;
            return;
        }
        m_body_mode = BodyMode::None;
        m_body_done = true; // no body
    }

    // Pull one body chunk on demand (Body::read calls this via the provider).
    // Returns a data chunk, end-of-body, or an error_code.
    asio::awaitable<std::expected<ReadResult, error_code>> pull_body_chunk() {
        if (m_body_done)
            co_return ReadResult::end();
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
            // Nothing buffered (the normal case once the head read-ahead runs
            // out): read straight into the chunk that is about to be handed to
            // the handler, so a byte moves socket -> chunk instead of
            // socket -> scratch -> m_buf -> chunk. The engine's read_some also
            // refreshes the shared idle deadline, like every other transport
            // read here.
            const std::size_t want =
                static_cast<std::size_t>(std::min<std::uint64_t>(m_body_remaining, kReadChunkBytes));
            // resize_and_overwrite sizes the string without value-initializing
            // it: the read overwrites every byte, so zero-filling first only
            // doubled the memory traffic for the whole body.
            std::string chunk;
            chunk.resize_and_overwrite(want, [](char *, std::size_t n) noexcept { return n; });
            auto [ec, n] =
                co_await read_some(std::span<std::byte>{reinterpret_cast<std::byte *>(chunk.data()), chunk.size()});
            if (ec) {
                m_body_done = true;
                co_return std::unexpected(ec);
            }
            chunk.resize(n);
            m_body_remaining -= n;
            if (m_body_remaining == 0)
                m_body_done = true;
            co_return ReadResult::chunk(std::move(chunk));
        }
        std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(m_body_remaining, m_buf.size()));
        std::string chunk = m_buf.substr(0, take);
        m_buf.erase(0, take);
        m_body_remaining -= take;
        if (m_body_remaining == 0)
            m_body_done = true;
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
                // Trailers are part of the body volume: count them so a peer cannot
                // stream trailers forever (each line is itself bounded by read_line).
                m_body_total += trailer.size() + 2;
                if (m_body_total > m_limits.max_body_bytes) {
                    m_body_done = true;
                    co_return std::unexpected(make_error_code(asio::error::message_size));
                }
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
        if (!co_await read_exact(2, crlf)) { // trailing CRLF after chunk data
            m_body_done = true;
            co_return std::unexpected(make_error_code(asio::error::eof));
        }
        // The trailer is framing, not padding. Accepting any two bytes here is
        // the lenient half of a request-smuggling split with a CRLF-strict
        // front-end — RFC 9112 §7.1 requires the CRLF, and a proxy is exactly
        // where the disagreement gets exploited.
        if (crlf != "\r\n") {
            m_body_done = true;
            co_return std::unexpected(make_error_code(asio::error::invalid_argument));
        }
        co_return ReadResult::chunk(std::move(chunk));
    }

    // Reads and discards any unread request body so the connection is aligned to
    // the next request. Returns false if the body could not be fully drained
    // (error/EOF) — the caller then closes the connection instead of reusing it.
    asio::awaitable<bool> drain_body() {
        while (!m_body_done) {
            auto r = co_await pull_body_chunk();
            if (!r)
                co_return false; // error mid-body: cannot reuse the connection
        }
        co_return true;
    }

    // Parses an unsigned integer (base 10 or 16) with explicit overflow and
    // upper-bound checks. `max` is the largest accepted value; anything larger
    // (or that would overflow) is reported as Overflow rather than wrapping.
    enum class NumParse { Ok, Invalid, Overflow };
    static NumParse parse_uint(std::string_view s, unsigned base, std::uint64_t max, std::uint64_t &out) {
        if (s.empty())
            return NumParse::Invalid;
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
            if (v > (max - d) / base)
                return NumParse::Overflow; // would exceed max
            v = v * base + d;
            any = true;
        }
        if (!any)
            return NumParse::Invalid;
        out = v;
        return NumParse::Ok;
    }

    static bool connection_keep_alive(const ParsedHead &head, Version version) {
        auto conn = head.headers.get("connection");
        if (conn) {
            if (icontains(*conn, "close"))
                return false;
            if (icontains(*conn, "keep-alive"))
                return true;
        }
        return version != Version::Http1; // HTTP/1.1 defaults to keep-alive, 1.0 to close
    }

    // Allocation-free ASCII case-insensitive substring test. Called per request
    // for keep-alive detection and body framing, so it avoids the temporary
    // lowercased copies the previous implementation made.
    static bool icontains(std::string_view haystack, std::string_view needle) {
        if (needle.empty())
            return true;
        if (needle.size() > haystack.size())
            return false;
        auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        const std::size_t last = haystack.size() - needle.size();
        for (std::size_t i = 0; i <= last; ++i) {
            std::size_t j = 0;
            for (; j < needle.size(); ++j) {
                if (lower(haystack[i + j]) != lower(needle[j]))
                    break;
            }
            if (j == needle.size())
                return true;
        }
        return false;
    }

    // Reads one CRLF- or LF-terminated line from m_buf (topping up from the
    // transport as needed), stripping the terminator. Returns false on EOF.
    asio::awaitable<bool> read_line(std::string &out) {
        for (;;) {
            std::size_t nl = m_buf.find('\n');
            if (nl != std::string::npos) {
                std::size_t end = (nl > 0 && m_buf[nl - 1] == '\r') ? nl - 1 : nl;
                out.assign(m_buf, 0, end);
                m_buf.erase(0, nl + 1);
                co_return true;
            }
            // No line ending yet, so everything buffered belongs to this line (a
            // chunk-size line or a trailer line). Bound it: read_some refreshes the
            // idle deadline on every call, so an endless line (never a newline)
            // would otherwise grow m_buf until the process runs out of memory.
            if (m_buf.size() >= m_limits.max_header_bytes) {
                SIMPLE_HTTP_ERROR_LOG("h1 line longer than {} bytes; closing", m_limits.max_header_bytes);
                co_return false;
            }
            auto [ec, n] = co_await read_some(std::span<std::byte>{m_read_scratch});
            if (ec)
                co_return false;
            m_buf.append(reinterpret_cast<const char *>(m_read_scratch.data()), n);
        }
    }

    // Reads exactly `len` bytes from m_buf (topping up from the transport).
    asio::awaitable<bool> read_exact(std::size_t len, std::string &out) {
        while (m_buf.size() < len) {
            auto [ec, n] = co_await read_some(std::span<std::byte>{m_read_scratch});
            if (ec)
                co_return false;
            m_buf.append(reinterpret_cast<const char *>(m_read_scratch.data()), n);
        }
        out.assign(m_buf, 0, len);
        m_buf.erase(0, len);
        co_return true;
    }

    // Runs the handler; captures failure so run() can close the connection.
    asio::awaitable<void> run_handler(const Dispatcher &dispatch, std::shared_ptr<Request> request,
                                      std::shared_ptr<ResponseWriter> response, bool &failed) {
        try {
            co_await dispatch(std::move(request), std::move(response), m_transport->tls_handle());
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("h1 handler threw: {}", e.what());
            failed = true;
        } catch (...) {
            SIMPLE_HTTP_ERROR_LOG("h1 handler threw unknown exception");
            failed = true;
        }
        co_return;
    }

    asio::awaitable<void> send_error_response(int status, Headers extra = {}) {
        auto writer = std::make_shared<Http1ResponseSink<Transport>>(m_transport, Version::Http11, m_deadline,
                                                                     m_limits.idle_timeout, m_limits.alt_svc_value());
        writer->set_keep_alive(false);
        std::string body{reason_phrase(status)};
        (void)co_await writer->send(status, std::move(extra), std::move(body));
    }

    // Detects a WebSocket upgrade handshake: a GET carrying
    // `Upgrade: websocket`, `Connection: Upgrade` and a Sec-WebSocket-Key.
    // `upgrade` is the already-fetched Upgrade field (serve_loop reads it once
    // for both this check and the h2c check, so neither re-scans the headers).
    static bool is_websocket_upgrade(const ParsedHead &head, std::string_view upgrade) {
        if (!icontains(upgrade, "websocket"))
            return false;
        auto connection = head.headers.get("connection");
        if (!connection || !icontains(*connection, "upgrade"))
            return false;
        return head.headers.get("sec-websocket-key").has_value();
    }

    // Performs the WebSocket handshake for a matched route: sends the 101
    // response, builds a WebSocket over this transport, then runs the ws handler
    // and its write pump concurrently. Returns true if the connection was
    // upgraded (the caller must not close it), false if no route matched.
    // Strips the query string from a request target, yielding the path used for
    // route/proxy lookup.
    // The path part of a request target, query string stripped. A view into
    // `target` — every caller only reads it, so building a string here would be
    // a copy per request.
    static std::string_view request_path(std::string_view target) {
        if (auto q = target.find('?'); q != std::string_view::npos) {
            return target.substr(0, q);
        }
        return target;
    }

    asio::awaitable<bool> try_websocket_upgrade(std::shared_ptr<Request> request, std::shared_ptr<ResponseWriter> res,
                                                WsHandler handler) {
        // permessage-deflate (RFC 7692): only when the server enabled it and the
        // client offered it. A declined offer simply continues uncompressed.
        WsDeflateConfig deflate;
        if (m_limits.ws_compression) {
            const auto offered = request->header("sec-websocket-extensions").value_or(std::string_view{});
            if (auto parsed = ws_parse_deflate_offer(offered)) {
                deflate = *parsed;
            }
        }

        // Build and send the 101 Switching Protocols handshake response: the
        // fixed handshake fields, the accept key, the negotiated extension, and
        // any fields the middleware chain set on `res` before reaching this
        // terminal (request_id's X-Request-Id, a middleware's Set-Cookie, ...).
        auto key = request->header("sec-websocket-key");
        if (!key) {
            // is_websocket_upgrade() guarantees the key; defensive only.
            co_await send_error_response(400);
            co_return false;
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
        for (const auto &[name, value] : res->headers()) {
            resp += name;
            resp += ": ";
            resp += value;
            resp += "\r\n";
        }
        resp += "\r\n";
        if (auto ec = co_await write_all(resp); ec) {
            co_return false; // could not send handshake; connection is unusable
        }

        m_upgraded = true;

        // The backend is shared-owned: the detached write pump keeps a reference
        // to it (and hence to the transport) while a write is in flight, even
        // after this WebSocket handle is gone.
        auto backend =
            std::make_shared<WsBackendImpl<Transport>>(m_transport, m_limits.max_body_bytes, m_limits.idle_timeout,
                                                       /*expect_masked=*/true, m_limits.ws_read_ahead_bytes, deflate);

        // Bytes the parser read past the request head (m_buf) are the start of the
        // WebSocket stream - typically the client's first frame, which many clients
        // pipeline behind the upgrade request. Hand them to the frame parser
        // instead of dropping them.
        backend->feed(m_buf);
        m_buf.clear();

        auto ws = std::make_shared<WebSocket>(std::move(backend));

        // Start the write pump as an independent coroutine on this executor so it
        // outlives the handler: after the handler returns we still need the pump
        // running to flush a graceful Close frame (ws->close()).
        asio::co_spawn(m_executor, ws->run_writer(), asio::detached);

        // Run the handler to completion.
        try {
            co_await run_ws_handler(handler, request, ws);
        } catch (const std::exception &e) {
            SIMPLE_HTTP_ERROR_LOG("ws handler threw: {}", e.what());
        } catch (...) {
            SIMPLE_HTTP_ERROR_LOG("ws handler threw unknown exception");
        }

        // Graceful shutdown: close() enqueues a Close frame, waits for the pump to
        // write it (if the handler did not already close), then stops the pump and
        // tears down the transport.
        (void)co_await ws->close();
        co_return true;
    }

    asio::awaitable<void> run_ws_handler(const WsHandler &handler, std::shared_ptr<Request> request,
                                         std::shared_ptr<WebSocket> ws) {
        co_await handler(std::move(request), std::move(ws));
        co_return;
    }

    // RFC 6455 §4.2.1 version gate, checked after an Upgrade: websocket head
    // parses: a missing Sec-WebSocket-Version is answered 400, a value other than
    // 13 with 426 + Sec-WebSocket-Version: 13, closing the connection either way.
    // (Previously a handshake with the wrong version got a 101 like a valid one.)
    asio::awaitable<bool> check_ws_version(const ParsedHead &head) {
        const auto version = head.headers.get("sec-websocket-version");
        if (!version) {
            co_await send_error_response(400); // handshake without a version is malformed
            co_return false;
        }
        if (*version != "13") {
            Headers extra;
            extra.add_lower("sec-websocket-version", "13");
            co_await send_error_response(426, std::move(extra));
            co_return false;
        }
        co_return true;
    }

    // Detects an h2c cleartext upgrade: Upgrade: h2c, Connection listing both
    // "Upgrade" and "HTTP2-Settings", and the HTTP2-Settings header itself
    // (RFC 7540 §3.2). `upgrade` is the already-fetched Upgrade field.
    static bool is_h2c_upgrade(const ParsedHead &head, std::string_view upgrade) {
        if (!icontains(upgrade, "h2c"))
            return false;
        auto connection = head.headers.get("connection");
        if (!connection || !icontains(*connection, "upgrade"))
            return false;
        return head.headers.get("http2-settings").has_value();
    }

    // Performs the h2c upgrade: reads any request body, sends 101, then hands the
    // connection to an Http2Engine that replays this request as stream 1.
    asio::awaitable<void> do_h2c_upgrade(const Dispatcher &dispatch, const ParsedHead &head) {
        // Drain the (small) upgrade-request body so it can be replayed on stream 1.
        auto h2c_te = head.headers.get("transfer-encoding");
        setup_body_framing(h2c_te && icontains(*h2c_te, "chunked"), head.headers.get("content-length"));
        std::string body;
        while (!m_body_done) {
            auto r = co_await pull_body_chunk();
            if (!r)
                break; // body error: proceed with whatever we have
            if (!r->eof)
                body.append(r->data);
        }

        // 101 Switching Protocols handshake response — entirely constant.
        if (auto ec = co_await write_all(kH2cUpgradeResponse); ec) {
            m_transport->close();
            co_return;
        }

        // Bytes buffered past the request body are the client's HTTP/2 connection
        // preface and whatever it sent right after (RFC 9113 §3.2 allows the
        // preface immediately). They must travel into the h2 engine's receive
        // buffer, or the engine reads the client's next real frame as the preface
        // and answers GOAWAY(PROTOCOL_ERROR).
        std::string buffered = std::exchange(m_buf, {});
        m_upgraded = true;
        std::string settings_b64{*head.headers.get("http2-settings")};

        auto engine = std::make_shared<Http2Engine<Transport>>(m_transport, m_limits);
        co_await engine->run_h2c(dispatch, settings_b64, head.method, std::string{head.target}, head.headers,
                                 std::move(body), std::move(buffered), m_ws_lookup, m_ws_regex_lookup,
                                 m_ws_proxy_lookup);
        co_return;
    }

    // Writes all of `out` to the transport (partial-write loop).
    // The 101 h2c handshake is fixed, so its constant part lives here; the
    // WebSocket 101 is assembled per upgrade in try_websocket_upgrade (its head
    // varies: negotiated extensions, middleware fields).
    static constexpr std::string_view kH2cUpgradeResponse = "HTTP/1.1 101 Switching Protocols\r\n"
                                                            "Connection: Upgrade\r\n"
                                                            "Upgrade: h2c\r\n\r\n";
    // How many bytes one transport read grabs (the head-read buffers and the
    // engine's body-framing scratch).
    static constexpr std::size_t kReadChunkBytes = 8192;

    // Writes all of `out` to the transport. `out` must outlive the await, so
    // callers pass either a local buffer or a string constant.
    asio::awaitable<error_code> write_all(std::string_view out) {
        // Composed async_write: whole buffer or error, no partial-write loop.
        auto [ec, n] = co_await m_transport->async_write(std::as_bytes(std::span<const char>{out.data(), out.size()}));
        (void)n;
        co_return ec;
    }

    // --- pull-mode request-body state (one request at a time; h1 is serial) ---
    enum class BodyMode { None, Fixed, Chunked };
    BodyMode m_body_mode = BodyMode::None;
    std::uint64_t m_body_remaining = 0; // Fixed: bytes left to read
    std::uint64_t m_body_total = 0;     // Chunked: running total (bounded by kMaxBodyBytes)
    bool m_body_done = false;           // body fully delivered (EOF reached)

    std::shared_ptr<Transport> m_transport;
    decltype(std::declval<Transport &>().get_executor()) m_executor;
    EngineLimits m_limits;
    // Shared so response writers can refresh it on writes too (see
    // SharedDeadline).
    SharedDeadline m_deadline{std::make_shared<std::chrono::steady_clock::time_point>()};
    std::string m_buf; // bytes read past the most recently parsed head
    // Reused transport-read scratch for the body-framing helpers (read_line /
    // read_exact). Pull mode reads sequentially, so there is only ever one
    // reader; hoisting the 8 KiB out of their coroutine frames stops every
    // chunk-line / chunk read from allocating a fresh scratch on the heap.
    std::array<std::byte, kReadChunkBytes> m_read_scratch{};
    WsLookup m_ws_lookup;            // local WebSocket route lookup, exact only (empty if
                                     // ws disabled)
    WsProxyLookup m_ws_proxy_lookup; // WebSocket proxy-route lookup (empty if none)
    WsLookup m_ws_regex_lookup;      // local WebSocket route lookup, regex only
                                     // (empty if none)
    bool m_upgraded{false};          // connection handed off to the WebSocket / proxy layer
};

} // namespace simple_http
