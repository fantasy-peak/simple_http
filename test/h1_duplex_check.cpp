// h1_duplex_check — verifies the client's HTTP/1.1 half-duplex guard.
//
// HTTP/1.1 is half-duplex by framing: the response cannot begin until the
// request is complete (RFC 9112 §6.3), so reading the response while the
// request body is still open — a caller that forgets finish() on a streamed
// body — must be refused fast, not left hanging for the TTFB timeout.
//
// Pair with a Python server that accepts the request and never answers
// (test/python/h1_stall_server.py):
//   test/python/.venv/bin/python test/python/h1_stall_server.py 7822 &
//   xmake run h1_duplex_check -- http://127.0.0.1:7822/ 1500
//
// Prints:
//   REFUSED <message> after <N>ms   — the guard fired (the conforming outcome)
//   READ_FAILED... after <N>ms      — pre-fix: hung until the head timeout
//   READ_OK...                      — unexpected: the stall server answered
//
// Exit code 0 = guard fired; 2 = the call hung and timed out (pre-fix).

#include "simple_http.h"

#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <string>

namespace sh = simple_http;
namespace asio = boost::asio;

namespace {

asio::awaitable<int> run(std::string url, long head_ms) {
    sh::ClientConfig cfg;
    cfg.retry.max_retries = 0; // exactly one attempt; the stall server never answers anyway
    sh::Client client{co_await asio::this_coro::executor, cfg};

    sh::StreamSpec spec;
    spec.method = sh::Method::Post;
    spec.response_head_timeout = std::chrono::milliseconds(head_ms);

    auto opened = co_await client.open_stream(url, spec);
    if (!opened) {
        std::printf("OPEN_FAILED %s\n", opened.error().message().c_str());
        co_return 1;
    }
    // A partial request body: it stays open on purpose.
    if (auto ec = co_await opened->write("hello"); ec) {
        std::printf("WRITE_FAILED %s\n", ec.message().c_str());
        co_return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto head = co_await opened->read_head();
    const long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (!head) {
        std::printf("READ_FAILED %s after %ldms\n", head.error().message().c_str(), ms);
        co_return head.error() == make_error_code(sh::client_errc::half_duplex) ? 0 : 2;
    }
    std::printf("READ_OK status=%d after %ldms\n", head->status, ms);
    co_return 3;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: h1_duplex_check <url> <head_timeout_ms>\n");
        return 2;
    }
    const std::string url = argv[1];
    const long head_ms = std::stol(argv[2]);

    asio::io_context ctx;
    asio::co_spawn(ctx, run(url, head_ms), [](std::exception_ptr ep, int rc) {
        if (ep)
            std::fprintf(stderr, "h1_duplex_check: exception\n");
        std::exit(rc);
    });
    ctx.run();
    return 1; // unreachable
}