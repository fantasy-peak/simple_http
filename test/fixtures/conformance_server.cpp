// Minimal fixture for the external conformance and stress suites.
//
// The old fixture_server.cpp also hosted the in-repo Python suites and carried
// dozens of demo routes (reverse proxy, SSE, middleware, mTLS, ...). Those
// suites are gone; the external suites only need four routes and fixed ports.
// Keeping this file small makes a protocol change easy to review and keeps the
// conformance surface honest: nothing here exists for any other reason.
//
// Ports are contracts with test/external/*/run.sh — do not change them:
//   7788  plaintext, h1 + h2c sniffing, WebSocket (Autobahn)
//   7790  plaintext, HTTP/2 only                (h2spec)
//   7791  plaintext, HTTP/1.x only              (h1spec)
//   7792  TCP (h1/h2c) + UDP (HTTP/3)          (h3spec, stress h3)
//
// Routes:
//   any  /        echo the request body (h1spec sends every case here)
//   any  /world   drain the body, answer ?n= bytes of 'a' (h2spec, stress)
//   ws   /echo    verbatim echo, preserving text/binary (Autobahn)
//   ws   /echo1k  answer every message with 1024 bytes (stress)

#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "simple_http.h"

#include "../support/coro_compat.h"

namespace asio = boost::asio;
using namespace simple_http;
using namespace simple_http::test;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void request_stop(int) { g_stop = 1; }

std::size_t query_uint(std::string_view query, std::string_view key, std::size_t fallback) {
    std::string needle{key};
    needle += '=';
    for (std::size_t pos = 0; pos < query.size();) {
        const auto amp = query.find('&', pos);
        const auto pair = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (pair.starts_with(needle)) {
            const auto value = pair.substr(needle.size());
            std::size_t out = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
            return ec == std::errc{} ? out : fallback;
        }
        if (amp == std::string_view::npos) {
            break;
        }
        pos = amp + 1;
    }
    return fallback;
}

template <typename ServerT> void register_routes(ServerT &server) {
    // h1spec sends every case to "/" and requires the body back verbatim.
    // Drain first: an unread request body would desynchronise keep-alive
    // framing on the next request over the same connection.
    server.route(
        any_methods, "/",
        [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
            auto body = co_await body_read_all(req->body());
            co_await res->status(status::ok).async_send(body ? std::move(*body) : std::string{}, asio::use_awaitable);
        });

    // h2spec targets /world; the stress suite chooses 2k/10k/20k bodies with
    // ?n= and POSTs a 1 KiB body that must be drained.
    server.route(any_methods, "/world",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     auto body = co_await body_read_all(req->body());
                     if (!body) {
                         co_await res->status(status::bad_request).async_send("read error", asio::use_awaitable);
                         co_return;
                     }
                     co_await res->status(status::ok)
                         .async_send(std::string(query_uint(req->query(), "n", 1024), 'a'), asio::use_awaitable);
                 });

    // Autobahn requires an exact echo: same text/binary type, no extra frames.
    server.ws_route("/echo", [](std::shared_ptr<Request>, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws_read(*ws);
            if (!msg) {
                break;
            }
            if (auto ec = co_await ws_write(*ws, msg->data, msg->text)) {
                break;
            }
        }
    });

    // Stress workload shape: 512 B request -> 1 KiB response, same type.
    server.ws_route("/echo1k", [](std::shared_ptr<Request>, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        const std::string reply(1024, 'a');
        for (;;) {
            auto msg = co_await ws_read(*ws);
            if (!msg) {
                break;
            }
            if (auto ec = co_await ws_write_view(*ws, reply, msg->text)) {
                break;
            }
        }
    });
}

} // namespace

int main() {
    set_log_sink(make_stderr_sink(LogLevel::Warn));
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    // 7788: the sniffing endpoint. Autobahn needs permessage-deflate negotiable
    // here; whether a client uses it is the client's choice.
    ServerConfig sniff_cfg;
    sniff_cfg.listen = InetAddress{"0.0.0.0", 7788, false};
    sniff_cfg.worker_threads = 4;
    sniff_cfg.limits.ws_compression = true;
    Server sniff{sniff_cfg};
    register_routes(sniff);

    // Single-protocol endpoints exist because a malformed HTTP/2 preface and a
    // malformed HTTP/1.x request line are the same bytes on a sniffing port:
    // h2spec wants GOAWAY, h1spec wants 400. Only a declared protocol can do
    // both (see net/connection.h).
    ServerConfig h2c_cfg;
    h2c_cfg.listen = InetAddress{"0.0.0.0", 7790, false};
    h2c_cfg.worker_threads = 4;
    h2c_cfg.plaintext_protocols = PlaintextProtocols::Http2;
    Server h2c{h2c_cfg};
    register_routes(h2c);

    ServerConfig h1_cfg;
    h1_cfg.listen = InetAddress{"0.0.0.0", 7791, false};
    h1_cfg.worker_threads = 4;
    h1_cfg.plaintext_protocols = PlaintextProtocols::Http1;
    Server h1{h1_cfg};
    register_routes(h1);

    bool ok = sniff.start() && h2c.start() && h1.start();
    std::cout << "conformance fixture: :7788 sniffing (ws), :7790 h2c, :7791 h1 (ok=" << ok << ")\n";

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // TCP + UDP on the same number: one Server, one router, two transports.
    // No client certificate; h3spec/curl do not have one.
    ServerConfig dual_cfg;
    dual_cfg.listen = InetAddress{"0.0.0.0", 7792, false};
    dual_cfg.quic = InetAddress{"0.0.0.0", 7792, false};
    dual_cfg.worker_threads = 4;
    dual_cfg.reuse_port = true;
    dual_cfg.tls = TlsConfig{
        .cert_chain_file = "./test/tls_certificates/server_cert.pem",
        .private_key_file = "./test/tls_certificates/server_key.pem",
    };
    Server dual{dual_cfg};
    register_routes(dual);
    const bool dual_ok = static_cast<bool>(dual.start());
    std::cout << "conformance fixture: :7792 tcp+udp h3 (ok=" << dual_ok << ")\n";
    ok = ok && dual_ok;
#endif

    if (!ok) {
        return 1;
    }
    std::fflush(stdout);

    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sniff.stop();
    h2c.stop();
    h1.stop();
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    dual.stop();
#endif
    return 0;
}
