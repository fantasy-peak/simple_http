#pragma once

// Common header field names, so a handler can write `field::content_type`
// instead of a string literal.
//
// Plain string_views rather than an enum, for the same reason the status codes
// are plain ints: the set is open — any field may appear on the wire, and
// registered names keep being added — so an enum would only ever be a partial
// list that forces a cast at every edge. These mix with literals freely.

#include <string_view>

#include "types.h" // iequals_ci

namespace simple_http {

namespace field {

inline constexpr std::string_view accept = "accept";
inline constexpr std::string_view accept_encoding = "accept-encoding";
inline constexpr std::string_view accept_language = "accept-language";
inline constexpr std::string_view accept_ranges = "accept-ranges";
inline constexpr std::string_view access_control_allow_credentials = "access-control-allow-credentials";
inline constexpr std::string_view access_control_allow_headers = "access-control-allow-headers";
inline constexpr std::string_view access_control_allow_methods = "access-control-allow-methods";
inline constexpr std::string_view access_control_allow_origin = "access-control-allow-origin";
inline constexpr std::string_view access_control_expose_headers = "access-control-expose-headers";
inline constexpr std::string_view access_control_max_age = "access-control-max-age";
inline constexpr std::string_view access_control_request_headers = "access-control-request-headers";
inline constexpr std::string_view access_control_request_method = "access-control-request-method";
inline constexpr std::string_view age = "age";
inline constexpr std::string_view allow = "allow";
inline constexpr std::string_view authorization = "authorization";
inline constexpr std::string_view cache_control = "cache-control";
inline constexpr std::string_view connection = "connection";
inline constexpr std::string_view content_disposition = "content-disposition";
inline constexpr std::string_view content_encoding = "content-encoding";
inline constexpr std::string_view content_language = "content-language";
inline constexpr std::string_view content_length = "content-length";
inline constexpr std::string_view content_range = "content-range";
inline constexpr std::string_view content_security_policy = "content-security-policy";
inline constexpr std::string_view content_type = "content-type";
inline constexpr std::string_view cookie = "cookie";
inline constexpr std::string_view cross_origin_opener_policy = "cross-origin-opener-policy";
inline constexpr std::string_view cross_origin_resource_policy = "cross-origin-resource-policy";
inline constexpr std::string_view date = "date";
inline constexpr std::string_view etag = "etag";
inline constexpr std::string_view expect = "expect";
inline constexpr std::string_view expires = "expires";
inline constexpr std::string_view host = "host";
inline constexpr std::string_view if_match = "if-match";
inline constexpr std::string_view if_modified_since = "if-modified-since";
inline constexpr std::string_view if_none_match = "if-none-match";
inline constexpr std::string_view if_range = "if-range";
inline constexpr std::string_view if_unmodified_since = "if-unmodified-since";
inline constexpr std::string_view last_modified = "last-modified";
inline constexpr std::string_view link = "link";
inline constexpr std::string_view location = "location";
inline constexpr std::string_view origin = "origin";
inline constexpr std::string_view permissions_policy = "permissions-policy";
inline constexpr std::string_view range = "range";
inline constexpr std::string_view referer = "referer";
inline constexpr std::string_view referrer_policy = "referrer-policy";
inline constexpr std::string_view retry_after = "retry-after";
inline constexpr std::string_view server = "server";
inline constexpr std::string_view set_cookie = "set-cookie";
inline constexpr std::string_view strict_transport_security = "strict-transport-security";
inline constexpr std::string_view transfer_encoding = "transfer-encoding";
inline constexpr std::string_view upgrade = "upgrade";
inline constexpr std::string_view user_agent = "user-agent";
inline constexpr std::string_view vary = "vary";
inline constexpr std::string_view via = "via";
inline constexpr std::string_view www_authenticate = "www-authenticate";
inline constexpr std::string_view x_content_type_options = "x-content-type-options";
inline constexpr std::string_view x_frame_options = "x-frame-options";
inline constexpr std::string_view x_request_id = "x-request-id";
inline constexpr std::string_view x_xss_protection = "x-xss-protection";

} // namespace field

// RFC 9110 §5.6.3: optional whitespace (SP / HTAB) around a field value or a
// list element. Every parser and header utility trims the same two bytes.
inline std::string_view trim_ows(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

// Whether a comma-separated field value contains `token`. The comparison is
// case-insensitive and each element is OWS-trimmed: `Connection`, `Upgrade`,
// `Vary` and `Cache-Control` are token lists, where a substring test would make
// "not-close" close a connection.
// Compatibility aliases for the pre-consolidation spellings. New code should
// use trim_ows / value_has_token; these keep downstream sources compiling after
// the helpers moved here.
inline std::string_view trim_ascii(std::string_view s) noexcept { return trim_ows(s); }

inline bool value_has_token(std::string_view value, std::string_view token) noexcept {
    std::size_t pos = 0;
    for (;;) {
        const std::size_t comma = value.find(',', pos);
        const std::string_view item =
            trim_ows(value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        if (iequals_ci(item, token))
            return true;
        if (comma == std::string_view::npos)
            return false;
        pos = comma + 1;
    }
}

// Compatibility alias for the pre-consolidation two-argument spelling that
// content_encoding.h exposed. proto/headers.h keeps the `Headers`-based overload
// of the same name, so both call shapes resolve.
inline bool header_has_token(std::string_view value, std::string_view token) noexcept {
    return value_has_token(value, token);
}

// Hop-by-hop fields that must not be forwarded (RFC 9110 §7.6.1), plus the
// h2c upgrade's `http2-settings` (RFC 7540 §3.2.2.2). Field names are expected
// lowercased, which is how Headers stores them.
[[nodiscard]] inline bool is_hop_by_hop_field(std::string_view name) noexcept {
    static constexpr std::string_view kHop[] = {
        "connection",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "proxy-connection",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade",
        "http2-settings",
    };
    for (auto h : kHop) {
        if (name == h)
            return true;
    }
    return false;
}

// HTTP/1.1 connection-specific fields, prohibited in HTTP/2 (RFC 9113 §8.2.2)
// and HTTP/3 (RFC 9114 §4.2). Kept separate from is_hop_by_hop_field: this is
// the narrower validator set, and it excludes proxy-authenticate/trailer.
[[nodiscard]] inline bool is_connection_specific_field(std::string_view name) noexcept {
    return name == field::connection || name == "keep-alive" || name == "proxy-connection" ||
           name == field::transfer_encoding || name == field::upgrade;
}

// Whether a field must be kept out of a trailer section (RFC 9110 §6.5.1): a
// pseudo-header, a field needed for framing (Content-Length, Transfer-Encoding)
// or routing (Host), or one of the hop-by-hop fields (RFC 9110 §7.6.1). Every
// engine's send_trailers() filters through this so the same field is refused on
// every version.
[[nodiscard]] inline bool is_forbidden_trailer_field(std::string_view name) {
    if (name.empty() || name.front() == ':')
        return true;
    return name == field::content_length || name == field::host || name == field::connection ||
           name == field::transfer_encoding || name == "keep-alive" || name == field::upgrade ||
           name == "proxy-connection" || name == "te" || name == "trailer";
}

} // namespace simple_http
