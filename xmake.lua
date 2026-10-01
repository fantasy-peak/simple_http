-- add_rules("mode.debug", "mode.release")

set_languages("c++23")

-- set_warnings("all", "error")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build", lsp="clangd" })

-- REPRODUCIBILITY --
set_policy("package.requires_lock", true)
set_policy("package.librarydeps.strict_compatibility", true)

-- PACKAGES --
-- HTTP/3 的 QUIC 与帧层来自 ngtcp2 + nghttp3（都是 C 库，只有 h3 路径用）。
-- 上游 xmake-repo 的 ngtcp2 包是 `-DENABLE_OPENSSL=OFF` 构建的，不产 crypto
-- helper——没有它就没有 TLS，QUIC 无从谈起。私有仓库里那份把它改成了
-- `-DENABLE_OPENSSL=ON`（openssl3 >= 3.5 才有上游 CMake 探测的
-- SSL_set_quic_tls_cbs，从而构建 libngtcp2_crypto_ossl），nghttp3 那份也在。
-- 注意：这里不能用 /opt/h3/lib 的预编译库——那是对系统 OpenSSL 3.5.5 编的，
-- 而本仓库用的是 openssl3 3.6.3，混链是 ABI 风险。让 xmake 从源码构建。
add_repositories("my_private_repo https://github.com/fantasy-peak/xmake-repo.git")
add_requires("boost", {configs = {cmake = true, asio=true, regex=true}})
add_requires("openssl3")
add_requires("nghttp2 1.70.0")
add_requires("ngtcp2", "nghttp3")
add_requires("catch2")  -- unit tests only (target `unittest`)
add_requires("glaze")  -- OpenAPI typed-route schemas (opt-in: SIMPLE_HTTP_ENABLE_OPENAPI); header-only
-- Response-body compression (core/content_encoding.h). Only the targets that
-- define SIMPLE_HTTP_ENABLE_COMPRESSION link these; the library itself stays
-- dependency-free for downstream consumers that do not want compression.
add_requires("zlib", "brotli")
-- glaze is an unconditional dependency: it backs the JSON helpers
-- (proto/json.h, read_json_body / write_json). Header-only, no transitive deps.
add_requires("glaze")

add_cxflags("-O2 -Wextra -Wno-missing-field-initializers -Wno-ignored-qualifiers")
add_defines("SIMPLE_HTTP_USE_BOOST_REGEX")

target("simple_http")
    set_kind("static")
    add_includedirs("include", { public = true })
    add_packages(
        "ngtcp2",
        "nghttp2",
        "nghttp3",
        "boost",
        "openssl3",
        "glaze",
        {public = true}
    )
target_end()

target("server")
    set_kind("binary")
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-mismatched-new-delete", "-Wno-type-limits")
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
    add_defines("SIMPLE_HTTP_ENABLE_HTTP3")
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
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-mismatched-new-delete", "-Wno-type-limits")
        end
    end)
    add_deps("simple_http")
    add_files("test/client.cpp")
    add_packages("zlib", "brotli")
    add_defines("SIMPLE_HTTP_ENABLE_COMPRESSION")
    set_rundir(".")
target_end()

-- Unit tests (Catch2): pure logic, no sockets — parsers, HPACK, frames, the
-- URL/config helpers, routing. Fast enough to run on every change:
--   xmake build unittest && xmake run unittest
target("unittest")
    set_kind("binary")
    set_default(false)
    add_defines("SIMPLE_HTTP_ENABLE_HTTP3")
    -- OpenAPI is opt-in for consumers; the unit target enables it so the
    -- openapi tests (and the glaze-heavy umbrella include) are compiled.
    add_defines("SIMPLE_HTTP_ENABLE_OPENAPI")
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-mismatched-new-delete", "-Wno-type-limits")
        end
    end)
    add_deps("simple_http")
    add_files("test/unit/*.cpp")
    add_packages("catch2", "zlib", "brotli", "glaze")
    add_defines("SIMPLE_HTTP_ENABLE_COMPRESSION")
    set_rundir(".")
target_end()

-- OpenAPI demo: typed routes (`route<Req, Res>`) collected into an OAS 3.1
-- document served at /openapi.json, browsed at /swagger (CDN-hosted Swagger UI).
-- A standalone binary so test/server.cpp — the conformance/stress surface —
-- stays untouched. Run from the repository root:
--   xmake build openapi_demo && xmake run openapi_demo
-- then open http://127.0.0.1:7795/swagger
target("openapi_demo")
    set_kind("binary")
    set_default(false)
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-mismatched-new-delete", "-Wno-type-limits")
        end
    end)
    add_deps("simple_http")
    add_files("test/openapi_demo.cpp")
    add_packages("glaze")
    add_defines("SIMPLE_HTTP_ENABLE_OPENAPI")
    set_rundir(".")
target_end()

-- Server regression suite: raw sockets against an in-process server, sending the
-- malformed and boundary cases a well-behaved client will not send.
--   xmake build regression && xmake run regression
target("regression")
    set_kind("binary")
    set_default(false)
    add_defines("SIMPLE_HTTP_ENABLE_HTTP3")
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-mismatched-new-delete", "-Wno-type-limits")
        end
    end)
    add_deps("simple_http")
    add_files("test/server_regression.cpp", "test/unit/main.cpp")
    add_packages("catch2")
    set_rundir(".")
target_end()

-- Python-side protocol tests. The C++ suites drive simple_http with its own
-- client, so a spec misreading shared by both halves cancels out; these use
-- httpx / hyper-h2 / websockets instead, which are independent implementations.
-- First run:
--   python3 -m venv test/python/.venv
--   test/python/.venv/bin/pip install -r test/python/requirements.txt
-- Then:
--   xmake python-tests
target("python-tests")
    set_kind("phony")
    add_deps("server")
    on_run(function ()
        local python = "test/python/.venv/bin/python"
        if not os.isfile(python) then
            raise("no virtualenv at test/python/.venv — see test/python/requirements.txt")
        end
        os.execv(python, {"test/python/run.py"})
    end)
target_end()

-- Compile check for the code samples in README.md. Documented code is not
-- exercised by anything else, and the README had quietly drifted to APIs that no
-- longer existed — a string of examples that looked plausible and would not
-- build. Reproducing them here turns that drift into a build failure:
--   xmake build readme_examples
target("readme_examples")
    set_kind("binary")
    set_default(false)
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-type-limits")
        end
    end)
    add_deps("simple_http")
    add_files("test/readme_examples.cpp")
    add_packages("glaze")
    add_defines("SIMPLE_HTTP_ENABLE_OPENAPI")  -- the README OpenAPI sample is compiled here
    set_rundir(".")
target_end()

-- Cross-validation of the client-side WebSocket against an independent server
-- (test/python/ws_echo_server.py, the `websockets` library). See the file's
-- header comment:   test/python/.venv/bin/python test/python/ws_echo_server.py
--                    xmake run ws_cross -- 27920
target("ws_cross")
    set_kind("binary")
    set_default(false)
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-type-limits")
        end
    end)
    add_deps("simple_http")
    add_files("test/ws_client_cross.cpp")
    set_rundir(".")
target_end()

-- Cross-validation of the client against an independent HTTP/1.1 server
-- (test/python/http_server.py, stdlib http.server). See the file header.
target("client_cross")
    set_kind("binary")
    set_default(false)
    on_load(function (target)
        if target:toolchain("gcc") then
            target:add("cxxflags", "-Wno-maybe-uninitialized", "-Wno-type-limits")
        end
    end)
    add_deps("simple_http")
    add_files("test/client_cross.cpp")
    set_rundir(".")
target_end()

-- One-shot: build the two client cross-validation binaries and drive them
-- against the independent Python servers (test/python/run_client_cross.py).
target("client-cross-python")
    set_kind("phony")
    set_default(false)
    add_deps("client_cross", "ws_cross")
    on_run(function ()
        local python = "test/python/.venv/bin/python"
        if not os.isfile(python) then
            raise("no virtualenv at test/python/.venv — see test/python/requirements.txt")
        end
        os.execv(python, {"test/python/run_client_cross.py"})
    end)
target_end()


