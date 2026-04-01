#ifdef SIMPLE_HTTP_USE_MODULES
#include <nghttp2/nghttp2.h>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket/ssl.hpp>

import std;
import simple_http;
#else
#include <optional>
#include <print>
#include <sstream>
#include <string>

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/x509.h>

#include "simple_http.h"
#endif

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

asio::awaitable<void> hello(std::shared_ptr<simple_http::HttpRequestReader> reader,
                            std::shared_ptr<simple_http::HttpResponseWriter> writer) {
    std::stringstream ss;
    ss << "Headers:\n";
    for (auto const& [name, value] : reader->header()) {
        ss << "   " << name << ": " << value << "\n";
    }
    ss << reader->target() << "\n";
    std::println("{}", ss.str());
    if (writer->version() == simple_http::Version::Http2) {
        // for http2 stream recv
        for (;;) {
            auto [ec, data] = co_await reader->asyncReadDataFrame();

            if (ec) {
                std::println("Error: {}", ec.message());
                co_return;
            }

            bool should_continue = std::visit(simple_http::overloaded{[](std::string str) {
                                                                          std::println("recv h2 data frame: {}", str);
                                                                          return true;
                                                                      },
                                                                      [](simple_http::Disconnect) { return false; },
                                                                      [](simple_http::Rst) { return false; },
                                                                      [](simple_http::Eof) { return false; }},
                                              std::move(data));

            if (!should_continue) {
                break;
            }
        }

        writer->writeHeader(http::field::content_type, simple_http::mime::text_plain);
        writer->writeHeader(http::field::server, simple_http::server_version);
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
        auto [stream_status, body] = co_await reader->asyncReadBody();
        if (stream_status == simple_http::StreamStatus::Disconnect) {
            std::println("client disconnect");
            co_return;
        }
        std::println("recv http1 data :", body.get());
        // curl --no-buffer  -v http://localhost:6666/hello -d "aaaa"
        http::response<http::empty_body> res{http::status::ok, 11};
        res.set(http::field::server, simple_http::server_version);
        res.set(http::field::content_type, simple_http::mime::text_plain);
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
}

asio::awaitable<void> world(std::shared_ptr<simple_http::HttpRequestReader> reader,
                            std::shared_ptr<simple_http::HttpResponseWriter> writer) {
    auto res = simple_http::makeHttpResponse(http::status::ok);
    res->body() = writer->version() == simple_http::Version::Http2 ? "/world http2 body" : "/world http1.1 body";
    writer->writeHttpResponse(res);
    co_return;
}

asio::awaitable<void> tls(std::shared_ptr<simple_http::HttpRequestReader> reader,
                          std::shared_ptr<simple_http::HttpResponseWriter> writer,
                          std::optional<asio::ssl::stream<asio::ip::tcp::socket>::native_handle_type> ssl_ctx) {
    auto response = simple_http::makeHttpResponse(http::status::ok);

    if (ssl_ctx) {
        const auto& x509_server_ref = SSL_get_certificate(*ssl_ctx);
        auto server_subject_name = certificate_subject_name(x509_server_ref);

        const auto& x509_client_ref = SSL_get_peer_certificate(*ssl_ctx);
        auto client_subject_name = certificate_subject_name(x509_client_ref);

        response->body() =
            std::format("server subject: {}, client subject: {}", server_subject_name, client_subject_name);
    } else {
        response->body() = "no TLS!";
    }

    writer->writeHttpResponse(response);

    co_return;
}

asio::awaitable<void> start() {
    simple_http::Config cfg{
        .ip = "0.0.0.0",
        .port = 7788,
        .worker_num = 8,
        .concurrent_streams = 200,
        .window_size = std::nullopt,
        .max_frame_size = std::nullopt,
        .ssl_crt = "./test/tls_certificates/server_cert.pem",
        .ssl_key = "./test/tls_certificates/server_key.pem",
        .ssl_mutual = true,
        .ssl_ca = "./test/tls_certificates/ca_cert.pem",
        .socket_setup_cb =
            [](asio::ip::tcp::socket& socket) {
                // Set socket properties
                socket.set_option(asio::socket_base::keep_alive(true));
            },
        .enable_ipv6 = true,
        .ipv6_addr = "::1",
        .ipv6_port = 7788,
        .unix_socket = std::nullopt,
        .websocket_setup_cb = [](auto socket) { std::visit([](auto&& arg) { arg->compress(false); }, socket); },
    };
    simple_http::LOG_CB = [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
        std::println("{} {} {} {}", toString(level), file, line, msg);
    };
    simple_http::HttpServer hs(cfg);
    hs.setHttpHandler("/hello", hello);
    hs.setHttpHandler("/world", world).setHttpHandler("/tls", tls);
    hs.setHttpRegexHandler(".*",
                           [](std::shared_ptr<simple_http::HttpRequestReader> reader,
                              std::shared_ptr<simple_http::HttpResponseWriter> writer) -> asio::awaitable<void> {
                               // This approach will unify HTTP1.1 and HTTP2 streaming responses.
                               writer->writeStatus(200);
                               writer->writeHeader(http::field::content_type, simple_http::mime::text_plain);
                               writer->writeStreamHeaderEnd();
                               writer->writeStreamBody("regex matched");
                               writer->writeStreamEnd();
                               co_return;
                           });
    hs.setWebsocketHandler("/wss",
                           [](const http::request<http::string_body>& req,
                              const simple_http::WssStreamPtr& wss_socket_ptr) -> asio::awaitable<bool> {
                               auto& stream = wss_socket_ptr->stream();
                               stream->text(true);
                               std::string data{"hello world"};
                               co_await stream->async_write(asio::buffer(data), asio::use_awaitable);
                               co_return true;
                           });
    std::println("started http server");
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
            std::println("Exception caught by co_spawn handler: {}", e.what());
        }
    });
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(100));
    return 0;
}
