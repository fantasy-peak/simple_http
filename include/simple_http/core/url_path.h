#pragma once

// Request-target path decoding and normalization (RFC 9110 §4.1 origin-form,
// RFC 3986 §3.3), plus the segment-aware prefix test that goes with the result.
//
// The point of this file is the *shape* of what it returns: a canonical lookup
// key, a pure string, never a filesystem path. A caller can compare it against a
// precomputed table of known names, and a request whose path normalizes to
// somewhere outside that table is simply "not found" — never a stat() on a
// location the peer chose. That is what makes directory traversal, symlink
// escape and TOCTOU structural impossibilities for the layers above rather than
// checks they have to remember to perform.
//
// Alternatives this deliberately rejects:
//   * sanitize-then-stat — still stats an attacker-chosen path, and leaves a
//     window between the check and the use;
//   * concatenate-then-contain — the `%2e%2e%2f` family is built to defeat it,
//     which is why an escaped separator is refused here outright rather than
//     caught later;
//   * full RFC 3986 dot-segment removal — safe only if the concatenation it
//     feeds is safe too; refusing ".." has a much smaller proof surface.
//
// under_prefix() lives here rather than with the callers because it is half of
// the same contract: it is only correct against keys this file produced. A plain
// starts_with would let "/apixyz" match a reserved "/api".

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace simple_http {

// Why a path was refused. Callers log it and pick a status code; nothing
// branches on it beyond that.
enum class PathError {
    None,
    Malformed,  // not an origin-form path, a control byte, or a bad escape
    Traversal,  // a ".." segment survived decoding
    TooLong,    // length/segment-count bound exceeded
};

// Bounds, deliberately independent of EngineLimits::max_header_bytes: on HTTP/2
// the `:path` pseudo-header arrives HPACK-decoded and is not subject to the
// header-size budget at all, so a limit expressed only in terms of the header
// block would not bound it.
inline constexpr std::size_t kMaxPathBytes = 2048;
// The filesystem's own per-component limit, so a name that cannot exist on disk
// is rejected before it becomes a table lookup that cannot succeed.
inline constexpr std::size_t kMaxSegmentBytes = 255;
inline constexpr std::size_t kMaxSegments = 32;

namespace detail {

inline int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace detail

// Decodes a request path and reduces it to a canonical lookup key.
//
// The result always starts with '/', never ends with '/' except for the root
// itself, and carries no empty, "." or ".." segments. Bytes >= 0x80 are let
// through unexamined: UTF-8 path components are legitimate, and because the
// result is compared against a table of real names, an overlong-UTF-8 encoding
// of ".." is just a key that does not exist.
//
// The rejection list is what makes that argument hold: a percent-encoded
// separator is refused outright, so no escape can create a segment boundary
// after the ".." test has already been applied to the raw text.
inline std::optional<std::string> decode_and_normalize(std::string_view raw, PathError& err) {
    err = PathError::None;

    if (raw.empty() || raw.front() != '/') {
        err = PathError::Malformed;  // also rejects absolute-form "GET http://host/x"
        return std::nullopt;
    }
    if (raw.size() > kMaxPathBytes) {
        err = PathError::TooLong;
        return std::nullopt;
    }

    std::vector<std::string> segments;
    std::string cur;
    cur.reserve(64);

    auto push_segment = [&]() -> bool {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return true;  // collapse "//" and "/./"
        }
        if (cur == "..") {
            err = PathError::Traversal;
            return false;
        }
        if (cur.size() > kMaxSegmentBytes || segments.size() >= kMaxSegments) {
            err = PathError::TooLong;
            return false;
        }
        segments.push_back(cur);
        cur.clear();
        return true;
    };

    for (std::size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);

        // Space and every control byte, plus DEL, must arrive escaped if they are
        // meant literally.
        if (c <= 0x20 || c == 0x7F) {
            err = PathError::Malformed;
            return std::nullopt;
        }
        if (c == '\\') {
            err = PathError::Malformed;  // backslash confusion; never a separator here
            return std::nullopt;
        }

        if (c != '%') {
            if (c == '/') {
                if (!push_segment()) return std::nullopt;
            } else {
                cur.push_back(static_cast<char>(c));
            }
            continue;
        }

        if (i + 2 >= raw.size()) {
            err = PathError::Malformed;
            return std::nullopt;
        }
        const int hi = detail::hex_digit(raw[i + 1]);
        const int lo = detail::hex_digit(raw[i + 2]);
        if (hi < 0 || lo < 0) {
            err = PathError::Malformed;
            return std::nullopt;
        }
        i += 2;
        const unsigned char d = static_cast<unsigned char>((hi << 4) | lo);

        // An escaped separator would let one segment become two *after* the ".."
        // check has already run on the raw text, which is exactly how
        // `%2e%2e%2f` sneaks past a naive filter. Refuse it: real static sites
        // never need an escaped slash in a path.
        if (d == '/' || d == '\\' || d == 0x00 || d < 0x20 || d == 0x7F) {
            err = PathError::Malformed;
            return std::nullopt;
        }
        cur.push_back(static_cast<char>(d));
    }
    if (!push_segment()) return std::nullopt;

    std::string out{"/"};
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) out.push_back('/');
        out += segments[i];
    }
    return out;
}

// Whether `path` lies at or under `prefix`, comparing whole segments.
//
// A plain starts_with would let "/apixyz" match a reserved "/api"; requiring a
// '/' (or exact equality) does not. A prefix that already ends in '/' is a
// complete boundary on its own, so anything continuing it is inside — without
// that case "/assets/" would never match "/assets/app.js", whose next character
// is 'a' rather than '/'.
//
// Both operands are expected to be keys from decode_and_normalize; a trailing
// slash on `path` would not be one.
inline bool under_prefix(std::string_view path, std::string_view prefix) noexcept {
    if (prefix.empty()) return false;
    if (path == prefix) return true;
    if (path.size() <= prefix.size()) return false;
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    if (prefix.back() == '/') return true;
    return path[prefix.size()] == '/';
}

}  // namespace simple_http
