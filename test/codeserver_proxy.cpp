// code-server reverse-proxy harness for the h2 (RFC 8441) WebSocket path.
//
// Mirrors the v2ray-cpp deployment shape: the frontend strips a
// `/codeserver/vscode` prefix and forwards the rest to code-server on
// 127.0.0.1:8080 (HTTP request-level, WebSocket byte-level). The idea is to
// reproduce "browser -> reverse proxy -> code-server" with the browser driven
// over HTTP/2, where the WebSocket arrives as an extended CONNECT (RFC 8441).
//
// Drive it with test/python/rfc8441_client.py --codeserver, or by hand:
//   xmake build codeserver_proxy && xmake run codeserver_proxy [backend_port] [frontend_port]
//   curl http://127.0.0.1:7794/codeserver/vscode  (HTTP half streams through)
//
// Requires code-server on the backend port (128.0.0.1; default 8080). This
// target is a test harness, not part of the library.
#include <simple_http.h>

#include <chrono>
#include <cstdlib>
#include <print>
#include <thread>

int main(int argc, char **argv) {
    using namespace simple_http;

    const int backend_port = argc > 1 ? std::atoi(argv[1]) : 8080;
    const int frontend_port = argc > 2 ? std::atoi(argv[2]) : 7794;

    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", static_cast<std::uint16_t>(frontend_port), false};
    cfg.worker_threads = 2;
    Server server{cfg};

    // Prefix-stripping reverse proxy: /codeserver/vscode/<rest> -> /<rest>.
    server.http_proxy_regex("^/codeserver/vscode(.*)$", "127.0.0.1", static_cast<std::uint16_t>(backend_port), "/$1");
    server.ws_proxy_regex("^/codeserver/vscode(.*)$", "127.0.0.1", static_cast<std::uint16_t>(backend_port), "/$1");
    server.fallback(
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(404).send("not found"); });

    if (!server.start()) {
        std::println("failed to bind :{}", frontend_port);
        return 1;
    }
    std::println("codeserver reverse proxy on :{} -> 127.0.0.1:{}", frontend_port, backend_port);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}