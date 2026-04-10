# simple_http 🚀

> **Lightweight, Modern, High-Performance C++20 HTTP/1.1 & HTTP/2 Framework.**

[![gcc](https://github.com/fantasy-peak/simple_http/actions/workflows/gcc.yaml/badge.svg)](https://github.com/fantasy-peak/simple_http/actions/workflows/gcc.yaml)
[![clang](https://github.com/fantasy-peak/simple_http/actions/workflows/clang.yaml/badge.svg)](https://github.com/fantasy-peak/simple_http/actions/workflows/clang.yaml)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%2F23%2F26-blue.svg)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

`simple_http` is a lightweight, asynchronous HTTP/1.1 & HTTP/2 framework for C++20/23/26. Built upon **Boost.Beast** and **nghttp2**, it leverages modern C++ coroutines to provide a clean, high-performance API for both servers and clients.

---

## 📖 Table of Contents
- [✨ Features](#-features)
- [🚀 Quick Start](#-quick-start)
- [📦 Requirements](#-requirements)
- [🧪 C++20 Modules Support (Experimental)](#-c20-modules-support-experimental)
- [🛠 Configuration Macros](#-configuration-macros)
- [📂 More Examples](#-more-examples)
- [📊 Performance](#-performance)
- [🧪 Testing Guide](#-testing-guide)
- [🤝 Contributing](#-contributing)

---

## ✨ Features

- **📦 Header-only**: Simple to integrate; just include and go.
- **🛡️ Modern C++**: Built with C++20/23/26 coroutines for intuitive async logic.
- **🔄 Dual Protocol**: Seamless HTTP/1.1 & HTTP/2 support with ALPN negotiation.
- **🔄 Server & Client**: Symmetrical API design for both roles.
- **🔒 Secure**: Robust HTTPS and mTLS (Mutual TLS) support.
- **🔌 Advanced Transport**: Supports **IPv4/IPv6** and **UNIX Domain Sockets** for high-speed local IPC.
- **🧩 Middleware**: Flexible `setBefore` interceptors for pre-processing.
- **🌊 Full Streaming**: Bi-directional streaming for client and server.
- **🌐 Proxy**: Built-in client-side HTTP proxy support.

---

## 🚀 Quick Start

### Simple Server Snippet
```cpp
import simple_http;

hs.setHttpHandler("/hello", [](auto req, auto writer) -> asio::awaitable<void> {
    writer->writeHttpResponse(simple_http::makeHttpResponse(http::status::ok, "Hello World!"));
    co_return;
});
```

<details>
<summary><b>Click to view Full Server Example</b></summary>

```cpp
import std;
import simple_http;

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
    hs.setHttpHandler("/world",
        [](auto req, auto writer) -> asio::awaitable<void> {
            auto response = simple_http::makeHttpResponse(http::status::ok);
            response->body() = "ok!";
            writer->writeHttpResponse(response);
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
</details>

<details>
<summary><b>Click to view Full Client Example</b></summary>

```cpp
import std;
import simple_http;

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
    w->writerBody("hello", simple_http::WriteMode::More);
    w->writerBody("client", simple_http::WriteMode::Last);
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
            [](std::string str) {
                std::println("recv data: {}", str);
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
            if (ep) std::rethrow_exception(ep);
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
</details>

---

## 📦 Requirements
- **C++20/23/26** (GCC 13+, Clang 20+)
- **xmake** Used for building examples and dependency management.
- **Dependencies**: Boost.Beast, nghttp2, OpenSSL

## 🧪 C++20 Modules Support (Experimental)
`simple_http` provides native C++20 Modules support via `include/simple_http.cppm`.

> **Status**: Verified on Clang 20+. Support for GCC and MSVC is on the roadmap.

#### xmake Integration

<details>
<summary><b>Click to view detailed xmake.lua configuration</b></summary>

The following example shows how to integrate `simple_http` as a C++20 module in your `xmake.lua` project:

```lua
add_requires("simple_http")

set_policy("build.c++.modules", true)
set_policy("build.c++.modules.std", true)

target("server")
    set_kind("binary")

    add_cxflags("-fuse-ld=mold")
    add_cxxflags("-stdlib=libc++")

    on_load(function (target)
        local pkg = target:pkg("simple_http")
        if pkg then
            local installdir = pkg:installdir()
            local module_file = path.join(installdir, "include", "simple_http.cppm")
            target:add("files", module_file)
            print("Successfully linked C++20 module from: " .. module_file)
        end
    end)

    add_files("src/main.cpp")
    add_packages("simple_http")
```
</details>

---

## 🛠 Configuration Macros

| Macro | Description |
| :--- | :--- |
| `SIMPLE_HTTP_EXPERIMENT_WEBSOCKET` | Enables experimental WebSocket support. |
| `SIMPLE_HTTP_EXPERIMENT_HTTP2CLIENT` | Enables experimental HTTP/2 client support. |
| `SIMPLE_HTTP_USE_BOOST_REGEX` | Uses `boost::regex` instead of `std::regex` for better performance. |
| `SIMPLE_HTTP_BIND_UNIX_SOCKET` | Enables support for binding to UNIX Domain Sockets (UDS). |

---

### 📂 More Examples
- **[server.cpp](test/server.cpp)**: A full-featured server example.
- **[client.cpp](test/client.cpp)**: A client example with HTTP/2.

---

## 📊 Performance

Benchmark conducted using `h2load` on **Ubuntu 25.10 | i7-13620H (16 vCPUs) | 41Gi RAM**.

| Metric | Result |
| :--- | :--- |
| **Throughput** | **~99,558 req/s** |
| **Transfer Rate** | **~976.28 MB/s** |
| **Success Rate** | 100% (1,000,000 requests) |

**Detailed Results:**

```text
finished in 10.04s, 99558.82 req/s, 976.28MB/s
requests: 1000000 total, 1000000 started, 1000000 done, 1000000 succeeded, 0 failed, 0 errored, 0 timeout
status codes: 1000000 2xx, 0 3xx, 0 4xx, 0 5xx
traffic: 9.58GB (10282450426) total, 2.89MB (3026000) headers (space savings 95.12%), 9.54GB (10243000000) data

                     min         max         mean         sd        +/- sd
time for request:     5.99ms       1.43s    342.76ms    137.20ms    77.08%
time for connect:     5.65ms    151.01ms     62.93ms     37.19ms    67.60%
time to 1st byte:    79.20ms       1.02s    537.90ms    308.48ms    51.80%
req/s           :     100.72      124.03      105.92        4.56    71.10%
```

> **Benchmark Command**:
> `h2load -t 4 -n 1000000 -c 1000 -m 40 -H 'Content-Type: application/json' --data=b.txt http://localhost:7788/hello`

---

## 🧪 Testing Guide

```bash
curl -N -v --http2-prior-knowledge http://localhost:7788/hello\?key1\=value1\&key2\=value2
curl -N -v --http2-prior-knowledge http://localhost:7788/hello -d "abcd"
curl -N -v --http2 http://localhost:7788/hello -d "abcd"

nghttp --upgrade -v http://127.0.0.1:7788/hello
nghttp --upgrade -v http://nghttp2.org
h2load -n 60000 -c 1000 -m 200 -H 'Content-Type: application/json' --data=b.txt http://localhost:7788/hello

need define SIMPLE_HTTP_BIND_UNIX_SOCKET macro
curl --unix-socket /tmp/simple_http.sock https://SimpleHttpServer:7788/hello?123456 --cacert ca_cert.pem --cert client_cert.pem --key client_key.pem -X POST -d "123"
```

---

## 🤝 Contributing
Contributions are welcome! Please check [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## 📄 License
`simple_http` is licensed under the [MIT License](LICENSE).
