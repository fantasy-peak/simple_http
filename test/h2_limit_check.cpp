// h2_limit_check — client-side check of SETTINGS_MAX_CONCURRENT_STREAMS.
//
// Verifies that the HTTP/2 client honours the *peer's* announced
// SETTINGS_MAX_CONCURRENT_STREAMS. RFC 9113 §6.5.2: a value of 0 means "no new
// streams may be opened" — the initial (unannounced) value is unlimited, but an
// *explicit* 0 must refuse new streams locally, before anything is written.
//
// A negative server-side counterpart (test/python/h2_max_concurrent_server.py)
// announces the limit and logs every frame it receives; the discriminator is
// whether a HEADERS (RequestReceived) ever arrives there:
//   * conforming client  → open_stream fails locally with too_many_streams,
//                          the server sees zero RequestReceived;
//   * non-conforming     → HEADERS hits the wire, the server resets it and the
//                          client reports stream_refused.
//
// Usage: h2_limit_check <url> <sleep_ms>
//   url        plaintext h2 (prior-knowledge), e.g. http://127.0.0.1:7813/
//   sleep_ms   how long to wait after dialing before opening the stream, so the
//              peer's SETTINGS is processed first (the limit only applies to
//              streams opened after the setting took effect).

#include "simple_http.h"

#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

namespace sh = simple_http;
namespace asio = boost::asio;

namespace {

int fail(const char *msg) {
    std::fprintf(stderr, "h2_limit_check: %s\n", msg);
    return 2;
}

asio::awaitable<int> run(std::string url, long sleep_ms) {
    sh::ClientConfig cfg;
    cfg.default_version = sh::HttpVersionPolicy::Http2;
    cfg.default_h2c = sh::H2cMode::PriorKnowledge;
    cfg.retry.max_retries = 0; // exactly one attempt: the result must be the first one

    auto engine = std::make_shared<sh::detail::ClientEngine>(co_await asio::this_coro::executor, cfg);
    auto ex = co_await asio::this_coro::executor;

    auto sess = co_await engine->connect(url);
    if (!sess) {
        std::printf("DIAL_ERR %s\n", sess.error().message().c_str());
        co_return 1;
    }
    if ((*sess)->version() != sh::Version::Http2) {
        std::printf("NOT_H2 version=%d\n", static_cast<int>((*sess)->version()));
        co_return 1;
    }

    // Let the peer's SETTINGS (with its MAX_CONCURRENT_STREAMS) be processed
    // before opening anything — the limit applies only to streams opened after
    // the setting took effect.
    if (sleep_ms > 0) {
        asio::steady_timer timer{ex};
        timer.expires_after(std::chrono::milliseconds(sleep_ms));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    }

    auto req = std::make_shared<sh::Request>(sh::Version::Http11, ex);
    req->set_method(sh::Method::Get);
    req->set_target("/");

    auto stream = co_await (*sess)->open_stream(std::move(req));
    if (!stream) {
        // Refused locally, before the open — the conforming outcome.
        std::printf("REFUSED_BEFORE_WIRE %s\n", stream.error().message().c_str());
        co_return 0;
    }

    // The stream went out. Read what comes back (200 on the control server,
    // stream_refused after the peer resets it on the 0-limit server).
    auto head = co_await (*stream)->read_head();
    if (!head) {
        std::printf("ERR_AFTER_WIRE %s\n", head.error().message().c_str());
        co_return 1;
    }
    std::printf("OK status=%d\n", head->status);
    co_return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        return fail("usage: h2_limit_check <url> <sleep_ms>");
    }
    const std::string url = argv[1];
    const long sleep_ms = std::stol(argv[2]);

    asio::io_context ctx;
    asio::co_spawn(ctx, run(url, sleep_ms), [](std::exception_ptr ep, int rc) {
        if (ep)
            std::fprintf(stderr, "h2_limit_check: exception\n");
        std::exit(rc);
    });
    ctx.run();
    return 1; // unreachable
}