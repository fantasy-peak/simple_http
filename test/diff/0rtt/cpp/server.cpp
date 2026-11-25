// Minimal HTTP/3 0-RTT server for the differential suite.
//
// The handler reports whether the request was accepted as early data, so a
// client can observe both ends of the exchange. QUIC 0-RTT is enabled
// explicitly; this fixture is the C++ side of the Go/C++ cross-checks.
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "simple_http.h"
#include <simple_http/core/asio_compat.h>

namespace asio = simple_http::asio;
using namespace simple_http;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sig(int) { g_stop = 1; }
} // namespace

int main(int argc, char **argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 7931;
    const bool enable_0rtt = argc > 2 ? std::atoi(argv[2]) != 0 : true;
    const int workers = argc > 3 ? std::atoi(argv[3]) : 2;
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", static_cast<std::uint16_t>(port), false};
    cfg.quic = InetAddress{"127.0.0.1", static_cast<std::uint16_t>(port), false};
    cfg.worker_threads = static_cast<unsigned>(workers > 0 ? workers : 1);
    cfg.quic_options.connection.enable_0rtt = enable_0rtt;
    cfg.tls = TlsConfig{};
    cfg.tls->cert_chain_file = "./test/tls_certificates/server_cert.pem";
    cfg.tls->private_key_file = "./test/tls_certificates/server_key.pem";

    Server server{cfg};
    server.route(any_methods, "/0rtt", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200)
            .content_type("text/plain")
            .async_send(req->used_0rtt() ? "early" : "normal", asio::use_awaitable);
    });
    if (!server.start()) {
        std::cerr << "start failed\n";
        return 1;
    }
    std::cout << "ready\n" << std::flush;
    while (g_stop == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop();
    return 0;
}
