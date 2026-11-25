// HTTP/3 0-RTT client: one normal request to obtain a ticket, then a GET_0RTT
// request on a fresh connection. Prints machine-readable observations for the
// Python differential runner.
//
// The optional fourth argument switches to rejection mode: the runner replaces
// the server with a fresh process (different ticket keys) while this client
// sleeps, so the second request's early data is refused and the client must
// report client_errc::zero_rtt_rejected.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "simple_http.h"
#include <simple_http/core/asio_compat.h>

namespace asio = simple_http::asio;
using namespace simple_http;
using namespace std::chrono_literals;

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: client <url> [expect_early=1] [client_0rtt=1] [expect_reject=0]\n";
        return 2;
    }
    const std::string url = argv[1];
    const bool expect_early = argc > 2 ? std::atoi(argv[2]) != 0 : true;
    const bool client_0rtt = argc > 3 ? std::atoi(argv[3]) != 0 : true;
    const bool expect_reject = argc > 4 ? std::atoi(argv[4]) != 0 : false;
    asio::io_context ioc;
    ClientConfig cfg;
    cfg.proxy_from_env = false;
    cfg.default_version = HttpVersionPolicy::Http3;
    cfg.h3_enable_0rtt = client_0rtt;
    cfg.tls.verify_peer = false;
    cfg.tls.verify_host = false;
    Client client{ioc.get_executor(), cfg};

    int ok = 0;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            {
                auto [ec, r] = co_await client.get(url).send(asio::as_tuple(asio::use_awaitable));
                if (ec) {
                    std::cout << "first_error=" << ec.message() << "\n";
                    ioc.stop();
                    co_return;
                }
                // Flush: the runner watches this line live to swap the server
                // while this process sleeps, and stdout to a pipe is buffered.
                std::cout << "first=" << r.text() << "\n" << std::flush;
            }
            // Give the NewSessionTicket time to arrive and cache. In rejection
            // mode this is also the window in which the runner swaps the server
            // for one whose ticket keys do not match. The window is generous so
            // a slow server teardown (LSan at process exit) cannot make the
            // second request race the replacement server.
            const auto wait = expect_reject ? std::chrono::milliseconds{15000} : std::chrono::milliseconds{1200};
            co_await asio::steady_timer(co_await asio::this_coro::executor, wait).async_wait(asio::use_awaitable);
            client.close();
            auto [ec, r] = co_await client.get_0rtt(url).send(asio::as_tuple(asio::use_awaitable));
            if (expect_reject) {
                if (ec == make_error_code(client_errc::zero_rtt_rejected)) {
                    std::cout << "second=rejected\n";
                    ok = 1;
                } else if (ec) {
                    std::cout << "second_error=" << ec.message() << "\n";
                    ok = 0;
                } else {
                    // Diagnostics: an accepted early request and a plain 1-RTT
                    // fallback both complete without an error.
                    std::cout << "second=" << r.text() << " used_0rtt=" << (r.used_0rtt() ? 1 : 0) << "\n";
                    ok = 0;
                }
                client.close();
                ioc.stop();
                co_return;
            }
            if (ec) {
                std::cout << "second_error=" << ec.message() << "\n";
                ioc.stop();
                co_return;
            }
            std::cout << "second=" << r.text() << "\n";
            std::cout << "used_0rtt=" << (r.used_0rtt() ? 1 : 0) << "\n";
            ok = expect_early ? (r.used_0rtt() && r.text() == "early") : (!r.used_0rtt() && r.text() == "normal");
            ok = ok ? 1 : 0;
            client.close();
            ioc.stop();
        },
        asio::detached);
    ioc.run();
    return ok ? 0 : 1;
}
