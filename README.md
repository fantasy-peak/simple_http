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
fetches — one route table covers HTTP/1.x, HTTP/2 (ALPN, h2c upgrade, prior
knowledge) and h2c, and the client speaks the same protocols outbound. HTTP/3
(the QUIC listener) is opt-in and built on ngtcp2/nghttp3.

---

## 📖 Table of Contents
- [✨ Features](#-features)
- [🚀 Quick Start](#-quick-start)
- [📦 Requirements](#-requirements)
- [🛠 Configuration Macros](#-configuration-macros)
- [📋 Logging](#-logging)
- [🔒 TLS & mTLS](#-tls--mtls)
- [🧭 Routing, Middleware & Proxy](#-routing-middleware--proxy)
- [🔌 WebSocket](#-websocket)
- [📑 Queries, Forms & SSE](#-queries-forms--sse)
- [🔗 Client: redirects, cookies & auth](#-client-redirects-cookies--auth)
- [📜 OpenAPI & Swagger UI](#-openapi--swagger-ui)
- [🌐 CORS](#-cors)
- [🔒 Security Headers](#-security-headers)
- [📁 Static Files](#-static-files)
- [🗜 Response Compression](#-response-compression)
- [📂 More Examples](#-more-examples)
- [📊 Performance](#-performance)
- [🧪 Testing Guide](#-testing-guide)
- [🤝 Contributing](#-contributing)

---

## ✨ Features

- **📦 Header-only** — include and go; the library links nothing but Boost.Asio and OpenSSL, with header-only glaze carrying the JSON helpers.
- **🛡️ Modern C++** — C++23 coroutines (`asio::awaitable`), concepts, `std::expected`; no callbacks to thread through.
- **🔄 One route table, every protocol** — HTTP/1.1, HTTP/2 (ALPN, h2c upgrade, prior knowledge) and h2c share handlers. Version differences live in the engines, not in your code.
- **🔁 Server *and* client** — the client negotiates ALPN or h2c on its own, over `http://` and `https://`, with HTTP/2 stream multiplexing and one kept-alive connection per origin.
- **🔒 HTTPS & mTLS** — server and client side, including client-certificate inspection from a handler.
- **🌊 Streaming both ways** — request and response bodies stream with flow-control backpressure; a large payload never has to be materialized.
- **🧱 Backpressure that survives a hostile peer** — a request body that backs up is parked per stream (HTTP/2) or drained by a bounded internal reader (WebSocket), so one stream or one serial handler cannot stall the connection.
- **🌐 Reverse proxy** — request-level HTTP proxying (plaintext, TLS or h2c backends, each route carrying its own client policy) and byte-level WebSocket pass-through.
- **🔌 TCP and UNIX-domain sockets** — bind a path instead of a port when the peer is a local sidecar; TLS, WebSocket and proxying all work over either.
- **🧩 Middleware** — Go `net/http` / tower-style middleware: global (`use`), per-group and per-route, with before/after wrapping, short-circuiting and per-request state — plus built-ins (`request_id`, `access_log`, `recovery`, `basic_auth`, `real_ip`, `clean_path`, `strip_prefix`, `redirect_slashes`, `secure_headers`, CORS, rate limiting).
- **📑 Forms, queries & JSON** — `req->query_params()` (Go `r.URL.Query` / axum `Query<T>`), `read_urlencoded_body`, `read_multipart_body` and the client-side `MultipartForm` / `RequestBuilder::multipart()`, and — on by default, via glaze — `read_json_body<T>` / `write_json` (axum `Json<T>`), plus `res->redirect()`, a Server-Sent Events wrapper, and rate limiting (`429` + `Retry-After`, token bucket or per-key).
- **🧵 Client ergonomics** — automatic redirect following (`max_redirects`, Go semantics), a `CookieJar`, `basic_auth()` / `bearer_auth()`, and per-request layered timeouts.
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

    server.route({simple_http::Method::Get}, "/hello", [](simple_http::RequestPtr, simple_http::ResponsePtr res) -> asio::awaitable<void> {
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
    // A Client is pinned to one executor (model A): pass an executor whose
    // single thread drives every operation of this client — here the caller's
    // own. The io_context must outlive the client's sessions.
    simple_http::Client http{co_await asio::this_coro::executor}; // default policy — connection reuse/retry/redirects are internal

    // Buffered convenience: builder chain → Response (text/json/error_for_status).
    auto r = co_await http.get("https://example.com/").query({{"q", "dogs"}}).send();
    if (!r) {
        std::println("failed: {}", r.error().message());
        co_return;
    }
    auto body = co_await r->text();
    std::println("{} {} ({} bytes)", r->status(), simple_http::to_string(r->version()), body->size());

    // Explicit full-duplex streaming: one Stream, write end + read end.
    auto up = co_await http.post("https://example.com/upload").send_stream();
    if (!up) co_return;
    co_await up->write("hello ");     // body chunks…
    co_await up->finish("world");     // …and the end of the body

    auto head = co_await up->read_head();       // status + headers
    while (auto chunk = co_await up->read()) {  // then the body
        if (chunk->eof) break;
        std::println("recv: {}", chunk->data);
    }

    // Multipart/form-data uploads: `MultipartForm` is the encoder counterpart of
    // the server's read_multipart_body (Go mime/multipart.Writer / reqwest Form):
    simple_http::MultipartForm form;
    form.field("title", "hello");
    form.file("upload", "notes.txt", "some text");  // content type from the extension
    auto r = co_await http.post("https://example.com/upload").multipart(std::move(form)).send();
}
```

On HTTP/2 those streams are multiplexed, so calls can be open on one connection at
once. Timeouts, limits and decompression live in
`simple_http::ClientConfig`; SNI, CA bundle, name verification, client
certificates and the ALPN list in `simple_http::TlsClientConfig`. Timeouts are
layered on the explicit `Stream` path: `response_head_timeout` (TTFB) and
`body_idle_timeout` (silence between body reads), overridable per request via
`StreamSpec`; the buffered `send()` keeps the overall `request_timeout`.

Concurrency is deliberately simple: each `Client` keeps **one** TCP connection to
its first target origin — HTTP/1.1 requests queue on it (FIFO), HTTP/2 requests
multiplex over it, and a transport failure redials it once, transparently, before
the retry policy comes into play. Requests to another origin dial a throwaway
connection that closes after the exchange. This replaces connection pools,
per-host limits and idle-connection bookkeeping entirely (no
`max_conns_per_host`/`close_idle`/`connections_reused`). For resilience
`cfg.retry` retries a failed request (`max_retries`, exponential backoff, a
caller-chosen condition — the default only retries idempotent methods /
provably-unprocessed streams / free-redial-that-still-failed transport loss /
dials that sent nothing, and the request's own `request_timeout` still bounds
the whole retry chain including backoff). Sharing one `Client` across threads
is supported — each thread's io_context must outlive the sessions bound to it,
so a retiring thread drains its context before destroying it.

`Request` is the one request message type shared by both sides: build one with
`set_method`/`set_url`/`set_target`/`set_body`/`basic_auth` and hand it to
`client.send(shared_ptr<Request>)` (the Go `http.Client.Do` shape), or use the
builder chain above. For WebSocket, `client.open_websocket("ws://host/chat")`
returns the same `WebSocket` handle a server hands its ws handlers. The session
layer (`detail::ClientStream`/`ClientSession`) is internal — not part of the
documented public API.

---

## 📦 Requirements

- **C++23** or newer (GCC 13+, Clang 20+).
- **Boost.Asio**, **OpenSSL** and **glaze** (header-only — it backs the JSON
  helpers, which are on by default). Everything else — compression — is opt-in
  and brings its own two (zlib, brotli). HTTP/3, when enabled, additionally
  needs ngtcp2 and nghttp3.
- **xmake** for the example targets and dependency management; **CMake** is supported for consumption.

---

## 🛠 Configuration Macros

| Macro | Description |
| :--- | :--- |
| `SIMPLE_HTTP_USE_BOOST_REGEX` | Uses `boost::regex` instead of `std::regex` for route matching. |
| `SIMPLE_HTTP_ENABLE_LOG` | Master switch for the logging facade (`1` by default). Set to `0` and every `SIMPLE_HTTP_*_LOG` expands to `((void)0)`. |
| `SIMPLE_HTTP_LOG_ACTIVE_LEVEL` | Compile-time floor, `0` (Trace) … `5` (Critical). Records below it are discarded at compile time, so they cost nothing at the call site. |
| `SIMPLE_HTTP_ENABLE_HTTP3` | Compiles in the HTTP/3 engine and the QUIC listener (ngtcp2 + nghttp3). Off by default: without it `ServerConfig::quic` and the QUIC tunables do not exist, so a build that does not define it cannot ask for a UDP side at all. |

WebSocket support, response-body compression (gzip/brotli), WebSocket
permessage-deflate and OpenAPI have **no macro** — they are always compiled in,
and whether they take effect is a runtime setting (an OpenAPI operation is
collected only for routes whose `openapi::doc()...` descriptor you wrote, and
is served only when you call `serve_openapi`). So is UNIX-domain socket support
(availability is reported by Asio, not configured). zlib, brotli and glaze are
normal library dependencies.

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
server.route({simple_http::Method::Get}, "/whoami", [](simple_http::RequestPtr, simple_http::ResponsePtr res,
                           simple_http::SslHandle ssl) -> asio::awaitable<void> {
    X509* cert = SSL_get_peer_certificate(*ssl);  // owned by the caller: X509_free it
    // …
});
```

---

## 🧭 Routing, Middleware & Proxy

```cpp
// Exact and regex routes; the first match wins, `fallback` takes the rest.
// A route is a (method, path) pair: methods first, as a braced list or any
// iterable of `simple_http::Method` (a `std::vector` from config works too).
// `any_methods` registers every method. GET implies HEAD, and a path whose
// method does not match is answered 405 + `Allow`.
server.route({simple_http::Method::Get}, "/world", handler);
server.route({simple_http::Method::Post, simple_http::Method::Put}, "/users", handler);
server.route(simple_http::any_methods, "/webhook", handler);      // any method
server.route_regex({simple_http::Method::Get}, "^/api/(.*)$", handler);
server.fallback(not_found);

// Middleware — the Go net/http / tower shape. A middleware wraps the rest of
// the chain: code before `co_await next(...)` runs before the handler, code
// after it runs after, and never calling next() short-circuits the request:
server.use([](simple_http::RequestPtr req, simple_http::ResponsePtr res,
              simple_http::SslHandle ssl, simple_http::Next next) -> asio::awaitable<void> {
    if (!authorized(req)) {
        co_await res->status(401).send("unauthorized");
        co_return; // answered here — the chain never runs
    }
    co_await next(std::move(req), std::move(res), ssl);
    log(res->status()); // the after-phase can observe the response
});

// Multiple `use()` run in order, outermost first. The request has a
// per-request state slot (Go context / tower Extensions) for handing data from
// a middleware to the handler:
server.use([](simple_http::RequestPtr req, simple_http::ResponsePtr res,
              simple_http::SslHandle ssl, simple_http::Next next) -> asio::awaitable<void> {
    req->set_state(Principal{authorized_name(req)}); // handler reads it with req->get_state<Principal>()
    co_await next(std::move(req), std::move(res), ssl);
});

// A group scopes a prefix and middleware to just its routes (chi/gin Group,
// axum nest); a route can also carry its own per-route middleware (gin/echo
// per-route, axum route_layer) that wraps only its handler:
server.group("/api", {simple_http::middleware::basic_auth("svc", "s3cret")},
             [](simple_http::Router &api) {
                 api.route({simple_http::Method::Get}, "/users", users_handler); // → /api/users
                 api.route({simple_http::Method::Post}, "/users",
                           {rate_limit_middleware}, create_user_handler);
             });

// Built-ins: request_id / access_log / recovery / basic_auth (+ CORS below).
server.use(simple_http::middleware::request_id());
server.use(simple_http::middleware::access_log());

// Trusted-proxy awareness and path normalization, like chi/tower-http:
server.use(simple_http::middleware::real_ip({"127.0.0.1", "10.0.0.0"})); // X-Forwarded-For → ClientIp state
server.use(simple_http::middleware::clean_path()); // %XX / "//" / "/./" normalized; ".." → 400
server.use(simple_http::middleware::strip_prefix("/api")); // mount a sub-app registered from "/"
server.use(simple_http::middleware::redirect_slashes());   // "/x/" → 301 "/x"; cookies via req->cookie() / res->set_cookie()

// Security response headers, one line (tower-http SetResponseHeader / Spring
// Security Headers): X-Frame-Options DENY + nosniff + Referrer-Policy by
// default; CSP / HSTS are off until configured (HSTS only ever goes out over
// TLS); a handler overrides per-route with res->replace_header(...):
server.use(simple_http::middleware::secure_headers(simple_http::middleware::SecureHeadersConfig{
    .content_security_policy = "default-src 'self'",
    .hsts_max_age = std::chrono::seconds{31536000},
}));

// CORS: an OPTIONS preflight is answered 204 here and never reaches a route.
// See the CORS section for the policy.
server.cors(simple_http::CorsConfig{.allow_origins = {"https://app.example"}});
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
headers are stripped. Each proxy route carries its **own** `ClientConfig` (TLS
trust, client certificates, timeouts), so an internal mTLS backend and a public
one never share credentials, and a `Client` is built per proxied request — no
pool, no cross-request head-of-line blocking.

---

## 🔌 WebSocket

`ws_route` hands you the socket directly:

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

The handle is safe from any coroutine and for full-duplex use: `read()` and
`write*()` may run concurrently, writes are serialized through an internal pump,
and every operation first hops back to the connection's executor. `write`/`write_text`/
`write_binary` take the payload **by value** (moved into the queue, safe to build
and await later); the `_view` variants borrow the caller's bytes and complete
before returning, so a direct `co_await ws->write_view(buf, text)` needs `buf` to
outlive only the await.

Reads are decoupled from the handler on purpose. The transport is drained by an
internal reader into a bounded inbound queue and `read()` dequeues from it, so a
handler that serializes `read → write` cannot deadlock the connection against a
peer that stops reading while still sending: the socket keeps draining even while
the handler's write is blocked. The queue cap is `EngineLimits::ws_read_ahead_bytes`
(default 4 MiB, payload plus a per-message overhead), and it is the back-pressure —
once full the reader stops until `read()` makes room. Client-side, the same knob is
`WebSocketSpec::read_ahead_bytes`.

An `Upgrade: websocket` request is answered 101 by the HTTP/1.1 engine; a
protocol error, a received Close and invalid UTF-8 are all answered with the
correct Close code **after** the handler's pending echoes, so ordering on the
wire matches what a read-driven handler would have produced.

---

## 📑 Queries, Forms & SSE

Query strings and request bodies are parsed into typed views — the Go
`r.URL.Query()` / axum `Query<T>` / `Form<T>` base:

```cpp
// The URL query, decoded (+ → space, %XX → byte), repeated names kept:
if (auto q = req->query_params().get("q")) { /* q is a std::string_view */ }
for (auto tag : req->query_params().get_all("tag")) { /* repeated */ }

// a urlencoded form body:
auto form = co_await simple_http::read_urlencoded_body(*req); // expected<QueryParams, error_code>

// a multipart body (file uploads): boundary from Content-Type
auto upload = co_await simple_http::read_multipart_body(*req); // expected<vector<MultipartPart>, error_code>
//   part.name / part.filename / part.content_type / part.data

// a JSON body and response — structs via glaze reflection, on by default
// (axum Json<T> / Go json.Marshal+Decode):
struct Pet { std::int64_t id{}; std::string name; };
auto pet = co_await simple_http::read_json_body<Pet>(*req);      // expected<Pet, error_code>
co_await simple_http::write_json(res, *pet, 201);                // one-shot JSON, app/json

// typed request-parameter extraction — the axum Path<T> / Query<T> extractors:
struct Params { std::int64_t page; std::string tag; };
const auto p = simple_http::path_params<ItemParams>(*req);       // template captures, by field name
const auto q = simple_http::query_params<Params>(*req);          // ?page=2&tag=x
// (a urlencoded form body parses the same way: parse_params<Params>(form))
```

Rate limiting — a global token bucket or per-key buckets, answered 429 with
`Retry-After` (tower-http RateLimitLayer / golang.org/x/time/rate):

```cpp
server.use(simple_http::middleware::rate_limit(
    simple_http::middleware::make_token_bucket(100, 200)));        // 100 req/s, burst 200

server.use(simple_http::middleware::rate_limit(
    std::make_shared<simple_http::middleware::RateLimiter>(10, 20), // per-client: 10/s, burst 20
    [](const simple_http::RequestPtr &req) { return std::string{req->peer_address()}; }));
```

Responses get a fluent redirect and a thin SSE (Server-Sent Events) wrapper:

```cpp
co_await res->redirect("/login").send("");            // 302 + Location
co_await res->redirect("/gone", simple_http::status::permanent_redirect).send("");

co_await res->sse_begin();                            // text/event-stream + no-cache
co_await res->sse_event("hello");                     // data: hello\n\n
co_await res->sse_event("l1\nl2");                    // two data: lines, one event
co_await res->sse_event("payload", "update", "42");   // event: / id: fields too
co_await res->sse_comment("keepalive");               // : keepalive\n\n
```

---

## 🔗 Client: redirects, cookies & auth

The convenience level (`get`/`post`/`put`/`head`/`del`/`patch`) can follow
redirects and maintain a cookie jar — the `http.Client` experience:

```cpp
simple_http::ClientConfig cfg;
cfg.max_redirects = 10;                                  // 0 (default) = return the 3xx itself
cfg.cookie_jar = std::make_shared<simple_http::CookieJar>();
simple_http::Client client{co_await asio::this_coro::executor, cfg}; // pinned to the caller's executor (model A)

auto r = co_await client.get("https://host/start");      // follows 301/302/303/307/308
```

Followed hops follow Go's semantics: 301/302/303 turn a POST/PUT/DELETE into a
GET without a body, 307/308 keep the method and body, an `https → http`
downgrade is refused (`client_errc::redirect_to_insecure`), and a
`Set-Cookie` from a redirect is replayed on the next hop. A `Request` also
gains `req->basic_auth("user", "pass")` for outbound HTTP Basic, and the builder
has `bearer_auth(token)`.

---

## 📜 OpenAPI & Swagger UI

`route` is the registration point, so an OpenAPI 3.1 document can be *collected*
from your routes and schemas instead of maintained by hand. Documentation is
per-route metadata: the optional trailing argument of `route()` is an operation
descriptor built with `openapi::doc()`. A route without one is not documented
and pays nothing; the document is served only when you call `serve_openapi`.
There is no compile-time on/off switch — glaze is a default dependency (it backs
the JSON helpers below), and every templated setter on the descriptor reduces
its type to a schema string at the call site:

```cpp
struct Pet {
    std::int64_t id{};
    std::string name;
    std::string status{"available"};
    struct glaze_json_schema {  // optional field-level metadata
        glz::schema status{.description = "lifecycle state",
                           .enumeration = std::vector<std::string_view>{"available", "pending", "sold"}};
    };
};

server.openapi().title("petshop").version("1.0.0").server("http://127.0.0.1:7795");
server.route({Method::Get}, "/pets/{id}",
    [](simple_http::RequestPtr req, simple_http::ResponsePtr res) -> asio::awaitable<void> {
        const auto p = simple_http::openapi::path_params<PetParams>(*req);  // typed captures
        const auto body = glz::write_json(Pet{.id = p->id, .name = "rex"});
        co_await res->status(200).content_type(simple_http::mime::app_json)
            .send(body ? *body : std::string{"{}"});
    },
    simple_http::openapi::doc()
        .path_params<PetParams>()              // field names must equal the {name}s, checked at registration
        .response<Pet>()                       // success body (status defaults to 200)
        .summary("get a pet")
        .operation_id("getPet")
        .error<simple_http::openapi::ErrorBody>(404, "no such pet"));
```

- One `route()` serves both plain and documented routes — there is no separate
  typed overload set and no macro split. Build the descriptor with the named
  methods `.request_body<T>()`, `.response<T>(status)`, `.error<T>(status)`,
  `.path_params<P>()`, `.query<T>()`/`.header<T>()`/`.cookie<T>()`,
  `.response_header<T>()`, `.security({...})`, `.summary()`, `.tag()`, ...
- The document's `(method, path)`, request/response **schemas**, **path/query/header
  parameters**, **security schemes**, and **error responses** are derived from your
  types — `{name}` segments must match a path-parameter struct's field names, checked
  at registration. Path parameters you do not declare are implied by the template
  (string schema).
- Bodies are **validated**: `openapi::read_body<T>(req, err)` rejects unreadable or
  malformed bodies with 400 and JSON that misses a required field with 422.
- Serve it all:
  ```cpp
  server.serve_openapi("/openapi.json");                 // the OAS 3.1 document
  server.serve_swagger_ui("/swagger", "/openapi.json");  // Swagger UI (CDN assets)
  ```
  Object schemas are deduplicated into `components.schemas` and referenced by `$ref`
  (the document is rendered without `$defs`, which Swagger UI rejects mid-document).
- See `test/openapi_demo.cpp` (a rite-of-passage petshop with template routes, headers,
  security and tricky types) and `test/openapi_verify.py` (41 end-to-end checks).

---

## 🌐 CORS

`CorsConfig` is the built-in policy. An `OPTIONS` carrying `Origin` **and**
`Access-Control-Request-Method` is answered **204** right here and never reaches a
route — a preflight for an origin the policy rejects gets the same 204, just
without the CORS headers. That is why it is answered here rather than left to
routing: nothing in the engines special-cases `OPTIONS`, so a preflight passed to
the router comes back 404 and the browser fails it whatever the route offers.

```cpp
server.cors(simple_http::CorsConfig{
    .allow_origins = {"https://app.example.com", "https://*.corp.example"}, // exact + subdomain wildcards; {} = any
    .allow_credentials = true,                     // mirrors the origin, never "*"
    .allow_methods = {"GET", "POST", "DELETE"},    // {} = whatever the browser asks
    .allow_headers = {"content-type", "authorization"},
    .expose_headers = {"x-request-id"},
    .max_age = std::chrono::seconds{600},
});
```

| Field | Default | Meaning |
| --- | --- | --- |
| `allow_origins` | `{}` | Serialized origins — `scheme://host[:port]`, no trailing slash — compared whole and case-insensitively. Empty, or a literal `"*"`, allows any origin. A `*` in the host is a **subdomain wildcard** (gin/rs-cors form): `"https://*.example.com"` matches `https://app.example.com` and `https://a.b.example.com` but not the bare `https://example.com`, and a scheme-less `"*.example.com"` matches any scheme. |
| `allow_origin_fn` | `nullptr` | A custom `bool(std::string_view origin)` decision consulted after `allow_origins` — an origin accepted by either passes (rs/cors `AllowedOriginValidator`, tower-http `AllowOrigin::predicate`): per-tenant tables, suffix rules, a live config lookup. |
| `allow_credentials` | `false` | Sends `Access-Control-Allow-Credentials: true`. `*` is illegal alongside it, so the request's own origin is mirrored instead. |
| `allow_methods` | `{}` | Methods advertised on a preflight. Empty echoes the preflight's own `Access-Control-Request-Method`, so the default does not silently break a `PUT`/`DELETE` API. |
| `allow_headers` | `{}` | Empty echoes the preflight's `Access-Control-Request-Headers` (bounded — see `max_echoed_request_headers`). Non-empty is enforced: it is echoed only if every requested name is on it. |
| `expose_headers` | `{}` | Response headers a browser may read. Sent on actual responses only. |
| `max_age` | `0` | `Access-Control-Max-Age` on a preflight. Zero omits the field and leaves the browser its own (very short) default. |
| `max_echoed_request_headers` | `1024` | Byte cap on an echoed `Access-Control-Request-Headers`; past it the field is omitted and the preflight fails. |

Things worth knowing before relying on it:

- **`Vary: Origin`** is added whenever the origin is mirrored — credentials on, or
  an explicit allowlist — because the reply then depends on the request's
  `Origin`, and a cache keyed on the URL alone must not hand one origin's answer
  to another. Under a `*` policy the reply is origin-independent and no `Vary` is
  sent.
- **A rejected origin is not a blocked request.** The route still runs; the CORS
  headers are simply omitted, so the browser is the one that refuses the reply.
  CORS is a browser gate, not authorization — enforce access in the handler or in
  middleware.
- **WebSocket upgrades are not covered.** `ws_route` and `ws_proxy` connections are
  looked up ahead of dispatch, so they get no CORS handling here. A browser
  WebSocket handshake is not subject to CORS in the first place, so this is a
  boundary to know rather than a gap to work around — validate `Origin` in the
  handler if you need it.
- **A reverse-proxied upstream that sends its own `Access-Control-Allow-Origin`**
  produces two of them, which browsers reject outright. Strip CORS headers at the
  upstream, or handle CORS in a `use()` middleware instead.

A policy this config cannot express — a per-path allowlist, one looked up at
request time, a PNA preflight — goes in a `use()` middleware instead. The built-in
CORS middleware runs only for requests carrying an `Origin`; a `use()` middleware
runs for every request, so check first (or start from `make_cors_middleware()`),
and add your own `Vary: Origin`:

```cpp
server.use([](simple_http::RequestPtr req, simple_http::ResponsePtr res,
              simple_http::SslHandle ssl, simple_http::Next next) -> asio::awaitable<void> {
    if (const auto origin = req->header("origin");
        origin && (req->path().starts_with("/public") || *origin == "https://app.example")) {
        res->header(simple_http::field::access_control_allow_origin, std::string{*origin});
        res->header(simple_http::field::vary, "Origin");
    }
    co_await next(std::move(req), std::move(res), ssl);
});
```

`make_cors_middleware()` is public if you would rather narrow the built-in policy
than rebuild it — mix it into a group or a route like any other middleware.

---

## 🔒 Security Headers

One line gives every response a defensive header baseline — the equivalent of
tower-http `SetResponseHeader`, Go `unrolled/secure`, or Spring Security's
header writers:

```cpp
server.use(simple_http::middleware::secure_headers());  // the baseline
// or a per-site policy:
server.use(simple_http::middleware::secure_headers(simple_http::middleware::SecureHeadersConfig{
    .content_security_policy = "default-src 'self'",
    .frame_options = "SAMEORIGIN",                       // a site that embeds itself
    .hsts_max_age = std::chrono::seconds{31536000},
    .hsts_include_subdomains = true,
}));
```

| Field | Default | Meaning |
| --- | --- | --- |
| `content_security_policy` | `""` | `Content-Security-Policy` value. **Off until configured** — CSP is a per-site policy, and a guessed one breaks the site. |
| `frame_options` | `"DENY"` | `X-Frame-Options` — `"DENY"` / `"SAMEORIGIN"`; empty = off. |
| `no_sniff` | `true` | `X-Content-Type-Options: nosniff`. |
| `referrer_policy` | `"strict-origin-when-cross-origin"` | `Referrer-Policy`. |
| `x_xss_protection` | `""` | `X-XSS-Protection` — off by default (deprecated; browsers ignore it). |
| `permissions_policy` | `""` | `Permissions-Policy` — off until configured. |
| `hsts_max_age` | `0` | `Strict-Transport-Security` — `0` = off. **Only ever written over TLS** (a plaintext hop would see the browser ignore it). |
| `hsts_include_subdomains` / `hsts_preload` | `false` | The matching HSTS directives. |

The headers are written **before** routing (a one-shot `send()` moves the head
away, so after would be too late), so every answer — the handler's, and the
router's built-in 404/405/OPTIONS — carries them. A handler that needs a
different value overrides it per-route without a duplicate on the wire:

```cpp
co_await res->replace_header(simple_http::field::x_frame_options, "SAMEORIGIN").send("");
```

---

## 📁 Static Files

Serve a directory — build output, a docs site, a single-page app — without ever
letting a request path near the filesystem. The root is scanned **once**, at
startup, into a table of canonical paths; at request time a URL is decoded,
normalized and looked up in that table. Nothing else.

That single decision is the whole design. Directory traversal cannot be
expressed in a normalized key, and even if it could, no such key exists. Symlinks
are skipped during the scan and never followed, so nothing outside the root can
become reachable. No `stat()` or `open()` ever happens on a path the peer chose,
so there is no window between the check and the use. Only regular files are
admitted, so a FIFO in the tree cannot hang a worker. And because ETag and
`Last-Modified` come from the scan's own metadata, a `304` is answered without
opening anything.

```cpp
sh::StaticFilesConfig cfg;
cfg.table.root = "./web/dist";
cfg.table.immutable_prefixes = {"/assets/"};  // content-hashed build output
cfg.spa_fallback = "index.html";              // for a client-side router; "" = off

auto site = std::make_shared<sh::StaticFiles>(std::move(cfg));
std::string error;
if (!site->load(error)) {
    std::cerr << error << '\n';  // a bad root is a startup failure, not a 404
    return 1;
}
server.static_files(std::move(site));
```

The site is a **stage** of routing rather than a route, so it runs after every
real route and before the fallback — `/api/...` always wins over a file that
happens to share its name, and a request the site declines still gets the
router's 404 rather than being left unanswered.

It answers `GET` and `HEAD`, conditional requests (`ETag`, `Last-Modified`,
`If-Range`), single-part `Range` with `206`/`416`, and negotiated pre-compressed
siblings (`app.js.br`, `app.js.gz`) with a per-representation validator. It never
compresses anything itself — it only picks between bytes already on disk.

What it deliberately leaves out: directory listings, multi-range responses,
following symlinks, and cache-freshness policy beyond three buckets (`immutable`
for `immutable_prefixes`, `no-cache` for HTML, an hour or a day for the rest).
Each of `StaticTableConfig`'s fields documents its own default and the reason for
it.

## 🗜 Response Compression

A server can gzip or brotli response bodies for clients that ask for it. The
codecs are **always compiled in** (zlib and brotli are library dependencies);
whether compression takes effect is a **runtime** decision, on both ends.

**Server** — enable it by mounting the middleware, the chi/gin/echo/fiber way:

```cpp
sh::Server server{cfg};
// Global: every route is compressible.
server.use(simple_http::middleware::compress({.min_bytes = 1024, .gzip_level = 6, .brotli_quality = 5}));
// Or scope it: a group, or a single route's middleware list.
server.group("/api", {simple_http::middleware::compress()}, register_api);
server.route({Method::Get}, "/report", {simple_http::middleware::compress()}, report);
```

There is no server-level compression switch — **the middleware is the switch**
(like `e.Use(middleware.Gzip())`). Mounting it on `use()` covers everything;
mounting it on a group or a route covers just that scope. `excluded_paths` and a
`skip` predicate are the Go `ExcludedPaths` / `Skipper` knobs.

Whether compression actually takes effect is then decided **per request**: the
request's `Accept-Encoding` and the response's size/type. A request that sends
`Accept-Encoding: identity`, a `Cache-Control: no-transform`, a response under
`min_bytes`, or a type that is already entropy-coded is served uncompressed.

Handlers need no changes. Everything written through `Response` — the reverse
proxy included — is compressed when the per-request rules allow it, with brotli
preferred over gzip.

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

**Client** — the other direction, controlled per `Client` (Go's
`http.Transport.DisableCompression`, inverted):

```cpp
simple_http::ClientConfig cfg;
cfg.auto_decompress = true;  // advertise br, gzip and decode what comes back
simple_http::Client http{co_await asio::this_coro::executor, cfg}; // pinned to the caller's executor
auto r = co_await http.get(url).send();
auto body = co_await r->read_all();
// body holds the original bytes; content-encoding and content-length are gone
```

The client takes `Accept-Encoding` from `cfg.accept_encodings`, filtered to what
the build can actually decode, unless the request sets that header itself (then it
is left alone). Decoding happens at the stream level, so `Response::read()`
hands out decoded bytes too — and the size caps in `read_all()` and the
convenience layer apply to the *decoded* size, which means a compression bomb
cannot slip past them by being small on the wire. A body that fails to decode
comes back as `client_errc::body_decode_failed` rather than as half a body.

---

## 📂 More Examples

- **[test/server.cpp](test/server.cpp)** — a server exercising every feature: routes, regex routes, middleware, h2c, WebSocket, a TLS listener with mTLS, and the reverse-proxy paths. Run it with `xmake run server`.
- **[test/client.cpp](test/client.cpp)** — the client's exercise program. It starts a server in-process and drives the client over http/https × HTTP/1.1/HTTP/2, including h2c, streaming and multipart uploads, multiplexing, single-connection reuse and TLS verification. Run it with `xmake run client`.
- **[test/loadgen.cpp](test/loadgen.cpp)** — a load generator built on the library's own client: HTTP/1.1, HTTP/2 (connection × stream multiplexing, the h2load `-c/-m` model) and WebSocket, printing an h2load-shaped summary for direct comparison with k6 / h2load / wrk (`xmake run loadgen http://127.0.0.1:7788/world -p h1 -n 300000 -c 100`).
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

Note what `/hello` does: it echoes the request body frame by frame and prints a line
per frame. That makes the server's stdout part of the measurement — leave it on the
terminal or send it to `/dev/null`. Redirecting it to a file turns the run into a test
of log throughput, and the number comes out around a third of the one above without
anything on the wire being slower.

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

Two more suites drive the same server with tools the repository does not carry, so
each one checks for what it needs and prints the preparation command rather than
skipping silently. They are described in full in [AGENTS.md](AGENTS.md):

```bash
test/conformance/run.sh   # h2spec, h1spec and Autobahn against the :7790/:7791/:7788 endpoints
test/stress/run.sh        # connection reuse and load, one gate per protocol
```

The first asks whether the protocol is implemented correctly and starts every case
on a fresh connection; the second asks whether a connection survives being reused,
which is the question the first cannot see. Both exit non-zero when a suite falls
below its recorded baseline.

Against the example server (`xmake run server`, plaintext on `:7788`, mTLS on `:7789`,
h2c-only on `:7790`, h1-only on `:7791`, a large-window h2 listener on `:7793`, and the
dual TCP+QUIC endpoint on `:7792`), with external clients:

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
