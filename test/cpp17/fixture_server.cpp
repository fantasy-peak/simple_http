// Minimal C++17-port server for the Python interoperability test.
//
// Prints "PORT <n>" and "TLSPORT <n>" once listening (the Python runner uses
// port 0 and reads the assigned ports), then serves until SIGINT/SIGTERM:
//
//   GET  /        -> "hello"
//   POST /echo    -> the request body
//   WS   /ws      -> text echo
//
// The plaintext listener sniffs prior-knowledge HTTP/2 and serves HTTP/1.x
// otherwise; the TLS listener offers h2 and http/1.1 through ALPN. The Python
// client exercises both wire formats with independent implementations.

#include <simple_http.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <charconv>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

std::string make_fixture_file() {
    auto path = std::filesystem::temp_directory_path() /
                ("simple_http_cpp17_fixture_" + std::to_string(static_cast<long>(::getpid())) + ".txt");
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << "streamed file body";
    return path.string();
}

void register_routes(simple_http::Server &server, const std::string &file_path) {
    server.route({simple_http::Method::Get}, "/",
                 [](simple_http::RequestPtr, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     res->status(200).content_type("text/plain");
                     res->async_send("hello", simple_http::finish_with(std::move(done)));
                 });
    server.route({simple_http::Method::Post}, "/echo",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     req->body().async_read_all(
                         [res, done = std::move(done)](simple_http::error_code ec, std::string body) mutable {
                             if (ec) {
                                 res->status(400);
                                 res->async_send("", simple_http::finish_with(std::move(done)));
                                 return;
                             }
                             res->status(200).content_type("application/octet-stream");
                             res->async_send(std::move(body), simple_http::finish_with(std::move(done)));
                         });
                 });
    server.route(simple_http::any_methods, "/world",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     std::size_t count = 1024;
                     const auto query = req->query();
                     if (auto q = query.find("n="); q != std::string_view::npos) {
                         std::size_t value = 0;
                         const auto [ptr, ec] =
                             std::from_chars(query.data() + q + 2, query.data() + query.size(), value);
                         if (ec == std::errc{})
                             count = value;
                     }
                     req->body().async_read_all(
                         [res, count, done = std::move(done)](simple_http::error_code ec, std::string) mutable {
                             if (ec) {
                                 res->status(400);
                                 res->async_send("", simple_http::finish_with(std::move(done)));
                                 return;
                             }
                             res->status(200);
                             res->async_send(std::string(count, 'a'), simple_http::finish_with(std::move(done)));
                         });
                 });
    server.route(
        {simple_http::Method::Get}, "/file",
        [file_path](simple_http::RequestPtr, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
            res->status(200).content_type("text/plain");
            simple_http::async_send_file(*res, file_path, simple_http::finish_with(std::move(done)));
        });
    server.route({simple_http::Method::Post}, "/upload",
                 [](simple_http::RequestPtr req, simple_http::ResponsePtr res, simple_http::HandlerCompletion done) {
                     auto path = std::make_shared<std::string>(
                         (std::filesystem::temp_directory_path() /
                          ("simple_http_cpp17_upload_" + std::to_string(static_cast<long>(::getpid())) + ".bin"))
                             .string());
                     auto ptr = std::make_shared<simple_http::HandlerCompletion>(std::move(done));
                     simple_http::async_read_body_to_file(
                         req->body(), *path,
                         [res, path, ptr](simple_http::error_code ec, std::uint64_t written) mutable {
                             std::filesystem::remove(*path, ec);
                             if (ec) {
                                 res->status(400);
                                 res->async_send("", simple_http::finish_with(std::move(*ptr)));
                                 return;
                             }
                             res->status(200).content_type("text/plain");
                             res->async_send(std::to_string(written), simple_http::finish_with(std::move(*ptr)));
                         });
                 });
    server.ws_route("/ws", [](simple_http::RequestPtr, std::shared_ptr<simple_http::WebSocket> ws,
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
}

} // namespace

int main(int argc, char **argv) {
    if (std::getenv("SIMPLE_HTTP_TEST_LOG") != nullptr) {
        simple_http::set_log_sink(simple_http::make_stdout_sink(simple_http::LogLevel::Debug));
    }
    const std::uint16_t requested_port =
        argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : static_cast<std::uint16_t>(0);
    // Optional second argument: a WebSocket backend to expose through the
    // byte-level reverse proxy at /ws-proxy.
    const std::uint16_t ws_proxy_backend =
        argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : static_cast<std::uint16_t>(0);
    const std::string file_path = make_fixture_file();

    simple_http::ServerConfig plain_cfg;
    plain_cfg.listen = simple_http::InetAddress{"127.0.0.1", requested_port, false};
    plain_cfg.worker_threads = 2;
    simple_http::Server plain{plain_cfg};
    register_routes(plain, file_path);
    if (ws_proxy_backend != 0) {
        simple_http::WsProxyTarget target;
        target.host = "127.0.0.1";
        target.port = ws_proxy_backend;
        // The backend exposes the echo route at /ws; the frontend route keeps
        // its own path unless a rewrite is configured.
        target.rewrite_path = "/ws";
        plain.ws_proxy("/ws-proxy", std::move(target));
    }

    simple_http::ServerConfig tls_cfg;
    tls_cfg.listen = simple_http::InetAddress{"127.0.0.1", 0, false};
    tls_cfg.worker_threads = 2;
    simple_http::TlsConfig tls;
    tls.cert_chain_file = "./test/tls_certificates/server_cert.pem";
    tls.private_key_file = "./test/tls_certificates/server_key.pem";
    tls_cfg.tls = std::move(tls);
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    // Same TLS policy, UDP transport: prints QUICPORT below.
    tls_cfg.quic = simple_http::InetAddress{"127.0.0.1", 0, false};
#endif
    simple_http::Server secure{tls_cfg};
    register_routes(secure, file_path);

    if (!plain.start()) {
        std::fprintf(stderr, "fixture: plaintext start failed\n");
        return 1;
    }
    if (!secure.start()) {
        std::fprintf(stderr, "fixture: TLS start failed\n");
        plain.stop();
        return 1;
    }
    std::printf("PORT %u\n", plain.port());
    std::printf("TLSPORT %u\n", secure.port());
#ifdef SIMPLE_HTTP_ENABLE_HTTP3
    std::printf("QUICPORT %u\n", secure.quic_port());
#endif
    std::fflush(stdout);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    plain.stop();
    secure.stop();
    return 0;
}
