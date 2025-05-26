#include <chrono>
#include <cstdio>
#include <ostream>
#include <string>
#include <syncstream>

#include <boost/beast/core/string_type.hpp>

#include "boost/asio/as_tuple.hpp"
#include "boost/asio/use_awaitable.hpp"
#include "simple_http.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

asio::awaitable<void> start()
{
    simple_http::Config cfg{.ip = "0.0.0.0",
                            .port = 6666,
                            .worker_num = 4,
                            .concurrent_streams = 200};
    cfg.ssl_crt = "./v.crt";
    cfg.ssl_key = "./v.key";
    static simple_http::HttpServer hs(cfg);
    simple_http::LOG_CB =
        [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
            std::osyncstream out(std::cout);
            out << to_string(level) << " " << file << ":" << line << " " << msg
                << std::endl;
        };
    hs.setHttpHandler(
        "/hello", [](auto req, auto writer) -> asio::awaitable<void> {
            std::osyncstream out(std::cout);
            out << "Headers:\n";
            // std::println("meth: {}", std::string{req.method_string()});
            for (auto const &field : req)
            {
                out << field.name_string() << ": " << field.value() << "\n";
            }
            out << req.target() << std::endl;
            auto str = req.body();
            out << "body:" << str << std::endl;
            if (writer->version() == simple_http::Version::Http2)
            {
#if 0
                http::response<http::string_body> res;
                res.result(http::status::ok);
                res.set(http::field::content_type, "text/plain");
                res.body() = "hello h2";
                res.prepare_payload();
                writer->writeHttpResponse(
                    std::make_shared<http::response<http::string_body>>(res));
#else
                writer->writeHeader("content-type", "text/plain");
                writer->writeHeader(http::field::server, "test");
                writer->writeHeaderEnd();
                writer->writeBody("123");
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(4));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeBody("456");
                timer.expires_after(std::chrono::seconds(4));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeBodyEnd("789");
#endif
            }
            else
            {
#if 0
                http::response<http::string_body> res;
                res.version(11);
                res.result(http::status::ok );
                res.set(http::field::server, "MyBeastServer");
                res.set(http::field::content_type, "text/plain");
                res.body() = "hello world";
                res.prepare_payload();
                writer->writeHttpResponse(
                    std::make_shared<http::response<http::string_body>>(res));
#endif
                // curl --no-buffer  -v http://localhost:6666/hello -d "aaaa"
                http::response<http::empty_body> res{http::status::ok, 11};
                res.set(http::field::server, "BeastChunkedServer/1.0");
                res.set(http::field::content_type, "text/plain");
                res.set(http::field::transfer_encoding, "chunked");  // 关键字段
                res.keep_alive(true);
                writer->writeChunkHeader(res);
                writer->writeChunkData("123");
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(4));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeChunkData("456");
                timer.expires_after(std::chrono::seconds(4));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeChunkEnd();
            }
            co_return;
        });
    co_await hs.start();
}

// export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
asio::awaitable<void> client()
{
    static simple_http::IoCtxPool pool{2};
    pool.start();
    asio::steady_timer timer(*pool.getMainContext());
    timer.expires_after(std::chrono::seconds(3));
    co_await timer.async_wait();

    std::cout << "client1:" << std::this_thread::get_id() << std::endl;
    static simple_http::Http2Client client("learnrust.site",
                                           6666,
                                           pool.getIoContextPtr());
    co_await client.connect();
    co_await client.start();
    std::cout << "client2:" << std::this_thread::get_id() << std::endl;

    co_await asio::post(*pool.getMainContext(), asio::use_awaitable);
    std::cout << "client3:" << std::this_thread::get_id() << std::endl;

    auto ch = co_await client.send_post_request();
    std::cout << "send_post_request3:" << std::this_thread::get_id()
              << std::endl;

    auto [ec, data] =
        co_await ch->async_receive(asio::as_tuple(asio::use_awaitable));
    std::cout << "收到 http 头\n";
    auto res = std::get<http::response<http::empty_body>>(data);
    std::cout << "send_post_request4:" << std::this_thread::get_id()
              << std::endl;

    for (const auto &field : res)
    {
        std::cout << field.name_string() << ": " << field.value() << "\n";
    }
    for (;;)
    {
        auto [ec, body] =
            co_await ch->async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec)
        {
            std::cout << ec.message() << std::endl;
            break;
        }
        auto body_str = std::get<std::string>(body);
        std::cout << "recv body:" << body_str << std::endl;
    }
}

int main()
{
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), start(), asio::detached);
    asio::co_spawn(pool.getIoContext(), client(), asio::detached);
    while (true)
        sleep(1000);
    return 0;
}
