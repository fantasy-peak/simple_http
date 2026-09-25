cmake_minimum_required(VERSION 3.14)
project(simple_http_header_only VERSION 1.0 LANGUAGES CXX)

add_library(simple_http INTERFACE)

# Response-body compression is opt-in, and so is its dependency: this is the
# only thing that pulls in zlib and brotli. Off by default, so a consumer that
# does not want compression needs neither the packages nor the define.
option(SIMPLE_HTTP_WITH_COMPRESSION "Compile in gzip/brotli response compression" OFF)
if(SIMPLE_HTTP_WITH_COMPRESSION)
    find_package(ZLIB REQUIRED)
    find_package(Brotli REQUIRED)
    target_link_libraries(simple_http INTERFACE ZLIB::ZLIB brotli::brotlienc brotli::brotlidec)
    target_compile_definitions(simple_http INTERFACE SIMPLE_HTTP_ENABLE_COMPRESSION)
endif()

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
