#pragma once

// Cache validators and byte ranges: the two halves of RFC 9110 §13, which HTTP
// conditional and partial responses are built from.
//
// They travel together because they share one input. `etag_matches` serves both
// `If-None-Match` (was this representation already delivered?) and `If-Range`
// (is this resumption still describing the same bytes?), and `parse_range` is
// only ever reached from the same request that produced the validator being
// compared. Splitting them would put the two halves of one decision in two
// files.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace simple_http {

// nginx's shape: `"<mtime-hex>-<size-hex>"`, plus a representation suffix so the
// brotli, gzip and identity encodings of one file never share a validator —
// without it, a client that saw the gzip form would revalidate the identity one
// as unchanged.
//
// Strong (no W/) on purpose. `If-Range` only honours a strong validator, so a
// weak ETag would silently disable range resumption — the request would still
// succeed, just as a full 200 every time.
//
// Built from stat metadata rather than a content hash: that keeps startup free
// of file reads and lets a 304 be answered without opening anything. The cost is
// that a rewrite with the same size and mtime keeps the old validator, which is
// why the odd in-place-edit-same-second case cannot be detected.
inline std::string make_etag(std::int64_t mtime_unix, std::uint64_t size, std::string_view suffix) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"%llx-%llx", static_cast<unsigned long long>(mtime_unix),
                  static_cast<unsigned long long>(size));
    std::string out{buf};
    if (!suffix.empty()) {
        out.push_back('-');
        out += suffix;
    }
    out.push_back('"');
    return out;
}

// Weak comparison (RFC 9110 §8.8.3.2): the W/ prefix is ignored, because two
// semantically equivalent representations may carry different validator forms.
// Accepts a comma-separated list and the "*" wildcard.
inline bool etag_matches(std::string_view header_value, std::string_view etag) {
    auto strip = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
        if (v.starts_with("W/")) v.remove_prefix(2);
        return v;
    };
    std::size_t pos = 0;
    while (pos <= header_value.size()) {
        auto comma = header_value.find(',', pos);
        auto item =
            header_value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        auto trimmed = strip(item);
        if (trimmed == "*") return true;
        if (!trimmed.empty() && trimmed == strip(etag)) return true;
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return false;
}

// An inclusive byte range.
struct ByteRange {
    std::uint64_t first{0};
    std::uint64_t last{0};
    std::uint64_t length() const { return last - first + 1; }
};

// Parses a single-range `bytes=` header against a representation of `size`.
//
// Multi-range and non-bytes units report "no range" (nullopt with
// `unsatisfiable` left false), and the caller falls back to a full 200 — which
// RFC 9110 explicitly permits, and which avoids `multipart/byteranges`, a
// response shape that is framed differently on HTTP/1.1 and HTTP/2 for no gain.
//
// A syntactically valid range lying entirely past the end sets `unsatisfiable`
// (a 416), which is a different answer from "could not parse this".
inline std::optional<ByteRange> parse_range(std::string_view header, std::uint64_t size, bool& unsatisfiable) {
    unsatisfiable = false;
    constexpr std::string_view kPrefix = "bytes=";
    if (!header.starts_with(kPrefix)) return std::nullopt;
    std::string_view spec = header.substr(kPrefix.size());

    while (!spec.empty() && (spec.front() == ' ' || spec.front() == '\t')) spec.remove_prefix(1);
    while (!spec.empty() && (spec.back() == ' ' || spec.back() == '\t')) spec.remove_suffix(1);

    if (spec.find(',') != std::string_view::npos) return std::nullopt;  // multi-range: not supported

    auto dash = spec.find('-');
    if (dash == std::string_view::npos) return std::nullopt;

    auto to_u64 = [](std::string_view v) -> std::optional<std::uint64_t> {
        if (v.empty() || v.size() > 20) return std::nullopt;
        std::uint64_t out = 0;
        for (char c : v) {
            if (c < '0' || c > '9') return std::nullopt;
            out = out * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return out;
    };

    std::string_view first_s = spec.substr(0, dash);
    std::string_view last_s = spec.substr(dash + 1);

    if (first_s.empty()) {
        // "-N": the last N bytes.
        auto n = to_u64(last_s);
        if (!n || *n == 0) return std::nullopt;
        if (size == 0) {
            unsatisfiable = true;
            return std::nullopt;
        }
        const std::uint64_t take = std::min<std::uint64_t>(*n, size);
        return ByteRange{size - take, size - 1};
    }

    auto first = to_u64(first_s);
    if (!first) return std::nullopt;
    if (*first >= size) {
        unsatisfiable = true;
        return std::nullopt;
    }
    std::uint64_t last = size - 1;
    if (!last_s.empty()) {
        auto l = to_u64(last_s);
        if (!l) return std::nullopt;
        if (*l < *first) return std::nullopt;  // syntactically invalid: ignore
        last = std::min<std::uint64_t>(*l, size - 1);
    }
    return ByteRange{*first, last};
}

}  // namespace simple_http
