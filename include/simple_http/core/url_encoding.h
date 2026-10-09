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

// Percent-encodes `s` for a URL query component: everything outside the
// RFC 3986 unreserved set, plus space as '+', is written as %XX. Safe for the
// value side of `key=value` — '&' and '=' are encoded, so a value cannot
// splice additional parameters (Go `url.QueryEscape`, form encoding §2.2).
inline std::string query_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    constexpr std::string_view kHex = "0123456789ABCDEF";
    for (const unsigned char c : s) {
        // unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~"; space becomes '+'.
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

// Percent-encodes `s` for a path segment: like query_escape, but '/' is also
// encoded (a name cannot create a segment boundary) and space stays %20
// (Go `url.PathEscape`; routers decode both forms, so only the wire bytes
// differ).
inline std::string path_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    constexpr std::string_view kHex = "0123456789ABCDEF";
    for (const unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

} // namespace simple_http