add_rules("mode.debug", "mode.release")

set_languages("c++23")

set_warnings("all", "error")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build", lsp="clangd" })

-- REPRODUCIBILITY --
set_policy("package.requires_lock", true)
set_policy("package.librarydeps.strict_compatibility", true)

-- PACKAGES --
add_requires("boost", {configs = {asio=true, regex=true}})
add_requires("openssl3")
add_requires("catch2")  -- unit tests only (target `unittest`)

add_defines("SIMPLE_HTTP_EXPERIMENT_WEBSOCKET", "SIMPLE_HTTP_USE_BOOST_REGEX")

target("simple_http")
    set_kind("static")
    add_includedirs("include", { public = true })
    add_packages(
        "boost",
        "openssl3",
        {public = true}
    )
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

-- The client layer's exercise program: starts a server in the same process and
-- drives the client at it (and at a raw responder) across the protocol matrix.
-- Run it from the repository root: `xmake run client`.
target("client")
    set_kind("binary")
    on_load(function (target)
        if target:toolchain("gcc") then
            -- GCC false positives around asio's coroutine frames; the server
            -- target needs the first one too.
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-mismatched-new-delete")
        end
    end)
    add_deps("simple_http")
    add_files("test/client.cpp")
    set_rundir(".")
target_end()

-- Unit tests (Catch2): pure logic, no sockets — parsers, HPACK, frames, the
-- URL/config helpers, routing. Fast enough to run on every change:
--   xmake build unittest && xmake run unittest
target("unittest")
    set_kind("binary")
    set_default(false)
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-mismatched-new-delete")
        end
    end)
    add_deps("simple_http")
    add_files("test/unit/*.cpp")
    add_packages("catch2")
    set_rundir(".")
target_end()

-- Server regression suite: raw sockets against an in-process server, sending the
-- malformed and boundary cases a well-behaved client will not send.
--   xmake build regression && xmake run regression
target("regression")
    set_kind("binary")
    set_default(false)
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-mismatched-new-delete")
        end
    end)
    add_deps("simple_http")
    add_files("test/server_regression.cpp", "test/unit/main.cpp")
    add_packages("catch2")
    set_rundir(".")
target_end()


