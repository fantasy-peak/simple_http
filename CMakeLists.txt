cmake_minimum_required(VERSION 3.20)  # cxx_std_23 needs 3.20

# Single source of truth for the version: it is read out of the header the
# library reports at runtime, so the package version and the runtime version
# cannot drift apart (they had — CMake said 1.0 while version.h said 0.7.0).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/simple_http/core/version.h" _version_defs
     REGEX "^#define SIMPLE_HTTP_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_def ${_version_defs})
    if(_def MATCHES "SIMPLE_HTTP_VERSION_MAJOR ([0-9]+)")
        set(_major ${CMAKE_MATCH_1})
    elseif(_def MATCHES "SIMPLE_HTTP_VERSION_MINOR ([0-9]+)")
        set(_minor ${CMAKE_MATCH_1})
    elseif(_def MATCHES "SIMPLE_HTTP_VERSION_PATCH ([0-9]+)")
        set(_patch ${CMAKE_MATCH_1})
    endif()
endforeach()
set(SIMPLE_HTTP_VERSION "${_major}.${_minor}.${_patch}")

project(simple_http VERSION ${SIMPLE_HTTP_VERSION} LANGUAGES CXX)

# The library's own dependencies, carried on the INTERFACE so a consumer gets
# them by linking the target rather than by reading the README.
#
# Boost in CONFIG mode: CMake 4 removed the FindBoost module, so Boost has to
# supply its own BoostConfig.cmake — which every release since 1.70 does.
find_package(Boost REQUIRED CONFIG)
find_package(OpenSSL REQUIRED)

# glaze is an unconditional dependency: it backs the JSON helpers
# (proto/json.h, read_json_body / write_json). The OpenAPI layer stays opt-in.
find_package(glaze CONFIG REQUIRED)

# Compression codecs are compiled into the library unconditionally, so zlib and
# brotli are normal dependencies (like Boost and OpenSSL). Whether compression
# is used is a runtime setting on the server (EngineLimits::compression,
# EngineLimits::ws_compression) and the client (ClientConfig::auto_decompress).
find_package(ZLIB REQUIRED)
find_package(Brotli REQUIRED)

add_library(simple_http INTERFACE)

target_include_directories(simple_http INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# The library requires C++23. Stating it here turns "consumer sets C++20" into a
# configure-time error instead of a screen of parse errors inside the headers.
target_compile_features(simple_http INTERFACE cxx_std_23)

target_link_libraries(simple_http INTERFACE
    Boost::boost
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    brotli::brotlienc
    brotli::brotlidec
    glaze::glaze
)

install(DIRECTORY include/ DESTINATION include)

install(TARGETS simple_http
    EXPORT simple_httpTargets
)

install(EXPORT simple_httpTargets
    FILE simple_httpTargets.cmake
    NAMESPACE simple_http::
    DESTINATION lib/cmake/simple_http
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/simple_httpConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/simple_httpConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/simple_httpConfig.cmake"
    INSTALL_DESTINATION lib/cmake/simple_http
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/simple_httpConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/simple_httpConfigVersion.cmake"
    DESTINATION lib/cmake/simple_http
)
