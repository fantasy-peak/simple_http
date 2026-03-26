add_rules("mode.debug", "mode.release")

set_languages("c++23")

set_warnings("all", "error")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build", lsp="clangd" })

-- REPRODUCIBILITY --
set_policy("package.requires_lock", true)
set_policy("package.librarydeps.strict_compatibility", true)

-- PACKAGES --
add_requires("boost", {configs = {asio=true, regex=true}})
add_requires("nghttp2")
add_requires("openssl3")

set_policy("build.c++.modules", true)
set_policy("build.c++.modules.std", true)

add_cxflags("-fuse-ld=mold")
add_cxxflags("-stdlib=libc++")

target("server")
    set_kind("binary")
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized")
        end
    end)
    add_files("include/simple_http.cppm")
    add_defines("SIMPLE_HTTP_EXPERIMENT_WEBSOCKET", "SIMPLE_HTTP_USE_BOOST_REGEX", "SIMPLE_HTTP_EXPERIMENT_HTTP2CLIENT")
    add_files("test/server.cpp")
    add_packages(
        "boost",
        "nghttp2",
        "openssl3",
        {public = true}
    )
    set_rundir(".")
target_end()

target("client")
    set_kind("binary")
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized")
        end
    end)
    add_files("include/simple_http.cppm")
    add_defines("SIMPLE_HTTP_EXPERIMENT_HTTP2CLIENT", "SIMPLE_HTTP_EXPERIMENT_WEBSOCKET")
    add_files("test/client.cpp")
    add_packages(
        "boost",
        "nghttp2",
        "openssl3",
        {public = true}
    )
    set_rundir(".")
target_end()
