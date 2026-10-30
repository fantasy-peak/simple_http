// Cross-validation of simple_http's HTTP/3 client against an independent
// server: test/python/h3_server.py (aioquic).
//
// The library's C++ suites otherwise drive simple_http with its own client, so
// a shared misreading of the spec cancels out. Here the client meets a
// completely separate HTTP/3 stack.
//
// Run (from the repository root):
//   test/python/.venv/bin/python test/python/h3_server.py 27922 &
//   xmake run h3_client_cross -- 27922
//
// Exit status is 0 iff every check passed.

#include <simple_http.h>

#include <boost/asio.hpp>
#include <chrono>
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

std::string describe(const error_code &ec) { return ec.message(); }
} // namespace

int main(int argc, char **argv) {
    set_log_sink(make_stdout_sink(LogLevel::Error));
    const std::string port = argc > 1 ? argv[1] : "27922";
    asio::io_context ctx;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            ClientConfig cfg;
            cfg.default_version = HttpVersionPolicy::Http3;
            cfg.request_timeout = std::chrono::seconds(10);
            cfg.response_head_timeout = std::chrono::seconds(10);
            cfg.idle_timeout = std::chrono::seconds(20);
            cfg.tls.ca_file = "./test/tls_certificates/ca_cert.pem";
            // The test certificate has no SAN; match its CN, exactly as the
            // TCP suites do.
            cfg.tls.sni_override = "SimpleHttpServer";

            Client client{co_await asio::this_coro::executor, cfg};
            const std::string base = "https://127.0.0.1:" + port;

            // --- GET ------------------------------------------------------
            auto hello = co_await client.get(base + "/hello").send();
            check(hello && hello->status() == 200,
                  "GET /hello returns 200" + (hello ? "" : ": " + describe(hello.error())));
            if (hello) {
                check(hello->version() == Version::Http3, "the response reports HTTP/3");
                auto body = co_await hello->text();
                check(body && *body == "hello h3", "GET /hello body is correct");
                check(hello->header("x-server").value_or("") == "aioquic-h3",
                      "the response came from the independent server");
            }

            // --- POST echo ------------------------------------------------
            auto echo = co_await client.post(base + "/echo").body("cross body").send();
            check(echo && echo->status() == 200,
                  "POST /echo returns 200" + (echo ? "" : ": " + describe(echo.error())));
            if (echo) {
                auto body = co_await echo->text();
                check(body && *body == "cross body", "POST /echo echoes the body");
                check(echo->header("x-method").value_or("") == "POST", "X-Method is POST");
            }

            // --- streaming request body ----------------------------------
            {
                auto stream = co_await client.open_stream(base + "/echo", StreamSpec{.method = Method::Put});
                check(stream.has_value(),
                      stream ? "streaming PUT opens" : "streaming PUT opens: " + describe(stream.error()));
                if (stream) {
                    check(!co_await stream->write("alpha-"), "stream write 1");
                    check(!co_await stream->write("beta"), "stream write 2");
                    check(!co_await stream->finish("gamma"), "stream finish");
                    auto head = co_await stream->read_head();
                    check(head && head->status == 200, "streaming PUT response head");
                    auto body = co_await stream->read_all();
                    check(body && *body == "alpha-betagamma",
                          "streamed request body round-trips" +
                              (body ? ": got '" + *body + "'" : ": " + describe(body.error())));
                    check(stream->version() == Version::Http3, "the stream reports HTTP/3");
                }
            }

            // --- Expect: 100-continue ------------------------------------
            {
                auto stream = co_await client.open_stream(
                    base + "/expect", StreamSpec{.method = Method::Post, .expect_100_continue = true});
                check(stream.has_value(), stream ? "expect-continue request opens"
                                                 : "expect-continue request opens: " + describe(stream.error()));
                if (stream) {
                    auto interim = co_await stream->read_head();
                    check(interim && interim->status == 100, "the interim 100 head is delivered");
                    check(!co_await stream->write("payload"), "the body is sent after the 100");
                    check(!co_await stream->finish(), "the body ends after the 100");
                    auto final_head = co_await stream->read_head();
                    check(final_head && final_head->status == 200, "the final head follows the 100");
                    // aioquic cannot put DATA behind a second HEADERS, so the
                    // server reports what it received in a header instead.
                    check(final_head && final_head->header("x-received").value_or("") == "7",
                          "the body sent after the 100 reached the server");
                    auto body = co_await stream->read_all();
                    check(body && body->empty(), "the headers-only final response has no body");
                }
            }

            // --- HEAD -----------------------------------------------------
            {
                auto head = co_await client.head(base + "/hello").send();
                bool empty = false;
                if (head) {
                    auto text = co_await head->text();
                    empty = text && text->empty();
                }
                check(head && head->status() == 200 && head->bodyless() && empty, "HEAD has the status and no body");
            }

            // --- large response ------------------------------------------
            {
                auto big = co_await client.get(base + "/big?n=200000").send();
                check(big && big->status() == 200, "GET /big returns 200");
                if (big) {
                    auto body = co_await big->read_all();
                    check(body && body->size() == 200000 && *body == std::string(200000, 'x'),
                          "200000-byte body is intact");
                }
            }

            // --- response trailers ---------------------------------------
            {
                auto stream = co_await client.open_stream(base + "/trailers");
                check(stream.has_value(),
                      stream ? "GET /trailers opens" : "GET /trailers opens: " + describe(stream.error()));
                if (stream) {
                    check(!co_await stream->finish(), "finish the bodyless GET request");
                    auto body = co_await stream->read_all();
                    check(body && *body == "trailer body", "trailer response body");
                    check(stream->trailers().get("x-checksum") == "abc123",
                          "the trailing HEADERS section reaches Stream::trailers()");
                }
            }

            // --- two concurrent streams on one connection ----------------
            {
                auto a = co_await client.open_stream(base + "/big?n=100000");
                auto b = co_await client.open_stream(base + "/hello");
                check(a.has_value() && b.has_value(), "two streams open concurrently");
                if (a && b) {
                    check(!co_await a->finish(), "finish concurrent stream 1");
                    check(!co_await b->finish(), "finish concurrent stream 2");
                    auto ab = co_await a->read_all();
                    auto bb = co_await b->read_all();
                    check(ab && ab->size() == 100000 && *ab == std::string(100000, 'x'),
                          "concurrent stream 1 body is intact");
                    check(bb && *bb == "hello h3", "concurrent stream 2 body is intact");
                }
            }

            // --- h3 requires https ----------------------------------------
            {
                auto bad = co_await client.get("http://127.0.0.1:" + port + "/hello").send();
                check(!bad && bad.error() == client_errc::version_not_negotiated,
                      "http:// with the HTTP/3 policy is refused");
            }

            // --- one connection served every request ----------------------
            check(client.stats().connections_opened == 1, "all requests reused one QUIC connection (opened=" +
                                                              std::to_string(client.stats().connections_opened) + ")");
        },
        asio::detached);

    ctx.run();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}
