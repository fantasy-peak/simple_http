-- add_rules("mode.debug", "mode.release")

set_languages("c++26")

-- set_warnings("all", "error")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build", lsp="clangd" })

-- REPRODUCIBILITY --
set_policy("package.requires_lock", true)
set_policy("package.librarydeps.strict_compatibility", true)

-- PACKAGES --
-- HTTP/3 的 QUIC 与帧层来自 ngtcp2 + nghttp3（都是 C 库，只有 h3 路径用）。
-- 上游 xmake-repo 的 ngtcp2 包是 `-DENABLE_OPENSSL=OFF` 构建的，不产 crypto
-- helper——没有它就没有 TLS，QUIC 无从谈起。私有仓库里那份把它改成了
-- `-DENABLE_OPENSSL=ON`（openssl4 >= 3.5 才有上游 CMake 探测的
-- SSL_set_quic_tls_cbs，从而构建 libngtcp2_crypto_ossl），nghttp3 那份也在。
-- 注意：这里不能用 /opt/h3/lib 的预编译库——那是对系统 OpenSSL 3.5.5 编的，
-- 而本仓库用的是 openssl4 3.6.3，混链是 ABI 风险。让 xmake 从源码构建。
add_repositories("my_private_repo https://github.com/fantasy-peak/xmake-repo.git")
add_requires("boost", {configs = {cmake = true, asio=true, regex=true}})
add_requires("openssl4", {system = false})
add_requires("ngtcp2", "nghttp3")
add_requires("catch2")  -- Catch2 unit/regression suites
-- glaze is an unconditional dependency: OpenAPI schemas and the JSON helpers
-- (proto/json.h, read_json_body / write_json). Header-only, no transitive deps.
add_requires("glaze")
-- Response-body compression (core/content_encoding.h) and WebSocket
-- permessage-deflate (proto/ws_deflate.h) are both compiled in unconditionally:
-- zlib and brotli are normal dependencies of the library now. Whether either
-- takes effect is a runtime setting (EngineLimits::compression / ws_compression,
-- ClientConfig::auto_decompress).
add_requires("zlib", "brotli")

add_cxflags("-O2 -Wextra -Wno-missing-field-initializers -Wno-ignored-qualifiers")
add_defines("SIMPLE_HTTP_USE_BOOST_REGEX")

target("simple_http")
    set_kind("static")
    add_includedirs("include", { public = true })
    add_packages(
        "ngtcp2",
        "nghttp3",
        "boost",
        "openssl4",
        "glaze",
        -- Compression codecs are compiled in unconditionally, so zlib and
        -- brotli are normal public dependencies alongside boost.asio and
        -- OpenSSL. (HTTP/3, OpenAPI and the logging macro stay opt-in.)
        "zlib",
        "brotli",
        {public = true}
    )
target_end()

-- TEST TARGETS --
-- Three roles:
--   test_*      assertion programs (Catch2 unit suite)
--   fixture_*   servers an external suite drives from the outside
--   diff_*      differential drivers / servers (C++, plus Go built by run.py)
--
-- test/run.py is the only orchestration entry point; xmake only builds these
-- binaries. Every diff group is a separate executable: a failure keeps the
-- blast radius small and running one group never builds the others.
local test_cxxflags = {
    "-Wno-maybe-uninitialized",
    "-Wno-mismatched-new-delete",
    "-Wno-type-limits",
}

local function add_test_binary(name, files, opts)
    opts = opts or {}
    target(name)
        set_kind("binary")
        set_default(false)
        set_group(opts.group or "test/other")
        add_deps("simple_http")
        add_files(files)
        if opts.packages then
            for _, package in ipairs(opts.packages) do
                add_packages(package)
            end
        end
        if opts.defines then
            for _, define in ipairs(opts.defines) do
                add_defines(define)
            end
        end
        if opts.http3 then
            add_defines("SIMPLE_HTTP_ENABLE_HTTP3")
        end
        on_load(function (target)
            if target:toolchain("gcc") then
                target:add("cxxflags", test_cxxflags)
            end
        end)
        -- The TLS certificates are referenced relative to the repository root
        -- (./test/tls_certificates/...), so every test binary runs there.
        set_rundir(".")
    target_end()
end

-- Unit ----------------------------------------------------------------------
-- The Catch2 entry point lives in test/support so a glob over test/unit can
-- never pick up a second main().
add_test_binary("test_unit", {
    "test/unit/test_*.cpp",
    "test/support/catch_main.cpp",
}, {
    group = "test/unit",
    http3 = true,
    packages = {"catch2", "glaze", "zlib", "brotli"},
})

-- Conformance / stress fixture ---------------------------------------------
-- Deliberately tiny: exactly the four routes the external suites need
-- (/, /world, ws /echo, ws /echo1k) on the fixed ports 7788/7790/7791/7792.
-- Everything else lives in the differential servers.
add_test_binary("fixture_conformance_server", "test/fixtures/conformance_server.cpp", {
    group = "test/fixtures",
    http3 = true,
    packages = {"zlib", "brotli"},
})

-- Client differential -------------------------------------------------------
-- One binary per feature group. They share a source file and differ only in
-- the group define; the case data is read from test/diff/client/cases/*.json.
for _, group in ipairs({"basic", "body", "policy", "ws", "h3"}) do
    add_test_binary("diff_client_" .. group, "test/diff/client/cpp/driver.cpp", {
        group = "test/diff/client",
        http3 = group == "h3",
        packages = {"zlib", "brotli"},
        defines = {"DIFF_GROUP_" .. group:upper()},
    })
end

-- Server differential -------------------------------------------------------
-- One C++ server implements the whole shared route table; the Go reference
-- server and the Go probes are built by test/diff/server/run.py.
add_test_binary("diff_server_cpp", "test/diff/server/cpp/server.cpp", {
    group = "test/diff/server",
    http3 = true,
    packages = {"zlib", "brotli"},
})

-- OpenAPI -------------------------------------------------------------------
add_test_binary("diff_openapi_server", "test/diff/openapi/demo_server.cpp", {
    group = "test/diff/openapi",
    packages = {"glaze"},
})
