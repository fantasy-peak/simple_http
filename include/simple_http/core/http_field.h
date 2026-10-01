#pragma once

// Common header field names, so a handler can write `field::content_type`
// instead of a string literal.
//
// Plain string_views rather than an enum, for the same reason the status codes
// are plain ints: the set is open — any field may appear on the wire, and
// registered names keep being added — so an enum would only ever be a partial
// list that forces a cast at every edge. These mix with literals freely.

#include <string_view>

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

} // namespace simple_http
