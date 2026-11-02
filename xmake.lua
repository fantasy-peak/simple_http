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
-- Everything under test/ follows four role prefixes:
--   test_*    programs that assert behaviour (unit, regression, integration)
--   fixture_* servers a suite drives from the outside
--   probe_*   clients that talk to an independent peer, printing PASS/FAIL
--   tool_*    manual utilities (load profiling, public smoke tests, harnesses)
--   example_* documentation samples that must keep compiling
--
-- test/run.py is the one orchestration entry point; xmake only builds these
-- binaries and runs them directly when a developer asks for a single target.
-- Common GCC warning suppressions live in one helper instead of 23 copies.
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

-- Fixture servers -----------------------------------------------------------
-- The general-purpose fixture: plaintext 7788 (h1/h2c sniffing), TLS/mTLS 7789,
-- h2c-only 7790, h1-only 7791, TCP+UDP 7792, large-window h2 7793, h1 7794.
add_test_binary("fixture_server", "test/server/fixtures/server.cpp", {
    group = "test/fixtures",
    http3 = true,
    packages = {"zlib", "brotli"},
})

-- Built-in middleware + compression policy fixture (127.0.0.1:7796, TLS :7797).
add_test_binary("fixture_middleware_server", "test/server/fixtures/middleware_server.cpp", {
    group = "test/fixtures",
    packages = {"zlib", "brotli"},
})

-- Framework-layer reference server for the Go differential (default :7798).
add_test_binary("fixture_framework_server", "test/server/fixtures/framework_server.cpp", {
    group = "test/fixtures",
    packages = {"zlib", "brotli"},
})

-- Unit and in-process integration ------------------------------------------
-- The Catch2 entry point lives in test/support so both suites share it, and so
-- a glob over test/unit can never pick up a second main().
add_test_binary("test_unit", {
    "test/unit/test_*.cpp",
    "test/support/catch_main.cpp",
}, {
    group = "test/unit",
    http3 = true,
    packages = {"catch2", "glaze", "zlib", "brotli"},
})

-- Raw-socket regression suite: malformed and boundary requests against an
-- in-process server (Catch2).
add_test_binary("test_server_regression", {
    "test/integration/server_regression.cpp",
    "test/support/catch_main.cpp",
}, {
    group = "test/integration",
    http3 = true,
    packages = {"catch2", "zlib", "brotli"},
})

-- Client-layer self-check: in-process servers plus raw responders, 245 checks.
add_test_binary("test_client", "test/integration/client.cpp", {
    group = "test/integration",
    packages = {"zlib", "brotli"},
})

-- Multi-thread concurrency harness for the client (sanitizer-oriented).
add_test_binary("test_threading", "test/integration/thread_check.cpp", {
    group = "test/integration",
    packages = {"zlib", "brotli"},
})

-- Deferred body read after the h1 engine is gone must be a clean error (ASan).
add_test_binary("test_h1_lifetime", "test/integration/h1_pull_lifetime.cpp", {
    group = "test/integration",
})

-- Client probes against independent servers ---------------------------------
add_test_binary("probe_client_cross", "test/client/probes/client_cross.cpp", {
    group = "test/probes",
})

add_test_binary("probe_ws_cross", "test/client/probes/ws_client_cross.cpp", {
    group = "test/probes",
    packages = {"zlib"},
})

add_test_binary("probe_h3_cross", "test/client/probes/h3_client_cross.cpp", {
    group = "test/probes",
    http3 = true,
})

add_test_binary("probe_client_diff", "test/client/probes/client_diff_probe.cpp", {
    group = "test/probes",
    packages = {"zlib", "brotli"},
})

-- Manual proxy probe against Go reference servers (SOCKS5 / HTTPS / CONNECT).
add_test_binary("probe_client_go", "test/client/probes/client_go_probe.cpp", {
    group = "test/probes",
    http3 = true,
    packages = {"zlib", "brotli"},
})

add_test_binary("probe_body_cap", "test/client/probes/body_cap.cpp", {
    group = "test/probes",
})

add_test_binary("probe_h1_duplex", "test/client/probes/h1_duplex.cpp", {
    group = "test/probes",
})

add_test_binary("probe_h2_limit", "test/client/probes/h2_limit.cpp", {
    group = "test/probes",
})

add_test_binary("probe_read_all_timeout", "test/client/probes/read_all_timeout.cpp", {
    group = "test/probes",
})

add_test_binary("probe_retry", "test/client/probes/retry.cpp", {
    group = "test/probes",
})

-- Examples ------------------------------------------------------------------
add_test_binary("example_openapi", "test/examples/openapi_demo.cpp", {
    group = "test/examples",
    packages = {"glaze"},
})

add_test_binary("example_readme", "test/examples/readme_examples.cpp", {
    group = "test/examples",
    packages = {"glaze"},
})

-- Manual tools --------------------------------------------------------------
add_test_binary("tool_loadgen", "test/tools/loadgen.cpp", {
    group = "test/tools",
})

add_test_binary("tool_h3_public", "test/tools/h3_public_check.cpp", {
    group = "test/tools",
    http3 = true,
})

add_test_binary("tool_codeserver_proxy", "test/tools/codeserver_proxy.cpp", {
    group = "test/tools",
})
