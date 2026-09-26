# simple_http 🚀

> **A lightweight, header-only HTTP/1.1, HTTP/2 and WebSocket framework for modern C++.**

[![gcc](https://github.com/fantasy-peak/simple_http/actions/workflows/gcc.yaml/badge.svg)](https://github.com/fantasy-peak/simple_http/actions/workflows/gcc.yaml)
[![clang](https://github.com/fantasy-peak/simple_http/actions/workflows/clang.yaml/badge.svg)](https://github.com/fantasy-peak/simple_http/actions/workflows/clang.yaml)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

`simple_http` is an asynchronous HTTP/1.1, HTTP/2 and WebSocket framework built on
**Boost.Asio** and **OpenSSL**. Its HTTP/1.1, HTTP/2 and WebSocket codecs are its
own: no Beast, no nghttp2, nothing between your handler and the socket but Asio.
The whole thing is C++23 coroutines from the acceptor down, it serves and it
fetches — one route table covers HTTP/1.x, HTTP/2 and h2c, and the client speaks
the same protocols outbound.

---

## 📖 Table of Contents
- [✨ Features](#-features)
- [🚀 Quick Start](#-quick-start)
- [📦 Requirements](#-requirements)
- [🛠 Configuration Macros](#-configuration-macros)
- [📋 Logging](#-logging)
- [🔒 TLS & mTLS](#-tls--mtls)
- [🧭 Routing, Middleware & Proxy](#-routing-middleware--proxy)
- [🗜 Response Compression](#-response-compression)
- [📂 More Examples](#-more-examples)
- [📊 Performance](#-performance)
- [🧪 Testing Guide](#-testing-guide)
- [🤝 Contributing](#-contributing)

---

## ✨ Features

- **📦 Header-only** — include and go; the library itself links nothing but Boost.Asio and OpenSSL.
- **🛡️ Modern C++** — C++23 coroutines (`asio::awaitable`), concepts, `std::expected`; no callbacks to thread through.
- **🔄 One route table, every protocol** — HTTP/1.1, HTTP/2 (ALPN, h2c upgrade, prior knowledge) and h2c share handlers. Version differences live in the engines, not in your code.
- **🔁 Server *and* client** — the client negotiates ALPN or h2c on its own, over `http://` and `https://`, with HTTP/2 stream multiplexing and a keep-alive pool.
- **🔒 HTTPS & mTLS** — server and client side, including client-certificate inspection from a handler.
- **🌊 Streaming both ways** — request and response bodies stream with flow-control backpressure; a large payload never has to be materialized.
- **🌐 Reverse proxy** — request-level HTTP proxying (plaintext, TLS or h2c backends) and byte-level WebSocket pass-through.
- **🔌 TCP and UNIX-domain sockets** — bind a path instead of a port when the peer is a local sidecar; TLS, WebSocket and proxying all work over either.
- **🧩 Middleware** — `before` filters that can short-circuit, plus a CORS hook driven by the request's `Origin`.
- **🧵 Lock-free connection handling** — each connection is pinned to one single-threaded `io_context` for its whole life, so engines and writers never synchronize.
- **📋 Logging that does not pick a side** — a four-field `LogSink` interface with no third-party types in it; wire it to spdlog, an in-house library, or nothing.
- **🗜 Optional compression** — gzip/brotli response bodies and transparent client-side decompression, both opt-in.

---

## 🚀 Quick Start

### Server

```cpp
#include <simple_http.h>

namespace asio = boost::asio;

int main() {
    simple_http::ServerConfig cfg;
    cfg.listen = InetAddress{"0.0.0.0", 7788, false};  // InetAddress{host, port, v6_only}
    cfg.worker_threads = 4;

    simple_http::Server server{cfg};

    server.route("/hello", [](simple_http::RequestPtr, simple_http::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).content_type(simple_http::mime::text_plain).send("Hello World!");
    });

    server.fallback([](simple_http::RequestPtr, simple_http::ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(404).send("not found");
    });

    if (!server.start()) {
        return 1;  // a listener failed to bind
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(1));  // the pool's threads do the work
    }
}
```

`cfg.listen` is a single endpoint on purpose. Serving several addresses or
protocol stacks is a multi-process concern: start another `Server`, or another
process sharing the port via `cfg.reuse_port`. IPv4 and IPv6 come from one
listener — `{"::", port, false}` is dual-stack, and falls back to IPv4 where the
platform has no IPv6.

A UNIX-domain socket replaces the port with a path. There is no macro and
nothing extra to link: `boost::asio::local::stream_protocol` is part of Asio, so
the support is there wherever the platform has AF_UNIX. The socket file is
unlinked before binding, so a restart reuses the path instead of failing on the
leftover:

```cpp
cfg.listen = UnixAddress{"/run/myapp.sock"};
```

`Listen` is a `std::variant` of the two, so naming both a port and a path is not
a rule to remember — it does not compile.

Handlers take arguments by shape, picked at compile time:

```cpp
asio::awaitable<void>(RequestPtr, ResponsePtr)                  // ordinary
asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle)       // + the TLS handle
asio::awaitable<void>(RequestPtr, std::shared_ptr<WebSocket>)   // WebSocket route
asio::awaitable<bool>(RequestPtr, ResponsePtr)                  // filter: false short-circuits
```

A response is fluent, one-shot or streamed:

```cpp
co_await res->status(200).content_type(simple_http::mime::app_json).send(payload);  // one-shot

co_await res->status(200).begin();          // streamed: chunked on HTTP/1.1, DATA frames on HTTP/2
co_await res->write("first ");
co_await res->finish("last");
```

### Client

```cpp
#include <print>
#include <simple_http.h>

asio::awaitable<void> fetch() {
    simple_http::HttpClient http;  // default policy

    auto r = co_await http.get("https://example.com/");
    if (!r) {
        std::println("failed: {}", r.error().message());
        co_return;
    }
    std::println("{} {} ({} bytes)", r->status, simple_http::to_string(r->version), r->body.size());
}
```

One connection, streamed both ways:

```cpp
simple_http::ClientTarget target{.host = "127.0.0.1", .port = 7789, .use_tls = true};
auto session = co_await http.connect(target);
if (!session) co_return;

simple_http::RequestSpec spec{.method = simple_http::Method::Post, .target = "/upload", .stream_body = true};
auto stream = co_await (*session)->open_stream(spec);
if (!stream) co_return;

co_await (*stream)->write("hello ");     // body chunks…
co_await (*stream)->finish("world");     // …and the end of the body

auto head = co_await (*stream)->read_head();       // status + headers
while (auto chunk = co_await (*stream)->read()) {  // then the body
    if (chunk->eof) break;
    std::println("recv: {}", chunk->data);
}
```

On HTTP/2 those streams are multiplexed, so several can be open on one session at
once. Timeouts, limits, pool sizing and decompression live in
`simple_http::ClientConfig`; SNI, CA bundle, name verification, client
certificates and the ALPN list in `simple_http::TlsClientConfig`.

---

## 📦 Requirements

- **C++23** or newer (GCC 13+, Clang 20+).
- **Boost.Asio** and **OpenSSL**. Nothing else — compression is opt-in and brings
  its own two (zlib, brotli).
- **xmake** for the example targets and dependency management; **CMake** is supported for consumption.

---

## 🛠 Configuration Macros

| Macro | Description |
| :--- | :--- |
| `SIMPLE_HTTP_USE_BOOST_REGEX` | Uses `boost::regex` instead of `std::regex` for route matching. |
| `SIMPLE_HTTP_ENABLE_COMPRESSION` | Compiles in gzip/brotli response-body compression. Needs zlib and brotli; see [Response Compression](#-response-compression). |
| `SIMPLE_HTTP_ENABLE_LOG` | Master switch for the logging facade (`1` by default). Set to `0` and every `SIMPLE_HTTP_*_LOG` expands to `((void)0)`. |
| `SIMPLE_HTTP_LOG_ACTIVE_LEVEL` | Compile-time floor, `0` (Trace) … `5` (Critical). Records below it are discarded at compile time, so they cost nothing at the call site. |
| `SIMPLE_HTTP_ENABLE_HTTP3` | HTTP/3 skeleton; off by default and a no-op unless enabled. |

WebSocket support has no macro — it is always compiled in.

---

## 📋 Logging

The library formats a record and hands it to whatever `LogSink` is installed. It
never writes to a stream itself, and it names no logging library — not in its
headers, not in its build.

```cpp
simple_http::set_log_sink(simple_http::make_stdout_sink(simple_http::LogLevel::Info));
```

`make_stdout_sink` / `make_stderr_sink` take a minimum level and cover examples,
tests and small tools. Installing a sink is safe while other threads are logging,
so a deployment can swap its routing into place at any point.

### Wiring up an existing logging library

The adapter is deliberately **not** shipped in the library. A `spdlog_sink.h` in
here would put spdlog on every consumer's include path and make the library
responsible for tracking spdlog's API across versions. `LogSink` carries four
fields and no third-party types, so adapting a backend is the ~30 lines below —
`v2ray-cpp/src/flux.cpp` runs this exact code against an async spdlog logger.

```cpp
class SpdlogSink final : public simple_http::LogSink {
  public:
    explicit SpdlogSink(std::shared_ptr<spdlog::logger> logger) : m_logger(std::move(logger)) {}

    bool enabled(simple_http::LogLevel level) const noexcept override {
        return m_logger && m_logger->should_log(to_spdlog(level));
    }

    void log(const simple_http::LogRecord& record) override {
        m_logger->log(to_spdlog(record.level), "{}:{}: {}",
                      simple_http::basename(record.where.file_name()),
                      record.where.line(), record.message);
    }

  private:
    static spdlog::level::level_enum to_spdlog(simple_http::LogLevel level) noexcept {
        switch (level) {
            case simple_http::LogLevel::Trace: return spdlog::level::trace;
            case simple_http::LogLevel::Debug: return spdlog::level::debug;
            case simple_http::LogLevel::Info: return spdlog::level::info;
            case simple_http::LogLevel::Warn: return spdlog::level::warn;
            case simple_http::LogLevel::Error: return spdlog::level::err;
            case simple_http::LogLevel::Critical: return spdlog::level::critical;
        }
        return spdlog::level::info;
    }

    std::shared_ptr<spdlog::logger> m_logger;
};

simple_http::set_log_sink(std::make_shared<SpdlogSink>(spdlog::default_logger()));
```

Four things worth knowing before writing your own:

- **The sink owns the level.** `enabled()` is asked *before* the message is
  formatted, so a record the sink would drop costs one virtual call — and
  verbosity is configured in the logger you already have, not in two places.
- **A record is valid only during the call.** `message` and `category` point at a
  stack buffer; copy them if you need to keep them. The example above hands them
  straight to spdlog, so it does not.
- **Records carry a category, empty by default.** The library logs untagged; use
  `SIMPLE_HTTP_ERROR_LOG_CAT("h2", …)` and friends when a backend routes on it.
- **Nothing is allocated for the common case.** Messages up to 512 bytes are
  formatted onto the stack.

---

## 🔒 TLS & mTLS

TLS is one optional field on the server config, and one on the client's:

```cpp
simple_http::ServerConfig cfg;
cfg.listen = {"0.0.0.0", 8443, false};
cfg.tls = simple_http::TlsConfig{
    .cert_chain_file = "server_cert.pem",
    .private_key_file = "server_key.pem",
    .mutual = true,                 // require a client certificate
    .ca_file = "ca_cert.pem",
};
```

ALPN picks HTTP/2 when the client offers it and HTTP/1.1 otherwise, so the same
handlers serve both. On the client side the knobs are `verify_peer`,
`verify_host`, `ca_file`, `cert_chain_file`, `private_key_file`, `sni_override`
and `alpn`, all on `simple_http::TlsClientConfig`.

A handler that takes the third argument receives the connection's `SslHandle`
(`std::optional<SSL*>`, nullopt on plaintext), which is how you inspect a client
certificate:

```cpp
server.route("/whoami", [](simple_http::RequestPtr, simple_http::ResponsePtr res,
                           simple_http::SslHandle ssl) -> asio::awaitable<void> {
    X509* cert = SSL_get_peer_certificate(*ssl);  // owned by the caller: X509_free it
    // …
});
```

---

## 🧭 Routing, Middleware & Proxy

```cpp
// Exact and regex routes; the first match wins, `fallback` takes the rest.
server.route("/world", handler);
server.route_regex("^/api/(.*)$", handler);
server.fallback(not_found);

// A filter runs before routing; returning false short-circuits the request
// (whatever it already wrote is the response).
server.before([](simple_http::RequestPtr req, simple_http::ResponsePtr res) -> asio::awaitable<bool> {
    if (!authorized(req)) {
        co_await res->status(401).send("unauthorized");
        co_return false;
    }
    co_return true;
});

// The CORS hook runs only for requests carrying an Origin header.
server.cors(cors_filter);
```

Reverse proxy — request-level for HTTP, byte-level for WebSocket:

```cpp
// Plaintext HTTP/1.1 backend: the short form.
server.http_proxy("/api", "backend.internal", 8080);
// An https backend gets HTTP/2 for free when it offers it over ALPN — set `h2c`
// instead to reach a plaintext HTTP/2 backend.
server.http_proxy("/v2", HttpProxyTarget{.host = "10.0.0.5", .port = 8443, .tls = true});
// Regex routes take capture groups in the rewrite.
server.http_proxy_regex("^/v1/(.*)$", "10.0.0.5", 9000, "/$1");
// WebSocket frames pass through untouched.
server.ws_proxy("/ws", "backend.internal", 9000);
```

Proxying runs through the client layer, so the upstream may be plaintext or TLS
and may speak HTTP/1.1 or HTTP/2, regardless of what the frontend speaks. Bodies
stream in both directions; `X-Forwarded-*` headers are added and hop-by-hop
headers are stripped.

WebSocket routes hand you the socket directly:

```cpp
server.ws_route("/chat", [](simple_http::RequestPtr, std::shared_ptr<simple_http::WebSocket> ws)
                              -> asio::awaitable<void> {
    for (;;) {
        auto msg = co_await ws->read();        // expected<WsMessage, error_code>
        if (!msg) break;                       // peer closed, or an error
        if (co_await ws->write("echo: " + msg->data, msg->text)) break;
    }
    co_return;  // returning sends a Close frame and shuts the socket down gracefully
});
```

---

## 🗜 Response Compression

A server can gzip or brotli response bodies for clients that ask for it. It is
**off by default** and gets enabled twice: at build time, because the codecs sit
behind a macro so a build that does not want compression needs neither the
dependency nor the symbols; and at run time, through the server config.

**Build** — define `SIMPLE_HTTP_ENABLE_COMPRESSION` and link zlib + brotli:

```lua
-- xmake
add_requires("zlib", "brotli")
target("app")
    add_packages("zlib", "brotli")
    add_defines("SIMPLE_HTTP_ENABLE_COMPRESSION")
```

```cmake
# CMake
set(SIMPLE_HTTP_WITH_COMPRESSION ON)  # before add_subdirectory / find_package
```

**Run** — turn it on in the config:

```cpp
sh::ServerConfig cfg;
cfg.limits.compression.enabled = true;
cfg.limits.compression.min_bytes = 1024;  // below this, compression is not worth it
sh::Server server{cfg};
```

Handlers need no changes. Everything written through `Response` — the reverse
proxy included — is compressed when the request's `Accept-Encoding` allows it,
with brotli preferred over gzip.

What it deliberately leaves alone:

| Skipped | Why |
| :--- | :--- |
| Responses already carrying `Content-Encoding` | Double-encoding. This is also what lets a pre-compressed upstream pass through untouched. |
| `206` / `Content-Range` | A byte range has to stay addressable. |
| `204`, `304`, HEAD | No body. |
| `Cache-Control: no-transform` | RFC 9110 §7.7 forbids rewriting the representation. |
| Already entropy-coded types (`image/*` except `image/svg+xml`, `video/*`, `audio/*`, `font/*`) | Costs CPU and can make the body larger. |
| Bodies under `min_bytes` | Same reason. Streamed responses are exempt — their length is not known in advance — so set `compress_streamed = false` to leave those alone too. |

A `Vary: Accept-Encoding` is added and merged with any existing `Vary`, so caches
key on it, and a strong `ETag` is demoted to weak, because the compressed body is
a different representation. Both behaviours are configurable.

**Client** — the other direction, also opt-in:

```cpp
sh::ClientConfig cfg;
cfg.auto_decompress = true;  // advertise br, gzip and decode what comes back
sh::HttpClient http{cfg};
auto r = co_await http.get(url);
// r->body is the original bytes; content-encoding and content-length are gone
```

The client takes `Accept-Encoding` from `cfg.accept_encodings`, filtered to what
the build can actually decode, unless the request sets that header itself (then it
is left alone). Decoding happens at the stream level, so `ClientStream::read()`
hands out decoded bytes too — and the size caps in `read_all()` and the
convenience layer apply to the *decoded* size, which means a compression bomb
cannot slip past them by being small on the wire. A body that fails to decode
comes back as `client_errc::body_decode_failed` rather than as half a body.

---

## 📂 More Examples

- **[test/server.cpp](test/server.cpp)** — a server exercising every feature: routes, regex routes, middleware, h2c, WebSocket, a TLS listener with mTLS, and the reverse-proxy paths. Run it with `xmake run server`.
- **[test/client.cpp](test/client.cpp)** — the client's exercise program. It starts a server in-process and drives the client over http/https × HTTP/1.1/HTTP/2, including h2c, streaming uploads, multiplexing and TLS verification. Run it with `xmake run client`.
- **[v2ray-cpp/](v2ray-cpp/)** — a real deployment: a VLESS proxy built on this library, including the spdlog adapter shown above.

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

Three self-contained suites (no external services needed):

```bash
xmake build unittest   && xmake run unittest     # unit tests (Catch2): parsers, HPACK, frames, routing, URLs
xmake build regression && xmake run regression   # server regression: malformed/boundary requests over raw sockets
xmake build client     && xmake run client       # client integration: protocol matrix, streaming, multiplexing, TLS
```

`unittest` and `regression` are Catch2 binaries (filter with e.g. `xmake run unittest "[h2]"`);
`client` prints PASS/FAIL per check and exits non-zero on failure. All three are self-contained
C++ — no Python or external services.

A fourth suite drives the same server with **third-party clients** — httpx,
hyper-h2 and websockets — and that is the point rather than an implementation
detail: the C++ suites use simple_http's own client, so a spec misreading shared
by both halves cancels out and the two agree on something wrong. An independent
implementation disagrees exactly where the server is wrong. It found, for
instance, a handler that threw taking the connection down instead of answering
500, which no C++ client surfaced.

```bash
python3 -m venv test/python/.venv
test/python/.venv/bin/pip install -r test/python/requirements.txt
xmake run python-tests      # needs `xmake build server` first
```

Against the example server (`xmake run server`, plaintext on `:7788` and mTLS on `:7789`),
with external clients:

```bash
curl -N -v --http2-prior-knowledge http://localhost:7788/hello\?key1\=value1\&key2\=value2
curl -N -v --http2-prior-knowledge http://localhost:7788/hello -d "abcd"

nghttp --upgrade -v http://127.0.0.1:7788/hello
h2load -n 60000 -c 1000 -m 200 -H 'Content-Type: application/json' --data=b.txt http://localhost:7788/hello

curl -k --cert test/tls_certificates/client_cert.pem --key test/tls_certificates/client_key.pem \
     https://127.0.0.1:7789/whoami
```

---

## 🤝 Contributing
Contributions are welcome! Please check [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## 📄 License
`simple_http` is licensed under the [MIT License](LICENSE).
