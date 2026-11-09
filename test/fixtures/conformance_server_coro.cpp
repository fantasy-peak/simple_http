// Conformance/stress fixture for the C++17 port, written with coroutine
// handlers on top of the port's *token* API.
//
// Same routes and ports as test/fixtures/conformance_server.cpp:
//   7788  plaintext, h1 + prior-knowledge h2 sniffing, WebSocket
//   7790  plaintext, HTTP/2 only                (h2spec)
//   7791  plaintext, HTTP/1.x only              (h1spec)
//   7792  TCP (h1/h2c) + UDP (HTTP/3)          (h3spec, stress)
//
// The handlers are coroutines (built as C++23); every library operation they
// await is the port's token-based initiation function, so this proves the
// dispatcher's coroutine adapter and use_awaitable work end to end.

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "simple_http.h"

namespace asio = boost::asio;
using namespace simple_http;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void request_stop(int) { g_stop = 1; }

std::size_t query_uint(std::string_view query, std::string_view key, std::size_t fallback) {
    std::string needle{key};
    needle += '=';
    for (std::size_t pos = 0; pos < query.size();) {
        const auto amp = query.find('&', pos);
        const auto pair = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (pair.substr(0, needle.size()) == needle) {
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

void register_routes(Server &server) {
    // h1spec sends every case to "/" and requires the body back verbatim.
    server.route(any_methods, "/",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     auto [ec, body] = co_await req->body().async_read_all(asio::as_tuple(asio::use_awaitable));
                     if (ec) {
                         co_await res->status(status::bad_request).async_send("", asio::use_awaitable);
                         co_return;
                     }
                     co_await res->status(status::ok).async_send(std::move(body), asio::use_awaitable);
                 });

    // h2spec/stress: drain the POST body, answer ?n= bytes of 'a'.
    server.route(any_methods, "/world",
                 [](std::shared_ptr<Request> req, std::shared_ptr<ResponseWriter> res) -> asio::awaitable<void> {
                     const std::size_t count = query_uint(req->query(), "n", 1024);
                     auto [ec, ignored] = co_await req->body().async_read_all(asio::as_tuple(asio::use_awaitable));
                     if (ec) {
                         co_await res->status(status::bad_request).async_send("read error", asio::use_awaitable);
                         co_return;
                     }
                     (void)ignored;
                     co_await res->status(status::ok).async_send(std::string(count, 'a'), asio::use_awaitable);
                 });

    // Autobahn: verbatim echo, preserving text/binary.
    server.ws_route("/echo", [](std::shared_ptr<Request>,
                                std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto [ec, msg] = co_await ws->async_read(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return;
            }
            auto [wec] = co_await ws->async_write(std::move(msg.data), msg.text,
                                                   asio::as_tuple(asio::use_awaitable));
            if (wec) {
                co_return;
            }
        }
    });

    // Stress: answer every message with 1024 bytes.
    server.ws_route("/echo1k", [](std::shared_ptr<Request>,
                                  std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        const std::string reply(1024, 'a');
        for (;;) {
            auto [ec, msg] = co_await ws->async_read(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return;
            }
            auto [wec] = co_await ws->async_write_view(reply, msg.text,
                                                        asio::as_tuple(asio::use_awaitable));
            if (wec) {
                co_return;
            }
        }
    });
}

} // namespace

int main() {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    ServerConfig sniff_cfg;
    sniff_cfg.listen = InetAddress{"0.0.0.0", 7788, false};
    sniff_cfg.worker_threads = 4;
    sniff_cfg.limits.ws_compression = true;
    Server sniff{sniff_cfg};
    register_routes(sniff);

    ServerConfig h2_cfg;
    h2_cfg.listen = InetAddress{"0.0.0.0", 7790, false};
    h2_cfg.worker_threads = 4;
    h2_cfg.plaintext_protocols = PlaintextProtocols::Http2;
    Server h2{h2_cfg};
    register_routes(h2);

    ServerConfig h1_cfg;
    h1_cfg.listen = InetAddress{"0.0.0.0", 7791, false};
    h1_cfg.worker_threads = 4;
    h1_cfg.plaintext_protocols = PlaintextProtocols::Http1;
    Server h1{h1_cfg};
    register_routes(h1);

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    ServerConfig dual_cfg;
    dual_cfg.listen = InetAddress{"0.0.0.0", 7792, false};
    dual_cfg.quic = InetAddress{"0.0.0.0", 7792, false};
    dual_cfg.worker_threads = 4;
    dual_cfg.reuse_port = true;
    TlsConfig dual_tls;
    dual_tls.cert_chain_file = "./test/tls_certificates/server_cert.pem";
    dual_tls.private_key_file = "./test/tls_certificates/server_key.pem";
    dual_cfg.tls = std::move(dual_tls);
    Server dual{dual_cfg};
    register_routes(dual);
    const bool dual_ok = static_cast<bool>(dual.start());
    std::printf("cpp17 coro fixture: :7792 tcp+udp h3 (ok=%d, quic_port=%u)\n", dual_ok ? 1 : 0,
                static_cast<unsigned>(dual.quic_port()));
    std::fflush(stdout);
#endif

    const bool ok = static_cast<bool>(sniff.start()) && static_cast<bool>(h2.start()) && static_cast<bool>(h1.start())
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
                    && dual_ok
#endif
        ;
    std::printf("cpp17 coro fixture: :7788 sniffing (ws), :7790 h2, :7791 h1 (ok=%d)\n", ok ? 1 : 0);
    std::fflush(stdout);
    if (!ok) {
        return 1;
    }

    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    sniff.stop();
    h2.stop();
    h1.stop();
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    dual.stop();
#endif
    return 0;
}
