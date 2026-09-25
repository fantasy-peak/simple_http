#pragma once

// HTTP/1.1 client session.
//
// HTTP/1.x has no multiplexing, so one session serves one exchange at a time: a
// second open_stream() while an exchange is in flight fails with
// client_errc::session_busy (the facade opens another connection instead).
//
// Request framing follows the usual client rules: a body that is already known
// goes out with Content-Length; a streamed one (RequestSpec::stream_body) is
// sent chunked, one chunk per ClientStream::write(); a method that implies a
// body gets Content-Length: 0 when the caller supplied none.
//
// The response head is parsed incrementally (H1ResponseParser) and the body is
// framed from it:
//   * Transfer-Encoding: chunked  -> chunked, trailers skipped;
//   * Content-Length              -> fixed length;
//   * neither, but a body is due  -> read until the peer closes (the close ends
//                                    the body, RFC 9112 §6.3);
//   * HEAD / 204 / 304            -> no body at all.
// Framing decides reusability: only an explicitly ended body leaves the
// connection at a request boundary. An EOF-delimited body ends the connection by
// definition, and a body the caller stopped reading leaves the peer mid-message,
// so neither session goes back to the pool.
//
// Idle reads are bounded by the client's idle_timeout: a peer that accepts the
// request and then says nothing must not pin the coroutine (and the connection)
// forever. A timeout closes the connection, since a half-read message leaves it
// unusable.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include "../core/http_method.h"
#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../core/version.h"
#include "../engine/h1/h1_parser.h"
#include "../proto/headers.h"
#include "../transport/transport.h"
#include "client_config.h"
#include "client_stream.h"

namespace simple_http {

namespace asio = boost::asio;

template <TransportLike Transport>
class Http1ClientSession;

// One HTTP/1.1 exchange, seen from the caller's side. All state lives in the
// session (there is only ever one exchange in flight); this handle routes to it
// and, being a strong owner, keeps the session alive for as long as the caller
// holds the stream — nothing else would, since an idle HTTP/1.x session has no
// loop of its own.
template <TransportLike Transport>
class Http1ClientStream final : public ClientStream {
  public:
    explicit Http1ClientStream(std::shared_ptr<Http1ClientSession<Transport>> session) : m_session(std::move(session)) {
    }

    asio::awaitable<error_code> write(std::string data) override {
        co_return co_await m_session->exchange_write(std::move(data), /*last=*/false);
    }

    asio::awaitable<error_code> finish(std::string data) override {
        co_return co_await m_session->exchange_write(std::move(data), /*last=*/true);
    }

    asio::awaitable<std::expected<ReadResult, error_code>> read() override {
        co_return co_await m_session->exchange_read();
    }

    bool finished() const override {
        return m_session->exchange_finished_state();
    }

    Version version() const override {
        return Version::Http11;
    }

    std::uint32_t id() const override {
        return 0;
    }

    void cancel() override {
        m_session->cancel_exchange();
    }

  private:
    friend class Http1ClientSession<Transport>;  // publishes the parsed head here

    asio::awaitable<error_code> await_head() override {
        co_return co_await m_session->ensure_head();
    }

    std::shared_ptr<Http1ClientSession<Transport>> m_session;
};

template <TransportLike Transport>
class Http1ClientSession final : public ClientSession,
                                 public std::enable_shared_from_this<Http1ClientSession<Transport>> {
  public:
    using Executor = decltype(std::declval<Transport&>().get_executor());

    // Handing the connection to HTTP/2 after a successful h2c upgrade. Supplied
    // by the facade (which owns the h2 session type) so this header needs no
    // knowledge of it: it builds an HTTP/2 session over `transport`, replays
    // `seed` as stream 1 and hands back both the session and that stream.
    // `transport` is the connection, `seed` the request that carried the
    // upgrade, and `initial` the bytes the peer sent right after its 101 (its h2
    // preface/SETTINGS, possibly the response) — the successor must take them or
    // they are lost.
    using H2UpgradeFactory = std::function<asio::awaitable<
        std::expected<std::pair<std::shared_ptr<ClientSession>, std::shared_ptr<ClientStream>>,
                      error_code>>(std::shared_ptr<Transport> transport, RequestSpec seed, std::string initial)>;

    Http1ClientSession(std::shared_ptr<Transport> transport,
                       std::string authority,
                       EngineLimits limits,
                       std::chrono::milliseconds idle_timeout)
        : m_transport(std::move(transport)),
          m_executor(m_transport->get_executor()),
          m_authority(std::move(authority)),
          m_limits(limits),
          m_idle_timeout(idle_timeout),
          m_idle_timer(m_executor) {
    }

    // --- ClientSession ---

    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> open_stream(RequestSpec spec) override {
        co_await hop();
        co_return co_await open_exchange(std::move(spec), /*allow_upgrade=*/false, {});
    }

    // Like open_stream(), but in h2c Upgrade mode the request doubles as the
    // protocol-switch request (`Upgrade: h2c` + `HTTP2-Settings`). When the peer
    // answers 101 the connection becomes HTTP/2, this request is replayed there
    // as stream 1, and the returned stream is the HTTP/2 one; otherwise the
    // returned stream serves an ordinary HTTP/1.1 exchange (the peer ignored the
    // upgrade, which is the common case for a server that does not speak h2c).
    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> open_stream_upgradeable(
        RequestSpec spec,
        std::string http2_settings_b64) {
        co_await hop();
        co_return co_await open_exchange(std::move(spec), /*allow_upgrade=*/true, std::move(http2_settings_b64));
    }

    // Whether the connection is worth an upgrade attempt: the facade installs a
    // factory only when the target's policy asks for h2c, and a peer that already
    // refused (or a request with a streamed body, which the upgrade cannot carry
    // in one shot) is never asked again.
    bool upgrade_available(bool streaming_body) const {
        return !m_successor.lock() && !m_upgrade_failed && static_cast<bool>(m_h2_factory) && !streaming_body;
    }

    bool alive() const override {
        if (auto successor = m_successor.lock())
            return successor->alive();
        return m_alive;
    }

    bool reusable() const override {
        if (auto successor = m_successor.lock())
            return successor->reusable();
        return m_alive && m_reusable && !m_busy;
    }

    Version version() const override {
        if (auto successor = m_successor.lock())
            return successor->version();
        return Version::Http11;
    }

    std::string_view authority() const override {
        return m_authority;
    }

    void close() override {
        // Safe from any thread (and from a non-awaitable context): the teardown
        // itself runs on the connection executor.
        asio::post(m_executor, [self = this->shared_from_this()] {
            if (auto successor = self->m_successor.lock()) {
                successor->close();
                return;
            }
            self->m_alive = false;
            self->m_reusable = false;
            if (self->m_transport)
                self->m_transport->close();
        });
    }

    // --- pooling hooks (executor-confined; see ClientSession) ---
    void arm_idle_close(std::chrono::milliseconds ttl) override {
        m_pooled = true;
        if (auto successor = m_successor.lock()) {
            successor->arm_idle_close(ttl);  // the live connection is the successor's
            return;
        }
        m_idle_armed = true;
        m_idle_timer.expires_after(ttl);
        m_idle_timer.async_wait([self = this->shared_from_this()](const error_code& ec) {
            if (ec || !self->m_idle_armed)
                return;  // cancelled by disarm() or a re-arm
            self->m_idle_armed = false;
            SIMPLE_HTTP_ERROR_LOG("h1 client: closing idle pooled connection to {}", self->m_authority);
            if (auto successor = self->m_successor.lock()) {
                successor->close();
                return;
            }
            self->fail_session();
        });
    }

    void disarm_idle_close() override {
        m_pooled = false;
        if (auto successor = m_successor.lock())
            successor->disarm_idle_close();
        m_idle_armed = false;
        m_idle_timer.cancel();
    }

    // Invoked (on this session's executor) when the session has finished
    // everything in flight and is reusable again — the facade returns it to the
    // pool from here. Also used by an h2c successor session, which reports its
    // own idleness through this handle (the pool holds the h1 session).
    void set_on_idle(std::function<void()> cb) {
        m_on_idle = std::move(cb);
    }

    void notify_idle() {
        if (m_pooled || !m_on_idle || !reusable())
            return;
        m_on_idle();
    }

    // --- wiring (facade only) ---
    void set_h2_upgrade_factory(H2UpgradeFactory factory) {
        m_h2_factory = std::move(factory);
    }

    std::shared_ptr<Transport> transport() {
        return m_transport;
    }

    Executor get_executor() {
        return m_executor;
    }

  private:
    friend class Http1ClientStream<Transport>;

    // Outcome of the body framing decision.
    enum class BodyMode { None, Fixed, Chunked, EofDelimited };
    enum class Fill { Ok, Eof, Error };

    asio::awaitable<void> hop() {
        co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable));
    }

    static bool method_expects_body(Method method) {
        return method == Method::Post || method == Method::Put || method == Method::Patch;
    }

    static error_code closed_error() {
        return make_error_code(client_errc::session_closed);
    }

    // --- opening an exchange ---

    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> open_exchange(RequestSpec spec,
                                                                                            bool allow_upgrade,
                                                                                            std::string settings_b64) {
        if (auto successor = m_successor.lock()) {
            co_return co_await successor->open_stream(std::move(spec));
        }
        if (!m_alive)
            co_return std::unexpected{closed_error()};
        if (m_busy)
            co_return std::unexpected{make_error_code(client_errc::session_busy)};

        auto stream = std::make_shared<Http1ClientStream<Transport>>(this->shared_from_this());
        m_stream = stream;
        m_busy = true;
        m_reusable = false;
        reset_exchange(std::move(spec));

        const bool want_upgrade = allow_upgrade && upgrade_available(m_spec.stream_body);
        error_code ec;
        if (want_upgrade) {
            ec = co_await start_upgrade_exchange(std::move(settings_b64));
        } else {
            ec = co_await send_request_head();
        }
        if (ec) {
            SIMPLE_HTTP_ERROR_LOG("h1 client: sending the request to {} failed: {}", m_authority, ec.message());
            fail_session();
            co_return std::unexpected{ec};
        }

        if (m_switched)
            co_return co_await finish_upgrade(std::move(stream));
        co_return stream;
    }

    // Completes an h2c upgrade: builds the HTTP/2 session over this transport and
    // replays the request as stream 1. The caller gets the HTTP/2 stream, and
    // this session becomes a facade over the successor (so a pooled handle keeps
    // working and reports version Http2).
    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> finish_upgrade(
        std::shared_ptr<ClientStream> h1_stream) {
        (void)h1_stream;  // the h1 side of the exchange is gone: its stream was replayed as stream 1
        auto transport = std::exchange(m_transport, nullptr);
        std::string initial = std::exchange(m_buf, {});
        auto result = co_await m_h2_factory(std::move(transport), std::move(m_spec), std::move(initial));
        if (!result) {
            m_alive = false;
            m_busy = false;
            co_return std::unexpected{result.error()};
        }
        m_successor = result->first;
        m_busy = false;
        SIMPLE_HTTP_ERROR_LOG("h1 client: {} switched to HTTP/2 (h2c upgrade)", m_authority);
        co_return std::move(result->second);
    }

    // Resets the per-exchange state for a new request.
    void reset_exchange(RequestSpec spec) {
        m_spec = std::move(spec);
        m_parser = H1ResponseParser{};
        m_buf.clear();
        m_head_parsed = false;
        m_head_published = false;
        m_head = ResponseHead{};
        m_body_mode = BodyMode::None;
        m_body_remaining = 0;
        m_body_done = false;
        m_close_after = false;
        m_req_streaming = m_spec.stream_body;
        m_req_finished = false;
        m_resp_truncated = false;
        m_switched = false;
        m_cancelled = false;
        m_keep_alive = !m_spec.close;
    }

    // --- request side ---

    // Serializes and sends the request head, plus the body when the caller
    // supplied it up front.
    asio::awaitable<error_code> send_request_head() {
        std::string head;
        if (auto ec = build_request_head(head); ec)
            co_return ec;

        if (!m_req_streaming) {
            if (!m_spec.body.empty())
                head.append(m_spec.body);
            m_req_finished = true;
        }
        auto ec = co_await write_raw(head);
        if (!ec)
            m_reusable = false;  // only the response's framing can make it reusable
        co_return ec;
    }

    // The h2c Upgrade form: the same request, plus the upgrade headers.
    asio::awaitable<error_code> start_upgrade_exchange(std::string settings_b64) {
        m_upgrade_settings = std::move(settings_b64);
        m_upgrading = true;  // build_request_head has to see it: it adds the upgrade headers
        std::string head;
        if (auto ec = build_request_head(head); ec) {
            m_upgrading = false;
            co_return ec;
        }
        if (!m_spec.body.empty())
            head.append(m_spec.body);
        m_req_finished = true;
        auto ec = co_await write_raw(head);
        if (ec) {
            m_upgrading = false;
            co_return ec;
        }
        // Read the response head: 101 means the peer switched (and this request
        // is now stream 1 of the HTTP/2 connection).
        if (auto pec = co_await read_response_head(/*allow_switching=*/true); pec) {
            m_upgrading = false;
            co_return pec;
        }
        if (m_head.status == 101) {
            m_switched = true;  // no head to deliver: the exchange becomes stream 1
            m_body_mode = BodyMode::None;
            m_body_done = true;
        } else {
            m_upgrade_failed = true;  // do not ask this peer again
        }
        m_upgrading = false;
        co_return error_code{};
    }

    // Builds the request head. Framing headers are ours: Host, Content-Length /
    // Transfer-Encoding and Connection are (re)written here, and any field with a
    // control byte is dropped rather than spliced into the message.
    error_code build_request_head(std::string& out) {
        const std::string_view method = to_string(m_spec.method);
        if (method.empty())
            return make_error_code(client_errc::protocol_error);
        if (contains_ctl(m_spec.target) || contains_ctl(method))
            return make_error_code(client_errc::protocol_error);

        out.append(method);
        out.push_back(' ');
        out.append(m_spec.target.empty() ? "/" : m_spec.target);
        out.append(" HTTP/1.1\r\n");
        out.append("host: ");
        out.append(m_authority);
        out.append("\r\n");

        bool saw_agent = false;
        for (const auto& [name, value] : m_spec.headers) {
            if (name == "host" || name == "content-length" || name == "transfer-encoding" || name == "connection" ||
                name == "upgrade" || name == "http2-settings" || name == "keep-alive") {
                continue;  // ours to set (and hop-by-hop fields are never forwarded blindly)
            }
            if (contains_ctl(name) || contains_ctl(value)) {
                SIMPLE_HTTP_ERROR_LOG("h1 client: dropping header '{}' with CR/LF/NUL", name);
                continue;
            }
            if (name == "user-agent")
                saw_agent = true;
            out.append(name);
            out.append(": ");
            out.append(value);
            out.append("\r\n");
        }
        if (!saw_agent) {
            out.append("user-agent: ");
            out.append(client_version);
            out.append("\r\n");
        }

        // Body framing first. An h2c upgrade request is still an HTTP/1.1
        // request — RFC 9113 §3.2 requires the whole thing (body included) to be
        // on the wire before the peer switches, so it must declare its length
        // like any other request; without this the peer sees a body-less request
        // and reads the body bytes as the start of the HTTP/2 connection.
        if (m_req_streaming) {
            out.append("transfer-encoding: chunked\r\n");
        } else if (!m_spec.body.empty()) {
            out.append("content-length: ");
            out.append(std::to_string(m_spec.body.size()));
            out.append("\r\n");
        } else if (method_expects_body(m_spec.method)) {
            out.append("content-length: 0\r\n");
        }

        if (m_upgrading) {
            out.append("connection: Upgrade, HTTP2-Settings\r\n");
            out.append("upgrade: h2c\r\n");
            out.append("http2-settings: ");
            out.append(m_upgrade_settings);
            out.append("\r\n");
        } else {
            out.append("connection: ");
            out.append(m_keep_alive ? "keep-alive" : "close");
            out.append("\r\n");
        }
        out.append("\r\n");
        return error_code{};
    }

    // Body chunk for a streaming request.
    asio::awaitable<error_code> exchange_write(std::string data, bool last) {
        co_await hop();
        if (!m_alive || m_switched)
            co_return closed_error();
        if (!m_req_streaming || m_req_finished)
            co_return make_error_code(client_errc::body_not_streaming);

        std::string out;
        if (!data.empty())
            out = encode_chunk(data);
        if (last) {
            out.append("0\r\n\r\n");  // last-chunk + trailer-less terminator
            m_req_finished = true;
        }
        if (out.empty())
            co_return error_code{};

        auto ec = co_await write_raw(out);
        if (ec) {
            fail_session();
            co_return ec;
        }
        co_return error_code{};
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

    // --- response side ---

    asio::awaitable<std::expected<ReadResult, error_code>> exchange_read() {
        co_await hop();
        if (m_cancelled)
            co_return std::unexpected{closed_error()};
        if (!m_alive)
            co_return std::unexpected{closed_error()};

        // The caller may go straight to the body: parse (and publish) the head
        // on the way, so status()/head() stay available either way.
        if (auto ec = co_await ensure_head(); ec) {
            fail_session();
            co_return std::unexpected{ec};
        }

        if (m_body_done) {
            // A bodyless response (HEAD, 204/304) ends here. Retiring the
            // exchange is what hands the connection back to the pool, so it must
            // happen even when the caller only reads the head.
            if (m_busy)
                finish_exchange();
            co_return ReadResult::end();
        }

        switch (m_body_mode) {
            case BodyMode::None:
                m_body_done = true;
                finish_exchange();
                co_return ReadResult::end();
            case BodyMode::Fixed:
                co_return co_await read_fixed_body();
            case BodyMode::Chunked:
                co_return co_await read_chunked_body();
            case BodyMode::EofDelimited:
                co_return co_await read_to_eof_body();
        }
        co_return std::unexpected{make_error_code(client_errc::protocol_error)};
    }

    // Idempotent: parses the response head if that has not happened yet, and
    // publishes it to the stream handle (once). Used by both read() and
    // read_head().
    asio::awaitable<error_code> ensure_head() {
        if (!m_head_parsed) {
            if (auto ec = co_await read_response_head(); ec) co_return ec;
        }
        if (!m_head_published) {
            if (auto stream = m_stream.lock()) stream->set_head(m_head);
            m_head_published = true;
        }
        co_return error_code{};
    }

    // Reads and parses the response head, skipping the informational responses
    // that may precede it (100 Continue, 103 Early Hints). A 101 here — outside
    // the h2c upgrade path — is a protocol switch this session cannot honour, so
    // it is rejected rather than mistaken for a final response.
    // `allow_switching` accepts a 101 as a final answer: only the h2c upgrade
    // path can act on one, and every other exchange must treat it as an error it
    // cannot honour.
    asio::awaitable<error_code> read_response_head(bool allow_switching = false) {
        if (m_head_parsed)
            co_return error_code{};  // already done (h2c upgrade path)
        for (;;) {
            auto state = m_parser.parse_head();
            while (state == H1ResponseParser::State::NeedMore) {
                if (m_parser.buffered() > m_limits.max_header_bytes) {
                    SIMPLE_HTTP_ERROR_LOG("h1 client: response head from {} exceeds {} bytes",
                                          m_authority,
                                          m_limits.max_header_bytes);
                    co_return make_error_code(client_errc::header_too_large);
                }
                auto [fill, ec] = co_await fill_buf();
                if (fill != Fill::Ok) {
                    SIMPLE_HTTP_ERROR_LOG("h1 client: no response head from {}: {}", m_authority, ec.message());
                    co_return ec;
                }
                // The parser owns the head buffer, so newly-read bytes go to it
                // rather than to the body buffer.
                m_parser.feed(std::string_view{m_buf});
                m_buf.clear();
                state = m_parser.parse_head();
            }
            if (state == H1ResponseParser::State::Error) {
                SIMPLE_HTTP_ERROR_LOG("h1 client: malformed response head from {}", m_authority);
                co_return make_error_code(client_errc::protocol_error);
            }

            const auto& parsed = m_parser.head();
            if (parsed.status >= 200)
                break;
            if (parsed.status == 101 && allow_switching)
                break;
            if (parsed.status != 100 && parsed.status != 103) {
                SIMPLE_HTTP_ERROR_LOG("h1 client: unexpected {} from {}", parsed.status, m_authority);
                co_return make_error_code(client_errc::protocol_error);
            }
            // Informational: drop it and parse whatever follows.
            m_parser.reset_after_head();
        }

        const auto& parsed = m_parser.head();
        m_head.status = parsed.status;
        m_head.version = parsed.version;
        m_head.headers = parsed.headers;
        // The body framing, and with it the session's reusability.
        decide_body_framing();
        m_head.bodyless = m_body_mode == BodyMode::None;
        // Bytes read past the head are the start of the body (or of the next
        // response on a bodyless one).
        m_buf.assign(m_parser.remainder());
        m_parser.reset_after_head();
        m_head_parsed = true;
        co_return error_code{};
    }

    // Decides how the response body is delimited (RFC 9112 §6) and whether the
    // connection may be reused afterwards.
    void decide_body_framing() {
        const auto& head = m_parser.head();
        // What the response says about keep-alive, plus what we asked for.
        auto conn = head.headers.get("connection");
        if (conn && icontains(*conn, "close"))
            m_close_after = true;
        if (head.version == Version::Http1) {
            // HTTP/1.0 closes unless the peer opted in explicitly.
            m_close_after = !(conn && icontains(*conn, "keep-alive"));
        }

        // A response to HEAD, or to any request whose response carries no body
        // by definition (RFC 9110 §6.4.1), ends at the head.
        const bool method_head = m_spec.method == Method::Head;
        if (method_head || head.status == 204 || head.status == 304) {
            m_body_mode = BodyMode::None;
            m_body_done = true;
            m_explicit_length = true;  // nothing follows: the connection is at a boundary
            m_reusable = !m_close_after;
            return;
        }

        auto te = head.headers.get("transfer-encoding");
        if (te && icontains(*te, "chunked")) {
            m_body_mode = BodyMode::Chunked;
            m_body_total = 0;
            m_explicit_length = true;
            return;
        }
        if (auto cl = head.headers.get("content-length")) {
            std::uint64_t len = 0;
            if (!parse_uint(*cl, len)) {
                SIMPLE_HTTP_ERROR_LOG("h1 client: bad Content-Length '{}' from {}", *cl, m_authority);
                m_body_mode = BodyMode::EofDelimited;  // treat as unframed; the connection will not be reused
                return;
            }
            m_body_mode = BodyMode::Fixed;
            m_body_remaining = len;
            m_explicit_length = true;
            if (len == 0)
                m_body_done = true;
            return;
        }
        // No length, no chunked: the body runs until the peer closes.
        m_body_mode = BodyMode::EofDelimited;
    }

    asio::awaitable<std::expected<ReadResult, error_code>> read_fixed_body() {
        if (m_body_remaining == 0) {
            m_body_done = true;
            finish_exchange();
            co_return ReadResult::end();
        }
        if (m_buf.empty()) {
            switch (auto [fill, ec] = co_await fill_buf(); fill) {
                case Fill::Ok:
                    break;
                case Fill::Eof:
                    // The peer closed mid-body: the response is truncated.
                    SIMPLE_HTTP_ERROR_LOG("h1 client: {} closed after {}/{} body bytes",
                                          m_authority,
                                          m_body_remaining,
                                          "?");
                    fail_session();
                    // The peer closed mid-body: what we have is a truncation,
                    // not a body.
                    co_return std::unexpected{make_error_code(asio::error::connection_reset)};
                case Fill::Error:
                    fail_session();
                    co_return std::unexpected{ec};
            }
        }
        std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(m_body_remaining, m_buf.size()));
        std::string data = m_buf.substr(0, take);
        m_buf.erase(0, take);
        m_body_remaining -= take;
        if (m_body_remaining == 0) {
            m_body_done = true;
            finish_exchange();
        }
        co_return ReadResult::chunk(std::move(data));
    }

    asio::awaitable<std::expected<ReadResult, error_code>> read_chunked_body() {
        for (;;) {
            // Chunk-size line.
            std::size_t nl = m_buf.find('\n');
            if (nl == std::string::npos) {
                if (m_buf.size() > m_limits.max_header_bytes) {
                    fail_session();
                    co_return std::unexpected{make_error_code(client_errc::protocol_error)};
                }
                switch (auto [fill, ec] = co_await fill_buf(); fill) {
                    case Fill::Ok:
                        continue;
                    case Fill::Eof:
                        fail_session();
                        // The peer closed mid-body: what we have is a truncation,
                    // not a body.
                    co_return std::unexpected{make_error_code(asio::error::connection_reset)};
                    case Fill::Error:
                        fail_session();
                        co_return std::unexpected{ec};
                }
            }
            // Copy the size line out before consuming it: erasing m_buf shifts the
            // bytes, so a view into it would then point at the chunk data.
            std::string size_line{m_buf.data(), nl};
            if (!size_line.empty() && size_line.back() == '\r') size_line.pop_back();
            m_buf.erase(0, nl + 1);
            if (auto semi = size_line.find(';'); semi != std::string::npos) {
                size_line.resize(semi);  // chunk extensions are ignored
            }
            std::uint64_t chunk_len = 0;
            // Bounded as it is parsed: `chunk_len + 2` below must not wrap, and a
            // single chunk larger than the configured body cap is refused rather
            // than buffered.
            if (!parse_uint(size_line, chunk_len, 16) || chunk_len > m_limits.max_body_bytes) {
                SIMPLE_HTTP_ERROR_LOG("h1 client: malformed or oversized chunk size from {}", m_authority);
                fail_session();
                co_return std::unexpected{make_error_code(client_errc::protocol_error)};
            }

            if (chunk_len == 0) {
                // Last chunk: skip the trailers up to the terminating blank line.
                for (;;) {
                    std::size_t tnl = m_buf.find('\n');
                    if (tnl == std::string::npos) {
                        if (m_buf.size() > m_limits.max_header_bytes) {
                            fail_session();
                            co_return std::unexpected{make_error_code(client_errc::header_too_large)};
                        }
                        switch (auto [fill, ec] = co_await fill_buf(); fill) {
                            case Fill::Ok:
                                continue;
                            case Fill::Eof:
                                fail_session();
                                // The peer closed mid-body: what we have is a truncation,
                    // not a body.
                    co_return std::unexpected{make_error_code(asio::error::connection_reset)};
                            case Fill::Error:
                                fail_session();
                                co_return std::unexpected{ec};
                        }
                    }
                    std::string line{m_buf.data(), tnl};
                    m_buf.erase(0, tnl + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty())
                        break;  // end of trailers: the body is complete
                }
                m_body_done = true;
                finish_exchange();
                co_return ReadResult::end();
            }

            // Chunk data (+ its trailing CRLF) must be buffered whole.
            while (m_buf.size() < chunk_len + 2) {
                switch (auto [fill, ec] = co_await fill_buf(); fill) {
                    case Fill::Ok:
                        break;
                    case Fill::Eof:
                        fail_session();
                        // The peer closed mid-body: what we have is a truncation,
                    // not a body.
                    co_return std::unexpected{make_error_code(asio::error::connection_reset)};
                    case Fill::Error:
                        fail_session();
                        co_return std::unexpected{ec};
                }
            }
            std::string data = m_buf.substr(0, static_cast<std::size_t>(chunk_len));
            m_buf.erase(0, static_cast<std::size_t>(chunk_len));
            if (m_buf.size() >= 2 && m_buf[0] == '\r' && m_buf[1] == '\n')
                m_buf.erase(0, 2);
            co_return ReadResult::chunk(std::move(data));
        }
    }

    asio::awaitable<std::expected<ReadResult, error_code>> read_to_eof_body() {
        for (;;) {
            if (!m_buf.empty()) {
                std::string data = std::move(m_buf);
                m_buf.clear();
                co_return ReadResult::chunk(std::move(data));
            }
            switch (auto [fill, ec] = co_await fill_buf(); fill) {
                case Fill::Ok:
                    continue;
                case Fill::Eof:
                    // The close *is* the end of the body (RFC 9112 §6.3), but it
                    // also ends the connection: never reusable.
                    m_body_done = true;
                    m_explicit_length = false;
                    finish_exchange();
                    co_return ReadResult::end();
                case Fill::Error:
                    fail_session();
                    co_return std::unexpected{ec};
            }
        }
    }

    // The exchange reached its end: the connection is at a request boundary
    // exactly when the body's length was explicit and neither side asked to
    // close.
    void finish_exchange() {
        m_busy = false;
        // An explicit body length is what leaves the connection at a request
        // boundary; anything else (EOF-delimited body, a `Connection: close` from
        // either side, stray bytes the peer already sent) means it must be closed.
        m_reusable = m_alive && m_explicit_length && !m_close_after && m_buf.empty();
        notify_idle();
    }

    bool exchange_finished_state() const {
        return !m_busy;
    }

    void cancel_exchange() {
        // dispatch, not post: a caller that abandons an exchange and immediately
        // starts another must not race the teardown (it would open a second
        // connection instead of reusing this one).
        asio::dispatch(m_executor, [self = this->shared_from_this()] {
            if (self->m_cancelled)
                return;
            self->m_cancelled = true;
            self->m_body_done = true;
            // A half-written request (or a half-read response) leaves the peer at
            // an unknown position, so HTTP/1.x has to close the connection.
            self->fail_session();
        });
    }

    void fail_session() {
        m_alive = false;
        m_reusable = false;
        m_busy = false;
        if (m_transport)
            m_transport->close();
    }

    // --- byte helpers ---

    // Reads more bytes into m_buf, bounded by the idle timeout: a peer that goes
    // quiet mid-message must not pin the coroutine forever.
    asio::awaitable<std::pair<Fill, error_code>> fill_buf() {
        using namespace asio::experimental::awaitable_operators;
        std::array<std::byte, 16 * 1024> tmp;  // no init: read_some fills [0,n)
        auto read_op = [this, &tmp]() -> asio::awaitable<IoResult> {
            co_return co_await m_transport->async_read_some(std::span<std::byte>{tmp});
        };

        if (m_idle_timeout.count() <= 0) {  // no idle deadline: a plain read
            auto [ec, n] = co_await read_op();
            if (ec) {
                if (ec == asio::error::eof)
                    co_return std::pair{Fill::Eof, ec};
                co_return std::pair{Fill::Error, ec};
            }
            if (n == 0)
                co_return std::pair{Fill::Eof, make_error_code(asio::error::eof)};
            m_buf.append(reinterpret_cast<const char*>(tmp.data()), n);
            co_return std::pair{Fill::Ok, error_code{}};
        }

        auto deadline_op = [this]() -> asio::awaitable<void> {
            asio::steady_timer timer{m_executor};
            timer.expires_after(m_idle_timeout);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        };

        auto outcome = co_await (read_op() || deadline_op());
        if (auto* result = std::get_if<IoResult>(&outcome)) {
            auto [ec, n] = *result;
            if (ec) {
                if (ec == asio::error::eof)
                    co_return std::pair{Fill::Eof, ec};
                co_return std::pair{Fill::Error, ec};
            }
            if (n == 0)
                co_return std::pair{Fill::Eof, make_error_code(asio::error::eof)};
            m_buf.append(reinterpret_cast<const char*>(tmp.data()), n);
            co_return std::pair{Fill::Ok, error_code{}};
        }
        // Idle timeout: the message is abandoned mid-way, so the connection goes.
        SIMPLE_HTTP_ERROR_LOG("h1 client: {} went idle for {}ms", m_authority, m_idle_timeout.count());
        fail_session();
        auto ec = make_error_code(client_errc::request_timeout);
        if (m_transport)
            m_transport->close();
        co_return std::pair{Fill::Error, ec};
    }

    asio::awaitable<error_code> write_raw(const std::string& out) {
        auto [ec, n] = co_await m_transport->async_write(std::as_bytes(std::span<const char>{out.data(), out.size()}));
        (void)n;
        co_return ec;
    }

    // Parses an unsigned integer in `base` with overflow protection.
    static bool parse_uint(std::string_view s, std::uint64_t& out, unsigned base = 10) {
        if (s.empty())
            return false;
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
                return false;
            }
            if (d >= base)
                return false;
            if (v > (UINT64_MAX - d) / base)
                return false;
            v = v * base + d;
        }
        out = v;
        return true;
    }

    // Allocation-free ASCII case-insensitive substring test (as in the h1 engine).
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

    // --- members ---
    std::shared_ptr<Transport> m_transport;
    Executor m_executor;
    std::string m_authority;
    EngineLimits m_limits;
    std::chrono::milliseconds m_idle_timeout;

    bool m_alive{true};
    bool m_busy{false};      // an exchange is in flight
    bool m_reusable{false};  // idle and positioned at a request boundary
    bool m_upgrade_failed{false};
    asio::steady_timer m_idle_timer;  // pool keep-alive bound (armed on put)
    bool m_idle_armed{false};
    bool m_pooled{false};
    std::function<void()> m_on_idle;
    H2UpgradeFactory m_h2_factory;
    // Set once an h2c upgrade succeeded. Weak on purpose: the successor owns
    // *this* session (it is the handle the pool holds, and the successor's idle
    // callback keeps it alive), so a strong link here would be a cycle.
    std::weak_ptr<ClientSession> m_successor;

    // Current exchange (HTTP/1.x serves one at a time). The stream handle is
    // weak: it owns this session, so a strong link here would be a cycle.
    std::weak_ptr<Http1ClientStream<Transport>> m_stream;
    RequestSpec m_spec;
    H1ResponseParser m_parser;
    std::string m_buf;  // bytes read from the transport, not yet consumed
    ResponseHead m_head;
    BodyMode m_body_mode{BodyMode::None};
    std::uint64_t m_body_remaining{0};
    std::uint64_t m_body_total{0};
    bool m_body_done{false};
    bool m_head_parsed{false};
    bool m_head_published{false};
    bool m_req_streaming{false};
    bool m_req_finished{false};
    bool m_keep_alive{true};
    bool m_close_after{false};
    bool m_explicit_length{false};
    bool m_upgrading{false};
    bool m_switched{false};
    bool m_cancelled{false};
    bool m_resp_truncated{false};
    std::string m_upgrade_settings;
};

}  // namespace simple_http
