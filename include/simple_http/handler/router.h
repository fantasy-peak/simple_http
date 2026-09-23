#pragma once

// Router: matches a request to a handler and runs it.
//
// Matching order: exact path map -> regex list (first match) -> fallback.
// Optional `before` and `cors` filters run first and may short-circuit. The
// Router's dispatch(Request&, Response&, SslHandle) satisfies the engine's
// Dispatcher type, so the same router serves every protocol version.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/status.hpp>

#include "../core/logging.h"
#include "handler.h"

#ifdef SIMPLE_HTTP_USE_BOOST_REGEX
#include <boost/regex.hpp>
namespace simple_http_regex = boost;
#else
#include <regex>
namespace simple_http_regex = std;
#endif

namespace simple_http {

namespace asio = boost::asio;
namespace http = boost::beast::http;

class Router {
  public:
    // --- registration (fluent) ---
    template <typename F>
    Router& route(std::string path, F&& handler) {
        m_exact.emplace(std::move(path), make_handler(std::forward<F>(handler)));
        return *this;
    }

    template <typename F>
    Router& route_regex(const std::string& pattern, F&& handler) {
        try {
            m_regex.emplace_back(simple_http_regex::regex{pattern}, make_handler(std::forward<F>(handler)));
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("invalid route regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    template <typename F>
    Router& fallback(F&& handler) {
        m_fallback = make_handler(std::forward<F>(handler));
        return *this;
    }

    Router& before(Filter filter) {
        m_before = std::move(filter);
        return *this;
    }
    Router& cors(Filter filter) {
        m_cors = std::move(filter);
        return *this;
    }

    // --- WebSocket route registration ---
    Router& ws_route(std::string path, WsHandler handler) {
        m_ws_exact.emplace(std::move(path), std::move(handler));
        return *this;
    }

    Router& ws_route_regex(const std::string& pattern, WsHandler handler) {
        try {
            m_ws_regex.emplace_back(simple_http_regex::regex{pattern}, std::move(handler));
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("invalid ws route regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // Looks up a WebSocket handler for a path (exact then regex). Returns nullptr
    // if none matches. Used by the engine after a successful upgrade handshake.
    const WsHandler* find_ws(std::string_view path) const {
        if (auto it = m_ws_exact.find(path); it != m_ws_exact.end()) {
            return &it->second;
        }
        std::string p{path};
        for (const auto& [pattern, handler] : m_ws_regex) {
            if (simple_http_regex::regex_match(p, pattern)) {
                return &handler;
            }
        }
        return nullptr;
    }

    // --- dispatch (satisfies the engine Dispatcher) ---
    asio::awaitable<void> dispatch(RequestPtr req, ResponsePtr res, SslHandle ssl) const {
        // CORS filter runs only for cross-origin requests.
        if (m_cors && req->header("origin").has_value()) {
            if (!co_await m_cors(req, res)) {
                co_return;
            }
        }
        if (m_before) {
            if (!co_await m_before(req, res)) {
                co_return;
            }
        }

        std::string path{req->path()};

        if (auto it = m_exact.find(path); it != m_exact.end()) {
            co_await invoke_handler(it->second, std::move(req), std::move(res), ssl);
            co_return;
        }
        for (const auto& [pattern, handler] : m_regex) {
            if (simple_http_regex::regex_match(path, pattern)) {
                co_await invoke_handler(handler, std::move(req), std::move(res), ssl);
                co_return;
            }
        }
        if (m_fallback) {
            co_await invoke_handler(*m_fallback, std::move(req), std::move(res), ssl);
            co_return;
        }
        // Built-in 404.
        co_await res->status(http::status::not_found).send("");
        co_return;
    }

  private:
    struct string_hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };

    std::unordered_map<std::string, Handler, string_hash, std::equal_to<>> m_exact;
    std::vector<std::pair<simple_http_regex::regex, Handler>> m_regex;
    std::optional<Handler> m_fallback;
    Filter m_before;
    Filter m_cors;

    std::unordered_map<std::string, WsHandler, string_hash, std::equal_to<>> m_ws_exact;
    std::vector<std::pair<simple_http_regex::regex, WsHandler>> m_ws_regex;
};

}  // namespace simple_http
