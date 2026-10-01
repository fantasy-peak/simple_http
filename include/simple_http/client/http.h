#pragma once

// The new client API — `simple_http::http`. A complete rebuild of the client
// surface, modelled on Go net/http and Rust reqwest (with hyper/tokio on the
// explicit-stream side):
//
//   借用 Go / reqwest：连接不可见（Client 内部管池/复用/重试/重定向/Cookie），
//   请求用 RequestBuilder 链式构造（query 自动编码、basic/bearer auth、json body），
//   响应对象可 error_for_status()/text()/json<T>()。
//   借用 hyper/tokio：Stream 是一条双向流（写端=请求体、读端=响应体；h2 可交织），
//   send_stream()/open_stream() 是它的入口 —— 也是未来 WebSocket 客户端的底座。
//
//   client.get(url).query({{"page","2"}}).basic_auth(u,p).send();  // → expected<Response, error_code>
//   auto up = co_await client.open_stream(url, {.method=Method::Post});
//   co_await up->write(chunk); … co_await up->finish();
//   auto head = co_await up->read_head(); while (auto c = co_await up->read()) …;
//
// 便捷 send() 复用既有的、经过验证的便捷层（重定向/连接重试/Cookie jar/h2c/
// 自动解压都在里面），因此它是"缓冲投递"（Response 已持有 body）；显式
// open_stream() 是全流式新管道（Stream 上写与读自行驱动）。两条路径共享同一
// 底层会话内核（h1 半双工时序、h2 独立收发队列 + 双窗口/水位）。

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
#include "client_config.h"
#include "cookie_jar.h"
#include "http_client.h" // the underlying, verified convenience layer
#include "url.h"

namespace simple_http::http {

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
    // 100-continue). On a 4xx the server refuses without pulling the body —
    // the caller sees the head on read_head() and never writes.
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
    // Constructed by the http:: client (open_stream / open_target); public so
    // the stream can be owned via shared_ptr/make_shared.
    explicit Stream(std::shared_ptr<simple_http::ClientStream> stream, std::chrono::milliseconds head_timeout = {},
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
    [[nodiscard]] asio::awaitable<std::expected<ResponseHead, error_code>> read_head() {
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
    const ResponseHead &head() const { return m_stream->head(); }

    Version version() const { return m_stream->version(); }

    // Cancels the exchange: h2 resets the stream (and returns the window
    // credit), h1 closes the connection.
    [[nodiscard]] asio::awaitable<void> abort() {
        (void)co_await m_stream->cancel();
        co_return;
    }

  private:
    friend class Client;
    std::shared_ptr<simple_http::ClientStream> m_stream;
    std::chrono::milliseconds m_head_timeout{};
    std::chrono::milliseconds m_idle_timeout{};
};

// --- the response lens --------------------------------------------------------
//
// Two shapes, one interface:
//   * buffered  — from the convenience send(): the body is already in hand,
//                 text()/json<T>() are immediate and read() replays it;
//   * streaming — from open_stream()/send_stream(): the read end of the stream,
//                 text()/json<T>() pull it, read() streams it.
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

    // --- buffered consumption ---
    [[nodiscard]] asio::awaitable<std::expected<std::string, error_code>> text() { co_return co_await bytes(); }
    [[nodiscard]] asio::awaitable<std::expected<std::string, error_code>> bytes() { co_return co_await read_all(); }
    template <typename T> [[nodiscard]] asio::awaitable<std::expected<T, error_code>> json() {
        auto raw = co_await read_all();
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
        if (m_stream) {
            co_return co_await m_stream->read();
        }
        // Buffered shape: replay the captured body once, then eof.
        if (m_replay_index < m_buffer->body.size()) {
            std::string data = m_buffer->body.substr(m_replay_index);
            m_replay_index = m_buffer->body.size();
            co_return ReadResult::chunk(std::move(data));
        }
        co_return ReadResult::end();
    }
    [[nodiscard]] asio::awaitable<std::expected<std::string, error_code>> read_all(std::size_t max_bytes = 0) {
        if (m_stream) {
            co_return co_await m_stream->read_all(max_bytes);
        }
        if (max_bytes != 0 && m_buffer->body.size() > max_bytes) {
            co_return std::unexpected{make_error_code(client_errc::body_too_large)};
        }
        co_return m_buffer->body;
    }

    [[nodiscard]] asio::awaitable<void> abort() {
        if (m_stream) {
            (void)co_await m_stream->cancel();
        }
        co_return;
    }

  private:
    friend class RequestBuilder;
    explicit Response(simple_http::ClientResponse buffered)
        : m_head{buffered.status, buffered.version, std::move(buffered.headers), buffered.bodyless},
          m_buffer(std::move(buffered)) {}
    explicit Response(std::shared_ptr<simple_http::ClientStream> stream, ResponseHead head)
        : m_head(std::move(head)), m_stream(std::move(stream)) {}

    ResponseHead m_head;
    std::optional<simple_http::ClientResponse> m_buffer; // buffered convenience shape
    std::shared_ptr<simple_http::ClientStream> m_stream; // streaming shape
    std::size_t m_replay_index{0};
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

class Client {
  public:
    explicit Client(ClientConfig cfg = {})
        : m_cfg(std::move(cfg)), m_impl(std::make_shared<simple_http::HttpClient>(m_cfg)) {}

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

    // --- explicit streaming (full-duplex) ---
    [[nodiscard]] asio::awaitable<std::expected<Stream, error_code>> open_stream(std::string url,
                                                                                 StreamSpec spec = {}) const {
        auto parsed = parse_url(url);
        if (!parsed) {
            co_return std::unexpected{parsed.error()};
        }
        if (spec.target.empty()) {
            spec.target = std::string{parsed->target};
        }
        simple_http::RequestSpec inner;
        inner.method = spec.method;
        inner.target = std::move(spec.target);
        inner.headers = std::move(spec.headers);
        if (spec.expect_100_continue) {
            inner.headers.add(std::string{field::expect}, "100-continue");
        }
        inner.close = spec.close;
        inner.stream_body = true; // the write end stays open on the Stream
        auto stream = co_await m_impl->stream(std::move(url), std::move(inner));
        if (!stream) {
            co_return std::unexpected{stream.error()};
        }
        const std::chrono::milliseconds head =
            spec.response_head_timeout.count() > 0 ? spec.response_head_timeout : m_cfg.response_head_timeout;
        const std::chrono::milliseconds idle =
            spec.body_idle_timeout.count() > 0 ? spec.body_idle_timeout : m_cfg.body_idle_timeout;
        co_return Stream{std::move(*stream), head, idle};
    }

    // Closes idle pooled connections (in-flight ones are untouched).
    void close_idle() { m_impl->close_idle(); }

    // Connection counters (connections opened / reused) — the diagnostics and
    // the tests that assert reuse.
    simple_http::ClientStats stats() const { return m_impl->stats(); }

    // [internal — the reverse proxy's channel] Opens a stream by ClientTarget,
    // keeping the h2c-upgrade path and the pooled flag that the proxy's retry
    // rules need. Public users call open_stream(url) — this is library-internal
    // because it reaches into connection policy (client/http_proxy.h).
    struct OpenTargetStream {
        std::shared_ptr<Stream> stream;
        bool pooled{false};
    };
    [[nodiscard]] asio::awaitable<std::expected<OpenTargetStream, error_code>>
    open_target(simple_http::ClientTarget target, simple_http::RequestSpec spec) const {
        auto opened = co_await m_impl->open_stream(std::move(target), std::move(spec));
        if (!opened) {
            co_return std::unexpected{opened.error()};
        }
        co_return OpenTargetStream{std::make_shared<Stream>(std::move(opened->stream)), opened->pooled};
    }

  private:
    friend class RequestBuilder;
    ClientConfig m_cfg;
    std::shared_ptr<simple_http::HttpClient> m_impl;
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
    simple_http::RequestSpec spec;
    spec.method = m_method;
    spec.headers = m_headers;
    spec.body = m_body;
    simple_http::RequestOptions options;
    options.max_body_bytes = m_max_body_bytes;
    if (m_head_timeout.count() > 0) {
        options.timeout = m_head_timeout; // the underlying per-exchange budget
    }
    auto response = co_await m_client->m_impl->request(url, std::move(spec), options);
    if (!response) {
        co_return std::unexpected{response.error()};
    }
    co_return Response{std::move(*response)};
}

} // namespace simple_http::http