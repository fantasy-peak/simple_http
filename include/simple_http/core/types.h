#pragma once

// Core protocol-agnostic value types shared across every layer of the server.
// This header intentionally depends only on the standard library and
// boost.system (for error_code), so it can be included from anywhere without
// pulling in Beast/Asio I/O machinery.

#include <cstdint>
#include <string_view>

#include <boost/system/error_code.hpp>

namespace simple_http {

using error_code = boost::system::error_code;

// Whether a string carries a byte that must never appear in an HTTP field name,
// value or request target (CR, LF, NUL). HTTP/2 does not delimit fields itself,
// so a peer can put any byte there; anything that later synthesizes an HTTP/1.1
// message (a handler, or the reverse proxy) would splice it into a request line
// or a header and split the message in two.
inline bool contains_ctl(std::string_view s) noexcept {
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\0') return true;
    }
    return false;
}

// RFC 9110 §5.6.2's tchar — the bytes an HTTP token (a method name, a field
// name) may contain. Spelled out here rather than shared with the HTTP/1
// parser's `is_tchar`: that one lives in engine/, which handler/ must not
// include, and a value about to be echoed into a response head is not the place
// to start skipping the check.
inline bool is_token_char(unsigned char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

// Whether `s` is a non-empty run of tchars. Checked before a peer-supplied token
// (a preflight's Access-Control-Request-Method) is echoed into a response head,
// so what goes out is always a string that would re-parse as the same thing.
inline bool is_http_token(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        if (!is_token_char(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// ASCII case folding and comparison. They live here, in the layer that depends on
// nothing but the standard library, because both the field-name layer and the
// content-encoding layer need them — and the alternative was a copy in each.
inline char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Symmetric: either side may be mixed case. (proto/headers.h has a
// lower-`rhs`-only variant that is faster, but it is only valid where the other
// side is a stored, already-folded name — which is why it is private there.)
inline bool iequals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

// The wire protocol a request/response is being served over.
enum class Version : std::uint8_t {
    Http1 = 0,   // HTTP/1.0
    Http11 = 1,  // HTTP/1.1
    Http2 = 2,   // HTTP/2 (h2 / h2c)
    Http3 = 3,   // HTTP/3 (QUIC) — reserved; enabled via SIMPLE_HTTP_ENABLE_HTTP3
};

inline constexpr std::string_view to_string(Version v) noexcept {
    switch (v) {
        case Version::Http1:
            return "HTTP/1.0";
        case Version::Http11:
            return "HTTP/1.1";
        case Version::Http2:
            return "HTTP/2";
        case Version::Http3:
            return "HTTP/3";
        default:
            return "Unknown";
    }
}

// Whether a body write is the final piece of the response body.
enum class WriteMode : std::int8_t {
    More,  // more body chunks will follow
    Last,  // this is the last chunk; the response body is complete
};

// Outcome of reading from a request body stream.
enum class StreamStatus : std::int8_t {
    Ok,          // data read successfully, more may follow
    Eof,         // the peer finished the request body normally
    Rst,         // the stream was reset by the peer
    Disconnect,  // the underlying connection was lost
};

// Tag types used as sentinel values on the body/stream channels.
struct Eof {};
struct Rst {};
struct Disconnect {};

}  // namespace simple_http
