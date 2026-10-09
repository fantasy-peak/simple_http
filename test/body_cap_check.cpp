// body_cap_check — verifies the streaming Response's buffered readers honour an
// explicit body cap.
//
// The streaming Response (constructed from a live exchange) reads its body
// through bytes()/text()/json(), which used to call read_all() with no cap: a
// peer streaming an endless body would be read into memory without bound. The
// constructor now takes a `body_cap`; the check builds one over a live
// exchange against a big fixed-length body (test/python/big_body_server.py) and
// serves as the compile-time evidence that the cap parameter exists:
//
//   test/python/.venv/bin/python test/python/big_body_server.py 7823 8388608 &
//   xmake run body_cap_check -- http://127.0.0.1:7823/ 1048576
//
// Prints BYTES_FAILED <message> when the cap fired (the conforming outcome) or
// BYTES_OK <len> when the whole body was read (no cap / cap ignored).

#include "simple_http.h"

#include <boost/asio.hpp>
#include <cstdio>
#include <string>

namespace sh = simple_http;
namespace asio = boost::asio;

namespace {

asio::awaitable<int> run(std::string url, std::size_t cap) {
    sh::ClientConfig cfg;
    cfg.retry.max_retries = 0;
    sh::detail::ClientEngine engine{co_await asio::this_coro::executor, cfg};

    auto req = std::make_shared<sh::Request>(sh::Version::Http11, co_await asio::this_coro::executor);
    req->set_method(sh::Method::Get);
    req->set_target("/");

    auto stream = co_await engine.stream(url, std::move(req));
    if (!stream) {
        std::printf("OPEN_FAILED %s\n", stream.error().message().c_str());
        co_return 2;
    }
    auto head = co_await (*stream)->read_head();
    if (!head) {
        std::printf("HEAD_FAILED %s\n", head.error().message().c_str());
        co_return 2;
    }
    // The streaming Response over the live exchange, with the caller's cap.
    sh::Response resp{std::move(*stream), std::move(*head), co_await asio::this_coro::executor, cap};
    auto bytes = co_await resp.bytes();
    if (!bytes) {
        std::printf("BYTES_FAILED %s\n", bytes.error().message().c_str());
        co_return bytes.error() == make_error_code(sh::client_errc::body_too_large) ? 0 : 2;
    }
    std::printf("BYTES_OK len=%zu\n", bytes->size());
    co_return 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: body_cap_check <url> <cap_bytes>\n");
        return 2;
    }
    const std::string url = argv[1];
    const std::size_t cap = static_cast<std::size_t>(std::stoull(argv[2]));

    asio::io_context ctx;
    asio::co_spawn(ctx, run(url, cap), [](std::exception_ptr ep, int rc) {
        if (ep)
            std::fprintf(stderr, "body_cap_check: exception\n");
        std::exit(rc);
    });
    ctx.run();
    return 1; // unreachable
}