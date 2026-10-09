#pragma once

// An in-memory cookie jar for the convenience layer — the Go http.CookieJar /
// reqwest cookie_store equivalent. Cookies are collected from Set-Cookie
// response headers and returned as a Cookie request header, filtered by the
// usual rules: Domain (host suffix, defaults to the host itself), Path (prefix,
// defaults to the request's directory), Secure (https only) and Expires /
// Max-Age. HttpOnly and SameSite are accepted and stored, but ignored for
// sending (there is no script context here to protect).
//
// The jar is thread-safe; a HttpClient shares one across its redirect hops by
// default. It is advisory storage, not a spec-complete user agent: a single
// jar, no partitioning, no rejection of suspicious Domain attributes.

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/http_date.h" // parse_http_date
#include "../core/http_field.h"
#include "../core/types.h" // iequals_ci (Domain matching)
#include "../proto/headers.h"
#include "url.h" // parse_url

namespace simple_http {

class CookieJar {
  public:
    // Records every Set-Cookie in `headers`, scoped by `url`. Expired cookies
    // (Max-Age in the past / a past Expires) are dropped from the jar.
    void store(std::string_view url, const Headers &headers) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const detail::Url parsed = detail::parse_url(url).value_or(detail::Url{});
        for (const auto &[name, value] : headers) {
            if (name != "set-cookie") {
                continue;
            }
            const auto cookie = parse_set_cookie(value, parsed);
            if (cookie && !expired(*cookie)) {
                m_cookies.push_back(*cookie);
            }
        }
        drop_expired_locked();
    }

    // The Cookie header value for `url` ("a=b; c=d"), or empty when nothing
    // matches. Cookies are joined without regard to duplicate names.
    std::string cookie_header(std::string_view url) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const detail::Url parsed = detail::parse_url(url).value_or(detail::Url{});
        std::string out;
        for (const auto &cookie : m_cookies) {
            if (!matches(cookie, parsed)) {
                continue;
            }
            if (!out.empty()) {
                out += "; ";
            }
            out += cookie.name;
            out += '=';
            out += cookie.value;
        }
        return out;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cookies.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cookies.size();
    }

  private:
    struct Cookie {
        std::string name;
        std::string value;
        std::string domain; // host-only when it equals host; otherwise a suffix
        std::string path;   // request-directory prefix by default
        bool secure{false};
        // Unix seconds; std::nullopt = session cookie (never ages out here).
        std::optional<std::int64_t> expires{};
    };

    static bool expired(const Cookie &cookie) { return cookie.expires.has_value() && now_unix() >= *cookie.expires; }

    void drop_expired_locked() {
        m_cookies.erase(std::remove_if(m_cookies.begin(), m_cookies.end(), expired), m_cookies.end());
    }

    // Whether a request to `url` may carry this cookie. Domain: exact host when
    // host-only, or a '.'-bounded suffix otherwise (a Domain of "example.com"
    // matches "example.com" and "app.example.com" but not "notexample.com").
    // Path: the request path must begin with the cookie's. Secure: https only.
    static bool matches(const Cookie &cookie, const detail::Url &url) {
        if (url.scheme != "https" && cookie.secure) {
            return false;
        }
        if (!domain_matches(cookie.domain, url.host)) {
            return false;
        }
        if (!path_matches(cookie.path, url.path().empty() ? "/" : url.path())) {
            return false;
        }
        return true;
    }

    static bool domain_matches(std::string_view cookie_domain, std::string_view host) {
        if (cookie_domain.empty()) {
            return false;
        }
        if (host == cookie_domain) {
            return true;
        }
        if (host.size() > cookie_domain.size() && host.ends_with(cookie_domain) &&
            host[host.size() - cookie_domain.size() - 1] == '.') {
            return true;
        }
        return false;
    }

    static bool path_matches(std::string_view cookie_path, std::string_view request_path) {
        if (request_path.starts_with(cookie_path)) {
            // A cookie Path of "/" matches everything; otherwise the match must
            // end on a segment boundary ("/api" does not match "/apix").
            return cookie_path.size() == 1 /* "/" */ || request_path.size() == cookie_path.size() ||
                   request_path[cookie_path.size()] == '/';
        }
        return false;
    }

    // Parses one "name=value; Domain=...; ..." header against the request URL
    // that produced it (defaults: host-only, path = the request's directory).
    static std::optional<Cookie> parse_set_cookie(std::string_view header, const detail::Url &url) {
        const std::size_t semi = header.find(';');
        const std::size_t eq = header.find('=');
        if (eq == std::string_view::npos) {
            return std::nullopt;
        }
        Cookie out;
        out.name = std::string{trim(header.substr(0, eq))};
        if (out.name.empty()) {
            return std::nullopt;
        }
        const std::string_view first_value =
            header.substr(eq + 1, semi == std::string_view::npos ? std::string_view::npos : semi - eq - 1);
        out.value = std::string{trim(first_value)};
        out.domain = url.host; // host-only default
        out.path = request_directory(url.path());

        // Attributes follow the first ';'-separated segment.
        std::size_t pos = semi == std::string_view::npos ? header.size() : semi + 1;
        for (;;) {
            const std::size_t next = header.find(';', pos);
            const std::string_view attr =
                trim(header.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
            const std::size_t attr_eq = attr.find('=');
            const std::string_view key = trim(attr.substr(0, attr_eq));
            std::string_view val =
                attr_eq == std::string_view::npos ? std::string_view{} : trim(attr.substr(attr_eq + 1));
            if (key == "domain" || key == "Domain") {
                if (!val.empty() && val.front() == '.') {
                    val.remove_prefix(1);
                }
                if (!val.empty()) {
                    out.domain = std::string{val};
                }
            } else if (key == "path" || key == "Path") {
                if (!val.empty()) {
                    out.path = std::string{val};
                    if (out.path.front() != '/') {
                        out.path.insert(out.path.begin(), '/');
                    }
                }
            } else if (key == "secure" || key == "Secure" || key == "Secure;") {
                out.secure = true;
            } else if (key == "max-age" || key == "Max-Age") {
                (void)parse_int(val, out.expires, /*relative=*/true);
            } else if (key == "expires" || key == "Expires") {
                if (const auto unix = parse_http_date(val)) {
                    out.expires = *unix;
                }
            }
            if (next == std::string_view::npos) {
                break;
            }
            pos = next + 1;
        }
        return out;
    }

    // The directory of a request path, for the Path default ("/a/b/c" -> "/a/b";
    // "/a" -> "/"; "" -> "/").
    static std::string request_directory(std::string_view path) {
        if (path.empty()) {
            return "/";
        }
        const std::size_t slash = path.rfind('/');
        if (slash <= 1) {
            return "/";
        }
        return std::string{path.substr(0, slash)};
    }

    static std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
            s.remove_suffix(1);
        }
        return s;
    }

    // Parses a number into `out`; `relative` turns it into expires = now + n.
    static bool parse_int(std::string_view s, std::optional<std::int64_t> &out, bool relative) {
        if (s.empty()) {
            return false;
        }
        std::int64_t value = 0;
        for (char c : s) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        out = relative ? now_unix() + value : value;
        return true;
    }

    mutable std::mutex m_mutex;
    std::vector<Cookie> m_cookies;
};

} // namespace simple_http