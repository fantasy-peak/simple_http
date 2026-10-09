// Cross-validation of the client-side WebSocket (client/ws_client.h) against an
// independent RFC 6455 implementation: test/python/ws_echo_server.py (the
// `websockets` library).
//
// The library's own suites drive simple_http's server with simple_http's own
// client (test/client.cpp's /echo echoes through the same ws_frame/websocket
// code), so a spec misreading shared by both halves would cancel out. Here the
// client meets a server written by someone else; a disagreement means one of
// the two is wrong.
//
// Run:
//   test/python/.venv/bin/python test/python/ws_echo_server.py 27920 &
//   xmake run ws_cross -- 27920
//
// Exit status is 0 iff every check passed.

#include <simple_http.h>

#include <boost/asio.hpp>
#include <cstdio>
#include <string>

namespace asio = boost::asio;
using namespace simple_http;

namespace {
int g_failed = 0;
int g_checks = 0;

void check(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok)
        ++g_failed;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
}
} // namespace

int main(int argc, char **argv) {
    const std::string port = argc > 1 ? argv[1] : "27920";
    asio::io_context ctx;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            Client client{co_await asio::this_coro::executor}; // default policy; plaintext 127.0.0.1:port
            const std::string url = "ws://127.0.0.1:" + port + "/echo";

            auto ws = co_await client.open_websocket(url);
            if (!ws) {
                check(false, "open_websocket handshake: " + ws.error().message());
                co_return;
            }
            check(ws.has_value(), "handshake: ws:// upgrade to an independent server succeeds");

            // Text echo.
            if (auto ec = co_await (*ws)->write_text("hello cross"); ec) {
                check(false, "write_text: " + ec.message());
                co_return;
            }
            auto t = co_await (*ws)->read();
            check(t && t->text && t->data == "hello cross", "text echo round-trips (got " + (t ? t->data : "") + ")");

            // Binary echo keeps the opcode.
            const std::string bin{"\x00\x01\xfe\xff", 4};
            if (auto ec = co_await (*ws)->write_binary(bin); ec) {
                check(false, "write_binary: " + ec.message());
                co_return;
            }
            auto b = co_await (*ws)->read();
            check(b && !b->text && b->data == bin, "binary echo keeps type and bytes");

            // A larger message (crosses the client's frame header boundary).
            std::string big(70000, 'x');
            if (auto ec = co_await (*ws)->write_text(big); ec) {
                check(false, "write_text(70k): " + ec.message());
                co_return;
            }
            auto big_back = co_await (*ws)->read();
            check(big_back && big_back->text && big_back->data == big, "70 KiB message round-trips intact");

            // Graceful close: the server then sees the close handshake.
            if (auto ec = co_await (*ws)->close(); ec) {
                check(false, "close: " + ec.message());
                co_return;
            }
            auto after = co_await (*ws)->read();
            check(!after && !(*ws)->is_open(), "read() after close reports the end; is_open() is false");
        },
        asio::detached);

    ctx.run();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}