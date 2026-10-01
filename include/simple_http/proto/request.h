#pragma once

// Request: the HTTP request message, shared by both sides of the wire.
//
// Server-side: an engine populates the request line and headers, feeds the body
// into the owned Body stream, and the handler reads it back with
// `co_await req.body().read()`. Client-side: the caller sets method/target/
// headers/body (or streams the body via body().feed()), and the engine reads it
// to send. Go's http.Request is the same single type for both roles; so is this.

#include <any>
#include <boost/asio.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/base64.h" // basic_auth
#include "../core/http_field.h"
#include "../core/http_method.h"
#include "../core/types.h"
#include "body.h"
#include "headers.h"
#include "query.h"

namespace simple_http {

namespace asio = boost::asio;

class Request {
  public:
    // Movable but not copyable: the owned Body stream cannot be duplicated, and
    // once owned by a Request the body has already begun flowing. The path/query
    // views are rebound to the moved-to target.
    Request(Request &&other) noexcept
        : m_version(other.m_version), m_peer(std::move(other.m_peer)), m_body(std::move(other.m_body)),
          m_method(other.m_method), m_method_token(std::move(other.m_method_token)),
          m_target(std::move(other.m_target)), m_headers(std::move(other.m_headers)),
          m_params(std::move(other.m_params)), m_query_params(std::move(other.m_query_params)),
          m_close(other.m_close), m_stream_body(other.m_stream_body), m_content_length(other.m_content_length),
          m_body_source(std::move(other.m_body_source)) {
        rebind_views();
    }
    Request &operator=(Request &&other) noexcept {
        if (this == &other)
            return *this;
        m_version = other.m_version;
        m_peer = std::move(other.m_peer);
        m_body = std::move(other.m_body);
        m_method = other.m_method;
        m_method_token = std::move(other.m_method_token);
        m_target = std::move(other.m_target);
        m_headers = std::move(other.m_headers);
        m_params = std::move(other.m_params);
        m_query_params = std::move(other.m_query_params);
        m_close = other.m_close;
        m_stream_body = other.m_stream_body;
        m_content_length = other.m_content_length;
        m_body_source = std::move(other.m_body_source);
        rebind_views();
        return *this;
    }
    Request(const Request &) = delete;
    Request &operator=(const Request &) = delete;

  private:
    void rebind_views() {
        auto pos = m_target.find('?');
        if (pos != std::string::npos) {
            m_path = std::string_view{m_target}.substr(0, pos);
            m_query = std::string_view{m_target}.substr(pos + 1);
        } else {
            m_path = m_target;
            m_query = {};
        }
    }

  public:
    // Server-side construction: the engine fills the request as it parses it,
    // and the handler reads it back. `peer` is the connection's remote address
    // (the source of `X-Forwarded-For` and friend).
    template <typename Executor>
    Request(Version version, const Executor &exec, asio::ip::tcp::endpoint peer)
        : m_version(version), m_peer(std::move(peer)), m_body(std::make_unique<Body>(exec)) {}

    // Client-side construction: the caller builds a request to send and the
    // engine reads it back. No peer exists yet; one is only meaningful on the
    // receiving side. The body starts empty — feed it and finish() it before
    // handing the request to the client, or set stream_body(true) to write it
    // as the exchange goes (Go's http.Request.Body with Close semantics).
    template <typename Executor>
    explicit Request(Version version, const Executor &exec)
        : m_version(version), m_body(std::make_unique<Body>(exec)) {}

    // --- request line / metadata ---
    Method method() const { return m_method; }
    // Raw method token (preserves extension methods that map to Method::Unknown).
    std::string_view method_token() const { return m_method_token; }
    Version version() const { return m_version; }
    std::string_view target() const { return m_target; }
    std::string_view path() const { return m_path; }
    std::string_view query() const { return m_query; }
    // The URL query, parsed and decoded (Go's r.URL.Query() / axum Query<T>).
    // Values live on the Request, so the views stay valid for the request's
    // whole life.
    const QueryParams &query_params() const { return m_query_params; }
    asio::ip::tcp::endpoint peer() const { return m_peer; }
    std::string peer_address() const { return m_peer.address().to_string(); }

    const Headers &headers() const { return m_headers; }
    std::optional<std::string_view> header(std::string_view name) const { return m_headers.get(name); }

    // The value of a cookie in the Cookie request header (first match), or
    // nullopt. Values are returned as sent — no percent-decoding — like Go's
    // r.Cookie. The header is "name=value; name2=value2"; a quoted value is
    // returned without its quotes.
    std::optional<std::string_view> cookie(std::string_view name) const {
        const auto cookie_header = header(field::cookie);
        if (!cookie_header) {
            return std::nullopt;
        }
        std::size_t pos = 0;
        for (;;) {
            const std::size_t semi = cookie_header->find(';', pos);
            std::string_view pair =
                cookie_header->substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
            while (!pair.empty() && pair.front() == ' ') {
                pair.remove_prefix(1);
            }
            const std::size_t eq = pair.find('=');
            if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
                std::string_view value = pair.substr(eq + 1);
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value.remove_prefix(1);
                    value.remove_suffix(1);
                }
                return value;
            }
            if (semi == std::string_view::npos) {
                return std::nullopt;
            }
            pos = semi + 1;
        }
    }

    Body &body() { return *m_body; }
    const Body &body() const { return *m_body; }

    // Convenience for the client side: sets the whole body up front, as one
    // chunk, and signals the end. Equivalent to feeding the body directly.
    // Records the length so the engine can frame it with Content-Length. The
    // source string is kept so a pooled-connection replay can rebuild the
    // request (Go's http.Request.GetBody): the Body stream cannot be re-read
    // once consumed, but a *fresh* request fed from the same source can.
    void set_body(std::string data) {
        m_body_source = std::move(data);
        m_content_length = static_cast<std::int64_t>(m_body_source.size());
        (void)m_body->feed(m_body_source);
        m_body->finish();
    }

    // A fresh shared request carrying the same method/target/headers and a re-fed
    // body. `exec` drives the new body's channel. Used by the client when a
    // pooled connection turned out stale before the request was written, or
    // when the h2c-upgrade handshake consumes the request: the body is rebuilt
    // from the saved source, so the copy sends the same bytes.
    template <typename Executor> std::shared_ptr<Request> clone_for_replay(const Executor &exec) const {
        auto copy = std::make_shared<Request>(Version::Http11, exec);
        copy->set_method(m_method);
        copy->set_target(m_target);
        copy->mutable_headers() = m_headers;
        copy->m_close = m_close;
        copy->m_stream_body = m_stream_body;
        if (!m_body_source.empty()) {
            copy->set_body(m_body_source);
        }
        return copy;
    }

    // HTTP Basic (RFC 7617): sets `Authorization: Basic base64(user:pass)` —
    // the client-side counterpart of middleware::basic_auth. Returns *this for
    // chaining: req.basic_auth("svc", "s3cret").set_target("/x").
    Request &basic_auth(std::string username, std::string password) {
        mutable_headers().add(std::string{field::authorization},
                              "Basic " + base64_encode(username + ":" + password));
        return *this;
    }

    // --- client-side transmission knobs (Go's http.Request.Close / Body) ---
    // Ask the peer to close after this exchange (HTTP/1.1 `Connection: close`;
    // on HTTP/2 the connection is simply not pooled). Server-side requests never
    // set these; they describe what the *client* wants the wire to do.
    void set_close(bool close) { m_close = close; }
    bool close() const { return m_close; }
    // The body is written as the exchange goes (HTTP/1.1 chunked, HTTP/2 DATA),
    // rather than supplied up front. Set it and drive `body().feed()` from the
    // caller — the engine reads what you feed as it sends.
    void set_stream_body(bool stream) { m_stream_body = stream; }
    bool stream_body() const { return m_stream_body; }

    // The request body's length: set when the body is known up front
    // (set_body), -1 when it is streamed and its length is unknown (Go's
    // http.Request.ContentLength). A known length lets HTTP/1.1 frame with
    // Content-Length; unknown uses chunked.
    std::int64_t content_length() const { return m_content_length; }
    void set_content_length(std::int64_t n) { m_content_length = n; }

    // A path parameter captured by the router from a template route
    // (`/users/{id}`) — nullopt when the route was not a template or the name
    // was not in it. The value is a view into the request path.
    std::optional<std::string_view> param(std::string_view name) const {
        for (const auto &[n, v] : m_params) {
            if (n == name)
                return v;
        }
        return std::nullopt;
    }

    // --- per-request state (middleware handoff) ---
    // A typed value an outer middleware sets and an inner middleware or the
    // handler reads — the Go `context.Context` / tower `Extensions` slot, for
    // things that must not live in headers (an authenticated principal, a
    // request id, a deadline). Values are keyed by static type, so
    // `get_state<T>()` finds the value most recently set *as T*: last set wins.
    // The type must match exactly — `set_state<std::string>("x")` is not found
    // by `get_state<std::string_view>()`.
    template <typename T> void set_state(T value) { m_state.push_back(std::any{std::move(value)}); }

    // The value most recently set as T, or nullptr if none was.
    template <typename T> T *get_state() {
        for (auto it = m_state.rbegin(); it != m_state.rend(); ++it) {
            if (auto *p = std::any_cast<T>(&*it)) {
                return p;
            }
        }
        return nullptr;
    }
    template <typename T> const T *get_state() const {
        for (auto it = m_state.rbegin(); it != m_state.rend(); ++it) {
            if (auto *p = std::any_cast<T>(&*it)) {
                return p;
            }
        }
        return nullptr;
    }

    // --- population API (engine side) ---
    // Router side: records one captured template segment. The value must be a
    // view into the request path (it is, for dispatch's own captures).
    void set_param(std::string name, std::string_view value) { m_params.emplace_back(std::move(name), value); }

    void set_method(Method method) { m_method = method; }
    void set_method_token(std::string token) {
        m_method = method_from_string(token);
        m_method_token = std::move(token);
    }

    void set_target(std::string target) {
        m_target = std::move(target);
        rebind_views();
        m_query_params = QueryParams::parse(m_query);
    }

    Headers &mutable_headers() { return m_headers; }

  private:

    Version m_version;
    asio::ip::tcp::endpoint m_peer;
    std::unique_ptr<Body> m_body;

    bool m_close{false};
    bool m_stream_body{false};
    std::int64_t m_content_length{-1};
    std::string m_body_source; // for clone_for_replay (empty = no up-front body)

    Method m_method{Method::Get};
    std::string m_method_token{"GET"};
    std::string m_target;
    Headers m_headers;

    // Path parameters from a template route, published by the Router during
    // dispatch. Values are views into m_target (stable after set_target).
    std::vector<std::pair<std::string, std::string_view>> m_params;

    // The request URL query, parsed whenever the target is set.
    QueryParams m_query_params;

    // Per-request state set by middleware (see set_state/get_state). Stored
    // type-erased; lookups scan from the back so the most recent set wins.
    std::vector<std::any> m_state;

    // Views into m_target (stable after set_target/assign_head).
    std::string_view m_path;
    std::string_view m_query;
};

} // namespace simple_http
