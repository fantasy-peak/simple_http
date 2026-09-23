#pragma once

// Request: the read-only view of an incoming HTTP request handed to a handler.
//
// It is protocol-agnostic: an engine populates the request line and headers,
// and feeds the body into the owned Body stream. Handlers read the body with
// `co_await req.body().read()`.

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include "../core/http_method.h"
#include "../core/types.h"
#include "body.h"
#include "headers.h"

namespace simple_http {

namespace asio = boost::asio;
namespace http = boost::beast::http;

// Maps a beast verb to the beast-free Method enum. Kept only for assign_head
// (the h2c-upgrade path still hands over a beast request); the wire engines set
// Method directly.
inline Method method_from_verb(http::verb v) noexcept {
    switch (v) {
        case http::verb::get:
            return Method::Get;
        case http::verb::head:
            return Method::Head;
        case http::verb::post:
            return Method::Post;
        case http::verb::put:
            return Method::Put;
        case http::verb::delete_:
            return Method::Delete;
        case http::verb::options:
            return Method::Options;
        case http::verb::patch:
            return Method::Patch;
        case http::verb::connect:
            return Method::Connect;
        case http::verb::trace:
            return Method::Trace;
        default:
            return Method::Unknown;
    }
}

class Request {
  public:
    template <typename Executor>
    Request(Version version, const Executor& exec, asio::ip::tcp::endpoint peer)
        : m_version(version), m_peer(std::move(peer)), m_body(std::make_unique<Body>(exec)) {}

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

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

    const Headers& headers() const { return m_headers; }
    std::optional<std::string_view> header(std::string_view name) const { return m_headers.get(name); }

    Body& body() { return *m_body; }
    const Body& body() const { return *m_body; }

    // --- population API (engine side) ---
    void set_method(Method method) { m_method = method; }
    void set_method_token(std::string token) {
        m_method = method_from_string(token);
        m_method_token = std::move(token);
    }

    void set_target(std::string target) {
        m_target = std::move(target);
        split_path_and_query();
    }

    Headers& mutable_headers() { return m_headers; }

    // Populate the request line + headers from a parsed Beast HTTP/1.x request,
    // and stream its (already read) body into the Body.
    // Populate the request line + headers from any parsed Beast HTTP/1.x request
    // (works with string_body, buffer_body, etc. — only the header is read).
    template <typename Body>
    void assign_head(const http::request<Body>& req) {
        m_method = method_from_verb(req.method());
        m_method_token = std::string{http::to_string(req.method())};
        set_target(std::string{req.target()});
        m_headers.clear();
        for (const auto& field : req) {
            std::string name{field.name_string()};
            m_headers.add(std::move(name), std::string{field.value()});
        }
    }

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

    // Views into m_target (stable after set_target/assign_head).
    std::string_view m_path;
    std::string_view m_query;
};

}  // namespace simple_http
