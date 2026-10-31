// A small server dedicated to middleware and compression testing. It mounts
// every built-in middleware and a few routes whose responses exercise the
// compression policy (large/small bodies, compressible/incompressible types,
// streamed bodies). Run it with:
//
//   xmake build middleware_server && xmake run middleware_server
//
// It listens on 127.0.0.1:7796 and runs until killed.

#include "simple_http.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>

namespace asio = boost::asio;

namespace {

using namespace simple_http;

asio::awaitable<void> delay(std::chrono::milliseconds ms) {
    asio::steady_timer timer{co_await asio::this_coro::executor};
    timer.expires_after(ms);
    co_await timer.async_wait(asio::use_awaitable);
}

struct JsonEcho {
    std::string name;
    int age{};
};

} // namespace

int main() {
    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", 7796, false};
    cfg.worker_threads = 2;
    // Small limits so the 413/431 paths are reachable cheaply.
    cfg.limits.max_header_bytes = 4096;
    cfg.limits.max_body_bytes = 1024 * 1024;
    Server server{cfg};

    // Static document root shared with the Go reference server, so file
    // semantics can be compared byte for byte.
    const auto site_root = std::filesystem::temp_directory_path() / "simple_http_site";
    std::filesystem::remove_all(site_root);
    std::filesystem::create_directories(site_root / "sub");
    std::filesystem::create_directories(site_root / "assets");
    const auto write_file = [&](const std::filesystem::path &p, std::string_view body) {
        std::ofstream out(p, std::ios::binary);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    };
    write_file(site_root / "index.html", "<html>index</html>");
    write_file(site_root / "404.html", "<html>missing</html>");
    write_file(site_root / "app.js", "console.log('app');");
    write_file(site_root / "sub/page.html", "<html>page</html>");
    write_file(site_root / "assets/app.v1.js", "console.log('v1');");
    write_file(site_root / "data.bin", std::string(1u << 20, 'D'));

    StaticTableConfig table_cfg;
    table_cfg.root = site_root.string();
    table_cfg.immutable_prefixes = {"/assets/"};
    StaticFilesConfig site_cfg;
    site_cfg.table = table_cfg;
    site_cfg.spa_fallback = "index.html";
    auto site = std::make_shared<StaticFiles>(site_cfg);
    std::string site_error;
    if (!site->load(site_error)) {
        std::println(std::cerr, "static site load failed: {}", site_error);
        return 1;
    }
    server.static_files(site);

    // Global middleware, in chain order.
    server.use(middleware::request_id());
    server.use(middleware::access_log());
    server.use(middleware::recovery());
    server.use(middleware::clean_path());
    server.use(middleware::redirect_slashes());
    server.use(middleware::compress(CompressionConfig{.min_bytes = 1024}));

    // A body above min_bytes and a compressible type: compressed.
    server.route(any_methods, "/plain", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").send(std::string(2048, 'a'));
    });
    // Below min_bytes: sent as-is.
    server.route(any_methods, "/tiny", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").send("hi");
    });
    // Not a compressible type: sent as-is.
    server.route(any_methods, "/image", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("image/png").send(std::string(2048, 'p'));
    });
    // Streamed (compress_streamed defaults to true).
    server.route(any_methods, "/bigstream", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").begin();
        co_await res->write(std::string(2048, 's'));
        co_await res->finish();
    });

    // Per-route middleware.
    server.route(any_methods, "/secure", {middleware::secure_headers()},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(200).content_type("text/plain").send("ok");
                 });
    server.route(any_methods, "/auth", {middleware::basic_auth("user", "pass")},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(200).content_type("text/plain").send("secret");
                 });
    server.route(any_methods, "/realip", {middleware::real_ip({"127.0.0.1"})},
                 [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                     const auto *ip = req->get_state<middleware::ClientIp>();
                     co_await res->status(200).content_type("text/plain").send(ip ? ip->value : std::string{"none"});
                 });

    // strip_prefix inside a group.
    server.group("/api", {middleware::strip_prefix("/api")}, [](Router &api) {
        api.route(any_methods, "/plain", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(200).content_type("text/plain").send("api-plain");
        });
    });

    // redirect_slashes: registered without the trailing slash; asking for
    // "/slash/" redirects to "/slash".
    server.route(any_methods, "/slash", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").send("slash");
    });

    // Conditional requests (ETag + Last-Modified).
    server.route(any_methods, "/conditional", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        if (co_await maybe_not_modified(req, res, "\"v1\"", 1700000000)) {
            co_return;
        }
        co_await res->status(200).content_type("text/plain").send("conditional-body");
    });

    // The handler overruns the 50 ms deadline: answered 504.
    server.route(any_methods, "/timeout", {middleware::timeout(std::chrono::milliseconds(50))},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await delay(std::chrono::milliseconds(500));
                     co_await res->status(200).content_type("text/plain").send("late");
                 });

    // recovery() turns the exception into a 500.
    server.route(any_methods, "/panic", [](RequestPtr, ResponsePtr) -> asio::awaitable<void> {
        throw std::runtime_error("middleware recovery probe");
    });

    server.route(any_methods, "/empty", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(204).send_bodyless();
    });

    // --- forms, JSON, redirect, cookie, SSE ---
    server.route(any_methods, "/form", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto form = co_await read_urlencoded_body(*req);
        if (!form) {
            co_await res->status(400).send("bad form");
            co_return;
        }
        std::string out;
        for (const std::string_view name : {"a", "b"}) {
            for (const std::string_view value : form->get_all(name)) {
                out += std::string{name} + "=" + std::string{value} + "&";
            }
        }
        co_await res->status(200).content_type("text/plain").send(out);
    });
    server.route(any_methods, "/form-multipart", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto parts = co_await read_multipart_body(*req);
        if (!parts) {
            co_await res->status(400).send("bad multipart");
            co_return;
        }
        std::string out;
        for (const auto &part : *parts) {
            out += part.name + "|" + part.filename + "|" + part.data + ";";
        }
        co_await res->status(200).content_type("text/plain").send(out);
    });
    server.route(any_methods, "/json", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto value = co_await read_json_body<JsonEcho>(*req);
        if (!value) {
            co_await res->status(400).send("bad json");
            co_return;
        }
        co_await write_json(res, *value);
    });
    server.route(any_methods, "/redirect", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->redirect("/target").send("");
    });
    server.route(any_methods, "/target", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").send("target");
    });
    server.route(any_methods, "/cookie", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        res->set_cookie("sid", "abc123", std::chrono::seconds(3600), "/", "example.com", false, true, "Lax");
        co_await res->status(200).content_type("text/plain").send("cookie");
    });
    server.route(any_methods, "/sse", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        if (auto ec = co_await res->status(200).sse_begin(); ec) {
            co_return;
        }
        (void)co_await res->sse_event("hello", "greet", "1");
        (void)co_await res->sse_comment("keepalive");
        (void)co_await res->sse_event("l1\nl2");
        (void)co_await res->finish();
    });

    // Compression-policy probes.
    server.route(any_methods, "/bytes", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        std::size_t n = 1024;
        for (const auto value : req->query_params().get_all("n")) {
            n = std::stoul(std::string{value});
            break;
        }
        co_await res->status(200).content_type("text/plain").send(std::string(n, 'a'));
    });
    server.route(any_methods, "/etag", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        res->header("etag", "\"abc\"");
        co_await res->status(200).content_type("text/plain").send(std::string(2048, 'e'));
    });
    server.route(any_methods, "/vary", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        res->header("vary", "Origin");
        co_await res->status(200).content_type("text/plain").send(std::string(2048, 'v'));
    });
    server.route(any_methods, "/preencoded", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        res->header("content-encoding", "identity");
        co_await res->status(200).content_type("text/plain").send(std::string(2048, 'p'));
    });
    server.route(any_methods, "/notransform", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        res->header("cache-control", "no-transform");
        co_await res->status(200).content_type("text/plain").send(std::string(2048, 'n'));
    });

    // Reverse proxies: same upstream as the Go reference (the reference server
    // itself on :7801), plus an unreachable one for the 502 path.
    server.http_proxy_regex("/proxy/(.*)", "127.0.0.1", 7801, "/$1");
    server.http_proxy_regex("/deadproxy/(.*)", "127.0.0.1", 65533, "/$1");
    server.ws_proxy_regex("/wsproxy/(.*)", "127.0.0.1", 7801, "/$1");

    // The static stage's own 404 page, so the "site has a 404.html" path is
    // exercised rather than the plain-text fallback.
    server.fallback(
        [site](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> { co_await site->serve_not_found(req, res); });

    if (auto started = server.start(); !started) {
        for (const auto &failure : started.error().failures) {
            std::println(std::cerr, "startup failure: {}", failure.error.message());
        }
        return 1;
    }
    std::println("middleware server on 127.0.0.1:7796");

    // Opt-in TLS 1.2: the default server policy is TLS 1.3 only; this listener
    // explicitly lowers the floor, which is what TlsConfig::min_version is for.
    ServerConfig tls12_cfg;
    tls12_cfg.listen = InetAddress{"127.0.0.1", 7797, false};
    tls12_cfg.tls = TlsConfig{
        .cert_chain_file = "./test/tls_certificates/server_cert.pem",
        .private_key_file = "./test/tls_certificates/server_key.pem",
        .min_version = TlsMinVersion::Tls12,
    };
    Server tls12_server{tls12_cfg};
    tls12_server.route(any_methods, "/world", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type("text/plain").send(std::string(1024, 'a'));
    });
    if (auto started = tls12_server.start(); !started) {
        std::println(std::cerr, "TLS1.2 listener failed to start");
        return 1;
    }
    std::println("TLS1.2-enabled server on 127.0.0.1:7797");

    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
