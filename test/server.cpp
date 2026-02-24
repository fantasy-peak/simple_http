#include <chrono>
#include <iostream>
#include <ostream>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/x509.h>

#include "simple_http.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

// Taken from: https://gist.github.com/cseelye/adcd900768ff61f697e603fd41c67625
auto certificate_subject_name(const X509* x509_cert) -> std::string {
    auto constexpr max_len = 4096;
    char buffer[max_len];
    memset(buffer, 0, max_len);

    const auto& x509_name = X509_get_subject_name(x509_cert);

    auto output_bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);

    X509_NAME_print_ex(output_bio.get(), x509_name, 0, 0);
    BIO_read(output_bio.get(), buffer, max_len - 1);

    return std::string(buffer);
}

asio::awaitable<void> start() {
    simple_http::Config cfg{
        .ip = "0.0.0.0",
        .port = 7788,
        .worker_num = 8,
        .concurrent_streams = 200,
        .ssl_crt = "./test/tls_certificates/server_cert.pem",
        .ssl_key = "./test/tls_certificates/server_key.pem",
        .ssl_mutual = true,
        .ssl_ca = "./test/tls_certificates/ca_cert.pem",
        .set_socket_option =
            [](asio::ip::tcp::socket& socket) {
                // Set socket properties
                socket.set_option(asio::socket_base::keep_alive(true));
            },
    };
    simple_http::LOG_CB = [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
        std::cout << to_string(level) << " " << file << ":" << line << " " << msg << std::endl;
    };
    simple_http::HttpServer hs(cfg);
    hs.setHttpHandler("/hello",
                      [](http::request<http::string_body> req,
                         std::shared_ptr<simple_http::HttpResponseWriter> writer) -> asio::awaitable<void> {
                          std::cout << "Headers:" << std::endl;
                          // std::println("meth: {}", std::string{req.method_string()});
                          for (auto const& field : req) {
                              std::cout << field.name_string() << ": " << field.value() << "\n";
                          }
                          std::cout << req.target() << std::endl;
                          auto str = req.body();
                          std::cout << "request body:" << str << std::endl;
                          if (writer->version() == simple_http::Version::Http2) {
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
                          } else {
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
    hs.setHttpHandler("/world",
                      [](http::request<http::string_body> /* req */,
                         std::shared_ptr<simple_http::HttpResponseWriter> writer) -> asio::awaitable<void> {
                          auto res = simple_http::makeHttpResponse(http::status::ok);
                          res->body() = writer->version() == simple_http::Version::Http2 ? "/world http2 body"
                                                                                         : "/world http1.1 body";
                          res->prepare_payload();
                          writer->writeHttpResponse(res);
                          co_return;
                      })
        .setHttpRegexHandler(".*",
                             [](http::request<http::string_body> /* req */,
                                std::shared_ptr<simple_http::HttpResponseWriter> writer) -> asio::awaitable<void> {
                                 auto res = simple_http::makeHttpResponse(http::status::ok);
                                 res->body() = "regex matched";
                                 res->prepare_payload();
                                 writer->writeHttpResponse(res);
                                 co_return;
                             });

    hs.setHttpHandler("/tls",
                      [](http::request<http::string_body> /* req */,
                         std::shared_ptr<simple_http::HttpResponseWriter> writer,
                         std::optional<asio::ssl::stream<asio::ip::tcp::socket>::native_handle_type> ssl_ctx)
                          -> asio::awaitable<void> {
                          auto response = simple_http::makeHttpResponse(simple_http::http::status::ok);

                          if (ssl_ctx) {
                              const auto& x509_server_ref = SSL_get_certificate(*ssl_ctx);
                              auto server_subject_name = certificate_subject_name(x509_server_ref);

                              const auto& x509_client_ref = SSL_get_peer_certificate(*ssl_ctx);
                              auto client_subject_name = certificate_subject_name(x509_client_ref);

                              response->body() = std::format("server subject: {}, client subject: {}",
                                                             server_subject_name,
                                                             client_subject_name);
                          } else {
                              response->body() = "no TLS!";
                          }

                          writer->writeHttpResponse(response);

                          co_return;
                      });

    std::cout << "started http server\n";
    co_await hs.start();
}

int main() {
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), start(), [](const std::exception_ptr& eptr) {
        try {
            if (eptr)
                std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            std::cout << "Exception caught by co_spawn handler: " << e.what() << std::endl;
        }
    });
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(100));
    return 0;
}
