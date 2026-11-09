// Conformance/stress fixture for the C++17 port.
//
// Same four routes and ports as test/fixtures/conformance_server.cpp:
//   7788  plaintext, HTTP/1.x + prior-knowledge h2 sniffing, WebSocket
//   7790  plaintext, HTTP/2 only                (h2spec)
//   7791  plaintext, HTTP/1.x only
//
//   any  /        echo the request body
//   any  /world   drain the body, answer ?n= bytes of 'a'
//   ws   /echo    verbatim echo, preserving text/binary
//   ws   /echo1k  answer every message with 1024 bytes
//
// 7792 is TCP (h1/h2c) plus UDP (HTTP/3), present only in an
// SIMPLE_HTTP_ENABLE_HTTP3 build.

#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "simple_http.h"

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

void register_routes(simple_http::Server &server) {
    server.route(simple_http::any_methods, "/",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     req->body().async_read_all(
                         [res, done = std::move(done)](simple_http::error_code ec, std::string body) mutable {
                             if (ec) {
                                 res->status(400);
                                 res->async_send("", simple_http::finish_with(std::move(done)));
                                 return;
                             }
                             res->status(200);
                             res->async_send(std::move(body), simple_http::finish_with(std::move(done)));
                         });
                 });

    server.route(simple_http::any_methods, "/world",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     const std::size_t count = query_uint(req->query(), "n", 1024);
                     req->body().async_read_all(
                         [res, count, done = std::move(done)](simple_http::error_code ec, std::string) mutable {
                             if (ec) {
                                 res->status(400);
                                 res->async_send("read error", simple_http::finish_with(std::move(done)));
                                 return;
                             }
                             res->status(200);
                             res->async_send(std::string(count, 'a'), simple_http::finish_with(std::move(done)));
                         });
                 });

    server.ws_route("/echo", [](simple_http::RequestPtr, std::shared_ptr<simple_http::WebSocket> ws,
                                simple_http::HandlerCompletion done) {
        auto completion = std::make_shared<simple_http::HandlerCompletion>(std::move(done));
        auto loop = std::make_shared<std::function<void()>>();
        *loop = [ws, loop, completion] {
            ws->async_read([ws, loop, completion](simple_http::error_code ec, simple_http::WsMessage msg) {
                if (ec) {
                    auto finish = std::move(*completion);
                    std::move(finish)();
                    return;
                }
                ws->async_write(std::move(msg.data), msg.text, [loop, completion](simple_http::error_code write_ec) {
                    if (write_ec) {
                        auto finish = std::move(*completion);
                        std::move(finish)();
                        return;
                    }
                    (*loop)();
                });
            });
        };
        (*loop)();
    });

    server.ws_route("/echo1k", [](simple_http::RequestPtr, std::shared_ptr<simple_http::WebSocket> ws,
                                  simple_http::HandlerCompletion done) {
        auto reply = std::make_shared<const std::string>(1024, 'a');
        auto completion = std::make_shared<simple_http::HandlerCompletion>(std::move(done));
        auto loop = std::make_shared<std::function<void()>>();
        *loop = [ws, loop, completion, reply] {
            ws->async_read([ws, loop, completion, reply](simple_http::error_code ec, simple_http::WsMessage msg) {
                if (ec) {
                    auto finish = std::move(*completion);
                    std::move(finish)();
                    return;
                }
                ws->async_write_view(*reply, msg.text, [loop, completion](simple_http::error_code write_ec) {
                    if (write_ec) {
                        auto finish = std::move(*completion);
                        std::move(finish)();
                        return;
                    }
                    (*loop)();
                });
            });
        };
        (*loop)();
    });
}

} // namespace

int main() {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    simple_http::ServerConfig sniff_cfg;
    sniff_cfg.listen = simple_http::InetAddress{"0.0.0.0", 7788, false};
    sniff_cfg.worker_threads = 4;
    sniff_cfg.limits.ws_compression = true;
    simple_http::Server sniff{sniff_cfg};
    register_routes(sniff);

    simple_http::ServerConfig h2_cfg;
    h2_cfg.listen = simple_http::InetAddress{"0.0.0.0", 7790, false};
    h2_cfg.worker_threads = 4;
    h2_cfg.plaintext_protocols = simple_http::PlaintextProtocols::Http2;
    simple_http::Server h2{h2_cfg};
    register_routes(h2);

    simple_http::ServerConfig h1_cfg;
    h1_cfg.listen = simple_http::InetAddress{"0.0.0.0", 7791, false};
    h1_cfg.worker_threads = 4;
    h1_cfg.plaintext_protocols = simple_http::PlaintextProtocols::Http1;
    simple_http::Server h1{h1_cfg};
    register_routes(h1);

#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    simple_http::ServerConfig dual_cfg;
    dual_cfg.listen = simple_http::InetAddress{"0.0.0.0", 7792, false};
    dual_cfg.quic = simple_http::InetAddress{"0.0.0.0", 7792, false};
    dual_cfg.worker_threads = 4;
    dual_cfg.reuse_port = true;
    simple_http::TlsConfig dual_tls;
    dual_tls.cert_chain_file = "./test/tls_certificates/server_cert.pem";
    dual_tls.private_key_file = "./test/tls_certificates/server_key.pem";
    dual_cfg.tls = std::move(dual_tls);
    simple_http::Server dual{dual_cfg};
    register_routes(dual);
    const bool dual_ok = static_cast<bool>(dual.start());
    std::printf("cpp17 conformance fixture: :7792 tcp+udp h3 (ok=%d, quic_port=%u)\n", dual_ok ? 1 : 0,
                static_cast<unsigned>(dual.quic_port()));
    std::fflush(stdout);
#endif

    const bool ok = static_cast<bool>(sniff.start()) && static_cast<bool>(h2.start()) && static_cast<bool>(h1.start())
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
                    && dual_ok
#endif
        ;
    std::printf("cpp17 conformance fixture: :7788 sniffing (ws), :7790 h2, :7791 h1 (ok=%d)\n", ok ? 1 : 0);
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
