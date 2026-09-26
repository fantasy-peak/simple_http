#pragma once

// HTTP request method — a beast-free enum with string conversions.
//
// The v3 engines parse the method off the wire themselves (HTTP/1 request line,
// HTTP/2 :method pseudo-header) rather than going through boost.beast's verb, so
// this small type replaces http::verb across the request path.

#include <cstdint>
#include <string_view>

namespace simple_http {

enum class Method : std::uint8_t {
    Get,
    Head,
    Post,
    Put,
    Delete,
    Options,
    Patch,
    Connect,
    Trace,
    Unknown,
};

// Case-sensitive match against the canonical uppercase method tokens (HTTP
// methods are case-sensitive per RFC 7230 §3.1.1).
inline Method method_from_string(std::string_view s) noexcept {
    if (s == "GET") return Method::Get;
    if (s == "HEAD") return Method::Head;
    if (s == "POST") return Method::Post;
    if (s == "PUT") return Method::Put;
    if (s == "DELETE") return Method::Delete;
    if (s == "OPTIONS") return Method::Options;
    if (s == "PATCH") return Method::Patch;
    if (s == "CONNECT") return Method::Connect;
    if (s == "TRACE") return Method::Trace;
    return Method::Unknown;
}

// Whether replaying a request with this method is harmless when its outcome is
// unknown. RFC 9110 §9.2.2 defines exactly these as idempotent, which is what
// makes a retry safe: a duplicate has the same effect as the original. POST and
// PATCH are not on the list, and a replay of one can double a side effect.
inline constexpr bool is_idempotent(Method m) noexcept {
    switch (m) {
        case Method::Get:
        case Method::Head:
        case Method::Put:
        case Method::Delete:
        case Method::Options:
        case Method::Trace:
            return true;
        case Method::Post:
        case Method::Patch:
        case Method::Connect:
        case Method::Unknown:
            return false;
    }
    return false;
}

inline constexpr std::string_view to_string(Method m) noexcept {
    switch (m) {
        case Method::Get:
            return "GET";
        case Method::Head:
            return "HEAD";
        case Method::Post:
            return "POST";
        case Method::Put:
            return "PUT";
        case Method::Delete:
            return "DELETE";
        case Method::Options:
            return "OPTIONS";
        case Method::Patch:
            return "PATCH";
        case Method::Connect:
            return "CONNECT";
        case Method::Trace:
            return "TRACE";
        case Method::Unknown:
        default:
            return "";
    }
}

}  // namespace simple_http
