cmake_minimum_required(VERSION 3.14)
project(simple_http_header_only VERSION 1.0 LANGUAGES CXX)

add_library(simple_http INTERFACE)

target_include_directories(simple_http INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
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
