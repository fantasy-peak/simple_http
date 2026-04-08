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
add_requires("catch2 v3.13.0", {configs = {with_main = true}})

add_defines("SIMPLE_HTTP_EXPERIMENT_WEBSOCKET", "SIMPLE_HTTP_USE_BOOST_REGEX", "SIMPLE_HTTP_EXPERIMENT_HTTP2CLIENT")

target("simple_http")
    set_kind("static")
    add_includedirs("include", { public = true })
    add_packages(
        "boost",
        "nghttp2",
        "openssl3",
        {public = true}
    )
target_end()

target("unit_tests")
    set_kind("binary")
    add_deps("simple_http")
    add_packages("catch2")
    add_files("test/unit_tests.cpp")
    set_rundir(".")
target_end()

target("server")
    set_kind("binary")
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized")
        end
        -- xmake f --toolchain=llvm --runtimes=c++_static -c -v
        if target:toolchain("llvm") then
            target:add("files", "include/simple_http.cppm", {public = true})
            target:add("defines", "SIMPLE_HTTP_USE_MODULES")
            target:add("cxflags", "-fuse-ld=mold", "-stdlib=libc++")
            target:add("ldflags", "-fuse-ld=mold", "-stdlib=libc++")
            target:set("policy", "build.c++.modules", true)
            target:set("policy", "build.c++.modules.std", true)
        end
    end)
    add_deps("simple_http")
    add_files("test/server.cpp")
    set_rundir(".")
target_end()

target("client")
    set_kind("binary")
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized")
        end
    end)
    add_deps("simple_http")
    add_files("test/client.cpp")
    set_rundir(".")
target_end()
