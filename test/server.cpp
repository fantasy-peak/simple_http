#include <chrono>
#include <cstdio>
#include <ostream>
#include <string>
#include <iostream>

#include <boost/url.hpp>

#include "simple_http.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

asio::awaitable<void> start()
{
    simple_http::Config cfg{.ip = "0.0.0.0",
                            .port = 6666,
                            .worker_num = 8,
                            .concurrent_streams = 200};
    cfg.ssl_crt = "./v.crt";
    cfg.ssl_key = "./v.key";
    simple_http::HttpServer hs(cfg);
    simple_http::LOG_CB =
        [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
            std::cout << to_string(level) << " " << file << ":" << line << " "
                      << msg << std::endl;
        };
    hs.setBefore(
        [](const auto &req, const auto &writer) -> asio::awaitable<bool> {
#if 0
        boost::urls::url_view urlv =
            boost::urls::parse_origin_form(req.target()).value();
        if (urlv.path() != "/hello")
        {
            auto res = simple_http::makeHttpResponse(http::status::bad_request);
            res->prepare_payload();
            writer->writeHttpResponse(res);
            co_return false;
        }
        for (auto const &param : urlv.params())
        {
            std::cout << param.key << " = " << param.value << "\n";
        }
#endif
            co_return true;
        });
    hs.setHttpHandler(
        "/hello", [](auto req, auto writer) -> asio::awaitable<void> {
            std::cout << "Headers:" << std::endl;
            // std::println("meth: {}", std::string{req.method_string()});
            for (auto const &field : req)
            {
                std::cout << field.name_string() << ": " << field.value()
                          << "\n";
            }
            std::cout << req.target() << std::endl;
            auto str = req.body();
            std::cout << "body:" << str << std::endl;
            if (writer->version() == simple_http::Version::Http2)
            {
#if 0
                auto res = simple_http::makeHttpResponse(http::status::ok);
                res->body() = "hello h2";
                writer->writeHttpResponse(res);
#else
                writer->writeHeader("content-type", "text/plain");
                writer->writeHeader(http::field::server, "test");
                writer->writeHeaderEnd();
                writer->writeBody("123");
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeBody("456");
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeBodyEnd("789");
#endif
            }
            else
            {
#if 0
                auto res = simple_http::makeHttpResponse(http::status::ok);
                res->body() = "hello world";
                res->prepare_payload();
                writer->writeHttpResponse(res);
#else
                // curl --no-buffer  -v http://localhost:6666/hello -d "aaaa"
                http::response<http::empty_body> res{http::status::ok, 11};
                res.set(http::field::server, "simple_http_server");
                res.set(http::field::content_type, "text/plain");
                res.set(http::field::transfer_encoding, "chunked");  // 关键字段
                res.keep_alive(true);
                writer->writeChunkHeader(res);
                writer->writeChunkData("123");
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeChunkData("456");
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                writer->writeChunkEnd();
#endif
            }
            co_return;
        });
    hs.setHttpHandler("/world",
                      [](auto /* req */, auto writer) -> asio::awaitable<void> {
                          auto res =
                              simple_http::makeHttpResponse(http::status::ok);
                          res->body() =
                              writer->version() == simple_http::Version::Http2
                                  ? "/world http2 body"
                                  : "/world http1.1 body";
                          res->prepare_payload();
                          writer->writeHttpResponse(res);
                          co_return;
                      })
        .setHttpRegexHandler(
            ".*", [](auto /* req */, auto writer) -> asio::awaitable<void> {
                auto res = simple_http::makeHttpResponse(http::status::ok);
                res->body() = "regex matched";
                res->prepare_payload();
                writer->writeHttpResponse(res);
                co_return;
            });
    co_await hs.start();
}

int main()
{
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), start(), asio::detached);
    while (true)
        sleep(1000);
    return 0;
}
