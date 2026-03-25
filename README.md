# simple_http

[![CI](https://github.com/fantasy-peak/simple_http/actions/workflows/ci.yaml/badge.svg)](https://github.com/fantasy-peak/simple_http/actions/workflows/ci.yaml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

`simple_http` is a lightweight, high-performance, asynchronous HTTP/1.1 & HTTP/2 server and client library for C++20, built upon Boost.Asio and nghttp2. It is designed as header-only for easy integration and use.

## ✨ Features

- **Header-only**: Easy to integrate into any project by just including headers.
- **Modern C++**: Leverages C++20/23/26 and coroutines for clean, asynchronous logic.
- **Dual Protocol Support**: Supports both HTTP/1.1 and HTTP/2 with automatic negotiation.
- **Server & Client**: Provides a consistent API for both server and client functionalities.
- **Secure Communication**: Supports HTTPS and TLS mutual authentication (mTLS).
- **Highly Configurable**: Easily configure server worker threads, concurrent stream limits, window sizes, and more.
- **Middleware Support**: Offers a `setBefore` interface for middleware-style request pre-processing.
- **IPv4 & IPv6**: Full dual-stack support.
- **UNIX Domain Sockets**: High-performance local IPC with zero network overhead.

## 🚀 Requirements

- **C++20/23/26** compatible compiler (e.g., GCC 13+, Clang 17+).
- **xmake**: Used for building examples and dependency management.
- **Boost** (`beast`)
- **nghttp2**
- **OpenSSL**

> **Note**: When building with `xmake`, it automatically downloads and links all necessary dependencies, so you don't need to install them manually.

## 🛠 Configuration Macros

The following macros can be defined to enable or customize specific features:

- `SIMPLE_HTTP_EXPERIMENT_WEBSOCKET`: Enables experimental WebSocket support.
- `SIMPLE_HTTP_EXPERIMENT_HTTP2CLIENT`: Enables experimental HTTP/2 client support.
- `SIMPLE_HTTP_USE_BOOST_REGEX`: Uses `boost::regex` instead of `std::regex` for header parsing and routing. This can provide better performance and compatibility in certain environments.
- `SIMPLE_HTTP_BIND_UNIX_SOCKET`: Enables support for binding the server to UNIX Domain Sockets (UDS) for high-performance local IPC.

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
    simple_http::HttpServer hs(cfg);
    simple_http::LOG_CB = [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
        std::println("{} {} {} {}", to_string(level), file, line, msg);
    };
    hs.setBefore([](const auto& reader, const auto& writer) -> asio::awaitable<bool> {
        if (reader->target() != "/hello") {
            auto res = simple_http::makeHttpResponse(http::status::bad_request);
            writer->writeHttpResponse(res);
            co_return false;
        }
        co_return true;
    });
    hs.setHttpHandler("/hello",
        [](auto req, auto writer) -> asio::awaitable<void> {
            writer->writeStatus(200);
            writer->writeHeader(http::field::content_type, simple_http::mime::text_plain);
            writer->writeStreamHeaderEnd();
            writer->writeStreamBody("hello world");
            writer->writeStreamEnd();
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
## Client Example
```
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
    std::vector<std::pair<std::string, std::string>> headers{{"test", "hello"}};
    auto stream_spec = std::make_shared<simple_http::StreamSpec>(simple_http::http::verb::post, "/hello", headers);
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
        bool should_continue = std::visit(simple_http::overloaded{
            [](std::unique_ptr<std::string> str_ptr) {
                std::println("recv data: {}", *str_ptr);
                return true;
            },
            [](simple_http::Eof) { return false; },
            [](simple_http::Disconnect) { return false; },
            [](simple_http::ParseHeaderDone) { return false; }}
        , std::move(d));
        if (!should_continue) {
            break;
        }
    }
    co_return;
}

int main() {
    simple_http::LOG_CB = [](simple_http::LogLevel level, auto file, auto line, std::string msg) {
        std::println("{} {} {} {}", to_string(level), file, line, msg);
    };
    simple_http::IoCtxPool pool{1};
    pool.start();
    asio::co_spawn(pool.getIoContext(), client(pool), [](const std::exception_ptr& ep) {
        try {
            if (ep)
                std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            SIMPLE_HTTP_ERROR_LOG("{}", e.what());
        } catch (...) {
            SIMPLE_HTTP_ERROR_LOG("unknown exception");
        }
    });
    while (true)
        sleep(1000);
    return 0;
}
```

## 📊 Performance

The following benchmark was performed using `h2load`.

**Machine Configuration:**
- **OS**: Ubuntu 25.10
- **CPU**: 13th Gen Intel(R) Core(TM) i7-13620H (16 vCPUs)
- **Memory**: 41Gi RAM

**Test Configuration:**
- **Request Size**: 1KB (via `b.txt`)
- **Response Size**: 10KB
- **Command**: `h2load -t 4 -n 1000000 -c 1000 -m 100 -H 'Content-Type: application/json' --data=b.txt http://localhost:7788/hello`

**Results:**

```text
finished in 10.52s, 95040.47 req/s, 931.69MB/s
requests: 1000000 total, 1000000 started, 1000000 done, 1000000 succeeded, 0 failed, 0 errored, 0 timeout
status codes: 1000000 2xx, 0 3xx, 0 4xx, 0 5xx
traffic: 9.57GB (10279231000) total, 2.87MB (3012000) headers (space savings 92.83%), 9.54GB (10240000000) data
                     min         max         mean         sd        +/- sd
time for request:    24.80ms       2.25s    960.37ms    196.92ms    77.87%
time for connect:    18.73ms    177.97ms     89.05ms     39.54ms    59.10%
time to 1st byte:   110.19ms    641.54ms    363.77ms     88.02ms    75.00%
req/s           :      95.16      103.24       98.84        2.23    62.30%
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
