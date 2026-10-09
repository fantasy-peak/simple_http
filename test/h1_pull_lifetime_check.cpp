// h1_pull_lifetime_check — reading a request body after the connection (and its
// HTTP/1.x engine) is gone must be a clean error, not a use-after-free.
//
// The HTTP/1.x engine hands the handler a Request whose Body reads through a
// pull provider that used to capture a raw Http1Engine* — whose lifetime ended
// with the connection. A handler that stashes the Request for a deferred
// coroutine and returns (the response ends the exchange, the connection closes,
// the engine is destroyed) then reads the body would call into the destroyed
// engine. This driver reproduces exactly that shape:
//
//   * an in-process server whose /defer handler spawns a coroutine holding the
//     Request for 400 ms, then reads req->body();
//   * a client that opens a POST stream, writes a chunk, and aborts — closing
//     the connection while the deferred coroutine is still parked.
//
// Run with the repo's sanitizer build (ASan): pre-fix this aborts with
// heap-use-after-free (the provider dereferences the freed engine); post-fix it
// prints
//   DEFERRED_READ <error message>     (the engine is gone: a clean EOL error)
// and exits 0.
//
//   xmake build h1_pull_lifetime_check && xmake run h1_pull_lifetime_check -- 7824

#include "simple_http.h"

#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <string>

namespace sh = simple_http;
namespace asio = boost::asio;

namespace {

asio::awaitable<void> defer_route(sh::RequestPtr req, sh::ResponsePtr res) {
    // The deferred read: fire 400 ms after the handler returns, by which time
    // the exchange is over and the connection (and engine) are long gone.
    auto ex = co_await asio::this_coro::executor;
    asio::co_spawn(
        ex,
        [req = std::move(req)]() -> asio::awaitable<void> {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(std::chrono::milliseconds(400));
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            auto r = co_await req->body().read();
            if (r) {
                std::printf("DEFERRED_READ ok (%s)\n", r->eof ? "eof" : "chunk");
            } else {
                std::printf("DEFERRED_READ %s\n", r.error().message().c_str());
            }
        },
        asio::detached);
    co_await res->status(200).send("ok");
}

asio::awaitable<int> run(std::uint16_t port) {
    sh::ServerConfig cfg;
    cfg.listen = sh::InetAddress{"127.0.0.1", port, false};
    sh::Server server{cfg};
    server.route({sh::Method::Post}, "/defer", defer_route);
    if (!server.start()) {
        std::printf("SERVER_START_FAILED\n");
        co_return 2;
    }

    sh::ClientConfig cc;
    cc.retry.max_retries = 0;
    sh::Client client{co_await asio::this_coro::executor, cc};
    sh::StreamSpec spec;
    spec.method = sh::Method::Post;
    auto opened = co_await client.open_stream("http://127.0.0.1:" + std::to_string(port) + "/defer", spec);
    if (!opened) {
        std::printf("OPEN_FAILED %s\n", opened.error().message().c_str());
        co_return 2;
    }
    (void)co_await opened->write("hello");
    // Drop the connection: the server sees EOF, the engine tears down.
    (void)co_await opened->abort();

    // Give the deferred 400 ms read time to fire (and the engine to die first).
    asio::steady_timer t{co_await asio::this_coro::executor};
    t.expires_after(std::chrono::milliseconds(800));
    co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

    server.stop();
    co_return 0;
}

} // namespace

int main(int argc, char **argv) {
    const std::uint16_t port = static_cast<std::uint16_t>(argc > 1 ? std::stoul(argv[1]) : 7824);
    asio::io_context ctx;
    asio::co_spawn(ctx, run(port), [](std::exception_ptr ep, int rc) {
        if (ep)
            std::fprintf(stderr, "h1_pull_lifetime_check: exception\n");
        std::exit(rc);
    });
    ctx.run();
    return 1; // unreachable
}