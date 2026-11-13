# simple_http

**A header-only HTTP server and client library for C++, built on Boost.Asio coroutines.**

It speaks HTTP/1.0, HTTP/1.1, HTTP/2 (h2/h2c), optional HTTP/3 over QUIC, and
WebSocket — with a coroutine-first API, a version-agnostic request/response model,
and no locks in the protocol engines.

- **Version**: `0.9.0` (see `include/simple_http/core/version.h`)
- **Language standard**: C++23 (both the CMake interface and the in-repo xmake build)
- **License**: MIT
- **Umbrella header**: `#include <simple_http.h>`

[English](#toc) · [中文](README.zh-CN.md)

---

<a id="toc"></a>
## Table of Contents

- [What is this](#what-is-this)
- [Features](#features)
- [Quick start](#quick-start)
- [Framework layer](#framework-layer)
- [Build, dependencies and macros](#build)
- [Server guide](#server-guide)
- [Client guide](#client-guide)
- [Architecture and concurrency](#architecture)
- [Testing and verification](#testing)
- [Known limitations](#known-limitations)
- [Style and contributing](#contributing)

<a id="what-is-this"></a>
## What is this

`simple_http` splits HTTP into a set of layers. Everything is implemented in headers
under `include/`; there are no `.cpp` files:

| Layer | Directory | Responsibility |
|---|---|---|
| `core` | `include/simple_http/core/` | Protocol-agnostic infrastructure: types, status codes, dates, URL encoding, compression, logging, I/O pool, limits |
| `proto` | `include/simple_http/proto/` | Version-agnostic `Request` / `ResponseWriter` / `Body` model; forms, JSON, multipart, WebSocket message layer |
| `transport` | `include/simple_http/transport/` | Byte-stream abstraction: plain TCP and TLS (with ALPN / mTLS) |
| `engine` | `include/simple_http/engine/` | Per-version engines: `h1/`, `h2/`, `h3/`; each implements the same `ResponseSink` behind the `ResponseWriter` facade |
| `handler` | `include/simple_http/handler/` | Handler type system, router/middleware, app state, extractors/validation, sessions/CSRF/JWT, static files, reverse proxy, rate limiting, CORS, security headers |
| `net` | `include/simple_http/net/` | Listeners, protocol sniffing, the `Server` facade |
| `client` | `include/simple_http/client/` | Outbound client: HTTP/1.1, HTTP/2, optional HTTP/3, WebSocket, SSE, proxies, cookies |
| `quic` | `include/simple_http/quic/` | QUIC transport wrapper (ngtcp2), compiled only with `SIMPLE_HTTP_ENABLE_HTTP3` |
| `openapi` | `include/simple_http/openapi/` | Per-route OpenAPI 3.1 document and Swagger UI |

The upper layers (`proto` / `handler` / `net` / the client facade) contain no
`if (version == ...)` branches. Version differences are confined to `engine/` and the
concrete `ResponseSink` implementations; `ResponseWriter` only forwards.

### Design principles

1. **Coroutines all the way down.** Handlers are `asio::awaitable<void>`; reading a
   request body, writing a response and waiting for cancellation are all awaitable, so
   backpressure is natural.
2. **One model for every version.** Application code sees only `Request` /
   `ResponseWriter` / `Router`; it does not care whether the wire is HTTP/1.x, HTTP/2 or
   HTTP/3.
3. **Transport and protocol are decoupled.** Engines read and write bytes through the
   `Transport` abstraction and do not depend on a concrete socket type.
4. **Concurrency model A.** Each connection is pinned to one single-threaded
   `io_context` in the `IoCtxPool`; the engine, writers and handlers for that connection
   run only on that thread, so `engine/` contains no mutexes or atomics.
   `ResponseWriter` / `WebSocket` / `Body` must be used on the connection's thread only;
   a cross-thread call is logged and aborted by `require_owner_thread`. Hop explicitly
   with `asio::post(req->executor(), ...)` when work has to move between threads.
5. **Explicit backpressure.** HTTP/2 request bodies are parked per stream, WebSocket has
   a bounded inbound read pump, and streaming responses go through a write pump. No path
   lets the peer push unbounded memory.
6. **Logging is decoupled from backends.** The library defines only `LogSink` and depends
   on no third-party logging library; adapters live on the consumer side.

<a id="features"></a>
## Features

### Server

- HTTP/1.0, HTTP/1.1, HTTP/2 (TLS ALPN, h2c `Upgrade`, h2c prior knowledge).
- HTTP/3 over QUIC (requires `SIMPLE_HTTP_ENABLE_HTTP3`; ngtcp2 + nghttp3), with Alt-Svc
  advertisement, connection migration and configurable 0-RTT.
- WebSocket over HTTP/1.1 `Upgrade` and HTTP/2 extended CONNECT (RFC 8441).
- Method-aware routing: exact paths, `{param}` segment trie, `{*rest}` catch-all,
  regex routes, host scoping, `any_methods`, fallback.
- Framework layer: typed application state, compile-time extractors, validation,
  sessions, CSRF and JWT (HS256/RS256/ES256 + JWKS).
- Middleware chain at global, group and per-route scope: CORS, timeouts, recovery, auth,
  access logging, request-id, real-ip, rate limiting, security headers, response
  compression.
- Streamed responses, SSE, response trailers.
- Request cancellation and streaming-friendly timeouts (absolute before the response
  head, idle after it).
- Static files (ETag / Range / 304 / SPA fallback / precompressed `.br` / `.gz`),
  `send_file`, and streaming uploads to disk.
- Forms, JSON, multipart (whole-body and streaming), typed path/query parameters.
- HTTP reverse proxy and byte-level WebSocket reverse proxy.
- OpenAPI 3.1 document + Swagger UI, collected from per-route metadata.
- TLS 1.3 by default, optional TLS 1.2, mTLS, ALPN, UNIX domain sockets, multiple
  listeners.
- HTTP/2 Rapid Reset (CVE-2023-44487) protection, header/body size caps, idle timeouts.

### Client

- HTTP/1.1 and HTTP/2 (ALPN over HTTPS; h2c Upgrade or prior knowledge over plaintext).
- HTTP/3 (`HttpVersionPolicy::Http3`, explicit opt-in; QUIC carries its own TLS 1.3).
- Convenience API (`get` / `post` / … / `send`) and an explicit full-duplex `Stream`.
- Streaming multipart uploads and file-body uploads without buffering the whole body.
- Automatic `Expect: 100-continue`, request trailers, and 429/503 `Retry-After` retries.
- Redirects, CookieJar, Basic/Bearer auth, automatic decompression, buffered multipart.
- Outbound proxies: HTTP CONNECT, HTTPS proxy, SOCKS5, environment variables and
  `NO_PROXY`.
- Layered timeouts (connect, response-head TTFB, body idle), free transparent redials,
  and configurable retries.
- Server and client share one WebSocket message layer; the client WebSocket uses
  HTTP/1.1 Upgrade.
- SSE client (`SseStream`).

<a id="quick-start"></a>
## Quick start

### Minimal server

```cpp
#include <simple_http.h>

#include <chrono>
#include <csignal>
#include <print>
#include <thread>

namespace asio = boost::asio;
using namespace simple_http;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int main() {
    ServerConfig cfg;
    cfg.listen = InetAddress{"0.0.0.0", 7788, false}; // {host, port, v6_only}
    cfg.worker_threads = 4;

    Server server{cfg};
    server.route({Method::Get}, "/hello",
                 [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                     co_await res->status(status::ok).content_type(mime::text_plain).send("Hello World!");
                 });

    auto started = server.start(); // std::expected<StartReport, StartupError>
    if (!started) {
        for (const auto &failure : started.error().failures)
            std::println(stderr, "{}: {}", failure.name, failure.error.message());
        return 1;
    }

    // The I/O pool runs the workers; the main thread only waits for a signal.
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (g_stop == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    return 0;
}
```

`Server` creates the I/O thread pool in its constructor; `start()` binds the listeners
and starts the accept loops. `stop()` closes the listeners, waits for requests already
inside a handler (bounded by `shutdown_grace`), then closes remaining connections and
stops the pool.

### Routing, parameters and middleware

```cpp
Server server{cfg};

server.use(middleware::request_id());
server.use(middleware::access_log());
server.use(middleware::recovery());

server.route({Method::Get}, "/users/{id}",
             [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                 auto id = req->param("id"); // std::optional<std::string_view>
                 if (!id) {
                     co_await res->status(status::bad_request).send("bad id");
                     co_return;
                 }
                 co_await res->status(status::ok).send("user " + std::string{*id});
             });

server.group("/api", {middleware::basic_auth("svc", "s3cret")}, [](Router &api) {
    api.route({Method::Get, Method::Post}, "/items",
              [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                  co_await res->status(status::ok).send("items");
              });
});

server.fallback([](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
    co_await res->status(status::not_found).send("not found");
});
```

Implicit routing rules:

- Registering `{Method::Get}` also allows `HEAD`; the writer suppresses the response body.
- A path that exists with a different method → `405` + `Allow`; `OPTIONS` is answered
  automatically with `204` + `Allow`.
- A path that does not exist → `404` (or fallback).
- 405 and automatic OPTIONS are ordinary keep-alive responses; the engine drains an
  unread request body and reuses the connection.

### WebSocket

```cpp
server.ws_route("/chat",
    [](RequestPtr, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws->read(); // std::expected<WsMessage, error_code>
            if (!msg)
                break; // peer closed, or an error
            if (auto ec = co_await ws->write("echo: " + msg->data, msg->text))
                break;
        }
        co_return; // returning sends a Close frame and shuts the socket down gracefully
    });
```

### Client

```cpp
asio::awaitable<void> fetch() {
    Client http{co_await asio::this_coro::executor}; // pinned to a single-threaded executor

    auto r = co_await http.get("https://example.com/").send();
    if (!r)
        co_return;
    std::println("status={}", r->status());
    auto body = co_await r->read_all();
}
```

A full-duplex streaming request:

```cpp
auto up = co_await http.open_stream("https://example.com/upload",
                                    {.method = Method::Post});
if (!up)
    co_return;

co_await up->write("hello ");
co_await up->finish("world");

auto head = co_await up->read_head();
while (auto chunk = co_await up->read()) {
    if (chunk->eof)
        break;
    use(chunk->data);
}
```

### OpenAPI and Swagger UI

```cpp
struct Pet {
    std::int64_t id{};
    std::string name;
};
struct PetParams {
    std::int64_t id{};
};

server.openapi().title("petshop").version("1.0.0").server("http://127.0.0.1:7795");

server.route({Method::Get}, "/pets/{id}",
    [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        auto p = openapi::path_params<PetParams>(*req);
        auto body = glz::write_json(Pet{.id = p->id, .name = "rex"});
        co_await res->status(status::ok).content_type(mime::app_json).send(body ? *body : "{}");
    },
    openapi::doc()
        .path_params<PetParams>()
        .response<Pet>()
        .summary("get a pet")
        .operation_id("getPet")
        .error<ErrorBody>(status::not_found, "no such pet"));

server.serve_openapi("/openapi.json");
server.serve_swagger_ui("/swagger", "/openapi.json");
```

### Build and run

```sh
xmake build -j 12
xmake run diff_openapi_server   # petshop + OpenAPI document on :7795
```

`test/fixtures/conformance_server.cpp` is the minimal fixture the external suites
drive (`:7788` sniffing + WebSocket, `:7790` h2c, `:7791` h1, `:7792` TCP+UDP h3).
`test/diff/server/cpp/server.cpp` is the full differential server; it and its Go
counterpart register the same route table and are compared request by request.

<a id="framework-layer"></a>
## Framework layer

The `handler/` layer includes the pieces a small application would otherwise build
around the router: shared state, typed extraction, validation, sessions/CSRF/JWT, host
scoping and catch-all routes. Everything is opt-in and layer-local — no protocol engine
knows about it.

### Application state

Register shared, type-keyed state on the server, then read it without copying per
request:

```cpp
struct AppState {
    std::shared_ptr<Db> db;
};

server.state(std::make_shared<AppState>(AppState{db}));

server.route({Method::Get}, "/users", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
    const AppState *state = req->app_state<AppState>();
    if (!state) {
        co_await res->status(status::internal_server_error).send("state missing");
        co_return;
    }
    auto rows = co_await state->db->query(...);
    co_await res->status(status::ok).send(rows);
});
```

State is shared by every connection (the handler layer may use locks; the engines do
not). Register it before `start()`, like middleware. Host child routers share the same
state registry.

### Typed extractors and validation

`extract<...>(handler)` runs extractors before the handler and answers 400/422/500 on
failure; the handler is not entered. The wrapper is still an ordinary `Handler`, so
routes, groups, middleware and OpenAPI descriptors are unchanged.

```cpp
struct UserParams { int id{}; };
struct CreateUser { std::string name; int age{}; };

server.route({Method::Get}, "/users/{id}",
    extract<Path<UserParams>, State<AppState>>(
        [](const UserParams &p, AppState *state, RequestPtr, ResponsePtr res)
            -> asio::awaitable<void> {
            co_await res->status(status::ok).send(std::to_string(p.id));
        }));

server.route({Method::Post}, "/users",
    extract<Valid<Json<CreateUser>>>(
        [](CreateUser in, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(status::created).send(in.name);
        }));
```

Extractors: `Path<T>`, `Query<T>`, `Json<T>`, `Form<T>`, `State<T>`, `Header<T, "name">`, `Cookie<T, "name">`. At most one body
extractor (`Json`/`Form`) may appear per `extract<...>`.

Validation is an opt-in specialization; a failure becomes 422 with a field-error array
in the JSON `details`:

```cpp
template <> struct simple_http::Validator<CreateUser> {
    static std::expected<void, std::vector<FieldError>> validate(const CreateUser &v) {
        std::vector<FieldError> errors;
        push_if(errors, required_field("name", !v.name.empty()));
        push_if(errors, range("age", v.age, 1, 150));
        return errors.empty() ? std::expected<void, std::vector<FieldError>>{}
                              : std::unexpected(std::move(errors));
    }
};
```

### Sessions and CSRF

```cpp
auto sessions = std::make_shared<MemorySessionStore>();
server.use(middleware::session(sessions, SessionConfig{
    .cookie_name = "__Host-sid",
    .secure = true,
    .http_only = true,
    .same_site = "Lax",
}));
server.use(middleware::csrf(CsrfConfig{
    .cookie_name = "__Host-csrf",
    .header_name = "x-csrf-token",
}));

server.route({Method::Post}, "/login",
    [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        if (auto *s = session(*req)) {
            s->regenerate_id();     // session-fixation defense
            s->set("uid", "42");
        }
        co_await res->status(status::no_content).send("");
    });
```

The cookie carries only an opaque id; values live in the `SessionStore`. The interface
uses `any_completion_handler` completions; `save` takes `SessionData` by value, so an
asynchronous store can keep it safely. A Redis/database adapter is a consumer-side
implementation. The default `MemorySessionStore` is in-process and lock-protected.
CSRF is double-submit: a safe
request establishes the cookie, and an unsafe request must echo it in the header. Form
posts that cannot set a header should validate the hidden field themselves or use a
small adapter.

### JWT

```cpp
server.use(middleware::jwt(JwtConfig{
    .algorithms = {"RS256"},
    .public_key_pem = read_file("public.pem"),
    .issuer = "https://issuer.example",
    .audience = "my-api",
}));
server.use(middleware::require_scope("users:write"));

server.route({Method::Get}, "/me", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
    const Principal *principal = simple_http::principal(*req);
    co_await res->status(status::ok).send(principal ? principal->subject : "");
});
```

The verifier enforces the configured algorithm allowlist (rejecting `none`), checks the
signature with OpenSSL (HS256, RS256, ES256), and validates `exp` (required by default),
`nbf`, `iss` and `aud`. It publishes a `Principal` with subject, scopes and roles.

A static JWKS document selects keys by `kid` (RSA, EC P-256 and `oct` keys are
supported); fetch it with the provided helper and pass the JSON to the middleware:

```cpp
auto jwks_body = co_await fetch_jwks_json(client, jwks_url);
server.use(middleware::jwt(JwtConfig{
    .algorithms = {"RS256", "ES256"},
    .jwks_json = jwks_body ? *jwks_body : std::string{},
    .issuer = "https://issuer.example",
    .audience = "my-api",
}));
```

The document is parsed once, at middleware construction. Key refresh is the caller's
policy (fetch again and rebuild the middleware). Token issuance and OAuth2/OIDC
discovery are not part of this layer.

### Host routing and catch-all paths

```cpp
server.host("api.example.com", [](Router &api) {
    api.route({Method::Get}, "/users", users_handler);
});
server.host("*.example.com", [](Router &sub) {
    sub.route({Method::Get}, "/", subdomain_handler);
});

server.route({Method::Get}, "/files/{*rest}",
    [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(status::ok).send(std::string{req->param("rest").value_or("")});
    });
```

Exact hosts beat wildcard suffixes, and the longest wildcard suffix wins. A
`*.example.com` pattern does not match `example.com` or `evil-example.com`. A host
pattern without a port ignores the request's port; one with `:port` compares it too.
`{*rest}` must be the final segment and captures the remaining path including `/`.
Host routing currently applies to HTTP dispatch, not WebSocket lookup.

<a id="build"></a>
## Build, dependencies and macros

### Dependencies

| Dependency | Purpose |
|---|---|
| Boost (asio, regex) | Async I/O and regex |
| OpenSSL | TLS / mTLS / QUIC TLS |
| glaze | JSON reflection and OpenAPI schemas (a normal library dependency) |
| zlib, brotli | Response compression, request decompression and WebSocket permessage-deflate (always compiled in) |
| ngtcp2, nghttp3 | HTTP/3 only (QUIC/QPACK/framing) |
| Catch2 | Unit tests only |

Compression and WebSocket extensions have **no feature macro**: zlib/brotli are normal
dependencies, and whether they take effect is a runtime decision
(`middleware::compress`, `middleware::decompress_request`,
`EngineLimits::ws_compression`, `ClientConfig::auto_decompress`).

By default the server does not compress responses, decompress requests or rewrite
`Content-Encoding` on a reverse proxy — the same defaults as Go's `net/http`. Mount
`middleware::compress(cfg)` to compress responses and
`middleware::decompress_request(cfg)` to accept compressed request bodies; both can be
scoped globally, per group or per route. Reverse-proxy routes that do not mount the
decompression middleware forward the compressed body and headers untouched, matching
`httputil.ReverseProxy`.

### xmake

The repository uses xmake (`xmake.lua`). Packages come from the private repository
`fantasy-peak/xmake-repo` — the upstream ngtcp2 package does not provide the OpenSSL
crypto helper that HTTP/3 needs. The build enables dependency locking
(`package.requires_lock`).

```sh
xmake build -j 12                      # build; also refreshes build/compile_commands.json
xmake run test_unit                    # Catch2 unit tests
xmake run diff_openapi_server          # petshop OpenAPI demo on :7795
xmake run fixture_conformance_server   # minimal conformance/stress fixture
```

The test suites have one entry point, `test/run.sh` (see `test/README.md`):

```sh
test/run.sh fast      # unit
test/run.sh standard  # + independent Python implementations
test/run.sh full      # + Go differentials + external conformance/stress
```

The `simple_http` target itself is a `set_kind("static")` shell: it only exports
`include/` and the dependencies as `public`. All real content is in headers.

### CMake

`CMakeLists.txt` installs an INTERFACE target for downstream `find_package`:

```cmake
find_package(simple_http CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE simple_http::simple_http)
```

The CMake interface declares `cxx_std_23` and carries Boost, OpenSSL, glaze, zlib and
brotli. Three current details matter:

- The in-repo `xmake.lua` and the CMake interface both require C++23.
- The CMake INTERFACE target does **not** link ngtcp2/nghttp3, so HTTP/3 is not enabled
  through CMake; use the xmake build or wire those dependencies yourself.
- The exported `simple_httpTargets.cmake` links `glaze::glaze`, but
  `cmake/simple_httpConfig.cmake.in` has no `find_dependency(glaze)`; downstream may
  need to call `find_package(glaze CONFIG REQUIRED)` itself.

### Compile-time macros

| Macro | Default | Meaning |
|---|---|---|
| `SIMPLE_HTTP_ENABLE_HTTP3` | undefined | Compile the HTTP/3 engine and QUIC listener; without it, fields such as `ServerConfig::quic` do not exist |
| `SIMPLE_HTTP_USE_BOOST_REGEX` | defined by this repo | Use Boost.Regex for route regexes instead of `std::regex` |
| `SIMPLE_HTTP_ENABLE_LOG` | `1` | Master logging switch; when `0`, logging macros expand to `((void)0)` |
| `SIMPLE_HTTP_LOG_ACTIVE_LEVEL` | `0` (Trace) | Records below this level are discarded at compile time |

UNIX domain sockets, WebSocket and OpenAPI have no feature macro. UNIX socket support is
decided automatically by `BOOST_ASIO_HAS_LOCAL_SOCKETS`; OpenAPI is always compiled, and
only routes with an `openapi::doc()` descriptor enter the document.

<a id="server-guide"></a>
## Server guide

### ServerConfig and listeners

Single-listener convenience fields:

```cpp
ServerConfig cfg;
cfg.listen = InetAddress{"0.0.0.0", 7788, false}; // or UnixAddress{"/run/app.sock"}
cfg.worker_threads = 4;
cfg.max_connections = 0;      // 0 = unlimited; at the cap the kernel backlog applies backpressure
cfg.reuse_port = true;        // SO_REUSEPORT: one acceptor per worker
cfg.tcp_nodelay = true;
cfg.plaintext_protocols = PlaintextProtocols::Both; // Http1 / Http2 / Both (sniffing)
cfg.shutdown_grace = std::chrono::milliseconds{5000};
cfg.limits = EngineLimits{};  // protocol-engine tuning, see below

cfg.socket_setup   = [](asio::ip::tcp::socket &s) { /* per accepted connection */ };
cfg.listener_setup = [](asio::ip::tcp::acceptor &a) { /* per listener, after bind before listen */ };
```

Multiple listeners can be written directly into `cfg.listeners`, or configured with the
builder:

```cpp
server.listen(InetAddress{"0.0.0.0", 80})
      .name("http")
      .plaintext(PlaintextHttp::Http1WithH2cUpgrade);

server.listen(InetAddress{"0.0.0.0", 443})
      .tls(TlsConfig{.cert_chain_file = "cert.pem", .private_key_file = "key.pem"})
      .alpn(TlsHttp::Http1AndHttp2);
```

`start()` returns `std::expected<StartReport, StartupError>`; `run()` is the awaitable
form of the same operation:

- `StartPolicy::AllOrNothing` (default): if any listener fails, roll everything back.
- `StartPolicy::BestEffort`: keep the listeners that bound and report failures in
  `report.failures`.

`run()` is only a coroutine wrapper around `start()`; it does not drive an event loop.
Worker threads run in the internal I/O pool.

### TLS / mTLS

```cpp
cfg.tls = TlsConfig{
    .cert_chain_file = "server_cert.pem",
    .private_key_file = "server_key.pem",
    .mutual = true,
    .ca_file = "ca_cert.pem",
    .min_version = TlsMinVersion::Tls13, // default; Tls12 also accepts TLS 1.2
};
```

- The server accepts TLS 1.3 only by default; set `TlsMinVersion::Tls12` to also accept
  TLS 1.2.
- A TLS listener advertises ALPN `h2` + `http/1.1` by default.
- A handler that needs the peer certificate can take the third `SslHandle` argument:

```cpp
server.route({Method::Get}, "/whoami",
    [](RequestPtr, ResponsePtr res, SslHandle ssl) -> asio::awaitable<void> {
        if (!ssl) {
            co_await res->status(status::bad_request).send("plaintext");
            co_return;
        }
        X509 *cert = SSL_get1_peer_certificate(*ssl);
        if (cert)
            X509_free(cert);
        co_await res->status(status::ok).send("ok");
    });
```

### Handler shapes

A handler must be a coroutine returning `asio::awaitable<void>`; the form is selected by
arity:

```cpp
using RequestPtr  = std::shared_ptr<Request>;
using ResponsePtr = std::shared_ptr<ResponseWriter>;

// plain
awaitable<void>(RequestPtr, ResponsePtr);
// with the TLS handle
awaitable<void>(RequestPtr, ResponsePtr, SslHandle);
// WebSocket
awaitable<void>(RequestPtr, shared_ptr<WebSocket>);
```

If a handler throws, the engine answers 500; if the response head was already sent, it
closes the connection instead of writing a second status line. `middleware::recovery()`
turns this into a chain-level policy.

### Middleware

The middleware shape matches Go `net/http` / axum `from_fn` and supports both
coroutine and callback forms. A C++20 build (where Boost.Asio provides co_await)
accepts both; a C++17 build keeps only the callback form.

```cpp
// C++20: coroutine form
server.use([](RequestPtr req, ResponsePtr res, SslHandle ssl,
              CoroNext next) -> asio::awaitable<void> {
    auto t0 = std::chrono::steady_clock::now();          // before
    co_await next(std::move(req), std::move(res), ssl);  // run the chain
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    SIMPLE_HTTP_INFO_LOG("status={} elapsed={}ms", res->status(), ms); // after
});

// C++17 (and C++20): callback form
server.use([](RequestPtr req, ResponsePtr res, SslHandle ssl,
              Next next, HandlerCompletion done) {
    auto t0 = std::chrono::steady_clock::now();
    next(std::move(req), std::move(res), ssl,
         [res, t0, done = std::move(done)]() mutable {
             auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
             SIMPLE_HTTP_INFO_LOG("status={} elapsed={}ms", res->status(), ms);
             std::move(done)();
         });
});
```

- `use()` accepts either shape directly; use `make_middleware(...)` to normalize
  explicitly, and `make_middlewares(...)` to mix both in the
  `std::vector<Middleware>` arguments of `group()` / `route()` / `ws_route()`.
- Not calling `next()` short-circuits the chain.
- `use()` nests in registration order, outermost first; CORS is always the outermost.
- Group middleware applies only inside `group()`; per-route middleware wraps only that
  route.
- Per-request typed state: `req->set_state<T>(...)` / `req->get_state<T>()`, keyed by
  type, last-set-wins.

Built-in middleware (in `simple_http::middleware` unless noted):

| Factory | Purpose |
|---|---|
| `request_id()` | Reuse or generate `X-Request-Id`; write it to the response and expose `RequestId` state |
| `access_log()` | Record method, path, status, duration, peer and request-id after the chain |
| `recovery()` | Catch downstream exceptions; 500 before the head, close after it |
| `basic_auth(user, pass)` | HTTP Basic; 401 + `WWW-Authenticate` on failure |
| `real_ip(trusted)` | Parse `X-Real-IP` / `X-Forwarded-For` only when the direct peer is trusted; expose `ClientIp` state |
| `clean_path()` | Percent-decode and normalize `//`, `/./`; reject `..` / malformed paths with 400 |
| `strip_prefix("/api")` | Remove a path prefix, preserving the query |
| `redirect_slashes()` | 301 to the path without a trailing slash |
| `ws_origin(allowed, allow_missing=false)` | Validate WebSocket `Origin` (CSWSH defense) |
| `timeout(d)` | Streaming-friendly timeout: absolute before the head, idle after it; 504 or abort |
| `compress(cfg)` | Mounting enables response compression; decides per request by Accept-Encoding / type / size |
| `decompress_request(cfg)` | Mounting enables request-body decompression (gzip/br); unsupported coding → 415, decoded-size cap → 413 |
| `secure_headers(cfg)` | CSP / X-Frame-Options / nosniff / Referrer-Policy / HSTS, etc. |
| `rate_limit(bucket)` | Shared token bucket; 429 + `Retry-After` |
| `rate_limit(limiter, key_fn)` | Per-key buckets with a key cap and LRU eviction |
| `simple_http::make_cors_middleware(cfg)` | Mount the CORS policy as ordinary middleware on a group or route (top-level namespace) |

`server.cors(CorsConfig{...})` is the dedicated CORS entry point and always places the
policy outermost.

### Route matching order

A request is matched in this order (roughly nginx order):

1. Exact path (method-aware)
2. `{param}` template trie
3. HTTP reverse-proxy route
4. Local regex routes
5. Static-file stage
6. Fallback
7. Built-in 404

Details:

- `any_methods` is an empty method list and means any method, including extension tokens.
- If the same `(path, method)` is registered twice, the first registration wins.
- `route_regex` matches the whole path; inside a group the prefix is escaped.
- A 405 `Allow` header merges every method that matches the path; `OPTIONS` is always
  included.
- WebSocket lookup order is: local exact ws route → ws proxy → local regex ws route.
- CORS does not cover WebSocket upgrades; use `ws_origin()` for Origin validation.

### Request

```cpp
req->method();            // Method
req->method_token();      // raw method token (extension methods supported)
req->version();           // Version::Http1 / Http11 / Http2 / Http3
req->target();            // request target
req->path();              // path part of the target (raw bytes; use clean_path() to normalize)
req->query();             // raw query
req->query_params();      // QueryParams
req->param("id");         // template-route capture
req->header("host");      // optional<string_view>
req->headers();           // Headers (supports get_all)
req->cookie("session");   // optional<string_view>
req->peer_address();      // peer address
req->body();              // Body stream
req->set_state<T>(...);   // per-request typed state
req->get_state<T>();
req->cancel();            // cooperative cancellation
req->cancelled();
req->wait_cancelled();    // resumes on the waiter's own executor
```

Reading the body:

```cpp
// read it all
auto body = co_await req->body().read_all(); // expected<string, error_code>

// read chunk by chunk
for (;;) {
    auto frame = co_await req->body().read();
    if (!frame) break;
    if (frame->eof) break;
    use(frame->data);
}
```

Exceeding `max_body_bytes` produces 413; exceeding `max_header_bytes` produces 431.

### ResponseWriter

`ResponsePtr` is `std::shared_ptr<ResponseWriter>`. There are two response shapes:

```cpp
// one-shot
co_await res->status(status::ok).content_type("text/plain").send("hello");

// streamed (chunked on HTTP/1.1, DATA frames on HTTP/2)
co_await res->status(status::ok).begin();
co_await res->write("first ");
co_await res->finish("last");
```

Other capabilities:

```cpp
res->header("x-trace", "abc");          // append
res->replace_header("x-frame-options", "SAMEORIGIN"); // replace all same-name fields

co_await res->redirect("https://example.com/new", status::temporary_redirect).send("");

res->set_cookie("session", "abc", std::chrono::seconds{3600},
                "/", "", true, true, "Lax");

// response trailers (RFC 9110 §6.5): only on a streamed response
co_await res->status(status::ok).begin();
co_await res->write("body");
res->trailer("x-checksum", "abc123");
co_await res->finish_trailers();

// SSE
co_await res->sse_begin();
co_await res->sse_event(R"({"ok":true})", "message", "42");
co_await res->sse_comment("keep-alive");
```

`send()` fills in `Date`, `Server` and `Content-Type` (no Content-Type for 204/304).
Hop-by-hop fields set by the handler (`connection` / `transfer-encoding`, etc.) are
stripped by the engine; framing is decided only by the engine.

### Cancellation and timeouts

Request cancellation is **cooperative**:

- `req->cancel()` / `req->cancelled()` / `req->cancel_token()` are thread-safe.
- `req->wait_cancelled()` resumes on the waiter's own executor.
- HTTP/2 and HTTP/3 RST_STREAM and connection close trigger cancellation; HTTP/1.1 can
  only observe a disconnect on the next read or write.
- A handler that never observes cancellation is not forcibly stopped.

`middleware::timeout(d)` builds on the same signal and is streaming-friendly:

- Before the response head: an absolute deadline; the handler must start the response
  within `d`, otherwise 504.
- After the response head: an idle deadline reset by every write; SSE, downloads and
  streaming reverse proxies that keep writing are never killed.
- WebSocket handshakes bypass it.

### Static files, file responses and uploads

```cpp
StaticFilesConfig cfg;
cfg.table.root = "./web/dist";
cfg.table.immutable_prefixes = {"/assets/"}; // content-hashed build output
cfg.spa_fallback = "index.html";             // client-side router; "" disables it

auto site = std::make_shared<StaticFiles>(std::move(cfg));
std::string err;
if (!site->load(err)) {
    std::println(stderr, "static root error: {}", err);
    return;
}
server.static_files(std::move(site));
```

Static serving supports `ETag`, `Last-Modified`, `If-None-Match` / `If-Modified-Since`
→ 304, `Range` → 206/416, precompressed `.br` / `.gz` siblings,
`Vary: Accept-Encoding`, directory indexes, SPA fallback and path-traversal protection.
SPA fallback applies only when the last path segment has no `.`, so `/user/123` serves
`index.html` while a missing `/app.js` remains a 404.

Sending a file (64 KiB chunks, never loading the whole file into memory):

```cpp
(void)co_await send_file(*res, "./data/report.pdf");
```

Uploading to disk:

```cpp
auto n = co_await read_body_to_file(req->body(), "./upload.bin", 64ull * 1024 * 1024);
if (!n) {
    co_await write_error(res, status::bad_request,
                         {.code = "upload_failed", .message = n.error().message()});
    co_return;
}
co_await res->status(status::created).send(std::to_string(*n));
```

### Forms, JSON, parameters and multipart

```cpp
// URL-encoded body
auto form = co_await read_urlencoded_body(*req); // expected<QueryParams, error_code>

// multipart/form-data (whole body)
auto parts = co_await read_multipart_body(*req); // expected<vector<MultipartPart>, error_code>

// streaming multipart: write file parts straight to disk
MultipartStreamCallbacks cb;
cb.on_part_begin = [&](const MultipartPart &p) { /* open the file */ return true; };
cb.on_part_data  = [&](std::string_view d) { /* write */ return true; };
cb.on_part_end   = [&] { /* close the file */ };
auto boundary = multipart_boundary(req->header("content-type").value_or(""));
if (boundary)
    co_await read_multipart_stream(req->body(), *boundary, cb);

// JSON
struct CreateUser { std::string name; int age{}; };
struct User { std::string name; };
auto in = co_await read_json_body<CreateUser>(*req); // expected<CreateUser, error_code>
if (!in) {
    co_await write_error(res, status::bad_request,
                         {.code = "invalid_body", .message = "bad JSON"});
    co_return;
}
co_await write_json(res, User{.name = in->name}, status::created);

// typed parameters (field name == route template name / query key)
struct UserParams { std::int64_t id{}; };
auto p = path_params<UserParams>(*req);   // optional<UserParams>

struct ListQuery { std::optional<std::string> q; int page{1}; };
auto q = query_params<ListQuery>(*req);   // optional<ListQuery>
```

`parse_params<T>(q)` parses any `QueryParams q` (including a parsed urlencoded form body)
into a struct by field name.

Unified error responses:

```cpp
co_await write_error(res, status::not_found,
                     {.code = "not_found", .message = "no such pet", .details = "id=42"});

// RFC 9457 application/problem+json
co_await write_problem(res, status::bad_request, ProblemDetails{
    .title = "Invalid request",
    .detail = "age must be positive",
    .instance = std::string{req->path()},
});
```

### CORS, security headers, rate limiting and compression

```cpp
// CORS: preflight is answered 204 and never reaches a route
server.cors(CorsConfig{
    .allow_origins = {"https://app.example.com", "https://*.example.com"},
    .allow_credentials = true,
    .allow_methods = {"GET", "POST", "DELETE"},
    .allow_headers = {"content-type", "authorization"},
    .expose_headers = {"x-request-id"},
    .max_age = std::chrono::seconds{600},
});

// security response headers
server.use(middleware::secure_headers(middleware::SecureHeadersConfig{
    .content_security_policy = "default-src 'self'",
    .hsts_max_age = std::chrono::seconds{31536000},
}));

// shared token bucket: 100 req/s, burst 200
server.use(middleware::rate_limit(middleware::make_token_bucket(100, 200)));

// per-key buckets: 10 req/s, burst 20, at most 10000 keys
auto limiter = std::make_shared<middleware::RateLimiter>(10, 20, 10000);
server.use(middleware::rate_limit(limiter, [](const RequestPtr &req) {
    return std::string{req->peer_address()};
}));

// response compression: mounting enables it; group/route scopes work the same way
server.use(middleware::compress(CompressionConfig{
    .min_bytes = 1024,
    .gzip_level = 6,
    .brotli_quality = 5,
    .compress_streamed = true,
}));

// request decompression: also opt-in, and also scopeable to a group or one route
server.route({Method::Post}, "/upload",
    {middleware::decompress_request(RequestDecompressionConfig{.max_decoded_bytes = 1 << 20})},
    upload_handler);

// a globally mounted policy can leave reverse-proxy routes on the raw bytes
server.use(middleware::decompress_request(RequestDecompressionConfig{
    .excluded_paths = {"/proxy/"},
}));
```

`decompress_request` decodes `gzip`/`br` request bodies before the handler runs and
removes `Content-Encoding`/`Content-Length` (the handler sees the identity representation).
Unsupported codings (`deflate`, multi-coding lists) are 415, malformed/truncated streams
are 400, and decoded bytes beyond `max_decoded_bytes` are 413. Do not mount it on a
reverse-proxy route unless you intend to terminate the encoding: without it, the upstream
receives the compressed bytes and headers unchanged, like Go's `ReverseProxy`.

`allow_origins` accepts exact origins and subdomain wildcards such as
`https://*.example.com`. With `allow_credentials=true` the server mirrors the actual
origin rather than returning `*`.

### Reverse proxy

```cpp
// request-level HTTP proxy: one upstream connection per request, response streamed back
server.http_proxy("/api", "backend.internal", 8080);

server.http_proxy("/v2", HttpProxyTarget{
    .host = "10.0.0.5",
    .port = 8443,
    .tls = true,                 // TLS upstream; ALPN can select h2
    .h2c = false,                // plaintext upstream may use Upgrade
    .h2c_prior_knowledge = false,
    .preserve_host = false,
    .max_attempts = 3,
});

// regex route with capture-group rewrite: /v1/<rest> -> upstream /$1
server.http_proxy_regex("^/v1/(.*)$", "10.0.0.5", 9000, "/$1");

// byte-level WebSocket tunnel (validates 101, then splices verbatim)
server.ws_proxy("/ws", "backend.internal", 9000);
```

Proxy behavior:

- Filters hop-by-hop fields and authors `X-Forwarded-For` / `-Proto` / `-Host`.
- Idempotent requests may be retried (`max_attempts`); request cancellation aborts the
  upstream exchange.
- `CONNECT` requests are answered 405.
- The upstream goes through the client layer; a TLS upstream can use HTTP/2 via ALPN,
  and a plaintext upstream can use h2c Upgrade or prior knowledge.
- Ambient proxy environment variables are ignored by default; set
  `HttpProxyTarget::use_env_proxy = true`, or configure a `ClientConfig` on the route.
- WS proxying supports HTTP/1.1 plaintext upstream only; HTTP/1.1 Upgrade and HTTP/2
  extended CONNECT share the same upstream handshake.

### Logging

The library never writes to a stream directly; it hands records to a `LogSink`:

```cpp
#include <simple_http.h>

set_log_sink(make_stdout_sink(LogLevel::Info)); // text
// set_log_sink(make_json_sink(LogLevel::Info)); // JSON lines (stderr)

SIMPLE_HTTP_INFO_LOG("backend {} connected", peer);
```

`LogSink` has only `enabled(level)` and `log(record)`; a record carries a human-readable
`message` and structured `attrs`. Installation is atomic and safe at runtime;
`SIMPLE_HTTP_ENABLE_LOG=0` compiles logging out entirely.

### EngineLimits

```cpp
EngineLimits limits;
limits.idle_timeout = std::chrono::seconds{120};
limits.max_header_bytes = 64 * 1024;   // exceeding it: 431
limits.max_body_bytes = 64ull * 1024 * 1024; // exceeding it: 413
limits.ws_read_ahead_bytes = 4 * 1024 * 1024;
limits.ws_compression = false;
limits.h2_max_concurrent_streams = 200;
limits.h2_max_frame_size = 16384;
limits.h2_initial_window = 65535;
limits.h2_enable_connect_protocol = true; // RFC 8441
limits.h2_max_header_list_size = 0;       // 0 = follow max_header_bytes
limits.h2_max_queued_streams = 0;         // 0 = follow the concurrency cap
limits.h3_max_concurrent_streams = 0;     // 0 = follow the h2 value
```

HTTP/2 Rapid Reset protection: the number of concurrently running handlers is capped at
`h2_max_concurrent_streams`; excess streams wait in the `h2_max_queued_streams` queue,
and overflowing the queue sends `GOAWAY(ENHANCE_YOUR_CALM)`.

<a id="client-guide"></a>
## Client guide

### Creating a client

```cpp
asio::io_context io;
auto guard = asio::make_work_guard(io);
std::thread t{[&] { io.run(); }};

Client http{io.get_executor(), ClientConfig{}};
```

`Client` is pinned to a single-threaded executor (model A): after construction all of its
operations run on that one thread, and the engine contains no locks. The `io_context`
must outlive the `Client` and its sessions.

The configuration is copied into the client at construction; modifying `ClientConfig`
afterwards does not affect an existing instance. Invalid TLS configuration (for example a
client certificate without its private key) makes construction throw
`std::runtime_error`.

### Convenience API

```cpp
auto r = co_await http.post("https://api.example.com/items")
                    .query({{"dry_run", "1"}})
                    .json(CreateItem{.name = "book"})
                    .bearer_auth(token)
                    .timeout(std::chrono::seconds{10})
                    .send();

if (!r) {
    SIMPLE_HTTP_WARN_LOG("request failed: {}", r.error().message());
    co_return;
}
if (auto ok = r->error_for_status(); !ok) {
    co_return;
}
auto item = co_await r->json<Item>();
```

Methods: `get` / `post` / `put` / `del` / `head` / `patch` / `request`.
`RequestBuilder` supports `query`, `header(s)`, `content_type`, `body`, `json<T>`,
`multipart(MultipartForm)`, `basic_auth`, `bearer_auth`, `max_body_bytes`, `timeout`
and `version`. `timeout(head, idle)` overrides the per-request response-head and
body-idle budgets only; `ClientConfig::request_timeout` remains the overall bound.
`send()` returns a buffered response; `send_stream()` returns an explicit `Stream`.

You can also build a shared `Request` and call `client.send(req)` (the Go
`http.Client.Do` shape).

### Streaming multipart and file uploads

`MultipartForm` is the buffered convenience path. For large files, open a stream and
write parts incrementally:

```cpp
auto up = co_await MultipartStreamWriter::open(
    client, "https://host/upload", {.boundary = "B"});
co_await up->field("title", "hello");
co_await up->file("upload", "notes.txt", "./notes.txt"); // streamed from disk
co_await up->finish();

auto head = co_await up->stream().read_head();
auto response_body = co_await up->stream().read_all();
```

A raw file body (no multipart framing) uses `stream_file_body`:

```cpp
auto up = co_await client.open_stream("https://host/upload",
                                      {.method = Method::Put});
co_await stream_file_body(*up, "./blob.bin");
co_await up->finish();
```

Both helpers read the file in 64 KiB chunks and send each chunk through the normal
streaming-body path (chunked on HTTP/1.1, DATA frames on HTTP/2/3).

### Expect: 100-continue and request trailers

Set `auto_expect_continue` and the first `write()`/`finish()` waits for the interim
100 (HTTP/1.1 and HTTP/2; HTTP/3 sends the body immediately because the server has no
1xx API):

```cpp
auto up = co_await client.open_stream("https://host/upload",
    {.method = Method::Post, .auto_expect_continue = true,
     .continue_timeout = std::chrono::seconds{1}});
if (co_await up->write(body) == client_errc::expectation_failed) {
    // The peer refused the body; the final response is readable.
    auto head = co_await up->read_head();
}
```

Request trailers use `finish_trailers` (HTTP/1.1 and HTTP/2; HTTP/3 returns
`trailers_not_supported`). Set the `Trailer` header before opening the stream:

```cpp
StreamSpec spec;
spec.method = Method::Post;
spec.headers.add("trailer", "x-checksum");
auto up = co_await client.open_stream(url, std::move(spec));
co_await up->write(body);
Headers trailers;
trailers.add("x-checksum", "abc123");
co_await up->finish_trailers(std::move(trailers));
```

The server exposes request trailers through `req->trailers()` after the body ends
(HTTP/1.1 and HTTP/2).

### Explicit Stream

```cpp
auto up = co_await http.open_stream("https://host/upload",
                                    {.method = Method::Post,
                                     .response_head_timeout = std::chrono::seconds{5},
                                     .body_idle_timeout = std::chrono::seconds{30}});

co_await up->write(chunk);       // HTTP/1.1: chunked; HTTP/2: DATA
co_await up->finish();           // end the request body

auto head = co_await up->read_head(); // the TTFB budget applies here
while (auto c = co_await up->read()) { // each read is bounded by the body-idle budget
    if (c->eof) break;
    consume(c->data);
}
const Headers &trailers = up->trailers();
```

HTTP/1.1 is half-duplex: the response can only be read after `finish()`, and reading
before the request body ends returns `client_errc::half_duplex` immediately instead of
hanging. HTTP/2 has independent send and receive queues, so they can interleave.

`open_stream()` / `send_stream()` are the lowest-level explicit streams: they do **not**
follow redirects, apply the CookieJar, or run the convenience retry policy. Use `send()`
when you need those behaviors.

`Stream::read()` is bounded by `body_idle_timeout` per call; `Stream::read_all()` is not
wrapped by that deadline and is limited only by the caller's `max_bytes`.

### Configuration

```cpp
ClientConfig cfg;
cfg.connect_timeout = std::chrono::seconds{10};        // TCP / proxy tunnel / TLS handshake, each stage
cfg.request_timeout = std::chrono::seconds{60};        // total budget for convenience send()
cfg.response_head_timeout = std::chrono::seconds{30};  // TTFB
cfg.body_idle_timeout = std::chrono::seconds{0};       // 0 = a body may idle forever
cfg.max_redirects = 5;                                 // 0 = do not follow redirects
cfg.cookie_jar = std::make_shared<CookieJar>();
cfg.auto_decompress = true;                            // Go-like: send Accept-Encoding: gzip and decode
cfg.accept_encodings = {"gzip"};                       // add "br" to opt into brotli
cfg.default_version = HttpVersionPolicy::Auto;         // Auto / Http11 / Http2 / Http3
cfg.default_h2c = H2cMode::Upgrade;                    // Upgrade / PriorKnowledge / Off
cfg.retry.max_retries = 2;
cfg.retry.initial_backoff = std::chrono::milliseconds{100};
cfg.proxy = ProxyConfig{.kind = ProxyKind::Socks5, .host = "127.0.0.1", .port = 1080};
```

Client TLS configuration:

```cpp
cfg.tls = TlsClientConfig{
    .verify_peer = true,
    .verify_host = true,
    .ca_file = "ca.pem",
    .cert_chain_file = "client_cert.pem", // set together with the private key for mTLS
    .private_key_file = "client_key.pem",
    .min_version = TlsMinVersion::Tls12,  // client default 1.2; server default 1.3
};

// One configured SSL_CTX can be shared by many Clients. It is
// reference-counted and thread-safe after configuration; the per-Client
// TlsClientConfig fields (including the setup hook) are ignored when
// shared_context is set.
auto shared = make_client_tls_context(cfg.tls);
ClientConfig a, b;
a.tls.shared_context = shared;
b.tls.shared_context = shared;
```

### Redirects, cookies and auth

- 301/302/303 turn a POST/PUT/DELETE into a bodyless GET; 307/308 preserve method and body.
- An `https -> http` downgrade is refused (`client_errc::redirect_to_insecure`).
- `max_redirects` defaults to 0 (do not follow).
- `CookieJar` filters by Domain / Path / Secure / Expires and is shared across redirect hops.
- `basic_auth` / `bearer_auth` are `RequestBuilder` conveniences.

### Retries

- **Free transparent redials**: when a reused keep-alive connection is provably stale,
  the client redials and replays. The open phase and the response-read phase each allow
  one free replay, not counted against `max_retries`.
- **Policy retries**: `ClientConfig::retry` configures the maximum number of attempts,
  exponential backoff and a condition callback. By default it retries `stream_refused`,
  transport failures for idempotent methods, and dial failures before anything reached
  the wire.
- **Status retries**: `retry.retry_statuses` is empty by default, matching Go/reqwest;
  set `{429, 503}` to opt in. It applies to replayable, idempotent requests with
  `max_retries > 0`. A `Retry-After` header
  (seconds or IMF-fixdate) replaces the backoff for that attempt; a response body is
  consumed before retrying. `retry_status_on_non_idempotent` opts POST-like methods in.
- Retries always dial a fresh connection. The open and response-read phases of one
  exchange share a retry budget, bounded by `request_timeout` (each redirect hop gets its
  own full `request_timeout`).
- A streamed body that has already been consumed cannot be replayed and is not retried by
  default (`require_replayable`).

### Outbound proxies

```cpp
cfg.proxy = ProxyConfig{
    .kind = ProxyKind::Https,       // Http / Https / Socks5
    .host = "proxy.internal",
    .port = 3128,
    .username = "user",
    .password = "pass",
};
cfg.proxy_from_env = true;          // when unset, read HTTPS_PROXY / HTTP_PROXY / ALL_PROXY
cfg.no_proxy = {"localhost", ".internal", "127.0.0.1:8080"};
```

- HTTP proxies use CONNECT; an HTTPS proxy is TLS over TLS with a separate proxy
  `ssl::context` (the origin's client certificate is never presented to the proxy).
- SOCKS5 supports RFC 1929 username/password auth; target names are resolved by the proxy.
- `NO_PROXY` supports `*`, exact hosts, domain suffixes, `host:port` and IP literals; CIDR
  and glob forms are not supported.
- HTTP/3 always dials directly (QUIC needs CONNECT-UDP/MASQUE).

### HTTP/3

```cpp
ClientConfig cfg;
cfg.default_version = HttpVersionPolicy::Http3; // the URL must be https://
Client h3{io.get_executor(), cfg};
```

- Explicit opt-in only: no Alt-Svc / HTTPS RR discovery, no 0-RTT, no h3 WebSocket.
- One QUIC connection is kept per origin and multiplexes request streams.
- Without `SIMPLE_HTTP_ENABLE_HTTP3`, this policy fails at dial time with
  `client_errc::version_not_negotiated`.
- `quic_max_udp_payload_size` defaults to 1200 (QUIC's minimum MTU); clean Ethernet paths
  may raise it toward 1472, while VPNs/tunnels/WSL2 should stay at 1200.

### WebSocket and SSE clients

```cpp
auto ws = co_await http.open_websocket("wss://host/chat",
                                       {.origin = "https://app.example"});
if (ws) {
    for (;;) {
        auto msg = co_await (*ws)->read();
        if (!msg) break;
        co_await (*ws)->write(msg->data, msg->text);
    }
}
```

The client WebSocket uses HTTP/1.1 Upgrade (validating 101 and `Sec-WebSocket-Accept`)
and reuses the server's `WsBackendImpl`; the read pump, write pump and bounded inbound
queue are identical to the server side.

```cpp
auto sse = co_await SseStream::open(http, "https://host/events");
if (!sse)
    co_return;
while (auto ev = co_await sse->next()) {
    handle(ev->event, ev->data, ev->id);
}
// On reconnect, send sse->last_event_id() as Last-Event-ID.
```

`SseStream::open` requires status `200` and, by default, a `Content-Type` containing
`text/event-stream` (`SseOptions::require_event_stream`). Reconnection policy is left to
the caller.

<a id="architecture"></a>
## Architecture and concurrency

### Request path

```
socket bytes
  └─ Transport (TCP / TLS / QUIC stream)
      └─ version engine (h1 / h2 / h3)
          ├─ parse head / body framing
          ├─ build Request, feed the body stream
          ├─ Dispatcher (Router)
          │    ├─ global middleware → group middleware → per-route middleware
          │    ├─ route matching / static files / reverse proxy
          │    └─ Handler (coroutine)
          └─ ResponseWriter → versioned ResponseSink → Transport
```

- `ResponseWriter` is the version-agnostic facade; each engine supplies its own
  `ResponseSink` implementation.
- Request bodies use pull mode on HTTP/1.x (read from the socket when the handler asks)
  and push mode on HTTP/2 (the read loop places DATA into a per-stream queue that the
  handler consumes). Both expose the same `Body` interface.
- On HTTP/3, framing, QPACK, SETTINGS and loss recovery belong to ngtcp2/nghttp3; the
  library keeps only the adapter to `Dispatcher`.

### Concurrency model A

- Each connection is bound to one single-threaded `io_context` in `IoCtxPool` at accept time.
- All engine/writer/handler work for that connection runs on that thread, so no locking
  is needed.
- `ResponseWriter` / `WebSocket` may be captured in a `shared_ptr` and used across
  threads; every operation first hops back to the connection thread with
  `co_await asio::dispatch(asio::bind_executor(executor, ...))`.
- `engine/` contains no mutexes or atomics.
- Deliberate exceptions live in the handler layer: `RateLimiter`'s per-key buckets,
  Router's lazily built middleware chain, and the log sink's atomic installation.
- The HTTP/3 connection executor is the endpoint's executor; `reuse_port` is on by
  default, otherwise all QUIC connections for a listener share one thread.

### Backpressure

- HTTP/2 request bodies park per stream: a slow consumer pauses only its own stream, not
  the connection.
- WebSocket has a bounded inbound read pump: while the application is blocked writing
  because the peer's window is closed, the socket is still read into a bounded queue.
  When the queue is full, reading stops; `read()` makes room and resumes it.
- Streaming responses use a write pump, so the writes themselves provide backpressure.
- `max_body_bytes`, `max_header_bytes` and `h2_max_header_list_size` all impose caps.

### Graceful shutdown

- `Server::stop()` closes the listeners, waits for requests already inside a handler (up
  to `shutdown_grace`), then closes remaining connections and stops the I/O pool.
- Idle keep-alive connections and long-lived WebSockets are bounded by the same grace.
- Do not call `stop()` from a signal handler; set a `sig_atomic_t` flag and let the main
  loop call it.

### Logging and observability

- `LogSink` is the only logging interface; `make_stdout_sink` / `make_stderr_sink` /
  `make_json_sink` are the built-in implementations.
- `access_log()` emits method, path, status, duration, peer, request-id, etc.
- `Server::active_connections()` / `active_requests()` / `port()` / `quic_port()` expose
  runtime state.

<a id="testing"></a>
## Testing and verification

The suite is organized around one idea: **behavior is compared with Go's standard
library, not asserted in isolation**. Two differential harnesses run the same
requests through this library and through Go, and a registered difference is the
only way a divergence can pass.

```sh
test/run.sh fast        # unit
test/run.sh standard    # unit + OpenAPI
test/run.sh full        # + client-diff + server-diff + conformance + stress
test/run.sh list
test/run.sh doctor
test/run.sh <suite>     # unit / openapi / client-diff / server-diff / ...
```

| Suite | Tier | What it compares |
|---|---|---|
| `unit` | fast | Catch2 pure-logic tests (kept from the previous suite) |
| `openapi` | standard | OAS 3.1 metaschema, document/live-route contract, Swagger UI |
| `client-diff` | full | C++ client ↔ Go `net/http` client to the same Go server |
| `server-diff` | full | Go client → C++ server ↔ the same request → Go server |
| `conformance` | full | h2spec / h1spec / Autobahn / h3spec |
| `stress` | full | h2load / k6; the only gate is zero failures |

### Client differential

`test/diff/client/` builds one C++ probe and one Go probe per feature group
(`basic`, `body`, `policy`, `ws`, `h3`). Both load the same JSON cases and send
them to one Go reference server:

- `7810` plaintext HTTP/1.1 and WebSocket
- `7811` TLS with ALPN h2 + http/1.1
- `7812` h2c
- `7813` HTTP/3 over QUIC

The server echoes the method, headers, body and trailers it received, so a
difference in the two observations can only come from the clients. Groups cover
redirects, cookies, compression, retries, streaming multipart, streaming uploads,
100-continue, request/response trailers, SSE, WebSocket and HTTP/3.

### Server differential

`test/diff/server/` runs `diff_server_cpp` (`7820`–`7823`) and an equivalent Go
`net/http` server (`7830`–`7833`). A Go probe sends the same request to both and
prints two observations per case. Groups: `routing`, `body`, `semantics`,
`middleware`, `framework`, `static`, `ws`.

### Observations and known differences

Every probe emits one JSON object per case
(`case/target/status/version/headers/body/body_encoding/framing/error/notes`).
`test/diff/support/harness.py` lowercases header names, compares set-valued
headers as sets, ignores transport details such as `Date`, `Server`,
`Content-Length`, `Transfer-Encoding` and `Set-Cookie`, and supports `exact`,
`json` and `none` body modes plus per-case `scrub` patterns.

An unexpected difference fails the suite. A genuinely acceptable difference is
registered in `test/diff/known_diffs.json` with a reason (and RFC where relevant).
Each run writes `diff-report.md`, listing every difference with the C++ value, the
Go value, the verdict and the reason.

### OpenAPI

`test/diff/openapi/` validates the petshop document against the vendored official
OAS 3.1 metaschema, checks structural invariants (unique operationIds, path
parameters matching the template, resolvable `$ref`s), exercises every declared
response status with a real request, validates response bodies against their
declared JSON Schemas, and checks Swagger UI and document determinism.

### External conformance and stress

`test/external/conformance/run.sh` drives the minimal
`fixture_conformance_server` with h2spec (`:7790`), h1spec (`:7791`), Autobahn
(`:7788`) and h3spec (`udp :7792`). `test/external/stress/run.sh` drives the same
fixture with h2load and k6. Ports and baselines are unchanged; the fixture only
contains the four routes those tools need (`/`, `/world`, `ws /echo`,
`ws /echo1k`).

### Environment

```sh
python3 -m venv test/.venv
test/.venv/bin/pip install -r test/requirements.txt
cd test/diff && go mod download
```

Python packages are pinned in `test/requirements.txt`; Go modules are pinned in
`test/diff/go.mod`. `test/run.sh doctor` lists missing tools per suite.

<a id="known-limitations"></a>
## Known limitations

- Server-side HTTP/2 WebSocket (RFC 8441) is implemented; the **client**
  `open_websocket` still uses HTTP/1.1 Upgrade and does not support h2/h3 WebSocket.
- Server-side HTTP/3 has no WebSocket (RFC 9220 extended CONNECT is not implemented).
- nghttp3 v1.18 has no 1xx submission API, so **server-side HTTP/3**
  `Expect: 100-continue` is currently a no-op.
- The HTTP/3 client is explicit-only: no Alt-Svc / HTTPS RR discovery and no 0-RTT.
- Outbound proxying does not cover HTTP/3 (QUIC needs CONNECT-UDP/MASQUE).
- `NO_PROXY` does not support CIDR or glob patterns.
- The client uses a one-connection-per-origin model: it keeps one connection to its first
  origin and uses throwaway connections for others; there is no traditional connection pool.
- The client WebSocket does not negotiate permessage-deflate (the server implements RFC 7692).
- The client exposes the interim `100-continue` on HTTP/1.1 and HTTP/2; other 1xx
  responses (such as 103 Early Hints) are still dropped.
- CORS middleware does not apply to WebSocket upgrades; use `ws_origin()`.
- Host routing applies to HTTP dispatch only; the current WebSocket engine lookup is
  host-agnostic. A host child Router has its own OpenAPI document.
- JWT is a verifier only: no token issuance, OAuth2/OIDC discovery or refresh-token
  flow. JWKS is supported from a static/fetched JSON document; automatic background
  refresh is not built in. `aud` and `scope` are parsed as string claims; audience
  arrays are not supported.
- Request trailers are implemented for HTTP/1.1 and HTTP/2; HTTP/3 returns
  `client_errc::trailers_not_supported`.
- Automatic `Expect: 100-continue` waits for the interim response on HTTP/1.1 and
  HTTP/2; HTTP/3 sends the body immediately (nghttp3 has no 1xx submission API).
- Sessions ship with an in-process store; Redis/database adapters implement the
  `SessionStore` interface on the consumer side. CSRF is header-based (double submit);
  forms that cannot set a header need a handler-side field check.
- Validation is opt-in per type through `Validator<T>`; there is no tag/DSL validator.
- HTTP/2 one-shot responses deliberately omit `Content-Length` (END_STREAM delimits the
  body; HEAD is the exception).
- The CMake INTERFACE target does not wire ngtcp2/nghttp3, so HTTP/3 is currently
  available only through the xmake build.
- The server defaults to TLS 1.3 only; set `TlsConfig::min_version` for TLS 1.2.
- HTTP/3 builds depend on the ngtcp2 (with OpenSSL crypto helper) and nghttp3 packages
  from the private xmake repository.

<a id="contributing"></a>
## Style and contributing

- `.clang-format`: LLVM base + `IndentWidth: 4`, `ColumnLimit: 120`.
- Run `./format.sh` before committing; CI can use `./format.sh --check`.
- Layering rule: upper layers (`proto` / `handler` / `net` / the client facade) must not
  contain protocol-version branches; version differences belong in `engine/` and the
  corresponding `ResponseSink` implementations.
- RAII, no raw `new` / `delete`, explicit ownership.
- Run `test/external/conformance/run.sh` after protocol/transport changes, and
  additionally `test/external/stress/run.sh` after stream-lifecycle changes.
- When adding a public API or changing behavior, update `README.md`,
  `README.zh-CN.md` and `AGENTS.md`.

<a id="license"></a>
## License

MIT License, see [LICENSE](LICENSE).

完整中文文档见 [`README.zh-CN.md`](README.zh-CN.md)。
