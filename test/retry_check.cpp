// retry_check — verifies the default retry condition (test/retry_echo_server.py).
//
// A *client-side decision error* — the caller's body cap (body_too_large), a
// protocol error, a header too large — says nothing about the wire and nothing
// about the request: retrying it as if the attempt had failed on the network is
// wasted work (the whole body is re-transferred and re-read each time). The
// default condition's `is_idempotent(method)` arm used to make that happen for
// GET/HEAD/PUT/DELETE whenever max_retries was set.
//
// The driver sends one GET to a server that answers a body larger than the
// caller's cap and counts the requests it receives. With the bug, the server
// sees max_retries + 1 requests; fixed, exactly 1.
//
//   test/python/.venv/bin/python test/python/retry_echo_server.py 7825 8388608 &
//   xmake run retry_check -- http://127.0.0.1:7825/
//   ... then check the server's REQUEST #N lines.

#include "simple_http.h"

#include <boost/asio.hpp>
#include <cstdio>
#include <string>

namespace sh = simple_http;
namespace asio = boost::asio;

namespace {

asio::awaitable<int> run(std::string url) {
    sh::ClientConfig cfg;
    cfg.retry.max_retries = 2; // the bug's appetite: 3 attempts instead of 1
    cfg.retry.initial_backoff = std::chrono::milliseconds(10);
    sh::Client client{co_await asio::this_coro::executor, cfg};
    auto r = co_await client.get(url).max_body_bytes(1024 * 1024).send(); // 8 MiB body > 1 MiB cap
    if (!r) {
        std::printf("RESULT error=%s\n", r.error().message().c_str());
        co_return 0;
    }
    std::printf("RESULT ok status=%d\n", r->status());
    co_return 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: retry_check <url>\n");
        return 2;
    }
    asio::io_context ctx;
    asio::co_spawn(ctx, run(argv[1]), [](std::exception_ptr ep, int rc) {
        if (ep)
            std::fprintf(stderr, "retry_check: exception\n");
        std::exit(rc);
    });
    ctx.run();
    return 1; // unreachable
}