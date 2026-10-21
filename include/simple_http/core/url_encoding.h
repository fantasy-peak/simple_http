#pragma once

// Percent-encoding for building request targets and query strings — the Go
// `net/url.QueryEscape` / `PathEscape` counterparts. The library decodes
// (url_path.h, proto/query.h) but never publicly encodes; anything a handler
// or client builds onto a wire (a search box string into `?q=...`, a filename
// into a path segment) goes through here.

#include <cstddef>
#include <string>
#include <string_view>

namespace simple_http {

namespace detail {

// The one encoder behind query_escape/path_escape. `allow_slash` keeps '/'
// literal (path form), `plus_for_space` writes ' ' as '+' (form encoding).
inline std::string percent_encode(std::string_view s, bool allow_slash, bool plus_for_space) {
    std::string out;
    out.reserve(s.size());
    constexpr std::string_view kHex = "0123456789ABCDEF";
    for (const unsigned char c : s) {
        // unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~"
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~' || (allow_slash && c == '/')) {
            out.push_back(static_cast<char>(c));
        } else if (plus_for_space && c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

} // namespace detail

// Percent-encodes `s` for a URL query component: everything outside the
// RFC 3986 unreserved set, plus space as '+', is written as %XX. Safe for the
// value side of `key=value` — '&' and '=' are encoded, so a value cannot
// splice additional parameters (Go `url.QueryEscape`, form encoding §2.2).
inline std::string query_escape(std::string_view s) { return detail::percent_encode(s, /*allow_slash=*/false, true); }

// Percent-encodes `s` for a path: like query_escape, but '/' stays literal (it
// separates segments; callers escape a single name by passing it without
// slashes) and space is %20 (Go `url.PathEscape`-shaped for a whole path).
inline std::string path_escape(std::string_view s) { return detail::percent_encode(s, /*allow_slash=*/true, false); }

} // namespace simple_http