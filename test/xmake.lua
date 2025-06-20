add_rules("mode.release", "mode.debug")

set_project("test")
set_version("1.0.0", {build = "%Y%m%d%H%M"})
set_xmakever("2.9.9")

add_repositories("my_private_repo https://github.com/fantasy-peak/xmake-repo.git")

add_requires("asio asio-1-34-2")
add_requires("spdlog", {configs={std_format=true}})
add_requires("nlohmann_json", "openssl")
add_requires("boost", {configs={cmake=false, url=true}})

set_pcheader("include/simple_http.h")

set_languages("c++23")
add_includedirs("../include")

-- add_cxflags("-O2 -Wall -g -fno-omit-frame-pointer -fsanitize=address -Wextra -pedantic-errors -Wno-missing-field-initializers -Wno-ignored-qualifiers")
-- add_cxflags("-O2 -Wall -g -Wextra -pedantic-errors -Wno-missing-field-initializers -Wno-ignored-qualifiers")

target("server")
    set_kind("binary")
    add_files("server.cpp")
    add_packages("nlohmann_json", "spdlog", "asio", "openssl", "boost")
    -- add_syslinks("pthread", "asan")
    add_ldflags("-static-libstdc++", "-static-libgcc", {force = true})
target_end()

-- target("client")
--     set_kind("binary")
--     add_files("client.cpp")
--     add_packages("nlohmann_json", "spdlog", "asio", "gzip-hpp", "openssl", "nghttp2", "boost")
--     -- add_syslinks("pthread", "asan")
-- target_end()