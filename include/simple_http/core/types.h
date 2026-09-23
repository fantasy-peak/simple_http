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
