#include <chrono>
#include <memory>
#include <print>

#include "simple_http.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

// export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
asio::awaitable<void> client(simple_http::IoCtxPool& pool) {
    simple_http::HttpClientConfig cfg{
        .host = "127.0.0.1",
        .port = 7788,
        .concurrent_streams = 200,
        .use_tls = true,
        .verify_peer = true,
        .ssl_ca = "./test/tls_certificates/ca_cert.pem",
        .ssl_crt = "./test/tls_certificates/client_cert.pem",
        .ssl_key = "./test/tls_certificates/client_key.pem",
        .ssl_context = nullptr,
        .tlsext_host_name = "SimpleHttpServer",
    };
    // only support h2 and h2c not support http1.1
    auto client = std::make_shared<simple_http::Http2Client>(cfg, pool.getIoContextPtr());
    auto [ret, err] = co_await client->asyncStart(std::chrono::seconds(5), asio::use_awaitable);
    if (!ret) {
        std::println("{}", err);
        co_return;
    }
    auto stream_spec = std::make_shared<simple_http::StreamSpec>(http::verb::post, "/hello");
    stream_spec->writeHeader(http::field::content_type, simple_http::mime::text_plain);
    stream_spec->body() = "hi, im simple http client";
    auto opt = co_await client->openStream(stream_spec, asio::use_awaitable);
    if (!opt) {
        co_return;
    }
    auto& [w, r] = opt.value();
    w->writerBody(std::make_shared<std::string>("hello"), simple_http::WriteMode::More);
    w->writerBody(std::make_shared<std::string>("client"), simple_http::WriteMode::Last);

    auto [ec, d] = co_await r->asyncReadDataFrame();
    if (std::holds_alternative<simple_http::ParseHeaderDone>(d)) {
        std::println("recv ParseHeaderDone");
    }

    for (;;) {
        auto [ec, d] = co_await r->asyncReadDataFrame();

        if (ec) {
            std::println("read error: {}", ec.message());
            break;
        }

        bool should_continue = std::visit(simple_http::overloaded{[](std::unique_ptr<std::string> str_ptr) {
                                                                      std::println("recv data: {}", *str_ptr);
                                                                      return true;
                                                                  },
                                                                  [](simple_http::Eof) { return false; },
                                                                  [](simple_http::Disconnect) { return false; },
                                                                  [](simple_http::ParseHeaderDone) { return false; }},
                                          std::move(d));

        if (!should_continue) {
            break;
        }
    }

    client.reset();
    std::println("done");
    co_return;
}

int main() {
    simple_http::LOG_CB = [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
        std::println("{} {} {} {}", toString(level), file, line, msg);
    };
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), client(pool), [](const std::exception_ptr& ep) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::println("{}", e.what());
        } catch (...) {
            std::println("unknown exception");
        }
    });
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(100));
    return 0;
}
