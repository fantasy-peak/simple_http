// C++17 client interop driver: exercises the public Client against the Python
// reference server started by client_interop.py. Argument: the server port.
//
// Checks: GET, POST with a body, redirect following, Set-Cookie handling and
// the cookie jar. Prints one PASS/FAIL line per check and exits non-zero when
// any failed.

#include <simple_http/client/http.h>
#include <simple_http/client/multipart_stream.h>
#include <simple_http/client/sse.h>

#include <boost/asio/io_context.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 2;
    }
    const std::string base = "http://127.0.0.1:" + std::string{argv[1]};
    const std::string tls_port = argc > 2 ? std::string{argv[2]} : std::string{};
    const std::string ws_port = argc > 3 ? std::string{argv[3]} : std::string{};
    const std::string proxy_port = argc > 4 ? std::string{argv[4]} : std::string{};
    const std::string socks_port = argc > 5 ? std::string{argv[5]} : std::string{};

    boost::asio::io_context context;
    simple_http::ClientConfig config;
    config.max_redirects = 5;
    config.cookie_jar = std::make_shared<simple_http::CookieJar>();
    auto client = std::make_shared<simple_http::Client>(context.get_executor(), config);
    std::shared_ptr<simple_http::Client> tls_client;
    std::shared_ptr<simple_http::Client> h2_client;
    std::shared_ptr<simple_http::Client> proxy_client;
    std::shared_ptr<simple_http::Client> socks_client;
    std::shared_ptr<simple_http::WebSocket> ws_keep;

    int failures = 0;
    auto check = [&failures](bool ok, const char *what) {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        std::fflush(stdout);
        if (!ok)
            ++failures;
    };
    auto finish_all = [&] {
        client->close();
        if (tls_client)
            tls_client->close();
        if (h2_client)
            h2_client->close();
        if (proxy_client)
            proxy_client->close();
        if (socks_client)
            socks_client->close();
    };
    auto ws_check = [&] {
        if (ws_port.empty()) {
            finish_all();
            return;
        }
        client->async_open_websocket(
            "ws://127.0.0.1:" + ws_port + "/ws", simple_http::WebSocketSpec{},
            [&](simple_http::error_code ec, std::shared_ptr<simple_http::WebSocket> websocket) {
                if (ec || !websocket) {
                    check(false, "client websocket upgrade");
                    finish_all();
                    return;
                }
                ws_keep = websocket;
                websocket->async_write_text("ws-client", [&, websocket](simple_http::error_code write_ec) {
                    if (write_ec) {
                        check(false, "client websocket write");
                        finish_all();
                        return;
                    }
                    websocket->async_read(
                        [&, websocket](simple_http::error_code read_ec, simple_http::WsMessage message) {
                            check(!read_ec && message.text && message.data == "ws-client", "client websocket echo");
                            websocket->async_close([&](simple_http::error_code) { finish_all(); });
                        });
                });
            });
    };
    auto multipart_check = [&] {
        if (ws_port.empty()) {
            ws_check();
            return;
        }
        simple_http::MultipartStreamWriter::async_open(
            *client, "http://127.0.0.1:" + ws_port + "/echo", simple_http::MultipartStreamOptions{},
            [&](simple_http::error_code open_ec, std::shared_ptr<simple_http::MultipartStreamWriter> writer) {
                if (open_ec || !writer) {
                    check(false, "client multipart open");
                    ws_check();
                    return;
                }
                writer->async_field("title", "hello", [&, writer](simple_http::error_code field_ec) {
                    if (field_ec) {
                        check(false, "client multipart field");
                        ws_check();
                        return;
                    }
                    writer->async_file_data(
                        "upload", "a.txt", "filedata", [&, writer](simple_http::error_code file_ec) {
                            if (file_ec) {
                                check(false, "client multipart file");
                                ws_check();
                                return;
                            }
                            writer->async_finish([&, writer](simple_http::error_code finish_ec) {
                                if (finish_ec) {
                                    check(false, "client multipart finish");
                                    ws_check();
                                    return;
                                }
                                auto stream = writer->stream();
                                stream->async_read_head([&, stream](simple_http::error_code head_ec,
                                                                    simple_http::detail::ResponseHead head) {
                                    if (head_ec) {
                                        check(false, "client multipart head");
                                        ws_check();
                                        return;
                                    }
                                    stream->async_read_all(
                                        0, [&, head](simple_http::error_code read_ec, std::string body) {
                                            check(!read_ec && head.status == 200 &&
                                                      body.find("hello") != std::string::npos &&
                                                      body.find("filedata") != std::string::npos,
                                                  "client multipart streaming upload");
                                            ws_check();
                                        });
                                });
                            });
                        });
                });
            });
    };
    auto sse_check = [&] {
        simple_http::SseStream::async_open(
            *client, base + "/sse", simple_http::SseOptions{},
            [&](simple_http::error_code open_ec, std::shared_ptr<simple_http::SseStream> sse) {
                if (open_ec || !sse) {
                    check(false, "client sse open");
                    multipart_check();
                    return;
                }
                sse->async_next([&, sse](simple_http::error_code first_ec, simple_http::SseEvent first) {
                    if (first_ec) {
                        check(false, "client sse first event");
                        multipart_check();
                        return;
                    }
                    sse->async_next([&, sse, first](simple_http::error_code second_ec, simple_http::SseEvent second) {
                        check(second_ec == simple_http::error_code{} && first.data == "one" && second.data == "two",
                              "client sse events");
                        sse->abort();
                        multipart_check();
                    });
                });
            });
    };
    auto stream_gzip_check = [&] {
        client->async_open_stream(
            base + "/gzip", simple_http::StreamSpec{}, 0,
            [&](simple_http::error_code open_ec, std::shared_ptr<simple_http::Stream> stream) {
                if (open_ec || !stream) {
                    check(false, "client streaming gzip open");
                    ws_check();
                    return;
                }
                stream->async_finish(std::string{}, [&, stream](simple_http::error_code finish_ec) {
                    if (finish_ec) {
                        check(false, "client streaming gzip finish");
                        ws_check();
                        return;
                    }
                    stream->async_read_head(
                        [&, stream](simple_http::error_code head_ec, simple_http::detail::ResponseHead head) {
                            if (head_ec) {
                                check(false, "client streaming gzip head");
                                ws_check();
                                return;
                            }
                            stream->async_read_all(0, [&, head](simple_http::error_code read_ec, std::string body) {
                                check(!read_ec && body == "hello gzip" && head.uncompressed,
                                      "client streaming gzip decode");
                                sse_check();
                            });
                        });
                });
            });
    };
    auto gzip_check = [&] {
        client->get(base + "/gzip").send([&](simple_http::error_code gzip_ec, simple_http::Response gzip_response) {
            check(!gzip_ec && gzip_response.text() == "hello gzip" && gzip_response.uncompressed(),
                  "client auto-decompresses gzip");
            stream_gzip_check();
        });
    };

    auto socks_check = [&] {
        if (socks_port.empty()) {
            gzip_check();
            return;
        }
        simple_http::ClientConfig socks_config;
        simple_http::ProxyConfig socks;
        socks.kind = simple_http::ProxyKind::Socks5;
        socks.host = "127.0.0.1";
        socks.port = static_cast<std::uint16_t>(std::stoi(socks_port));
        socks_config.proxy = std::move(socks);
        socks_client = std::make_shared<simple_http::Client>(context.get_executor(), socks_config);
        socks_client->get(base + "/hello").send([&](simple_http::error_code socks_ec, simple_http::Response proxied) {
            if (socks_ec) {
                std::fprintf(stderr, "socks request error: %s\n", socks_ec.message().c_str());
            }
            check(!socks_ec && proxied.status() == 200 && proxied.text() == "hello",
                  "client GET through a SOCKS5 proxy");
            gzip_check();
        });
    };

    auto proxy_check = [&] {
        if (proxy_port.empty()) {
            socks_check();
            return;
        }
        simple_http::ClientConfig proxy_config;
        simple_http::ProxyConfig proxy;
        proxy.kind = simple_http::ProxyKind::Http;
        proxy.host = "127.0.0.1";
        proxy.port = static_cast<std::uint16_t>(std::stoi(proxy_port));
        proxy_config.proxy = std::move(proxy);
        proxy_client = std::make_shared<simple_http::Client>(context.get_executor(), proxy_config);
        proxy_client->get(base + "/hello").send([&](simple_http::error_code proxy_ec, simple_http::Response proxied) {
            if (proxy_ec) {
                std::fprintf(stderr, "proxy request error: %s\n", proxy_ec.message().c_str());
            }
            check(!proxy_ec && proxied.status() == 200 && proxied.text() == "hello",
                  "client GET through an HTTP CONNECT proxy");
            socks_check();
        });
    };

    client->get(base + "/hello").send([&](simple_http::error_code ec, simple_http::Response response) {
        check(!ec && response.status() == 200 && response.text() == "hello", "client GET /hello");

        client->post(base + "/echo")
            .body("roundtrip")
            .send([&](simple_http::error_code post_ec, simple_http::Response echoed) {
                check(!post_ec && echoed.status() == 200 && echoed.text() == "roundtrip",
                      "client POST /echo with a body");

                client->get(base + "/redirect")
                    .send([&](simple_http::error_code redirect_ec, simple_http::Response followed) {
                        check(!redirect_ec && followed.status() == 200 && followed.text() == "hello",
                              "client follows a redirect");

                        client->get(base + "/set-cookie")
                            .send([&](simple_http::error_code cookie_ec, simple_http::Response set) {
                                check(!cookie_ec && set.status() == 200, "client receives Set-Cookie");

                                client->get(base + "/cookie")
                                    .send([&](simple_http::error_code jar_ec, simple_http::Response echoed_cookie) {
                                        check(!jar_ec && echoed_cookie.text().find("session=abc") != std::string::npos,
                                              "client cookie jar sends the cookie");
                                        if (tls_port.empty()) {
                                            proxy_check();
                                            return;
                                        }
                                        simple_http::ClientConfig tls_config;
                                        tls_config.tls.ca_file = "./test/tls_certificates/ca_cert.pem";
                                        tls_config.tls.verify_host = false;
                                        tls_client =
                                            std::make_shared<simple_http::Client>(context.get_executor(), tls_config);
                                        tls_client->get("https://127.0.0.1:" + tls_port + "/")
                                            .send([&](simple_http::error_code tls_ec,
                                                      simple_http::Response tls_response) {
                                                check(!tls_ec && tls_response.status() == 200 &&
                                                          tls_response.text() == "hello",
                                                      "client HTTPS GET over TLS");
                                                simple_http::ClientConfig h2_config;
                                                h2_config.tls.ca_file = "./test/tls_certificates/ca_cert.pem";
                                                h2_config.tls.verify_host = false;
                                                h2_config.default_version = simple_http::HttpVersionPolicy::Http2;
                                                h2_client = std::make_shared<simple_http::Client>(
                                                    context.get_executor(), h2_config);
                                                h2_client->get("https://127.0.0.1:" + tls_port + "/")
                                                    .send([&](simple_http::error_code h2_ec,
                                                              simple_http::Response h2_response) {
                                                        check(!h2_ec && h2_response.status() == 200 &&
                                                                  h2_response.text() == "hello" &&
                                                                  h2_response.version() == simple_http::Version::Http2,
                                                              "client HTTP/2 over TLS");
                                                    });
                                                proxy_check();
                                            });
                                    });
                            });
                    });
            });
    });

    context.run();
    return failures == 0 ? 0 : 1;
}
