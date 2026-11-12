// Concurrency / teardown harness for sanitizer runs.
//
// It is a normal test binary, but it exists for ASan and TSan:
//
//   * one Server with several worker threads,
//   * several clients, each with its own single-threaded io_context (model A),
//   * a shared RateLimiter, compression middleware, WebSocket and a
//     cross-executor ResponseWriter write,
//   * a thread that keeps replacing the global log sink.
//
// ASan catches use-after-free during client/server teardown and any WebSocket
// pump lifetime mistakes. TSan catches races in handler-layer shared state,
// the log-sink replacement and the cross-executor write hop.
//
//   xmake build test_sanitizer_concurrency
//   ./build/linux/x86_64/release/test_sanitizer_concurrency [port]

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "simple_http.h"

#include "../support/coro_compat.h"

namespace asio = boost::asio;
using namespace simple_http;
using namespace simple_http::test;
using namespace std::chrono_literals;

namespace {

std::atomic<int> g_failures{0};
std::atomic<bool> g_streaming_logs{true};

void check(bool ok, std::string_view what) {
    if (!ok) {
        g_failures.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "FAIL " << what << '\n';
    }
}

asio::awaitable<void> run_client(unsigned short port, int id, int iterations) {
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    ClientConfig cfg;
    cfg.default_version = HttpVersionPolicy::Http11;
    cfg.default_h2c = H2cMode::Off;
    cfg.connect_timeout = 2s;
    cfg.request_timeout = 5s;
    Client client{co_await asio::this_coro::executor, cfg};

    for (int i = 0; i < iterations; ++i) {
        {
            auto response = co_await client_send(client.get(base + "/plain"));
            check(response.has_value(), "GET /plain transport");
            if (response) {
                const std::string &body = response->text();
                check(response->status() == status::ok, "GET /plain status");
                check(body == "ok", "GET /plain body");
            }
        }
        {
            const std::string payload = "body-" + std::to_string(id) + "-" + std::to_string(i);
            auto response = co_await client_send(client.post(base + "/echo").body(payload));
            check(response.has_value(), "POST /echo transport");
            if (response) {
                const std::string &body = response->text();
                check(body == payload, "POST /echo body");
            }
        }
        {
            auto response = co_await client_send(client.get(base + "/compress"));
            check(response.has_value(), "GET /compress transport");
            if (response) {
                const std::string &body = response->text();
                check(response->status() == status::ok, "GET /compress status");
                check(body.size() == 4096, "GET /compress body size");
            }
        }
        {
            // 200 or 429 are both correct: the shared limiter may be saturated.
            auto response = co_await client_send(client.get(base + "/rate"));
            check(response.has_value(), "GET /rate transport");
            if (response) {
                (void)response->text();
            }
        }
        {
            auto response = co_await client_send(client.get(base + "/cross"));
            check(response.has_value(), "GET /cross transport");
            if (response) {
                const std::string &body = response->text();
                check(response->status() == status::ok, "GET /cross status");
                check(body == "cross done\n", "GET /cross body");
            }
        }
    }

    // WebSocket lifecycle: open, exchange a few messages, close.
    auto ws = co_await client_open_websocket(client, "ws://127.0.0.1:" + std::to_string(port) + "/ws");
    check(ws.has_value(), "WebSocket open");
    if (ws) {
        for (int i = 0; i < 5; ++i) {
            const std::string message = "ws-" + std::to_string(id) + "-" + std::to_string(i);
            const auto write_result = co_await ws_write_text(**ws, message);
            check(write_result == error_code{}, "WebSocket write");
            auto reply = co_await ws_read(**ws);
            check(reply.has_value(), "WebSocket read");
            if (reply) {
                check(reply->data == message, "WebSocket echo");
            }
        }
        (void)co_await ws_close(**ws);
    }
    co_return;
}

void client_thread(unsigned short port, int id, int iterations) {
    asio::io_context io;
    asio::co_spawn(io, run_client(port, id, iterations), asio::detached);
    io.run();
}

} // namespace

int main(int argc, char **argv) {
    set_log_sink(make_stderr_sink(LogLevel::Error));
    const auto requested_port = static_cast<unsigned short>(argc > 1 ? std::atoi(argv[1]) : 0);
    constexpr int threads = 6;
    constexpr int iterations = 50;

    asio::io_context cross_io;
    auto cross_guard = asio::make_work_guard(cross_io);
    std::thread cross_thread{[&cross_io] { cross_io.run(); }};

    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", requested_port, false};
    cfg.worker_threads = 4;
    cfg.limits.max_body_bytes = 1 << 20;
    Server server{cfg};

    auto limiter = std::make_shared<middleware::RateLimiter>(/*rate=*/1000.0, /*burst=*/10000,
                                                             /*max_keys=*/64);
    server.route(any_methods, "/plain", [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
    });
    server.route(any_methods, "/echo", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto body = co_await body_read_all(req->body());
        co_await res->status(status::ok).async_send(body ? std::move(*body) : std::string{}, asio::use_awaitable);
    });
    server.route(any_methods, "/compress", {middleware::compress(CompressionConfig{.min_bytes = 1})},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::ok)
                         .content_type("text/plain")
                         .async_send(std::string(4096, 'c'), asio::use_awaitable);
                 });
    server.route(any_methods, "/rate", {middleware::rate_limit(limiter)},
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).async_send("ok", asio::use_awaitable);
                 });

    // The handler starts on the connection executor but the writes happen on a
    // different one. ResponseWriter hops internally; this is the path that
    // catches a missing hop or a lifetime mistake under TSan/ASan.
    server.route(any_methods, "/cross", [&cross_io](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
        if (auto ec = co_await as_ec(
                res->status(status::ok).content_type("text/plain").async_begin(asio::as_tuple(asio::use_awaitable)))) {
            co_return;
        }
        asio::co_spawn(
            cross_io,
            [res]() -> asio::awaitable<void> {
                (void)co_await res->async_write("cross ", asio::use_awaitable);
                (void)co_await res->async_finish("done\n", asio::use_awaitable);
            },
            asio::detached);
    });
    server.ws_route("/ws", [](RequestPtr, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto message = co_await ws_read(*ws);
            if (!message) {
                break;
            }
            if (auto ec = co_await ws_write(*ws, message->data, message->text)) {
                break;
            }
        }
    });

    auto started = server.start();
    if (!started) {
        std::cerr << "server failed to start\n";
        cross_guard.reset();
        cross_io.stop();
        cross_thread.join();
        return 1;
    }
    const auto port = server.port();
    if (port == 0) {
        std::cerr << "server started without a bound port\n";
        server.stop();
        cross_guard.reset();
        cross_io.stop();
        cross_thread.join();
        return 1;
    }

    // Concurrent log-sink replacement while requests are logging.
    std::thread sink_thread{[] {
        bool stdout_sink = true;
        while (g_streaming_logs.load(std::memory_order_relaxed)) {
            set_log_sink(stdout_sink ? make_stdout_sink(LogLevel::Error) : make_stderr_sink(LogLevel::Error));
            stdout_sink = !stdout_sink;
            std::this_thread::sleep_for(1ms);
        }
    }};

    std::vector<std::thread> clients;
    clients.reserve(threads);
    for (int i = 0; i < threads; ++i) {
        clients.emplace_back(client_thread, port, i, iterations);
    }
    for (auto &client : clients) {
        client.join();
    }

    g_streaming_logs.store(false, std::memory_order_relaxed);
    sink_thread.join();

    server.stop();
    cross_guard.reset();
    cross_io.stop();
    cross_thread.join();

    const int failures = g_failures.load(std::memory_order_relaxed);
    std::cout << "concurrency: " << threads << " clients x " << iterations << " iterations, failures=" << failures
              << '\n';
    return failures == 0 ? 0 : 1;
}
