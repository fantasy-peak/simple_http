// Cross-validation of simple_http's client against an independent HTTP server:
// test/python/http_server.py (stdlib `http.server`).
//
// The library's own suites drive simple_http's server with simple_http's own
// client, so a spec misreading shared by both halves would cancel out. Here the
// client meets a server written by someone else; a disagreement means one of
// the two is wrong.
//
// Run:
//   test/python/.venv/bin/python test/python/http_server.py 27921 &
//   xmake run client_cross -- 27921
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
    const std::string port = argc > 1 ? argv[1] : "27921";
    asio::io_context ctx;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            Client client{}; // default policy
            const std::string base = "http://127.0.0.1:" + port;

            // GET round-trip.
            auto r = co_await client.get(base + "/hello").send();
            check(r && r->status() == 200, "GET /hello returns 200");
            if (r) {
                auto body = co_await r->text();
                check(body && *body == "hello cross", "GET /hello body is correct");
            }

            // POST echo.
            auto e = co_await client.post(base + "/echo").body("cross body").send();
            check(e && e->status() == 200, "POST /echo returns 200");
            if (e) {
                auto body = co_await e->text();
                check(body && *body == "cross body", "POST /echo echoes the body");
            }

            // A body larger than one TCP packet, streamed back.
            auto big = co_await client.get(base + "/big?n=200000").send();
            check(big && big->status() == 200, "GET /big returns 200");
            if (big) {
                auto body = co_await big->read_all();
                check(body && body->size() == 200000 && *body == std::string(200000, 'x'),
                      "200 KiB streamed response is intact");
            }

            // Status codes map through.
            auto nf = co_await client.get(base + "/status/404").send();
            check(nf && nf->status() == 404, "GET /status/404 maps to 404");
            auto sr = co_await client.get(base + "/status/500").send();
            check(sr && sr->status() == 500, "GET /status/500 maps to 500");

            // Redirect following: 302 → /hello. Convenience layer follows by
            // default only when max_redirects > 0 — set it.
            {
                Client following{ClientConfig{.max_redirects = 5}};
                auto redir = co_await following.get(base + "/redirect").send();
                check(redir && redir->status() == 200, "a 302 redirect is followed");
                if (redir) {
                    auto body = co_await redir->text();
                    check(body && *body == "hello cross", "redirect lands on /hello");
                }
                auto loop = co_await following.get(base + "/redirect-loop").send();
                check(!loop, "a redirect loop reports an error (not an infinite loop)");
            }

            // Cookies: the jar stores the Set-Cookie and replays it.
            {
                auto jar = std::make_shared<CookieJar>();
                Client with_jar{ClientConfig{.cookie_jar = jar}};
                auto c = co_await with_jar.get(base + "/set-cookie").send();
                check(c && c->status() == 200 && jar->size() == 1, "Set-Cookie is stored in the jar");
            }

            // Connection reuse across requests (the server is keep-alive).
            {
                Client reuse{};
                auto a = co_await reuse.get(base + "/hello").send();
                auto b = co_await reuse.get(base + "/hello").send();
                check(a && b && a->status() == 200 && b->status() == 200,
                      "two requests reuse the connection (stats:");
                auto st = reuse.stats();
                std::printf("        opened=%zu reused=%zu\n", st.connections_opened, st.connections_reused);
            }
        },
        asio::detached);

    ctx.run();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}