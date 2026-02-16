#include <chrono>
#include <iostream>
#include <ostream>
#include <string>
#include <thread>

#include "simple_http.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

asio::awaitable<void> start()
{
    simple_http::Config cfg{
        .ip = "0.0.0.0",
        .port = 7788,
        .worker_num = 8,
        .concurrent_streams = 200,
        .ssl_crt = "./test/tls_certificates/cert.pem",
        .ssl_key = "./test/tls_certificates/key.pem",
    };
    simple_http::LOG_CB =
        [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
            std::cout << to_string(level) << " " << file << ":" << line << " "
                      << msg << std::endl;
        };
    simple_http::HttpServer hs(cfg);
    hs.setHttpHandler(
        "/hello",
        [](http::request<http::string_body> req,
           std::shared_ptr<simple_http::HttpResponseWriter> writer)
            -> asio::awaitable<void> {
            std::cout << "Headers:" << std::endl;
            // std::println("meth: {}", std::string{req.method_string()});
            for (auto const &field : req)
            {
                std::cout << field.name_string() << ": " << field.value()
                          << "\n";
            }
            std::cout << req.target() << std::endl;
            auto str = req.body();
            std::cout << "request body:" << str << std::endl;
            if (writer->version() == simple_http::Version::Http2)
            {
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
            }
            else
            {
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
            }
            co_return;
        });
    hs.setHttpHandler(
          "/world",
          [](http::request<http::string_body> /* req */,
             std::shared_ptr<simple_http::HttpResponseWriter> writer)
              -> asio::awaitable<void> {
              auto res = simple_http::makeHttpResponse(http::status::ok);
              res->body() = writer->version() == simple_http::Version::Http2
                                ? "/world http2 body"
                                : "/world http1.1 body";
              res->prepare_payload();
              writer->writeHttpResponse(res);
              co_return;
          })
        .setHttpRegexHandler(
            ".*",
            [](http::request<http::string_body> /* req */,
               std::shared_ptr<simple_http::HttpResponseWriter> writer)
                -> asio::awaitable<void> {
                auto res = simple_http::makeHttpResponse(http::status::ok);
                res->body() = "regex matched";
                res->prepare_payload();
                writer->writeHttpResponse(res);
                co_return;
            });
    std::cout << "started http server\n";
    co_await hs.start();
}

int main()
{
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(),
                   start(),
                   [](const std::exception_ptr &eptr) {
                       try
                       {
                           if (eptr)
                               std::rethrow_exception(eptr);
                       }
                       catch (const std::exception &e)
                       {
                           std::cout << "Exception caught by co_spawn handler: "
                                     << e.what() << std::endl;
                       }
                   });
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(100));
    return 0;
}
