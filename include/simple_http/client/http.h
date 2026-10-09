#pragma once

// The public client API, flat in `simple_http::` — a single surface modelled on
// Go net/http and Rust reqwest (with hyper/tokio on the explicit-stream side):
//
//   借用 Go / reqwest：连接不可见（Client 内部管池/复用/重试/重定向/Cookie），
//   请求用 RequestBuilder 链式构造（query 自动编码、basic/bearer auth、json body），
//   响应对象可 error_for_status()/text()/json<T>()，body() 是统一读流（Go resp.Body）。
//   借用 hyper/tokio：Stream 是一条双向流（写端=请求体、读端=响应体；h2 可交织），
//   send_stream()/open_stream() 是它的入口；客户端 WebSocket（open_websocket）也
//   从这里长出。
//
//   client.get(url).query({{"page","2"}}).basic_auth(u,p).send();  // → expected<Response, error_code>
//   client.send(shared_ptr<Request>)   // Go http.Client.Do —— Request 两端共用
//   client.open_websocket("ws://host") // → shared_ptr<WebSocket>（复用服务端句柄）
//   auto up = co_await client.open_stream(url, {.method=Method::Post});
//   co_await up->write(chunk); … co_await up->finish();
//   auto head = co_await up->read_head(); while (auto c = co_await up->read()) …;
//
// 便捷 send() 复用既有的、经过验证的便捷层（重定向/连接重试/Cookie jar/h2c/
// 自动解压都在里面），因此它是"缓冲投递"（Response 已持有 body）；显式
// open_stream() 是全流式新管道（Stream 上写与读自行驱动）。两条路径共享同一
// 底层会话内核（detail::ClientEngine；h1 半双工时序、h2 独立收发队列 + 双窗口/水位）。

#include <chrono>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#include "../core/base64.h" // basic_auth
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/http_status.h"
#include "../core/mime.h" // mime::app_json
#include "../core/types.h"
#include "../core/url_encoding.h" // query_escape
#include "../proto/multipart.h"   // MultipartForm (client-side multipart body)
#include "client_config.h"
#include "cookie_jar.h"
#include "http_client.h" // detail::ClientEngine — the connection engine
#include "url.h"
#include "ws_client.h" // WebSocketSpec / detail::ws_upgrade (open_websocket)

namespace simple_http {

namespace asio = boost::asio;

// The layered deadline used by Stream: bounds one expected-returning awaitable
// with a timer, reporting the given client_errc on timeout. limit <= 0 means no
// bound, so a zero budget (the default) never triggers.
namespace detail {
template <typename T>
asio::awaitable<std::expected<T, error_code>> deadline(asio::awaitable<std::expected<T, error_code>> op,
                                                       std::chrono::milliseconds limit, client_errc on_timeout) {
    using namespace asio::experimental::awaitable_operators;
    if (limit.count() <= 0) {
        co_return co_await std::move(op);
    }
    auto timer_op = [limit]() -> asio::awaitable<void> {
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(limit);
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    };
    auto outcome = co_await (std::move(op) || timer_op());
    if (auto *value = std::get_if<std::expected<T, error_code>>(&outcome)) {
        co_return std::move(*value);
    }
    co_return std::unexpected{make_error_code(on_timeout)};
}
} // namespace detail

// --- request-level knobs for the explicit stream path -------------------------

struct StreamSpec {
    Method method{Method::Get};
    // Path + query, in origin-form. Empty = the URL's own target.
    std::string target;
    Headers headers;
    // Ask the peer to acknowledge before the body is sent (Expect:
    // 100-continue). The interim 100 is delivered to the caller *as the head*
    // of the exchange: read_head() returns status 100 (bodyless) once the peer
    // agreed, and the caller then sends the body and reads again for the real
    // response. If the peer answers with a final status instead (e.g. 417 or a
    // 4xx that refuses the body), read_head() returns that and the caller
    // should not write. So: `read_head()` first, then write only on a 100.
    // (A caller that skips the read_head() and writes immediately makes the
    // header pointless but does not hang.)
    bool expect_100_continue{false};
    // Request `Connection: close` (HTTP/1.1) / keep the connection out of the
    // pool (any version): a one-shot request that must not be reused.
    bool close{false};
    // Per-stream budgets. 0 = the Client's defaults.
    std::chrono::milliseconds response_head_timeout{};
    std::chrono::milliseconds body_idle_timeout{};
};

// --- the explicit, full-duplex stream (hyper/tokio model) ---------------------
//
// One stream has a write end (the request body) and a read end (the response
// head and body) on the same object. HTTP/1.1 is half-duplex by protocol: the
// write end closes at finish() and the read end opens after; HTTP/2's separate
// send/receive queues let them interleave. This is also the shape a future
// WebSocket client grows from.
class Stream {
  public:
    // Constructed by the public client (open_stream / open_target); public so
    // the stream can be owned via shared_ptr/make_shared.
    explicit Stream(std::shared_ptr<detail::ClientStream> stream, std::chrono::milliseconds head_timeout = {},
                    std::chrono::milliseconds idle_timeout = {})
        : m_stream(std::move(stream)), m_head_timeout(head_timeout), m_idle_timeout(idle_timeout) {}

    // --- write end: the request body ---
    [[nodiscard]] asio::awaitable<error_code> write(std::string chunk) { return m_stream->write(std::move(chunk)); }
    // Ends the body; after this the write end is closed (h1: the read end
    // opens; h2: END_STREAM is sent).
    [[nodiscard]] asio::awaitable<error_code> finish(std::string last = {}) {
        return m_stream->finish(std::move(last));
    }

    // --- read end: the response ---
    // Bounded by the response-head (TTFB) budget: a slow service fails here as
    // client_errc::response_head_timeout, independently of body pace.
    [[nodiscard]] asio::awaitable<std::expected<detail::ResponseHead, error_code>> read_head() {
        return detail::deadline(m_stream->read_head(), m_head_timeout, client_errc::response_head_timeout);
    }
    // Each read is bounded by the body-idle budget (silence between chunks);
    // a download that keeps flowing is never cut off.
    [[nodiscard]] asio::awaitable<std::expected<ReadResult, error_code>> read() {
        return detail::deadline(m_stream->read(), m_idle_timeout, client_errc::body_idle_timeout);
    }
    [[nodiscard]] asio::awaitable<std::expected<std::string, error_code>> read_all(std::size_t max_bytes = 0) {
        return m_stream->read_all(max_bytes);
    }
    // The head, once read_head() (or read()) has completed; default-constructed
    // (status 0) before that.
    const detail::ResponseHead &head() const { return m_stream->head(); }

    Version version() const { return m_stream->version(); }

    // Cancels the exchange: h2 resets the stream (and returns the window
    // credit), h1 closes the connection.
    [[nodiscard]] asio::awaitable<void> abort() {
        (void)co_await m_stream->cancel();
        co_return;
    }

  private:
    friend class Client;
    std::shared_ptr<detail::ClientStream> m_stream;
    std::chrono::milliseconds m_head_timeout{};
    std::chrono::milliseconds m_idle_timeout{};
};

// --- the response lens --------------------------------------------------------
//
// Two shapes, one interface:
//   * buffered  — from the convenience send(): the body is already in hand,
//                 text()/json<T>() are immediate and read() replays it;
//   * streaming — from open_stream()/send_stream(): body() is a live Body bound
//                 to the exchange's read end, so text()/json<T>()/read_all()
//                 pull it and read() streams it. Go's http.Response.Body is the
//                 same shape: a read stream owned by the response.
class Response {
  public:
    int status() const { return m_head.status; }
    Version version() const { return m_head.version; }
    const Headers &headers() const { return m_head.headers; }
    // A view into this Response's headers: valid while the Response is alive.
    std::optional<std::string_view> header(std::string_view name) const { return m_head.header(name); }
    // An owned copy: safe to hold past the Response (production P0 — views
    // dangle the moment the Response goes away).
    std::optional<std::string> header_owned(std::string_view name) const {
        auto value = m_head.header(name);
        return value ? std::optional<std::string>{*value} : std::nullopt;
    }
    bool ok() const { return m_head.status >= 200 && m_head.status < 300; }
    bool bodyless() const { return m_head.bodyless; }

    // 2xx → this; anything else → unexpected{client_errc::http_status} (the
    // real status stays readable via status()): the reqwest error_for_status().
    std::expected<const Response *, error_code> error_for_status() const {
        if (ok()) {
            return this;
        }
        return std::unexpected{make_error_code(client_errc::http_status)};
    }

    // The response body as a uniform read stream — the counterpart of the
    // server's Request::body(). Buffered shape replays the captured body;
    // streaming shape reads the live exchange (Go's http.Response.Body).
    Body &body() { return *m_body; }
    const Body &body() const { return *m_body; }

    // --- buffered consumption ---
    [[nodiscard]] asio::awaitable<std::expected<std::string, error_code>> text() { co_return co_await bytes(); }
    [[nodiscard]] asio::awaitable<std::expected<std::string, error_code>> bytes() {
        co_return co_await read_all(m_body_cap);
    }
    template <typename T> [[nodiscard]] asio::awaitable<std::expected<T, error_code>> json() {
        auto raw = co_await read_all(m_body_cap);
        if (!raw) {
            co_return std::unexpected{raw.error()};
        }
        auto value = glz::read_json<T>(*raw);
        if (!value) {
            co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
        }
        co_return std::move(*value);
    }

    // --- streaming consumption ---
    [[nodiscard]] asio::awaitable<std::expected<ReadResult, error_code>> read() {
        if (m_body->is_pull() || !m_body->eof()) {
            co_return co_await m_body->read();
        }
        co_return ReadResult::end();
    }
    [[nodiscard]] asio::awaitable<std::expected<std::string, error_code>> read_all(std::size_t max_bytes = 0) {
        // Body::read_all caps nothing, so the convenience budget is enforced
        // here (the streaming shape's underlying ClientStream::read_all also
        // enforces it — this covers the buffered shape too).
        std::string out;
        for (;;) {
            auto r = co_await m_body->read();
            if (!r)
                co_return std::unexpected{r.error()};
            if (r->eof)
                co_return out;
            if (max_bytes != 0 && out.size() + r->data.size() > max_bytes)
                co_return std::unexpected{make_error_code(client_errc::body_too_large)};
            out.append(r->data);
        }
    }

    [[nodiscard]] asio::awaitable<void> abort() {
        if (m_stream) {
            (void)co_await m_stream->cancel();
        }
        co_return;
    }

  public:
    // Buffered shape (from the convenience send()): status/headers + a captured
    // body. Public so tests and Response decorators can build one by hand;
    // ordinary callers get it from Client::send().
    explicit Response(detail::ResponseHead head, std::string body)
        : m_head(std::move(head)), m_body(std::make_shared<Body>(asio::system_executor{})) {
        // Buffered replay: the captured body, delivered as one pull chunk.
        m_body->set_pull_provider(
            [body = std::move(body)]() mutable -> asio::awaitable<std::expected<ReadResult, error_code>> {
                if (body.empty())
                    co_return ReadResult::end();
                std::string out = std::move(body);
                co_return ReadResult::chunk(std::move(out));
            });
    }
    // Streaming shape (from open_stream()/send_stream()): the read end of a
    // live exchange, plus the head parsed on the way. The exchange's reads are
    // wired in as the Body's pull provider, so body()/read() pull the stream.
    // `body_cap` bounds what the buffered readers (bytes()/text()/json()) may
    // accumulate from the live exchange — a peer streaming a body without end
    // must not be read into memory unbounded; 0 = unlimited (the historical
    // behaviour, kept for callers that opt out explicitly).
    explicit Response(std::shared_ptr<detail::ClientStream> stream, detail::ResponseHead head,
                      asio::any_io_executor exec, std::size_t body_cap = 0)
        : m_head(std::move(head)), m_stream(std::move(stream)), m_body_cap(body_cap),
          m_body(std::make_shared<Body>(std::move(exec))) {
        // The provider holds the stream by weak_ptr, not shared_ptr: a strong
        // capture would give the Body (a handle the caller may keep) a hard
        // reference into the exchange graph — the exact shape that turns into a
        // reference cycle the moment a stream ever needs to point back at the
        // Body/Response (the h2 engine once leaked via such a cycle). The Body
        //'s lifetime is a subset of the Response's anyway (it is not copyable
        // and only reachable through body()), so a weak capture costs nothing
        // on the valid paths — a read only ever races the Response's death if
        // the caller has abused the Body's lifetime, and that gets a clean
        // not_connected rather than a use-after-free.
        m_body->set_pull_provider([weak = std::weak_ptr<detail::ClientStream>{
                                       m_stream}]() mutable -> asio::awaitable<std::expected<ReadResult, error_code>> {
            auto stream = weak.lock();
            if (!stream)
                co_return std::unexpected{make_error_code(asio::error::not_connected)};
            co_return co_await stream->read();
        });
    }

  private:
    friend class RequestBuilder;
    friend class Client;
    detail::ResponseHead m_head;
    std::shared_ptr<Body> m_body;                   // uniform read stream
    std::shared_ptr<detail::ClientStream> m_stream; // streaming shape
    std::size_t m_body_cap{0};                      // bytes()/text()/json() accumulate at most this
};

// --- the request builder (reqwest RequestBuilder) -----------------------------

class Client;

class RequestBuilder {
  public:
    // --- collection ---
    RequestBuilder &query(std::initializer_list<std::pair<std::string_view, std::string_view>> kv) {
        for (const auto &[k, v] : kv) {
            m_query.emplace_back(std::string{k}, std::string{v});
        }
        return *this;
    }
    RequestBuilder &header(std::string_view name, std::string value) {
        m_headers.add(std::string{name}, std::move(value));
        return *this;
    }
    // Cap on the response body a buffered send() will accept (0 = ClientConfig
    // / EngineLimits default).
    RequestBuilder &max_body_bytes(std::size_t max) {
        m_max_body_bytes = max;
        return *this;
    }
    RequestBuilder &headers(Headers h) {
        m_headers = std::move(h);
        return *this;
    }
    RequestBuilder &content_type(std::string_view ct) { return header("content-type", std::string{ct}); }
    RequestBuilder &basic_auth(std::string user, std::string pass) {
        m_headers.add(std::string{field::authorization}, "Basic " + base64_encode(user + ":" + pass));
        return *this;
    }
    RequestBuilder &bearer_auth(std::string token) {
        m_headers.add(std::string{field::authorization}, "Bearer " + std::move(token));
        return *this;
    }
    RequestBuilder &body(std::string body) {
        m_body = std::move(body);
        return *this;
    }
    template <typename T> RequestBuilder &json(const T &value) {
        auto serialized = glz::write_json(value);
        if (!serialized) {
            m_body.clear();
            m_json_failed = true;
            return *this;
        }
        m_body = std::move(*serialized);
        return content_type(mime::app_json);
    }
    // Multipart/form-data body (proto/multipart.h's MultipartForm): serializes
    // the form and sets `Content-Type: multipart/form-data; boundary=...`,
    // replacing any content type set earlier. Go's NewRequest with a
    // multipart.Writer, reqwest's .multipart(form).
    RequestBuilder &multipart(MultipartForm form) {
        m_headers.erase(field::content_type);
        m_body = form.release_body();
        return content_type(form.content_type());
    }
    // Layered timeouts: head = time to the first response byte, idle = silence
    // between body reads. 0 keeps the Client's default.
    RequestBuilder &timeout(std::chrono::milliseconds head, std::chrono::milliseconds idle = {}) {
        m_head_timeout = head;
        m_idle_timeout = idle;
        return *this;
    }

    // --- delivery ---
    // Buffered convenience: writes the collected body, reads the whole
    // response (redirects / cookie jar / pooled-retry / h2c / decompression all
    // handled), and returns a buffered Response.
    [[nodiscard]] asio::awaitable<std::expected<Response, error_code>> send() const;
    // Explicit streaming: returns the write-end-open Stream, so the caller can
    // stream the body and then read the response. No buffered body is premade.
    [[nodiscard]] asio::awaitable<std::expected<Stream, error_code>> send_stream() const;

  private:
    friend class Client;
    RequestBuilder(const Client *client, Method method, std::string url)
        : m_client(client), m_method(method), m_url(std::move(url)) {}

    // The final URL: the collected query is percent-encoded and appended.
    std::string build_url() const {
        if (m_query.empty()) {
            return m_url;
        }
        std::string out = m_url;
        if (out.find('?') == std::string::npos) {
            out.push_back('?');
        } else {
            out.push_back('&');
        }
        bool first = true;
        for (const auto &[k, v] : m_query) {
            if (!first) {
                out.push_back('&');
            }
            first = false;
            out += query_escape(k);
            out.push_back('=');
            out += query_escape(v);
        }
        return out;
    }

    const Client *m_client{nullptr};
    Method m_method{Method::Get};
    std::string m_url;
    std::vector<std::pair<std::string, std::string>> m_query;
    Headers m_headers;
    std::string m_body;
    std::size_t m_max_body_bytes{0};
    std::chrono::milliseconds m_head_timeout{};
    std::chrono::milliseconds m_idle_timeout{};
    bool m_json_failed{false};
};

// --- the client facade --------------------------------------------------------

// Connection dials, for diagnostics.
struct Stats {
    std::size_t connections_opened{0}; // fresh TCP/TLS connections dialed
};

class Client {
  public:
    // Pinned to `ex` for its whole life (model A): the caller guarantees that
    // executor is driven by a single thread. Every operation of this Client
    // runs on it — a call initiated from another executor hops onto it at the
    // entry point — which is what lets the engine below run without any locks.
    // (The io_context must outlive the Client's sessions: drain it before
    // tearing it down — the same contract as the server's worker contexts.)
    explicit Client(asio::any_io_executor ex, ClientConfig cfg = {})
        : m_executor(std::move(ex)), m_cfg(std::move(cfg)),
          m_impl(std::make_shared<detail::ClientEngine>(m_executor, m_cfg)) {}

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    const ClientConfig &config() const { return m_cfg; }

    // --- convenience (half-duplex, reqwest-style) ---
    RequestBuilder get(std::string url) const { return RequestBuilder(this, Method::Get, std::move(url)); }
    RequestBuilder post(std::string url) const { return RequestBuilder(this, Method::Post, std::move(url)); }
    RequestBuilder put(std::string url) const { return RequestBuilder(this, Method::Put, std::move(url)); }
    RequestBuilder del(std::string url) const { return RequestBuilder(this, Method::Delete, std::move(url)); }
    RequestBuilder head(std::string url) const { return RequestBuilder(this, Method::Head, std::move(url)); }
    RequestBuilder patch(std::string url) const { return RequestBuilder(this, Method::Patch, std::move(url)); }
    RequestBuilder request(Method method, std::string url) const {
        return RequestBuilder(this, method, std::move(url));
    }

    // --- explicit send (Go's http.Client.Do) ---
    // Sends a fully-built request (method/url/headers/body already set — see
    // Request::set_url) and returns the buffered response, applying the
    // convenience policy (redirects, cookies, pooled-retry). The request's body
    // travels alongside for replay (Request::clone_for_replay), so 307/308
    // redirects can resend it. `url` overrides the Request's own target when
    // the request only carries an origin-form target.
    [[nodiscard]] asio::awaitable<std::expected<Response, error_code>>
    send(std::shared_ptr<Request> request, std::string url = {}, std::size_t max_body_bytes = 0,
         std::chrono::milliseconds timeout = {}) const {
        co_await hop(); // everything below (incl. the request's Body channel) runs on the pinned executor
        std::string current_url = url;
        detail::ClientTarget target;
        std::string url_target;
        if (current_url.empty()) {
            // The Request carries a full URL: derive the authority from it.
            if (request->url().empty())
                co_return std::unexpected{make_error_code(client_errc::bad_url)};
            current_url = request->url();
        }
        auto parsed = detail::parse_url(current_url);
        if (!parsed)
            co_return std::unexpected{parsed.error()};
        target = parsed->to_target();
        target.version = m_cfg.default_version;
        target.h2c = m_cfg.default_h2c;
        url_target = request->target().empty() ? std::string{parsed->target} : std::string{request->target()};
        if (request->target().empty())
            request->set_target(url_target);

        const std::size_t cap = max_body_bytes != 0 ? max_body_bytes : m_cfg.limits.max_body_bytes;
        const std::chrono::milliseconds limit = timeout.count() != 0 ? timeout : m_cfg.request_timeout;
        co_return co_await follow_redirects(m_impl, m_cfg, std::move(target), request->method(),
                                            Headers{request->headers()}, request->body_source(), std::move(url_target),
                                            cap, limit, m_cfg.max_redirects, std::move(current_url));
    }

    // --- explicit streaming (full-duplex) ---
    [[nodiscard]] asio::awaitable<std::expected<Stream, error_code>> open_stream(std::string url,
                                                                                 StreamSpec spec = {}) const {
        co_await hop();
        // Hold the engine for the whole open: this coroutine references it
        // across suspensions (and the engine is only kept alive by the Client's
        // m_impl otherwise), so dropping the Client mid-await must not free it
        // out from under the resumed frame.
        const auto engine = m_impl;
        auto parsed = detail::parse_url(url);
        if (!parsed) {
            co_return std::unexpected{parsed.error()};
        }
        if (spec.target.empty()) {
            spec.target = std::string{parsed->target};
        }
        auto inner = std::make_shared<Request>(Version::Http11, co_await asio::this_coro::executor);
        inner->set_method(spec.method);
        inner->set_target(std::move(spec.target));
        inner->mutable_headers() = std::move(spec.headers);
        if (spec.expect_100_continue) {
            inner->mutable_headers().add(std::string{field::expect}, "100-continue");
        }
        inner->set_close(spec.close);
        inner->set_stream_body(true); // the write end stays open on the Stream
        auto stream = co_await engine->stream(std::move(url), std::move(inner));
        if (!stream) {
            co_return std::unexpected{stream.error()};
        }
        const std::chrono::milliseconds head =
            spec.response_head_timeout.count() > 0 ? spec.response_head_timeout : m_cfg.response_head_timeout;
        const std::chrono::milliseconds idle =
            spec.body_idle_timeout.count() > 0 ? spec.body_idle_timeout : m_cfg.body_idle_timeout;
        co_return Stream{std::move(*stream), head, idle};
    }

    // --- WebSocket client (RFC 6455) ---
    // Dial and upgrade to `url` (ws:// or wss://), returning a WebSocket handle
    // identical to the server's — read() pulls whole messages, write_text/
    // write_binary serialize via one write pump, close() sends a Close frame.
    // `spec` carries extra request headers and an optional Origin.
    [[nodiscard]] asio::awaitable<std::expected<std::shared_ptr<WebSocket>, error_code>>
    open_websocket(std::string url, WebSocketSpec spec = {}) const {
        co_await hop();
        // Keep the engine alive across the dial (see open_stream): the coroutine
        // references it while suspended, and only the Client's m_impl owns it.
        const auto engine = m_impl;
        // ws:// vs wss:// are http:// vs https:// with an upgrade; normalize so
        // the shared URL parser (which the Scheme-less client already uses) can
        // handle host/port/path, then remember the TLS choice.
        const bool is_tls = url.rfind("wss://", 0) == 0;
        const bool is_ws = url.rfind("ws://", 0) == 0;
        if (!is_ws && !is_tls) {
            co_return std::unexpected{make_error_code(client_errc::bad_url)};
        }
        const std::string http_url =
            std::string{is_tls ? "https://" : "http://"} + std::string{url}.substr(url.find("://") + 3);
        auto parsed = detail::parse_url(http_url);
        if (!parsed) {
            co_return std::unexpected{parsed.error()};
        }

        detail::ClientTarget target = parsed->to_target();
        target.use_tls = is_tls;
        target.version = HttpVersionPolicy::Http11; // WebSocket is HTTP/1-based
        target.h2c = H2cMode::Off;

        auto transport = co_await engine->dial_transport(target, co_await asio::this_coro::executor);
        if (!transport)
            co_return std::unexpected{transport.error()};

        const std::string authority = target.authority();
        const std::string path = std::string{parsed->target};
        co_return co_await std::visit(
            [&](auto &typed) -> asio::awaitable<std::expected<std::shared_ptr<WebSocket>, error_code>> {
                co_return co_await detail::ws_upgrade(typed, authority, path, spec, m_cfg.idle_timeout);
            },
            *transport);
    }

    // Connection dials (one Client keeps a single connection, re-dialed on a
    // transport loss).
    Stats stats() const { return Stats{m_impl->opened_count()}; }

    // [internal — the reverse proxy's channel] Opens a stream by ClientTarget,
    // keeping the h2c-upgrade path the proxy needs. Public users call
    // open_stream(url) — this is library-internal because it reaches into
    // connection policy (client/http_proxy.h).
    struct OpenTargetStream {
        std::shared_ptr<Stream> stream;
    };
    [[nodiscard]] asio::awaitable<std::expected<OpenTargetStream, error_code>>
    open_target(detail::ClientTarget target, std::shared_ptr<Request> request) const {
        co_await hop();
        const auto engine = m_impl; // keep it alive across the open (see open_stream)
        auto opened = co_await engine->open_stream(std::move(target), std::move(request));
        if (!opened) {
            co_return std::unexpected{opened.error()};
        }
        co_return OpenTargetStream{std::make_shared<Stream>(std::move(opened->stream))};
    }

  private:
    friend class RequestBuilder;

    // Hops onto the pinned executor (model A); an inline dispatch when already
    // there. Every public entry point (and RequestBuilder::send) does this
    // first, so a Client may be driven from any thread — the work lands on its
    // single executor.
    asio::awaitable<void> hop() const { co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable)); }

    // --- convenience internals (redirect / cookie / retry) ---
    // The convenience layer's one buffered exchange: one request, with the
    // configured retry policy. Returns a buffered Response. The engine owns the
    // connection lifecycle (the single kept connection, free transparent re-dial
    // on transport loss); what is left here is the read-phase retry policy.
    static asio::awaitable<std::expected<Response, error_code>>
    exchange_once(std::shared_ptr<detail::ClientEngine> impl, detail::ClientTarget target,
                  std::shared_ptr<Request> request, const std::string &url_target, std::size_t cap,
                  std::chrono::milliseconds limit, const RetryPolicy &retry) {
        if (request->target().empty())
            request->set_target(url_target.empty() ? "/" : url_target);

        // One budget for the whole request: the engine's open-phase retries and
        // this loop's read-phase retries count against the same max_retries, and
        // the request's overall deadline (request_timeout) bounds them so a set
        // of backoff sleeps cannot stretch the call past what the caller asked
        // for. Each attempt then reads under however much budget is left.
        detail::RetryBudget budget{&retry, 0};
        if (limit.count() > 0)
            budget.deadline = std::chrono::steady_clock::now() + limit;
        bool stale_redialed = false; // one free replay for a reused kept connection that dropped
        auto remaining_budget = [&budget, limit]() -> std::chrono::milliseconds {
            if (budget.deadline == std::chrono::steady_clock::time_point{})
                return limit;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(budget.deadline -
                                                                                    std::chrono::steady_clock::now());
            return left.count() > 0 ? std::chrono::milliseconds{left} : std::chrono::milliseconds{0};
        };
        for (;;) {
            auto opened = co_await impl->start_exchange(
                target, request->clone_for_replay(co_await asio::this_coro::executor), url_target, &budget);
            if (!opened)
                co_return std::unexpected{opened.error()};
            auto &stream = opened->stream;

            const auto remaining = remaining_budget();
            if (remaining.count() <= 0)
                co_return std::unexpected{make_error_code(client_errc::request_timeout)};
            auto response = co_await read_exchange(stream, cap, remaining);
            if (response)
                co_return std::move(*response);

            const error_code ec = response.error();
            // The exchange on the kept h1 connection is over: hand the gate back
            // now, before the retry/redial decision below queues anything onto
            // it — and before cancel() fails the session (that failure must not
            // be what releases the gate, or a waiter would wake onto a dying
            // connection).
            if (opened->gate_token)
                impl->release_h1_gate(opened->gate_token);
            (void)co_await stream->cancel(); // h2: reset the stream, keeping the
                                             // connection; h1: close it

            // A transport failure on a *reused* kept connection may be a stale
            // drop — the peer closed the connection while it sat idle, so this
            // request provably never arrived. Give it one free re-dial (like the
            // old rule that a stale pooled connection earns a replay) before the
            // policy retries come into play. Not counting against max_retries.
            if (opened->reused &&
                (detail::transport_failure(ec) || ec == make_error_code(client_errc::session_closed)) &&
                !stale_redialed) {
                SIMPLE_HTTP_ERROR_LOG("client: reading {} on the kept connection failed ({}), re-dialing once",
                                      target.authority(), ec.message());
                stale_redialed = true;
                continue;
            }

            // Policy retries: re-sending after the request was written is only
            // safe when the policy accepts it (the default keeps idempotent
            // methods and provably-unprocessed streams), and never when the
            // body cannot be rebuilt.
            if (detail::retry_allowed(budget, *request, ec, /*pooled=*/false, /*request_sent=*/true)) {
                SIMPLE_HTTP_ERROR_LOG("client: reading {} failed ({}), retry {} of {}", target.authority(),
                                      ec.message(), budget.used, retry.max_retries);
                co_await detail::retry_sleep(retry, budget.used, budget.deadline);
                continue;
            }
            co_return std::unexpected{ec};
        }
    }

    // One hop of the convenience layer: an exchange, then — when redirects are
    // enabled and the response is a redirect with a Location — a rewritten next
    // hop (method/body per the 301/302/303 vs 307/308 rules, cookies stored and
    // replayed, https→http refused, `remaining` hops bound).
    //
    // The body travels as a string alongside the Request: once fed into the
    // request's Body stream it cannot be taken back, but a redirect only ever
    // needs the *same* body for 307/308 (and no body at all after 301/302/303
    // rewrite to GET). So the convenience layer keeps the source and rebuilds
    // the Request per hop — Go's http.Request.GetBody, concretely.
    static asio::awaitable<std::expected<Response, error_code>>
    follow_redirects(std::shared_ptr<detail::ClientEngine> impl, const ClientConfig &cfg, detail::ClientTarget target,
                     Method method, Headers headers, std::string body, std::string url_target, std::size_t cap,
                     std::chrono::milliseconds limit, std::size_t remaining, std::string current_url) {
        // Saved before target is moved into the exchange: the next hop's origin
        // comparison needs the current authority.
        const std::string current_host = target.host;
        auto request = std::make_shared<Request>(Version::Http11, co_await asio::this_coro::executor);
        request->set_method(method);
        request->mutable_headers() = std::move(headers);
        request->set_target(url_target.empty() ? "/" : url_target);
        if (!body.empty())
            request->set_body(body);
        auto response =
            co_await exchange_once(impl, std::move(target), std::move(request), url_target, cap, limit, cfg.retry);
        if (!response)
            co_return std::unexpected{response.error()};

        if (cfg.cookie_jar) {
            cfg.cookie_jar->store(current_url, response->headers());
        }

        if (!is_redirect(response->status()))
            co_return response;
        // Over the configured hop budget: report the failure (and log where the
        // chain ended) instead of silently handing back a redirect the caller
        // asked us to follow — Go's http.Client returns the last response *and*
        // an error here. max_redirects == 0 means "never follow": that budget is
        // never spent, so the untouched 3xx comes back as-is.
        if (remaining == 0) {
            if (cfg.max_redirects == 0) {
                co_return response;
            }
            SIMPLE_HTTP_ERROR_LOG("client: {} redirects followed, still at {} ({}) — giving up", cfg.max_redirects,
                                  response->status(), current_url);
            co_return std::unexpected{make_error_code(client_errc::too_many_redirects)};
        }
        const auto location = response->header(field::location);
        if (!location || location->empty())
            co_return response;

        auto hop = resolve_redirect(current_url, cfg.default_version, cfg.default_h2c, url_target, *location);
        if (!hop)
            co_return std::unexpected{hop.error()};

        // RFC 9110 §15.4: 303 always becomes GET; 301/302 do for any method
        // other than GET/HEAD. 307/308 preserve the method and body.
        if (response->status() == status::see_other ||
            ((response->status() == status::moved_permanently || response->status() == status::found) &&
             !get_like(method))) {
            method = Method::Get;
            body.clear();
            headers.erase("content-type"); // an empty GET carries no entity
            headers.erase("content-length");
        }

        // A hop to a different origin must not carry the previous host's
        // credentials — Go's http.Client rule. Cookies are replayed through the
        // jar (domain-filtered) when one is configured; an ad-hoc Cookie header
        // is dropped across origins either way.
        if (!iequals_ci(hop->target.host, current_host)) {
            headers.erase("authorization");
            headers.erase("proxy-authorization");
            headers.erase("cookie");
        }

        if (cfg.cookie_jar) {
            std::string cookie = cfg.cookie_jar->cookie_header(hop->url);
            if (!cookie.empty()) {
                headers.erase("cookie");
                headers.add("cookie", std::move(cookie));
            }
        }

        // The next hop's origin-form target supersedes the old one; exchange_once
        // only fills an empty one.
        url_target = hop->url_target;
        co_return co_await follow_redirects(std::move(impl), cfg, std::move(hop->target), method, std::move(headers),
                                            std::move(body), std::move(url_target), cap, limit, remaining - 1,
                                            std::move(hop->url));
    }

    // Reads the response of one exchange: head, then whole body, both under the
    // request budget.
    static asio::awaitable<std::expected<Response, error_code>>
    read_exchange(std::shared_ptr<detail::ClientStream> stream, std::size_t cap, std::chrono::milliseconds limit) {
        // Both expected layers matter: the outer one is the deadline, the inner
        // is the exchange itself.
        auto head = co_await detail::await_with_deadline(stream->read_head(), limit);
        if (!head || !*head)
            co_return std::unexpected{head ? (*head).error() : head.error()};

        std::string body;
        for (;;) {
            auto chunk = co_await detail::await_with_deadline(stream->read(), limit);
            if (!chunk || !*chunk)
                co_return std::unexpected{chunk ? (*chunk).error() : chunk.error()};
            if ((*chunk)->eof)
                break;
            if (cap != 0 && body.size() + (*chunk)->data.size() > cap)
                co_return std::unexpected{make_error_code(client_errc::body_too_large)};
            body.append((*chunk)->data);
        }
        co_return Response{std::move(**head), std::move(body)};
    }

    // Whether `status` is a redirect the convenience layer may follow.
    static bool is_redirect(int status) noexcept {
        return status == status::moved_permanently || status == status::found || status == status::see_other ||
               status == status::temporary_redirect || status == status::permanent_redirect;
    }

    // GET and HEAD keep their method across a 301/302; every other method turns
    // into GET (RFC 9110 §15.4; 307/308 keep the method).
    static bool get_like(Method m) noexcept { return m == Method::Get || m == Method::Head; }

    // One hop of a redirect chain: where the next request goes and what its
    // request-target and absolute URL are.
    struct RedirectHop {
        detail::ClientTarget target;
        std::string url_target; // origin-form for the next request
        std::string url;        // absolute, for cookies and further hops
    };

    // Resolves one redirect hop from `current_url` (the absolute URL that
    // produced the response), `current_target` (that request's origin-form
    // target, for relative Locations) and the Location value. Accepts absolute
    // URLs, scheme-relative ("//host/path"), root-relative ("/path") and
    // path-relative ("next", "../up") forms. An https→http downgrade is
    // refused (redirect_to_insecure), like Go's http.Client.
    static std::expected<RedirectHop, error_code> resolve_redirect(const std::string &current_url,
                                                                   HttpVersionPolicy version, H2cMode h2c,
                                                                   const std::string &current_target,
                                                                   std::string_view location) {
        const detail::Url base = detail::parse_url(current_url).value_or(detail::Url{});
        std::string whole;
        if (location.starts_with("http://") || location.starts_with("https://")) {
            whole.assign(location);
        } else if (location.starts_with("//")) {
            whole = base.scheme + ":" + std::string{location};
        } else if (!location.empty() && location.front() == '/') {
            whole = base.scheme + "://" + base.authority() + std::string{location};
        } else {
            // Path-relative: join against the directory of the current request.
            std::string path{current_target};
            if (const auto q = path.find('?'); q != std::string::npos)
                path.erase(q);
            const auto slash = path.rfind('/');
            whole = base.scheme + "://" + base.authority() +
                    (slash == std::string::npos ? "/" : path.substr(0, slash + 1)) + std::string{location};
        }
        auto url = detail::parse_url(whole);
        if (!url)
            return std::unexpected{url.error()};
        if (base.use_tls() && !url->use_tls())
            return std::unexpected{make_error_code(client_errc::redirect_to_insecure)};
        RedirectHop hop;
        hop.target = url->to_target();
        hop.target.version = version;
        hop.target.h2c = h2c;
        hop.url_target = std::string{url->target};
        hop.url = std::move(whole);
        return hop;
    }

    ClientConfig m_cfg;
    asio::any_io_executor m_executor; // the executor this Client is pinned to (model A)
    std::shared_ptr<detail::ClientEngine> m_impl;
};

inline asio::awaitable<std::expected<Stream, error_code>> RequestBuilder::send_stream() const {
    if (m_json_failed) {
        co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
    }
    co_return co_await m_client->open_stream(build_url(), StreamSpec{m_method,
                                                                     {},
                                                                     m_headers,
                                                                     /*expect_100_continue=*/false,
                                                                     /*close=*/false,
                                                                     m_head_timeout,
                                                                     m_idle_timeout});
}

inline asio::awaitable<std::expected<Response, error_code>> RequestBuilder::send() const {
    const std::string url = build_url();
    if (m_json_failed) {
        co_return std::unexpected{make_error_code(asio::error::invalid_argument)};
    }

    // Hop onto the client's pinned executor *before* building the Request: its
    // Body channel must be created on that executor (model A).
    co_await asio::dispatch(asio::bind_executor(m_client->m_executor, asio::use_awaitable));

    auto request = std::make_shared<Request>(Version::Http11, co_await asio::this_coro::executor);
    request->set_method(m_method);
    request->set_url(url);
    request->mutable_headers() = m_headers;
    if (!m_body.empty()) {
        request->set_body(m_body);
    }

    co_return co_await m_client->send(std::move(request), url, m_max_body_bytes, m_head_timeout);
}

} // namespace simple_http