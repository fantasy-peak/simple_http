cmake_minimum_required(VERSION 3.14)
project(test)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(simple_http REQUIRED CONFIG PATHS ../build/./nqf/lib/cmake/simple_http)
find_package(Boost REQUIRED
             COMPONENTS beast asio)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)
find_package(OpenSSL REQUIRED)

# cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=on -DCMAKE_PREFIX_PATH=/root/boost;
add_executable(test main.cpp)
target_include_directories(test PRIVATE ${NGHTTP2_INCLUDE_DIRS})
target_link_libraries(test PRIVATE simple_http::simple_http Boost::asio Boost::beast ${NGHTTP2_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)
