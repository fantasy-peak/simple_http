#pragma once

// HTTP status reason phrases — a beast-free lookup used by the HTTP/1 response
// serializer to build the status line ("HTTP/1.1 <code> <reason>").

#include <string_view>

namespace simple_http {

inline constexpr std::string_view reason_phrase(int status) noexcept {
    switch (status) {
        case 100:
            return "Continue";
        case 101:
            return "Switching Protocols";
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 202:
            return "Accepted";
        case 204:
            return "No Content";
        case 206:
            return "Partial Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 303:
            return "See Other";
        case 304:
            return "Not Modified";
        case 307:
            return "Temporary Redirect";
        case 308:
            return "Permanent Redirect";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 406:
            return "Not Acceptable";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 410:
            return "Gone";
        case 411:
            return "Length Required";
        case 413:
            return "Payload Too Large";
        case 414:
            return "URI Too Long";
        case 415:
            return "Unsupported Media Type";
        case 416:
            return "Range Not Satisfiable";
        case 417:
            return "Expectation Failed";
        case 426:
            return "Upgrade Required";
        case 429:
            return "Too Many Requests";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        case 505:
            return "HTTP Version Not Supported";
        default:
            return "Unknown";
    }
}

// Common status codes, so a handler can write `status::not_found` rather than
// 404. Plain ints rather than an enum: the set of status codes is open (an
// application is free to invent 599), and Response::status() already takes an
// int, so these compose with literals instead of needing a cast.
namespace status {

inline constexpr int continue_ = 100;
inline constexpr int switching_protocols = 101;
inline constexpr int ok = 200;
inline constexpr int created = 201;
inline constexpr int accepted = 202;
inline constexpr int no_content = 204;
inline constexpr int partial_content = 206;
inline constexpr int moved_permanently = 301;
inline constexpr int found = 302;
inline constexpr int see_other = 303;
inline constexpr int not_modified = 304;
inline constexpr int temporary_redirect = 307;
inline constexpr int permanent_redirect = 308;
inline constexpr int bad_request = 400;
inline constexpr int unauthorized = 401;
inline constexpr int forbidden = 403;
inline constexpr int not_found = 404;
inline constexpr int method_not_allowed = 405;
inline constexpr int not_acceptable = 406;
inline constexpr int request_timeout = 408;
inline constexpr int conflict = 409;
inline constexpr int gone = 410;
inline constexpr int length_required = 411;
inline constexpr int payload_too_large = 413;
inline constexpr int uri_too_long = 414;
inline constexpr int unsupported_media_type = 415;
inline constexpr int range_not_satisfiable = 416;
inline constexpr int expectation_failed = 417;
inline constexpr int upgrade_required = 426;
inline constexpr int too_many_requests = 429;
inline constexpr int request_header_fields_too_large = 431;
inline constexpr int internal_server_error = 500;
inline constexpr int not_implemented = 501;
inline constexpr int bad_gateway = 502;
inline constexpr int service_unavailable = 503;
inline constexpr int gateway_timeout = 504;
inline constexpr int http_version_not_supported = 505;

}  // namespace status

}  // namespace simple_http
