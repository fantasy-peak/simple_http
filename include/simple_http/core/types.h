#pragma once

// Core protocol-agnostic value types shared across every layer of the server.
// This header intentionally depends only on the standard library and
// boost.system (for error_code), so it can be included from anywhere without
// pulling in Beast/Asio I/O machinery.

#if defined(__has_include)
#if __has_include(<version>)
#include <version> // __cpp_lib_generic_unordered_lookup
#endif
#endif

#include <boost/system/error_code.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

namespace simple_http {

using error_code = boost::system::error_code;

// Whether a string carries a byte that must never appear in an HTTP field name,
// value or request target (CR, LF, NUL). HTTP/2 does not delimit fields itself,
// so a peer can put any byte there; anything that later synthesizes an HTTP/1.1
// message (a handler, or the reverse proxy) would splice it into a request line
// or a header and split the message in two.
inline bool contains_ctl(std::string_view s) noexcept {
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\0')
            return true;
    }
    return false;
}

// RFC 3986: a request-target or authority contains no SP, no control byte,
// and no raw backslash. Backslash matters because several intermediaries
// (and WHATWG URL parsing) normalise it to '/', so `http://trusted.com\.evil/`
// and `GET /\evil` can mean different hosts/paths to different parsers.
// Percent-encode such bytes instead.
inline bool contains_invalid_url_byte(std::string_view s) noexcept {
    for (unsigned char c : s) {
        if (c <= 0x20 || c == 0x7f || c == '\\')
            return true;
    }
    return false;
}

// RFC 9113 §8.2.1 (required minimal validation for HTTP/2, applied to HTTP/3
// as well): a field value MUST NOT start or end with SP or HTAB. Internal
// whitespace is part of the value; leading/trailing whitespace is not, and
// forwarding it to HTTP/1.1 lets different parsers disagree about the value.
inline bool has_leading_or_trailing_ws(std::string_view s) noexcept {
    if (s.empty())
        return false;
    const auto is_ws = [](char c) noexcept { return c == ' ' || c == '\t'; };
    return is_ws(s.front()) || is_ws(s.back());
}

// RFC 9110 §5.5: a field value is *( HTAB / SP / VCHAR / obs-text ). Other
// control characters, including DEL, cannot be serialized into an HTTP/1.1
// field line and are not something HPACK peers expect either.
inline bool contains_invalid_field_value(std::string_view s) noexcept {
    for (unsigned char c : s) {
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return true;
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

// Whether `s` is a non-empty run of tchars. Checked before a peer-supplied
// token (a preflight's Access-Control-Request-Method) is echoed into a response
// head, so what goes out is always a string that would re-parse as the same
// thing.
inline bool is_http_token(std::string_view s) noexcept {
    if (s.empty())
        return false;
    for (char c : s) {
        if (!is_token_char(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

// ASCII case folding and comparison. They live here, in the layer that depends
// on nothing but the standard library, because both the field-name layer and
// the content-encoding layer need them — and the alternative was a copy in
// each.
inline char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// A lowercased copy, for callers that need to keep the folded spelling (field
// token lists, header-name comparisons against a stored value).
inline std::string ascii_lower_copy(std::string_view s) {
    std::string out{s};
    for (auto &c : out)
        c = ascii_lower(c);
    return out;
}

// Value of one hexadecimal digit, or -1 when `c` is not one. Shared by the
// percent-decoders and by the chunked/HPACK scanners so every layer spells the
// same digit the same way.
inline int hex_digit_value(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
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

// ASCII case-insensitive substring test (allocation-free).
inline bool icontains_ci(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        std::size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (ascii_lower(haystack[i + j]) != ascii_lower(needle[j]))
                break;
        }
        if (j == needle.size())
            return true;
    }
    return false;
}

// Transparent string hashing, so a lookup by std::string_view never has to
// build a std::string key. Used by the router and the static-file table.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

namespace detail {

// Heterogeneous lookup for a std::unordered_map keyed by std::string. P0919
// (transparent unordered lookup) only exists in C++20; on the C++17 baseline
// the key has to be materialized, so that path pays one std::string
// construction per lookup. Centralised here so callers do not each grow an
// #if, and so the cost is visible in one place.
#if defined(__cpp_lib_generic_unordered_lookup) && __cpp_lib_generic_unordered_lookup >= 201911L
template <typename Map, typename Key> inline auto unordered_find(Map &map, const Key &key) { return map.find(key); }
#else
template <typename Map, typename Key> inline auto unordered_find(Map &map, const Key &key) {
    // Map & deduces to const Map for const lvalues, so one overload serves both.
    if constexpr (std::is_same_v<Key, typename Map::key_type>) {
        return map.find(key);
    } else {
        return map.find(typename Map::key_type(key));
    }
}
#endif

} // namespace detail

// The wire protocol a request/response is being served over.
enum class Version : std::uint8_t {
    Http1 = 0,  // HTTP/1.0
    Http11 = 1, // HTTP/1.1
    Http2 = 2,  // HTTP/2 (h2 / h2c)
    Http3 = 3,  // HTTP/3 (QUIC) — reserved; enabled via SIMPLE_HTTP_ENABLE_HTTP3
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
    More, // more body chunks will follow
    Last, // this is the last chunk; the response body is complete
};

// Outcome of reading from a request body stream.
enum class StreamStatus : std::int8_t {
    Ok,         // data read successfully, more may follow
    Eof,        // the peer finished the request body normally
    Rst,        // the stream was reset by the peer
    Disconnect, // the underlying connection was lost
};

// Tag types used as sentinel values on the body/stream channels.
struct Eof {};
struct Rst {};
struct Disconnect {};

} // namespace simple_http
