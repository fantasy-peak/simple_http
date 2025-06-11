#include <chrono>
#include <cstdio>
#include <memory>
#include <ostream>
#include <string>
#include <iostream>
#include <variant>

#include <boost/beast/core/string_type.hpp>
#include "boost/asio/as_tuple.hpp"
#include "boost/asio/awaitable.hpp"
#include "boost/asio/co_spawn.hpp"
#include "boost/asio/detached.hpp"
#include "boost/asio/use_awaitable.hpp"

#include "simple_http.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

// export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
asio::awaitable<void> client()
{
    static simple_http::IoCtxPool pool{2};
    pool.start();

    asio::steady_timer timer(*pool.getMainContext());
    timer.expires_after(std::chrono::seconds(3));
    co_await timer.async_wait();

    std::shared_ptr<simple_http::HttpClient> client =
        std::make_shared<simple_http::HttpsClient>("learnrust.site",
                                                   6666,
                                                   pool.getIoContextPtr());
    assert(co_await client->start());

    auto ctx = pool.getMainContext();
    asio::co_spawn(
        *ctx,
        [=]() -> asio::awaitable<void> {
            auto req = simple_http::makeHttpRequest("/hello");
            req->set("X-Custom-Header", "value");
            req->body() = "client";
            auto ch = co_await client->sendRequest(req);
            std::cout << "recv http\n";
            for (;;)
            {
                auto [ec, data] = co_await ch->async_receive(
                    asio::as_tuple(asio::use_awaitable));
                if (ec)
                {
                    std::cout << ec.message() << std::endl;
                    break;
                }
                if (std::holds_alternative<simple_http::Disconnect>(data))
                {
                    std::cout << "Disconnect" << std::endl;
                    break;
                }
                else if (std::holds_alternative<std::string>(data))
                {
                    auto body_str = std::get<std::string>(data);
                    std::cout << "recv body:" << body_str << std::endl;
                }
                else
                {
                    auto res = std::get<http::response<http::empty_body>>(data);
                    for (const auto &field : res)
                    {
                        std::cout << field.name_string() << ": "
                                  << field.value() << "\n";
                    }
                }
            }
        },
        asio::detached);

    timer.expires_after(std::chrono::seconds(30));
    co_await timer.async_wait();

    co_await client->stop();

    co_return;
}

int main()
{
    simple_http::LOG_CB =
        [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
            std::cout << to_string(level) << " " << file << ":" << line << " "
                      << msg << std::endl;
        };
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), client(), asio::detached);
    while (true)
        sleep(1000);
    return 0;
}
