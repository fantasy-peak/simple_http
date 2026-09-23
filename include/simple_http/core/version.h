#pragma once

// Library version constants and the User-Agent / Server product strings.

#include <string_view>

#define SIMPLE_HTTP_VERSION_MAJOR 0
#define SIMPLE_HTTP_VERSION_MINOR 7
#define SIMPLE_HTTP_VERSION_PATCH 0

#define SIMPLE_HTTP_STR_HELPER(x) #x
#define SIMPLE_HTTP_STR(x) SIMPLE_HTTP_STR_HELPER(x)

#define SIMPLE_HTTP_VERSION_STR                \
    SIMPLE_HTTP_STR(SIMPLE_HTTP_VERSION_MAJOR) \
    "." SIMPLE_HTTP_STR(SIMPLE_HTTP_VERSION_MINOR) "." SIMPLE_HTTP_STR(SIMPLE_HTTP_VERSION_PATCH)

#define SIMPLE_HTTP_VERSION_CODE \
    ((SIMPLE_HTTP_VERSION_MAJOR) * 10000 + (SIMPLE_HTTP_VERSION_MINOR) * 100 + (SIMPLE_HTTP_VERSION_PATCH))

namespace simple_http {

inline constexpr int version_major = SIMPLE_HTTP_VERSION_MAJOR;
inline constexpr int version_minor = SIMPLE_HTTP_VERSION_MINOR;
inline constexpr int version_patch = SIMPLE_HTTP_VERSION_PATCH;

inline constexpr std::string_view server_version = "simple_http_server/" SIMPLE_HTTP_VERSION_STR;
inline constexpr std::string_view client_version = "simple_http_client/" SIMPLE_HTTP_VERSION_STR;

}  // namespace simple_http
