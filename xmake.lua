add_rules("mode.debug", "mode.release")

set_languages("c++23")
set_toolchains("gcc")

-- TEMP: false positives?
add_cxxflags("-Wno-maybe-uninitialized")
set_warnings("all", "error")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build", lsp="clangd" })

-- REPRODUCIBILITY --
set_policy("package.requires_lock", true)
set_policy("package.librarydeps.strict_compatibility", true)

-- PACKAGES --
add_requires("boost", {configs = {asio=true}})
add_requires("nghttp2")
add_requires("openssl3")

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

target("server")
    set_kind("binary")
    add_deps("simple_http")
    add_files("test/server.cpp")
    set_rundir(".")
target_end()

target("client")
    set_kind("binary")
    add_deps("simple_http")
    add_defines("_EXPERIMENT_HTTP_CLIENT_")
    add_files("test/client.cpp")
    set_rundir(".")
target_end()
