// HTTP/3 0-RTT client: one normal request to obtain a ticket, then a GET_0RTT
// request on a fresh connection. Prints machine-readable observations for the
// Python differential runner.
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
        std::cerr << "usage: client <url> [expect_early=1] [client_0rtt=1]\n";
        return 2;
    }
    const std::string url = argv[1];
    const bool expect_early = argc > 2 ? std::atoi(argv[2]) != 0 : true;
    const bool client_0rtt = argc > 3 ? std::atoi(argv[3]) != 0 : true;
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
                std::cout << "first=" << r.text() << "\n";
            }
            co_await asio::steady_timer(co_await asio::this_coro::executor, 1200ms).async_wait(asio::use_awaitable);
            client.close();
            auto [ec, r] = co_await client.get_0rtt(url).send(asio::as_tuple(asio::use_awaitable));
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
