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

} // namespace

int main() {
    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", 7796, false};
    cfg.worker_threads = 2;
    Server server{cfg};

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

    server.fallback(
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(404).send("not found"); });

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
