#pragma once

// Router: matches a request to a handler and runs it.
//
// Matching order: exact path map -> regex list (first match) -> fallback.
// Optional `before` and `cors` filters run first and may short-circuit. The
// Router's dispatch(shared_ptr<Request>, shared_ptr<Response>, SslHandle) matches the engine's
// Dispatcher type, so the same router serves every protocol version.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include "../client/http_client.h"  // HttpClient (reverse-proxy upstreams)
#include "../core/http_status.h"
#include "../core/logging.h"
#include "../engine/dispatcher.h"  // WsProxyTarget, HttpProxyTarget
#include "handler.h"
#include "http_proxy.h"

#ifdef SIMPLE_HTTP_USE_BOOST_REGEX
#include <boost/regex.hpp>
namespace simple_http_regex = boost;
#else
#include <regex>
namespace simple_http_regex = std;
#endif

namespace simple_http {

namespace asio = boost::asio;

class Router {
  public:
    // `proxy_client` configures the client used by reverse-proxy routes: TLS
    // policy for HTTPS backends (CA bundle, client certificate for mTLS, whether
    // to verify), timeouts and pool sizing. The default suits public backends.
    explicit Router(ClientConfig proxy_client = {})
        : m_http_client(std::make_shared<HttpClient>(std::move(proxy_client))) {}

    // --- registration (fluent) ---
    template <typename F>
    Router& route(std::string path, F&& handler) {
        // emplace, not insert_or_assign: the first registration wins, and that is
        // deliberate — test_router.cpp pins it by name. What it should not do is
        // keep the second one *silently*, since re-registering a path is a likely
        // result of moving a route around, so the duplicate says so.
        auto [it, inserted] = m_exact.emplace(std::move(path), make_handler(std::forward<F>(handler)));
        if (!inserted) {
            SIMPLE_HTTP_INFO_LOG("route [{}] already registered; the first handler stays", it->first);
        }
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

    // --- WebSocket proxy-route registration (byte-level pass-through) ---
    Router& ws_proxy(std::string path, WsProxyTarget target) {
        m_ws_proxy_exact.emplace(std::move(path), std::move(target));
        return *this;
    }

    Router& ws_proxy_regex(const std::string& pattern, WsProxyTarget target) {
        try {
            m_ws_proxy_regex.emplace_back(simple_http_regex::regex{pattern}, std::move(target));
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("invalid ws proxy regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // --- HTTP reverse-proxy route registration (request-level) ---
    Router& http_proxy(std::string path, HttpProxyTarget target) {
        m_http_proxy_exact.emplace(std::move(path), std::move(target));
        return *this;
    }

    Router& http_proxy_regex(const std::string& pattern, HttpProxyTarget target) {
        try {
            m_http_proxy_regex.emplace_back(simple_http_regex::regex{pattern}, std::move(target));
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("invalid http proxy regex [{}]: {}", pattern, e.what());
        }
        return *this;
    }

    // Looks up a WebSocket handler for a path (exact then regex). Returns nullptr
    // if none matches. Used by the engine after a successful upgrade handshake.
    const WsHandler* find_ws(std::string_view path) const {
        if (auto it = m_ws_exact.find(path); it != m_ws_exact.end()) {
            return &it->second;
        }
        if (m_ws_regex.empty()) {
            return nullptr;  // nothing to match, and no string to build for it
        }
        std::string p{path};
        for (const auto& [pattern, handler] : m_ws_regex) {
            if (simple_http_regex::regex_match(p, pattern)) {
                return &handler;
            }
        }
        return nullptr;
    }

    // Looks up a WebSocket proxy backend for a path (exact then regex). Returns
    // std::nullopt if the path is not a proxy route. Used by the engine before
    // the local ws handler lookup, so proxy routes take precedence.
    //
    // For a regex route whose rewrite_path is a substitution template, capture
    // groups from the match are expanded here, so the returned target's
    // rewrite_path is the final path to send to the backend.
    std::optional<WsProxyTarget> find_ws_proxy(std::string_view path) const {
        if (auto it = m_ws_proxy_exact.find(path); it != m_ws_proxy_exact.end()) {
            return it->second;  // exact route: rewrite_path used verbatim
        }
        if (m_ws_proxy_regex.empty()) {
            return std::nullopt;
        }
        std::string p{path};
        for (const auto& [pattern, target] : m_ws_proxy_regex) {
            simple_http_regex::smatch m;
            if (simple_http_regex::regex_match(p, m, pattern)) {
                WsProxyTarget out = target;
                if (!out.rewrite_path.empty()) {
                    out.rewrite_path = expand_rewrite(target.rewrite_path, m);
                }
                return out;
            }
        }
        return std::nullopt;
    }

    // Looks up an HTTP reverse-proxy backend for a path (exact then regex).
    // Returns std::nullopt if the path is not a proxy route. For a regex route
    // whose rewrite_path is a substitution template, capture groups are expanded
    // here, so the returned target's rewrite_path is the final target to send.
    std::optional<HttpProxyTarget> find_http_proxy(std::string_view path) const {
        if (auto it = m_http_proxy_exact.find(path); it != m_http_proxy_exact.end()) {
            return it->second;
        }
        if (m_http_proxy_regex.empty()) {
            return std::nullopt;
        }
        std::string p{path};
        for (const auto& [pattern, target] : m_http_proxy_regex) {
            simple_http_regex::smatch m;
            if (simple_http_regex::regex_match(p, m, pattern)) {
                HttpProxyTarget out = target;
                if (!out.rewrite_path.empty()) {
                    out.rewrite_path = expand_rewrite(target.rewrite_path, m);
                }
                return out;
            }
        }
        return std::nullopt;
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

        // A view, not a copy: the exact maps have transparent hashers, so they can
        // be probed with the request's own path, and the proxy lookups take a
        // string_view too. The copy is only needed for the regex walk — and only
        // when there are regex routes, which is the uncommon case. `req->path()`
        // stays valid until req is moved into a handler below, and it is not
        // touched after that.
        const std::string_view path = req->path();

        // HTTP reverse-proxy routes take precedence over local handlers.
        if (auto target = find_http_proxy(path)) {
            bool client_is_tls = ssl.has_value() && *ssl != nullptr;
            co_await run_http_proxy(std::move(req), std::move(res), client_is_tls, std::move(*target),
                                    *m_http_client);
            co_return;
        }

        if (auto it = m_exact.find(path); it != m_exact.end()) {
            co_await invoke_handler(it->second, std::move(req), std::move(res), ssl);
            co_return;
        }
        if (!m_regex.empty()) {
            const std::string owned_path{path};  // regex_match needs a string
            for (const auto& [pattern, handler] : m_regex) {
                if (simple_http_regex::regex_match(owned_path, pattern)) {
                    co_await invoke_handler(handler, std::move(req), std::move(res), ssl);
                    co_return;
                }
            }
        }
        if (m_fallback) {
            co_await invoke_handler(*m_fallback, std::move(req), std::move(res), ssl);
            co_return;
        }
        // Built-in 404.
        co_await res->status(status::not_found).send("");
        co_return;
    }

  private:
    // Expands a rewrite template against a regex match: $0 = whole match,
    // $1..$9 = capture groups (empty if the group did not participate), $$ = a
    // literal '$'. A lone '$' or '$' before a non-digit/non-'$' is kept verbatim.
    static std::string expand_rewrite(const std::string& tmpl, const simple_http_regex::smatch& m) {
        std::string out;
        out.reserve(tmpl.size());
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] != '$') {
                out.push_back(tmpl[i]);
                continue;
            }
            if (i + 1 >= tmpl.size()) {
                out.push_back('$');  // trailing '$'
                break;
            }
            char c = tmpl[i + 1];
            if (c == '$') {
                out.push_back('$');
                ++i;
            } else if (c >= '0' && c <= '9') {
                std::size_t idx = static_cast<std::size_t>(c - '0');
                if (idx < m.size() && m[idx].matched) {
                    out.append(m[idx].str());
                }
                ++i;
            } else {
                out.push_back('$');  // not a placeholder; keep the '$'
            }
        }
        return out;
    }

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

    std::unordered_map<std::string, WsProxyTarget, string_hash, std::equal_to<>> m_ws_proxy_exact;
    std::vector<std::pair<simple_http_regex::regex, WsProxyTarget>> m_ws_proxy_regex;

    std::unordered_map<std::string, HttpProxyTarget, string_hash, std::equal_to<>> m_http_proxy_exact;
    // One client for every proxy route: it owns the upstream connection pool and
    // the TLS context, and serves any number of origins (the target carries the
    // origin per request).
    std::shared_ptr<HttpClient> m_http_client;
    std::vector<std::pair<simple_http_regex::regex, HttpProxyTarget>> m_http_proxy_regex;
};

}  // namespace simple_http
