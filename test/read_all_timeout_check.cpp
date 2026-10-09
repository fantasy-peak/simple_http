// read_all_timeout_check — verifies that the Stream's body-idle budget bounds
// read_all(), exactly as it bounds read() (test/python/slow_body_server.py).
//
// A server that answers a head + half a body, then stalls the rest for
// `pause` seconds, distinguishes the two behaviours: with a body_idle_timeout
// shorter than the pause,
//   * read()        fails at ~idle_budget (the per-read deadline wrapper);
//   * read_all()    must fail at ~idle_budget too — pre-fix it bypassed the
//                   wrapper and sat out the whole pause, making the knob a
//                   no-op for the buffered reader.
//
// Prints TIMEOUT_AT <ms> for whichever read failed with body_idle_timeout, or
// READ_OK <ms> if the whole body arrived (the bug's shape).
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
    sh::ClientConfig cfg;
    cfg.retry.max_retries = 0;
    cfg.body_idle_timeout = std::chrono::milliseconds(idle_ms);
    sh::Client client{co_await asio::this_coro::executor, cfg};

    // read_all() on the explicit stream.
    {
        sh::StreamSpec spec;
        auto opened = co_await client.open_stream(url, spec);
        if (!opened) {
            std::printf("READALL_OPEN_FAILED %s\n", opened.error().message().c_str());
            co_return 2;
        }
        auto head = co_await opened->read_head();
        if (!head) {
            std::printf("READALL_HEAD_FAILED %s\n", head.error().message().c_str());
            co_return 2;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto body = co_await opened->read_all();
        const long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (!body) {
            std::printf("READALL %s at %ldms\n", body.error().message().c_str(), ms);
        } else {
            std::printf("READALL_OK len=%zu at %ldms\n", body->size(), ms);
        }
    }

    // read() until eof, as the contrast (the per-read wrapper is expected to
    // bound it — this is the control proving the server setup works).
    {
        sh::StreamSpec spec;
        auto opened = co_await client.open_stream(url, spec);
        if (!opened) {
            std::printf("READ_OPEN_FAILED %s\n", opened.error().message().c_str());
            co_return 2;
        }
        auto head = co_await opened->read_head();
        if (!head) {
            std::printf("READ_HEAD_FAILED %s\n", head.error().message().c_str());
            co_return 2;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto r = co_await opened->read();
        const long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (!r) {
            std::printf("READ %s at %ldms\n", r.error().message().c_str(), ms);
        } else if (r->eof) {
            std::printf("READ_EOF at %ldms\n", ms);
        } else {
            std::printf("READ_CHUNK len=%zu at %ldms\n", r->data.size(), ms);
        }
    }
    co_return 0;
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