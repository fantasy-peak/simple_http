#pragma once

// Request: the read-only view of an incoming HTTP request handed to a handler.
//
// It is protocol-agnostic: an engine populates the request line and headers,
// and feeds the body into the owned Body stream. Handlers read the body with
// `co_await req.body().read()`.

#include <any>
#include <boost/asio.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    template <typename Executor>
    Request(Version version, const Executor &exec, asio::ip::tcp::endpoint peer)
        : m_version(version), m_peer(std::move(peer)), m_body(std::make_unique<Body>(exec)) {}

    Request(const Request &) = delete;
    Request &operator=(const Request &) = delete;

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
        split_path_and_query();
        m_query_params = QueryParams::parse(m_query);
    }

    Headers &mutable_headers() { return m_headers; }

  private:
    void split_path_and_query() {
        auto pos = m_target.find('?');
        if (pos != std::string::npos) {
            m_path = std::string_view{m_target}.substr(0, pos);
            m_query = std::string_view{m_target}.substr(pos + 1);
        } else {
            m_path = m_target;
            m_query = {};
        }
    }

    Version m_version;
    asio::ip::tcp::endpoint m_peer;
    std::unique_ptr<Body> m_body;

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
