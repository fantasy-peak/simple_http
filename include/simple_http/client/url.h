#pragma once

// Absolute-URL parsing for the client layer.
//
// Only what an HTTP client needs: scheme, authority (host + optional port) and
// the request target. The grammar accepted is RFC 3986's absolute form without
// the parts HTTP does not put on the wire — userinfo is parsed and discarded,
// and a fragment is dropped (RFC 9110 §7.1: a client must strip it before
// sending). Relative or scheme-less input is rejected with client_errc::bad_url
// rather than guessed at.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "../core/types.h"
#include "client_config.h"

namespace simple_http {

struct Url {
    std::string scheme;     // "http" or "https"
    std::string host;       // host name, IPv4 or bare IPv6 literal (no brackets)
    std::uint16_t port{0};  // explicit port; 0 = the scheme's default
    // Path + query, in origin-form, always at least "/" and never containing a
    // fragment. This is what goes on the request line (HTTP/1.1) or into :path.
    std::string target{"/"};

    bool use_tls() const {
        return scheme == "https";
    }

    std::uint16_t effective_port() const {
        if (port != 0)
            return port;
        return use_tls() ? 443 : 80;
    }

    // The path part of `target` (no query).
    std::string_view path() const {
        auto q = target.find('?');
        return q == std::string::npos ? std::string_view{target} : std::string_view{target}.substr(0, q);
    }

    // The query part of `target`, without the leading '?'; empty if none.
    std::string_view query() const {
        auto q = target.find('?');
        return q == std::string::npos ? std::string_view{} : std::string_view{target}.substr(q + 1);
    }

    // "host:port", bracketing IPv6 literals as the Host header requires.
    std::string authority() const {
        std::string out;
        if (host.find(':') != std::string::npos) {
            out.push_back('[');
            out.append(host);
            out.push_back(']');
        } else {
            out.append(host);
        }
        out.push_back(':');
        out.append(std::to_string(effective_port()));
        return out;
    }

    // The ClientTarget that reaches this URL.
    ClientTarget to_target() const {
        ClientTarget t;
        t.host = host;
        t.port = effective_port();
        t.use_tls = use_tls();
        return t;
    }
};

namespace detail {

// Whether `c` is a valid URI scheme character (RFC 3986 §3.1).
inline bool url_scheme_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
           c == '.';
}

inline std::string url_lower(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

inline bool url_valid_port(std::string_view s, std::uint16_t& out) {
    if (s.empty() || s.size() > 5)
        return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10u + static_cast<unsigned>(c - '0');
    }
    if (v == 0 || v > 65535u)
        return false;
    out = static_cast<std::uint16_t>(v);
    return true;
}

}  // namespace detail

// Parses an absolute http:// or https:// URL. On failure the returned
// error_code is client_errc::unsupported_scheme (a well-formed URL for another
// scheme) or client_errc::bad_url.
inline std::expected<Url, error_code> parse_url(std::string_view url) {
    // scheme "://"
    const auto sep = url.find("://");
    if (sep == std::string_view::npos || sep == 0) {
        return std::unexpected{make_error_code(client_errc::bad_url)};
    }
    std::string scheme = detail::url_lower(url.substr(0, sep));
    for (char c : scheme) {
        if (!detail::url_scheme_char(c))
            return std::unexpected{make_error_code(client_errc::bad_url)};
    }
    if (scheme != "http" && scheme != "https") {
        return std::unexpected{make_error_code(client_errc::unsupported_scheme)};
    }

    std::string_view rest = url.substr(sep + 3);
    // Everything from the first '/' or '?' belongs to the target; '#' starts a
    // fragment, which is never sent.
    std::size_t authority_end = rest.find_first_of("/?#");
    std::string_view authority = authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);
    std::string_view tail = authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    // Drop userinfo ("user:pass@").
    if (auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.empty()) {
        return std::unexpected{make_error_code(client_errc::bad_url)};
    }

    Url out;
    out.scheme = std::move(scheme);

    std::string_view port_str;
    if (authority.front() == '[') {
        // IPv6 literal: the colon-delimited port can only follow the closing
        // bracket, so the host may keep its own colons.
        auto close = authority.find(']');
        if (close == std::string_view::npos)
            return std::unexpected{make_error_code(client_errc::bad_url)};
        out.host = std::string{authority.substr(1, close - 1)};
        std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':')
                return std::unexpected{make_error_code(client_errc::bad_url)};
            port_str = after.substr(1);
        }
        if (out.host.empty())
            return std::unexpected{make_error_code(client_errc::bad_url)};
    } else if (auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        out.host = std::string{authority.substr(0, colon)};
        port_str = authority.substr(colon + 1);
    } else {
        out.host = std::string{authority};
    }
    if (out.host.empty()) {
        return std::unexpected{make_error_code(client_errc::bad_url)};
    }
    if (!port_str.empty() || authority.back() == ':') {
        if (!detail::url_valid_port(port_str, out.port)) {
            return std::unexpected{make_error_code(client_errc::bad_url)};
        }
    }

    // Strip the fragment from the tail, then keep path + query as the target.
    if (auto hash = tail.find('#'); hash != std::string_view::npos) {
        tail = tail.substr(0, hash);
    }
    if (tail.empty()) {
        out.target = "/";
    } else if (tail.front() == '?') {
        out.target = "/" + std::string{tail};  // "http://h?q" -> "/?q"
    } else {
        out.target = std::string{tail};
    }

    // Reject control bytes outright: they would be spliced into a request line
    // or header downstream, which can split the message in two.
    if (contains_ctl(out.host) || contains_ctl(out.target)) {
        return std::unexpected{make_error_code(client_errc::bad_url)};
    }
    return out;
}

}  // namespace simple_http
