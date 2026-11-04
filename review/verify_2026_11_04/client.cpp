// Client driver for REVIEW-2026-11-04 verification.
//
//   verify_client get <url>
//   verify_client decompress <url> <max_body_bytes>
//   verify_client send_stream <url> <json>
//   verify_client ws_rsv <host> <port> <path>
//
// Prints machine-readable lines; the Python orchestrator asserts on them.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <sys/resource.h>

#include "simple_http.h"

namespace asio = boost::asio;
using namespace simple_http;

namespace {

long max_rss_kb() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;
}

std::string header_dump(const Headers &headers) {
    std::string out;
    for (const auto &[name, value] : headers) {
        out += name;
        out += ':';
        out += value;
        out += '|';
    }
    return out;
}

asio::awaitable<int> run_get(std::string url) {
    Client client{co_await asio::this_coro::executor, ClientConfig{}};
    auto response = co_await client.get(url).send();
    if (!response) {
        std::cout << "error=" << response.error().value() << ":" << response.error().message() << "\n";
        co_return 1;
    }
    std::cout << "status=" << response->status() << "\n";
    std::cout << "headers=" << header_dump(response->headers()) << "\n";
    auto text = co_await response->text();
    std::cout << "body=" << (text ? *text : std::string{"<text error>"}) << "\n";
    co_return 0;
}

asio::awaitable<int> run_decompress(std::string url, std::size_t cap) {
    ClientConfig cfg;
    cfg.limits.max_body_bytes = cap;
    Client client{co_await asio::this_coro::executor, std::move(cfg)};
    auto response = co_await client.get(url).send();
    if (!response) {
        std::cout << "error=" << response.error().value() << ":" << response.error().message() << "\n";
        std::cout << "maxrss_kb=" << max_rss_kb() << "\n";
        co_return 2;
    }
    std::cout << "status=" << response->status() << "\n";
    auto text = co_await response->text();
    std::cout << "body_len=" << (text ? text->size() : 0) << "\n";
    std::cout << "maxrss_kb=" << max_rss_kb() << "\n";
    co_return 0;
}

asio::awaitable<int> run_send_stream(std::string url, std::string json) {
    Client client{co_await asio::this_coro::executor, ClientConfig{}};
    auto stream = co_await client.post(url).json(json).send_stream();
    if (!stream) {
        std::cout << "error=" << stream.error().value() << ":" << stream.error().message() << "\n";
        co_return 1;
    }
    if (auto ec = co_await stream->finish()) {
        std::cout << "finish_error=" << ec.value() << ":" << ec.message() << "\n";
        co_return 1;
    }
    auto body = co_await stream->read_all(1u << 20);
    if (!body) {
        std::cout << "read_error=" << body.error().value() << ":" << body.error().message() << "\n";
        co_return 1;
    }
    std::cout << "body=" << *body << "\n";
    co_return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: verify_client <get|decompress|send_stream> <url> [arg]\n";
        return 2;
    }
    std::string mode = argv[1];
    std::string url = argv[2];

    asio::io_context ctx;
    int code = 2;
    if (mode == "get") {
        asio::co_spawn(ctx, run_get(url), [&](std::exception_ptr, int rc) { code = rc; });
    } else if (mode == "decompress") {
        const auto cap = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0;
        asio::co_spawn(ctx, run_decompress(url, static_cast<std::size_t>(cap)),
                       [&](std::exception_ptr, int rc) { code = rc; });
    } else if (mode == "send_stream") {
        const std::string json = argc > 3 ? argv[3] : "{}";
        asio::co_spawn(ctx, run_send_stream(url, json), [&](std::exception_ptr, int rc) { code = rc; });
    } else {
        std::cerr << "unknown mode\n";
        return 2;
    }
    try {
        ctx.run();
    } catch (const std::exception &e) {
        std::cout << "exception=" << e.what() << "\n";
        std::cout << "maxrss_kb=" << max_rss_kb() << "\n";
        return 3;
    }
    return code;
}
