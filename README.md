# simple_http

[![CI](https://github.com/fantasy-peak/simple_http/actions/workflows/ci.yaml/badge.svg)](https://github.com/fantasy-peak/simple_http/actions/workflows/ci.yaml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

`simple_http` is a lightweight, high-performance, asynchronous HTTP/1.1 & HTTP/2 server and client library for C++20, built upon Boost.Asio and nghttp2. It is designed as header-only for easy integration and use.

## ✨ Features

- **Header-only**: Easy to integrate into any project by just including headers.
- **Modern C++**: Leverages C++20/23 and coroutines for clean, asynchronous logic.
- **Dual Protocol Support**: Supports both HTTP/1.1 and HTTP/2 with automatic negotiation.
- **Server & Client**: Provides a consistent API for both server and client functionalities.
- **Secure Communication**: Supports HTTPS and TLS mutual authentication (mTLS).
- **Highly Configurable**: Easily configure server worker threads, concurrent stream limits, window sizes, and more.
- **Middleware Support**: Offers a `setBefore` interface for middleware-style request pre-processing.
- **IPv4 & IPv6**: Full dual-stack support.

## 🚀 Requirements

- **C++20/23** compatible compiler (e.g., GCC 13+, Clang 12+).
- **xmake**: Used for building examples and dependency management.
- **Boost** (`asio`)
- **nghttp2**
- **OpenSSL**

> **Note**: When building with `xmake`, it automatically downloads and links all necessary dependencies, so you don't need to install them manually.

## Server Example
```
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
        .set_socket_option =
            [](asio::ip::tcp::socket& socket) {
                // Set socket properties
                socket.set_option(asio::socket_base::keep_alive(true));
            },
        .enable_ipv6 = true,
        .ipv6_addr = "::1",
        .ipv6_port = 7788,
    };
    simple_http::HttpServer hs(cfg);
    simple_http::LOG_CB =
        [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
            std::out << to_string(level) << " " << file << ":" << line << " " << msg
                << std::endl;
        };
    hs.setBefore([](const std::shared_ptr<simple_http::HttpRequestReader>& reader,
                    const std::shared_ptr<simple_http::HttpResponseWriter>& writer) -> asio::awaitable<bool> {
        std::cout << "setBefore:" << reader->target() << std::endl;
        if (reader->target() != "/hello")
        {
            auto res = simple_http::makeHttpResponse(http::status::bad_request);
            writer->writeHttpResponse(res);
            co_return false;
        }
        co_return true;
    });
    hs.setHttpHandler(
        "/hello", [](auto req, auto writer) -> asio::awaitable<void> {
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
                writer->writeBody("456");
                writer->writeBodyEnd("789");
#endif
            }
            co_return;
        });
    co_await hs.start();
}

int main() {
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), start(), asio::detached);
    while (true)
        sleep(1000);
    return 0;
}
```

## Test Cmd
```
curl -N -v --http2-prior-knowledge http://localhost:7788/hello\?key1\=value1\&key2\=value2
curl -N -v --http2-prior-knowledge http://localhost:7788/hello -d "abcd"
curl -N -v --http2 http://localhost:7788/hello -d "abcd"

nghttp --upgrade -v http://127.0.0.1:7788/hello
nghttp --upgrade -v http://nghttp2.org
h2load -n 60000 -c 1000 -m 200 -H 'Content-Type: application/json' --data=b.txt http://localhost:7788/hello

need define SIMPLE_HTTP_BIND_UNIX_SOCKET macro
curl --unix-socket /tmp/simple_http.sock https://SimpleHttpServer:7788/hello?123456 --cacert ca_cert.pem --cert client_cert.pem --key client_key.pem -X POST -d "123"
```

## 🤝 Contributing

Contributions of any kind are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) to learn how to contribute to the project.

## 📄 License

`simple_http` is licensed under the [MIT License](LICENSE).
