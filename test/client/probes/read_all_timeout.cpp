// read_all_timeout_check — pins the documented split between the two read
// shapes (test/client/peers/slow_body_server.py):
//
//   * read()       is bounded per call by body_idle_timeout;
//   * read_all()   deliberately is NOT: a buffered whole-body read may sit out
//                  a stall, so the knob cannot turn a slow download into an
//                  error.
//
// The peer sends a response head + half a body, stalls for `pause` seconds,
// then sends the rest. With a body_idle_timeout shorter than the pause:
//   * read_all() must complete and return the full body;
//   * read()      must fail with client_errc::body_idle_timeout after the
//                 already-buffered half is drained.
//
// Two fresh Client instances are used so the second exchange does not reuse the
// first peer connection (the raw fixture closes after one exchange without
// sending Connection: close).
//
// usage: read_all_timeout_check <url> <idle_budget_ms>

#include "simple_http.h"

#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <string>

namespace sh = simple_http;
namespace asio = boost::asio;

namespace {

asio::awaitable<int> run(std::string url, long idle_ms) {
    const auto executor = co_await asio::this_coro::executor;
    sh::ClientConfig cfg;
    cfg.retry.max_retries = 0;
    cfg.body_idle_timeout = std::chrono::milliseconds(idle_ms);

    // 1. read_all() is not bounded by body_idle_timeout.
    {
        sh::Client client{executor, cfg};
        sh::StreamSpec spec;
        auto opened = co_await client.open_stream(url, spec);
        if (!opened) {
            std::printf("FAIL  read_all(): open failed: %s\n", opened.error().message().c_str());
            co_return 1;
        }
        auto head = co_await opened->read_head();
        if (!head) {
            std::printf("FAIL  read_all(): head failed: %s\n", head.error().message().c_str());
            co_return 1;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto body = co_await opened->read_all();
        const long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (!body) {
            std::printf("FAIL  read_all() applied the body-idle budget: %s at %ldms\n", body.error().message().c_str(),
                        ms);
            co_return 1;
        }
        if (body->empty()) {
            std::printf("FAIL  read_all() returned an empty body\n");
            co_return 1;
        }
        std::printf("PASS  read_all() completes across the stall (%zu bytes, %ldms)\n", body->size(), ms);
    }

    // 2. read() is bounded per call: drain the buffered half, then the stall
    //    must surface as client_errc::body_idle_timeout.
    {
        sh::Client client{executor, cfg};
        sh::StreamSpec spec;
        auto opened = co_await client.open_stream(url, spec);
        if (!opened) {
            std::printf("FAIL  read(): open failed: %s\n", opened.error().message().c_str());
            co_return 1;
        }
        auto head = co_await opened->read_head();
        if (!head) {
            std::printf("FAIL  read(): head failed: %s\n", head.error().message().c_str());
            co_return 1;
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            auto chunk = co_await opened->read();
            const long ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!chunk) {
                if (chunk.error() == sh::client_errc::body_idle_timeout) {
                    std::printf("PASS  read() stops at the body-idle budget (%ldms)\n", ms);
                    co_return 0;
                }
                std::printf("FAIL  read() failed with an unexpected error: %s at %ldms\n",
                            chunk.error().message().c_str(), ms);
                co_return 1;
            }
            if (chunk->eof) {
                std::printf("FAIL  read() reached EOF before the stall (server setup?)\n");
                co_return 1;
            }
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: read_all_timeout_check <url> <idle_budget_ms>\n");
        return 2;
    }
    const std::string url = argv[1];
    const long idle_ms = std::stol(argv[2]);
    asio::io_context ctx;
    asio::co_spawn(ctx, run(url, idle_ms), [](std::exception_ptr ep, int rc) {
        if (ep)
            std::fprintf(stderr, "read_all_timeout_check: exception\n");
        std::exit(rc);
    });
    ctx.run();
    return 1; // unreachable
}