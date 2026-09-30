#pragma once

// Request: the read-only view of an incoming HTTP request handed to a handler.
//
// It is protocol-agnostic: an engine populates the request line and headers,
// and feeds the body into the owned Body stream. Handlers read the body with
// `co_await req.body().read()`.

#include <boost/asio.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/http_method.h"
#include "../core/types.h"
#include "body.h"
#include "headers.h"

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
    asio::ip::tcp::endpoint peer() const { return m_peer; }
    std::string peer_address() const { return m_peer.address().to_string(); }

    const Headers &headers() const { return m_headers; }
    std::optional<std::string_view> header(std::string_view name) const { return m_headers.get(name); }

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

    // Views into m_target (stable after set_target/assign_head).
    std::string_view m_path;
    std::string_view m_query;
};

} // namespace simple_http
