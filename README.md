# simple_http

**A header-only HTTP server and client for C++17, built on Boost.Asio.**

`simple_http` implements HTTP/1.0, HTTP/1.1, HTTP/2 (TLS, h2c Upgrade and h2c
prior knowledge), optional HTTP/3 over QUIC, and WebSocket — both server and
client — behind one version-agnostic request/response model.

Every asynchronous operation is a native Asio initiating function with an
explicit completion token. Callbacks, `asio::deferred`, `asio::detached` and
`asio::as_tuple` work in C++17. When the translation unit is compiled as C++20
or newer, `asio::use_awaitable` and coroutine handlers are available as well.
The library is header-only: `include/` contains no `.cpp` files.

- **Version:** 0.9.0
- **License:** MIT
- **Minimum standard:** C++17
- **Umbrella header:** `#include <simple_http.h>`

---

## Contents

- [Why simple_http](#why-simple_http)
- [Protocol support](#protocol-support)
- [Requirements](#requirements)
- [Building and installing](#building-and-installing)
- [Quick start: server](#quick-start-server)
- [Quick start: client](#quick-start-client)
- [Routing and middleware](#routing-and-middleware)
- [Request, response and body](#request-response-and-body)
- [WebSocket](#websocket)
- [JSON, forms, typed extraction, validation and auth](#json-forms-typed-extraction-validation-and-auth)
- [Client reference](#client-reference)
- [Reverse proxy, static files and OpenAPI](#reverse-proxy-static-files-and-openapi)
- [Limits](#limits)
- [Testing](#testing)
- [Known limitations](#known-limitations)

---

## Why simple_http

- **One model on both sides of the wire.** Server requests and client requests
  are the same `Request` type; responses use the same `ResponseWriter` facade.
  Handlers never branch on the HTTP version.
- **No coroutines required.** The public API is token-based. A C++17 program
  can use callbacks everywhere; a C++20/23 program can opt into
  `asio::use_awaitable` or coroutine handlers without changing the library.
- **Bounded and backpressured.** Header/body limits, per-stream HTTP/2 flow
  control, a bounded WebSocket read-ahead queue, and output high/low
  watermarks are part of the design rather than an afterthought.
- **Single-threaded connection affinity.** Each accepted connection is pinned
  to one single-threaded `io_context`. Engines, writers and handlers for that
  connection run on one thread, so the protocol paths need no mutexes.
- **Async entry points may be started from any thread.** On a server-bound
  `Body` / `ResponseWriter` / `WebSocket`, the operation first dispatches to the
  connection's owner executor and then checks the connection/stream `Lifecycle`.
  Synchronous calls (`feed`/`finish`/`fail`, fluent setters) still require the
  owner thread. After teardown, an operation that has not started completes with
  `not_connected`; one already in flight completes through cancellation.
- **No exceptions for control flow.** Fallible operations return
  `error_code` or `boost::system::result`. Client failures are never thrown.
- **Pluggable edges.** DNS resolution, logging, TLS context construction,
  session storage and route lookup are all injectable.

## Protocol support

### Server

| Capability | Status |
|---|---|
| HTTP/1.0, HTTP/1.1 | Yes |
| HTTP/2 over TLS via ALPN | Yes |
| h2c Upgrade (HTTP/1.1 `Upgrade: h2c`) | Yes |
| h2c prior knowledge | Yes |
| HTTP/3 over QUIC | Optional (`SIMPLE_HTTP_ENABLE_HTTP3`) |
| WebSocket over HTTP/1.1 | Yes |
| WebSocket over HTTP/2 (RFC 8441 extended CONNECT) | Yes |
| WebSocket over HTTP/3 | No |
| Server push (HTTP/2 or HTTP/3) | No |
| SSE response helpers | Yes |
| IPv4 / IPv6 / dual stack | Yes |
| UNIX-domain listener | Not yet (configuration type is reserved) |

### Client

| Capability | Status |
|---|---|
| HTTP/1.1 (plaintext, TLS) | Yes |
| HTTP/2 over TLS via ALPN | Yes |
| h2c prior knowledge | Yes (`HttpVersionPolicy::Http2` on an `http://` URL) |
| h2c `Upgrade` | Not wired (`H2cMode` exists, but the client always uses prior knowledge) |
| HTTP/3 over QUIC | Optional, explicit only (`HttpVersionPolicy::Http3`) |
| WebSocket over HTTP/1.1 | Yes |
| WebSocket over HTTP/2 or HTTP/3 | No |
| Redirects | Yes, off by default (`max_redirects = 0`) |
| Cookie jar | Yes |
| Outbound proxy | HTTP CONNECT, HTTPS CONNECT, SOCKS5 |
| gzip / brotli response decompression | Yes (gzip advertised by default) |
| Streaming request upload | Yes |
| SSE | Yes |
| Request trailers | HTTP/1.1 and HTTP/2; not HTTP/3 |
| Automatic `Expect: 100-continue` | HTTP/1.1 and HTTP/2 |

## Requirements

| Dependency | Purpose |
|---|---|
| Boost (Asio, System, Regex, Core) | I/O, error model, routing regex |
| OpenSSL | TLS, certificate verification, mTLS, QUIC TLS |
| zlib, brotli | gzip/br content coding and WebSocket permessage-deflate |
| glaze | JSON reflection, typed params/extractors, JWT/JWKS, OpenAPI |
| ngtcp2 + nghttp3 | Optional HTTP/3 and QUIC |

### C++ standard and feature gates

- **C++17** is enough for core, proto, transport, HTTP/1.x, HTTP/2, WebSocket,
  routing, middleware, static files, reverse proxies and the client.
- **C++20 or newer** additionally enables coroutine handlers and
  `asio::use_awaitable` (`BOOST_ASIO_HAS_CO_AWAIT`).
- **C++23 + glaze** enables the glaze-backed helpers: JSON reflection, typed
  `path_params` / `query_params`, the `Path` / `Query` / `Json` / `Form`
  extractors, JWT/JWKS and the OpenAPI document model.

The gate is `SIMPLE_HTTP_HAS_GLAZE` (`core/glaze_support.h`). It is set to `1`
when `__cplusplus > 202002L` and `<glaze/glaze.hpp>` is available, otherwise to
`0`. Define `-DSIMPLE_HTTP_HAS_GLAZE=0` to opt out, or `=1` to force it on. The
gate is evaluated per translation unit; mixing C++17 and C++23 translation
units in one program is supported because the gated APIs are header-only
templates.

`Header<T, "name">` and `Cookie<T, "name">` extractors additionally require
C++20 class-type non-type template parameters.

## Building and installing

`simple_http` is header-only: adding `include/` to the include path is the
minimum integration.

### xmake

The repository's `xmake.lua` builds the header-only target and all test
binaries. It expects the packages `boost`, `openssl4`, `glaze`, `zlib`,
`brotli`, and optionally `ngtcp2` / `nghttp3`.

```sh
xmake f -m release
xmake build -j 12
```

Library consumers only need the include directory and the link dependencies.
Test targets are declared with `set_default(false)`, so a bare `xmake build`
does not build them; build one by name, for example
`xmake build -j 12 test_unit`.

### CMake

```cmake
find_package(simple_http CONFIG REQUIRED)
target_link_libraries(app PRIVATE simple_http::simple_http)
```

The current CMake interface target requires C++23 and links Boost, OpenSSL,
zlib, brotli and glaze. It does **not** link ngtcp2/nghttp3, so HTTP/3 is not
enabled through CMake. A strict C++17 consumer that does not use the
glaze-backed helpers can include the headers directly; see
[Known limitations](#known-limitations) for the current CMake rough edges.

## Quick start: server

### C++17, callback style

```cpp
#include <simple_http.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

using namespace simple_http;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

int main() {
    ServerConfig cfg;
    cfg.listen = InetAddress{"0.0.0.0", 8080};
    cfg.worker_threads = 4;

    Server server{cfg};

    server.route({Method::Get}, "/hello",
                 [](RequestPtr, ResponsePtr res, HandlerCompletion done) {
                     res->status(status::ok).content_type("text/plain");
                     res->async_send("hello, world\n", finish_with(std::move(done)));
                 });

    auto started = server.start();
    if (!started) {
        for (const auto &failure : started.error().failures)
            std::fprintf(stderr, "%s: %s\n", failure.name.c_str(), failure.error.message().c_str());
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds{100});

    server.stop();
}
```

`Server::start()` starts the worker threads itself. `Server::stop()` honors
`ServerConfig::shutdown_grace`, cancels remaining connections, and joins the
I/O threads.

### C++20/23, coroutine handler

```cpp
server.route({Method::Get}, "/hello",
             [](RequestPtr, ResponsePtr res) -> boost::asio::awaitable<void> {
                 res->status(status::ok).content_type("text/plain");
                 co_await res->async_send("hello, world\n", boost::asio::use_awaitable);
             });
```

Accepted handler shapes are normalized automatically:

- `void(RequestPtr, ResponsePtr)` — synchronous, completes immediately;
- `void(RequestPtr, ResponsePtr, SslHandle)`;
- `void(RequestPtr, ResponsePtr, HandlerCompletion)`;
- `void(RequestPtr, ResponsePtr, SslHandle, HandlerCompletion)`;
- `awaitable<void>(RequestPtr, ResponsePtr[, SslHandle])` in a C++20 build.

A handler that throws is turned into a 500 if the response has not started, or
the connection is closed if a response head is already on the wire.

### Listening options

```cpp
ServerConfig cfg;

TcpListenerConfig plain;
plain.address = InetAddress{"0.0.0.0", 8080};
plain.plaintext = PlaintextHttp::Http1AndHttp2;   // sniff + allow h2c Upgrade
plain.tcp_nodelay = true;
cfg.listeners.push_back(plain);

TcpListenerConfig secure;
secure.address = InetAddress{"0.0.0.0", 8443};
secure.tls = TlsConfig{
    "server_cert.pem",   // cert_chain_file
    "server_key.pem",    // private_key_file
    false,               // mutual
    std::nullopt,        // ca_file
    {},                  // setup hook
    TlsMinVersion::Tls13,
};
secure.alpn = TlsHttp::Http1AndHttp2;
cfg.listeners.push_back(secure);
```

Useful `ServerConfig` fields:

| Field | Default | Meaning |
|---|---|---|
| `worker_threads` | 4 | Single-threaded `io_context` workers |
| `max_connections` | 0 | 0 = unlimited; excess connections stay in the kernel backlog |
| `reuse_port` | true | One acceptor per worker with `SO_REUSEPORT` |
| `shutdown_grace` | 5000 ms | Time allowed for in-flight handlers before cancellation |
| `tcp_nodelay` | true | Copied into the implicit listener |
| `plaintext_protocols` | `Both` | `Http1`, `Http2` (prior knowledge), or `Both` |
| `limits` | `EngineLimits{}` | Protocol limits shared by all listeners |

`start(StartPolicy::AllOrNothing)` is the default; `BestEffort` keeps the
listeners that bound successfully and reports the failures in `StartReport`.

## Quick start: client

### C++17, callback style

```cpp
#include <simple_http.h>
#include <boost/asio.hpp>
#include <iostream>

int main() {
    boost::asio::io_context io;
    simple_http::Client client{io.get_executor()};

    client.get("http://127.0.0.1:8080/hello")
        .header("accept", "text/plain")
        .send([&](simple_http::error_code ec, simple_http::Response response) {
            if (ec)
                std::cerr << "request failed: " << ec.message() << '\n';
            else
                std::cout << response.status() << '\n' << response.text() << '\n';
            client.close();
            io.stop();
        });

    io.run();
}
```

### C++20/23, awaitable style

```cpp
boost::asio::awaitable<void> fetch() {
    auto ex = co_await boost::asio::this_coro::executor;
    simple_http::Client client{ex};

    auto [ec, response] = co_await client.get("http://127.0.0.1:8080/hello")
                              .send(boost::asio::as_tuple(boost::asio::use_awaitable));
    if (ec)
        co_return;

    std::cout << response.status() << '\n' << response.text() << '\n';
    client.close();
}
```

The client keeps one reusable connection per origin (HTTP/1.1 queues on it,
HTTP/2 multiplexes over it). Requests to a different origin dial a connection
that closes after the exchange. `Client::close()` should be called before
destroying an `io_context` that still has a keep-alive session pending.

## Routing and middleware

```cpp
server.route({Method::Get}, "/hello", handler);                 // exact
server.route({Method::Get, Method::Post}, "/items", handler);   // multiple methods
server.route({}, "/anything", handler);                         // any method
server.route_regex({Method::Get}, R"(/files/(\d+))", handler);  // regex
server.group("/api", [&](Router &r) {
    r.route({Method::Get}, "/status", handler);                 // GET /api/status
});
server.host("*.example.com", [&](Router &r) {
    r.route({Method::Get}, "/", host_handler);
});
server.fallback(not_found_handler);
server.ws_route("/chat", ws_handler);
server.static_files(site);
server.http_proxy("/api", target);
server.ws_proxy("/ws", ws_target);
```

Route matching order:

1. Host router (if the `Host` header matches a registered pattern)
2. Exact path (method-aware)
3. `{param}` template trie, including `{*rest}` as the final segment
4. Local regular expressions
5. HTTP reverse-proxy routes
6. Static-file stages
7. Fallback handler
8. Built-in 404

Implicit rules:

- A path that matched an exact/template/regex route but not the method answers
  405 with `Allow`, or 204 + `Allow` for `OPTIONS`. It does **not** fall through
  to proxy/static/fallback.
- Registering `GET` implicitly serves `HEAD`; the writer suppresses the body.
- An empty method list matches every method, including extension tokens.
- `Allow` always includes `OPTIONS`.

### Middleware

Middleware has the Go `net/http` / tower shape. The callback form works in
C++17:

```cpp
server.use([](RequestPtr req, ResponsePtr res, SslHandle ssl,
              Next next, HandlerCompletion done) {
    const auto t0 = std::chrono::steady_clock::now();
    next(std::move(req), std::move(res), ssl,
         [t0, done = std::move(done)]() mutable {
             // runs after the rest of the chain
             (void)t0;
             std::move(done)();
         });
});
```

In a C++20 build, a coroutine middleware returning
`awaitable<void>(RequestPtr, ResponsePtr, SslHandle, CoroNext)` is also
accepted. `make_middleware`, `make_middlewares` and `compose_middleware`
normalize mixed forms.

Built-in middleware (`simple_http::middleware`):

`request_id()`, `access_log()`, `recovery()`, `basic_auth(user, pass)`,
`real_ip(trusted)`, `clean_path()`, `strip_prefix(prefix)`,
`redirect_slashes()`, `ws_origin(allowed, allow_missing = false)`,
`timeout(deadline)`, `compress(cfg)`, `decompress_request(cfg)`,
`secure_headers(cfg)`, `rate_limit(bucket)`, `rate_limit(limiter, key_fn)`,
`session(store, cfg)`, `csrf(cfg)`, `jwt(cfg)`, `require_scope(scope)`,
`require_role(role)`.

CORS is installed with `server.cors(cfg)` or
`simple_http::make_cors_middleware(cfg)` and always runs as the outermost
middleware. An empty `allow_origins` combined with
`allow_credentials = true` fails closed and emits no CORS headers.

## Request, response and body

### Request

```cpp
req->method();              // Method
req->method_token();        // raw token (extension methods preserved)
req->version();             // Version::Http1 / Http11 / Http2 / Http3
req->target();              // request-target
req->path();                // path part, undecoded
req->query();               // raw query string
req->query_params();        // parsed QueryParams (lazy)
req->param("id");           // path parameter, if the engine populated it
req->header("host");        // optional<string_view>
req->headers();             // Headers
req->trailers();            // request trailers (HTTP/1.1 and HTTP/2)
req->cookie("sid");
req->peer_address();
req->app_state<T>();        // shared application state registered on the Router
req->get_state<T>() / set_state<T>(...);  // per-request state
```

Read the body with a token:

```cpp
// completion: void(error_code, std::string)
auto [ec, body] = co_await req->body().async_read_all(
    boost::asio::as_tuple(boost::asio::use_awaitable));

// completion: void(error_code, ReadResult); ReadResult{data, eof}
auto [ec2, chunk] = co_await req->body().async_read(
    boost::asio::as_tuple(boost::asio::use_awaitable));
```

HTTP/1.x bodies are pull-based: if the handler never reads, the engine never
reads the socket. HTTP/2 and HTTP/3 bodies are pushed into a bounded channel;
a full channel parks that stream only.

### Response

Synchronous setters are chainable:

```cpp
res->status(200)
   .content_type("application/json")
   .header("x-trace-id", id)
   .set_cookie("sid", value, std::chrono::seconds{3600},
               "/", {}, true, true, "Lax");
```

One-shot or streaming:

```cpp
// One response, complete body. Completion: void(error_code).
co_await res->async_send("hello\n", token);

// Streaming. Completion: void(error_code) for each operation.
co_await res->async_begin(token);
co_await res->async_write("chunk", token);
co_await res->async_finish("last chunk", token);

// Trailers replace the final finish.
res->trailer("x-checksum", value);
co_await res->async_finish_trailers(token);

// No body and no body framing: HEAD, 204, 304 only.
co_await res->async_send_bodyless(token);

// Interim response.
co_await res->async_send_continue(token);

// Connection state.
co_await res->async_connected(token);   // completion: void(error_code, bool)
co_await res->async_close(token);
```

Helpers: `redirect(location, code)`, `async_sse_begin`, `async_sse_event`,
`async_sse_comment`, `version()`, `started()`, `reset_for_error()`,
`activity()`.

Rules enforced by the writer:

- A `ResponseWriter` sends exactly one response head. A second
  `async_send` / `async_send_bodyless` / `async_begin` returns
  `invalid_argument`.
- Status must be 100–599. Invalid status, header names/values, cookies or
  trailers mark the response as rejected and make the write fail.
- `Date`, `Server` and (for content-bearing statuses) `Content-Type` are added
  automatically.
- `async_send_bodyless` is accepted only for HEAD, 204 and 304. A bodyless 200
  on a keep-alive HTTP/1.1 connection would be response smuggling.

HTTP/1.x streaming uses chunked encoding; HTTP/1.0 and HEAD use
close-delimited framing. HTTP/2 and HTTP/3 use DATA frames, with trailers
carried by a trailing field section.

## WebSocket

Server:

```cpp
server.ws_route("/chat",
                [](RequestPtr, std::shared_ptr<WebSocket> ws) -> boost::asio::awaitable<void> {
                    for (;;) {
                        auto [ec, msg] = co_await ws->async_read(
                            boost::asio::as_tuple(boost::asio::use_awaitable));
                        if (ec)
                            break;
                        auto [wec] = co_await ws->async_write(
                            msg.data, msg.text,
                            boost::asio::as_tuple(boost::asio::use_awaitable));
                        if (wec)
                            break;
                    }
                });
```

Client:

```cpp
simple_http::WebSocketSpec spec;
spec.origin = "http://127.0.0.1:8080";

auto [ec, ws] = co_await client.async_open_websocket(
    "ws://127.0.0.1:8080/chat", spec,
    boost::asio::as_tuple(boost::asio::use_awaitable));
if (!ec)
    co_await ws->async_write_text("hello", boost::asio::use_awaitable);
```

`WebSocket` operations: `async_read`, `async_write_text`,
`async_write_binary`, `async_write(data, text)`, the `*_view` variants,
`async_ping`, `async_close`, `async_enable_write_compression`,
`async_set_compression_level`, `compression_negotiated()`, `is_open()`.

Server-side notes:

- `ws_route` handlers may be callbacks or coroutines.
- HTTP/1.1 Upgrade and HTTP/2 RFC 8441 extended CONNECT are supported.
- `EngineLimits::ws_compression` enables permessage-deflate negotiation.
- An internal bounded read-ahead queue prevents the classic deadlock where an
  application does `read → write` while the peer sends without reading.
- `write_view` / `*_view` borrow the caller's buffer until the operation
  completes; the owning variants copy it.

## JSON, forms, typed extraction, validation and auth

> The APIs in this section require `SIMPLE_HTTP_HAS_GLAZE=1` (C++23 + glaze),
> except `State<T>`, `Valid<E>` and the validation helpers.

### JSON

```cpp
// completion: void(error_code, result<T>)
auto [ec, parsed] = co_await simple_http::async_read_json_body<CreateUser>(
    *req, boost::asio::as_tuple(boost::asio::use_awaitable));

// completion: void(error_code)
co_await simple_http::async_write_json(res, user, simple_http::status::created,
                                       boost::asio::use_awaitable);
```

`async_read_json_body` returns `invalid_argument` for malformed or
type-mismatched JSON; the handler decides whether that is a 400/422.

Types used with glaze need a `glz::meta<T>` specialization (or glaze's
reflection setup), exactly as in any other glaze-based program.

### Forms and multipart

```cpp
// completion: void(error_code, QueryParams)
co_await read_urlencoded_body(*req, token);

// completion: void(error_code, vector<MultipartPart>)
co_await read_multipart_body(*req, token);

// Incremental parse with callbacks. completion: void(error_code)
co_await read_multipart_stream(req->body(), boundary, callbacks, token);
```

### Typed extraction

```cpp
struct UserParams { std::string id; };

server.route({Method::Get}, "/users/{id}",
             extract<Path<UserParams>>(
                 [](const UserParams &p, RequestPtr, ResponsePtr res, HandlerCompletion done) {
                     res->async_send("user " + p.id, finish_with(std::move(done)));
                 }));
```

Extractors: `Path<T>`, `Query<T>`, `Json<T>`, `Form<T>`, `State<T>`,
`Header<T, "name">` (C++20), `Cookie<T, "name">` (C++20), and
`Valid<Extractor>`.

Extraction failure never enters the handler:

| Failure | Status |
|---|---|
| Missing/invalid path or query | 400 |
| Malformed JSON/form | 400 |
| Body exceeds the limit | 413 |
| Missing application state | 500 |
| `Valid<T>` rejects the value | 422 |

At most one body extractor (`Json` or `Form`) may appear in one `extract<...>`.

### Validation

```cpp
template <> struct simple_http::Validator<CreateUser> {
    static simple_http::ValidationResult validate(const CreateUser &in) {
        simple_http::ValidationResult out;
        simple_http::push_if(out.errors,
                             simple_http::required_field("name", !in.name.empty()));
        simple_http::push_if(out.errors,
                             simple_http::max_length("name", in.name, 64));
        return out;
    }
};
```

A failed `Valid<T>` extractor produces 422 with a JSON `details` array of
`{field, code, message}` objects. The validation helpers are
`required_field`, `min_length`, `max_length`, `range`, `one_of` and `push_if`.

### Sessions, CSRF and JWT

```cpp
auto store = std::make_shared<simple_http::MemorySessionStore>();
server.use(simple_http::middleware::session(store, simple_http::SessionConfig{}));
server.use(simple_http::middleware::csrf(simple_http::CsrfConfig{}));
simple_http::JwtConfig jwt_cfg;
jwt_cfg.algorithms = {"RS256"};
jwt_cfg.public_key_pem = pem;
server.use(simple_http::middleware::jwt(std::move(jwt_cfg)));
server.use(simple_http::middleware::require_scope("users:write"));

simple_http::Session *s = simple_http::session(*req);
const simple_http::Principal *p = simple_http::principal(*req);
```

- `SessionStore` is asynchronous: `load`, `save` and `destroy` take completion
  handlers. `MemorySessionStore` is the built-in in-process store.
- Session cookies carry an opaque id; default `secure = true`, `http_only =
  true`, `rolling = true`.
- CSRF is header-based double-submit (`x-csrf-token` by default). It checks
  the header, not a form field.
- JWT supports HS256, RS256 and ES256, with either a static PEM key or a JWKS
  document. It rejects `alg: none`, requires `exp` by default, and checks
  issuer/audience when configured. It is a verifier, not an authorization
  server: no token issuance, OIDC discovery or refresh flow.
- `fetch_jwks_json(client, url, token)` fetches a JWKS once; the application
  places the result in `JwtConfig::jwks_json`.

## Client reference

### Request builder

```cpp
client.get(url)
    .query({{"page", "2"}, {"q", "hello world"}})
    .header("accept", "application/json")
    .bearer_auth(token)
    .timeout(std::chrono::seconds{5}, std::chrono::seconds{30})
    .version(HttpVersionPolicy::Auto)
    .send(token);

client.post(url).content_type("text/plain").body("hello").send(token);
client.put(url).body(payload).send(token);
client.del(url).send(token);
client.head(url).send(token);
client.patch(url).body(patch_json).content_type("application/json").send(token);
```

`RequestBuilder` provides `query`, `header`, `headers`, `content_type`,
`basic_auth`, `bearer_auth`, `body`, `max_body_bytes`, `timeout` and
`version`. There is no `.json(...)` convenience in this implementation — use
`content_type("application/json")` and `body(serialized_json)`.

### Buffered response

`Response` exposes `status()`, `version()`, `headers()`, `header()`,
`header_owned()`, `ok()`, `bodyless()`, `uncompressed()`, `error_for_status()`,
`text()`, `bytes()` and `async_read_all(max_bytes, token)`. Buffered
`Response::text()` and `Response::bytes()` both return the same string.

### Streaming request/response

```cpp
simple_http::StreamSpec spec;
spec.method = simple_http::Method::Post;
spec.headers.add("content-type", "application/octet-stream");

// completion: void(error_code, shared_ptr<Stream>)
auto [oec, stream] = co_await client.async_open_stream(
    "http://127.0.0.1:8080/upload", std::move(spec), 0,
    boost::asio::as_tuple(boost::asio::use_awaitable));
if (oec)
    co_return;

co_await stream->async_write(chunk, boost::asio::use_awaitable);
co_await stream->async_finish("", boost::asio::use_awaitable);

auto [hec, head] = co_await stream->async_read_head(
    boost::asio::as_tuple(boost::asio::use_awaitable));
auto [bec, body] = co_await stream->async_read_all(
    0, boost::asio::as_tuple(boost::asio::use_awaitable));
```

`Stream` also exposes `async_finish_trailers`, `async_read`,
`async_ensure_continue`, `head()`, `trailers()`, `finished()` and `abort()`.
The explicit stream path does not follow redirects, apply cookies or retry.
HTTP/1.1 is half-duplex: finish the request body before reading the response.

### SSE

```cpp
auto [ec, sse] = co_await simple_http::SseStream::async_open(
    client, "http://127.0.0.1:8080/events", simple_http::SseOptions{},
    boost::asio::as_tuple(boost::asio::use_awaitable));

for (;;) {
    auto [nec, event] = co_await sse->async_next(
        boost::asio::as_tuple(boost::asio::use_awaitable));
    if (nec)   // boost::asio::error::eof on normal end
        break;
    std::cout << event.event << ": " << event.data << '\n';
}
```

### Streaming multipart upload

```cpp
simple_http::MultipartStreamOptions options;
options.spec.method = simple_http::Method::Post;

auto [oec, writer] = co_await simple_http::MultipartStreamWriter::async_open(
    client, "http://127.0.0.1:8080/upload", std::move(options),
    boost::asio::as_tuple(boost::asio::use_awaitable));

co_await writer->async_field("title", "hello", boost::asio::use_awaitable);
co_await writer->async_file("upload", "notes.txt", "./notes.txt",
                            boost::asio::use_awaitable);
co_await writer->async_finish(boost::asio::use_awaitable);
```

### Client configuration

| Field | Default |
|---|---|
| `TlsClientConfig::verify_peer` / `verify_host` | true / true |
| `TlsClientConfig::min_version` | TLS 1.2 |
| `ClientConfig::default_version` | `Auto` |
| `connect_timeout` | 10 s |
| `idle_timeout` | 120 s |
| `request_timeout` | 60 s |
| `response_head_timeout` | 30 s |
| `body_idle_timeout` | 0 (no limit) |
| `max_redirects` | 0 (do not follow) |
| `auto_decompress` | true, advertising `gzip` |
| `accept_encodings` | `{"gzip"}` |
| `retry` | no retries |
| `limits.h2_initial_window` | 1 MiB (client override) |
| `quic_max_udp_payload_size` | 1200 |
| `proxy` / `proxy_from_env` | none / true |
| `tcp_nodelay` / `tcp_keepalive` | true / false |

Automatic decompression matches Go's `http.Transport`: it is applied only when
the client itself injected `Accept-Encoding` (the caller did not set it), the
method is not HEAD, and the request has no `Range`. Setting
`accept_encodings = {"br", "gzip"}` opts into brotli.

Retries are opt-in. `RetryPolicy::retry_statuses` (for example `{429, 503}`)
enables status-code retries with `Retry-After` support. The
`condition` / `require_replayable` fields are present but are not consulted by
the current buffered path.

### Outbound proxies

```cpp
ClientConfig cfg;
ProxyConfig proxy;
proxy.kind = ProxyKind::Https;
proxy.host = "proxy.example.com";
proxy.port = 443;
proxy.username = "user";
proxy.password = "secret";
cfg.proxy = std::move(proxy);
```

Supported kinds are HTTP CONNECT, HTTPS CONNECT and SOCKS5 (with optional
username/password). `proxy_from_env = true` reads `HTTPS_PROXY`, `HTTP_PROXY`,
`ALL_PROXY` and `NO_PROXY` at dial time. `NO_PROXY` supports `*`, exact hosts,
domain suffixes and `host:port`; CIDR and glob forms are not supported. HTTP/3
is dialed directly and is not tunneled through a proxy.

### Cookies

```cpp
auto jar = std::make_shared<simple_http::CookieJar>();
ClientConfig cfg;
cfg.cookie_jar = jar;
cfg.max_redirects = 5;
```

`CookieJar` enforces Domain/Path/Secure/Expires rules, `__Host-`/`__Secure-`
prefixes, and treats loopback as a secure origin. Without a `PublicSuffixFn`,
single-label `Domain` values and IP literals are rejected.

## Reverse proxy, static files and OpenAPI

### HTTP reverse proxy

```cpp
simple_http::HttpProxyTarget target;
target.host = "127.0.0.1";
target.port = 9000;
target.preserve_host = false;

server.http_proxy("/api", target);
```

`HttpProxyTarget` also has `rewrite_path`, `tls`, `host_header`,
`preserve_inbound_forwarding`, `hooks` (`rewrite`, `modify_response`,
`error_handler`) and `use_env_proxy`. The request path is forwarded with a
configurable rewrite; hop-by-hop fields and `Connection`-nominated fields are
stripped on both sides. The current implementation buffers the whole request
and response body (response cap 64 MiB).

### Static files

```cpp
simple_http::StaticFilesConfig static_cfg;
static_cfg.table.root = "./public";

auto site = std::make_shared<simple_http::StaticFiles>(std::move(static_cfg));
std::string error;
if (!site->load(error))
    throw std::runtime_error(error);

server.static_files(site);
```

`StaticFiles` supports index files, SPA fallback, precompressed gzip/brotli
variants, `Range` (identity representation), `ETag` / `Last-Modified` /
conditional requests, and MIME detection by extension. It serves GET and HEAD;
a missing file falls through to the router's fallback/404.

### OpenAPI 3.1

```cpp
server.route({Method::Get}, "/pets/{id}", handler,
             simple_http::openapi::doc()
                 .path_params<PetParams>()
                 .response<Pet>(200)
                 .summary("get a pet")
                 .operation_id("getPet")
                 .error<ErrorBody>(404, "no such pet"));

server.openapi().title("petshop").version("1.0.0");
server.serve_openapi("/openapi.json");
server.serve_swagger_ui("/swagger", "/openapi.json");
```

Only routes registered with an `openapi::doc()` descriptor appear in the
document. Path template names must match the fields of `path_params<T>`; a
mismatch is rejected at registration. `serve_swagger_ui` serves a small HTML
page that loads Swagger UI from a CDN.

## Limits

`EngineLimits` is shared by all server engines and can be overridden per
listener.

| Field | Default |
|---|---|
| `idle_timeout` | 120 s (≤ 0 disables) |
| `max_header_bytes` | 64 KiB → 431 |
| `max_body_bytes` | 64 MiB → 413; 0 means unlimited |
| `ws_read_ahead_bytes` | 4 MiB |
| `ws_compression` | false |
| `h2_max_concurrent_streams` | 200 |
| `h2_max_frame_size` | 16384 |
| `h2_initial_window` | 65535 |
| `h2_enable_connect_protocol` | true |
| `h2_max_header_list_size` | 0 → follows `max_header_bytes` |
| `h2_max_queued_streams` | 0 → follows `h2_max_concurrent_streams` |
| `h3_max_concurrent_streams` | 0 → follows `h2_max_concurrent_streams` |
| `h3_alt_svc` | true (HTTP/3 builds only) |
| `h3_alt_svc_max_age` | 86400 |

## Testing

The only test entry point is `test/run.sh` (which drives `test/run.py`).
xmake builds the C++ test binaries; the Go reference servers and probes are
built by each diff runner.

```sh
test/run.sh fast        # unit + cpp17
test/run.sh standard    # fast + openapi
test/run.sh full        # all suites
test/run.sh list
test/run.sh doctor
test/run.sh unit '[h2]' # Catch2 filter
test/run.sh client-diff
test/run.sh server-diff
```

| Suite | Tier | What it does |
|---|---|---|
| `unit` | fast | Catch2 unit/regression tests |
| `cpp17` | fast | Compiles and runs the public API as C++17 |
| `openapi` | standard | OAS 3.1 metaschema, document/route contract, Swagger UI |
| `client-diff` | full | C++ client vs Go `net/http` client against one Go server |
| `server-diff` | full | Go probes against C++ and Go reference servers |
| `concurrency` | full | Multi-client/multi-thread/teardown harness |
| `conformance` | full | h2spec, h1spec, h3spec, Autobahn |
| `stress` | full | h2load and k6, zero failures required |

`--parallel N` defaults to 3. `conformance` and `stress` share ports
7788–7792 and always run in one serial lane. `--jobs` controls compile
parallelism (default 12). `full` treats missing external tools as an error
unless `--allow-skip` is given. Reports and per-suite logs are written under
`.cache/test-run/<timestamp>-<pid>/`.

Additional suites:

```sh
test/run.sh sanitize asan [suite...]   # ASan + LSan + UBSan
test/run.sh sanitize tsan [suite...]   # TSan
test/run.sh sanitize ubsan [suite...]  # UBSan
test/coverage.sh
test/fuzz/run.sh
test/fuzz/run.sh --libfuzzer --time 60
test/external/conformance/run.sh
test/external/stress/run.sh cpp h3 --no-conformance
```

See `test/README.md` for ports, diff groups, normalization rules and tool
prerequisites.

## Known limitations

- **Client h2c Upgrade is not wired.** `H2cMode::Upgrade` and
  `ClientConfig::default_h2c` exist, but the client currently uses prior
  knowledge for cleartext HTTP/2 (`HttpVersionPolicy::Http2`) and HTTP/1.1
  otherwise.
- **Client retry is status-code based only.** `RetryPolicy::condition` and
  `require_replayable` are not consulted by the buffered implementation.
- **Redirects are off by default.** Set `ClientConfig::max_redirects`.
- **No UNIX-domain server listener.** `UnixAddress` is reserved and rejected
  at startup; the transport types exist but no listener serves them.
- **No HTTP/3 WebSocket or extended CONNECT.**
- **HTTP/3 request trailers and `Expect: 100-continue` are not exposed.**
  Response trailers are supported.
- **No server push** in HTTP/2 or HTTP/3, and no 0-RTT in the HTTP/3 client.
- **HTTP/3 is explicit only.** The client does not read Alt-Svc or HTTPS RRs.
- **Outbound proxies do not cover HTTP/3.**
- **`NO_PROXY` does not support CIDR or glob patterns.**
- **The client keeps one connection per origin**, not a general connection
  pool. Requests to a second origin use a throwaway connection.
- **`CookieJar` has no built-in PSL data.** Inject `PublicSuffixFn` for full
  public-suffix checks; by default single-label domains and IP literals are
  rejected.
- **glaze-backed helpers need C++23.** In a real C++17 translation unit,
  `Path` / `Query` / `Json` / `Form` extraction, typed params, JWT/JWKS and
  OpenAPI are unavailable. `State<T>`, `Valid<E>` and validation helpers remain
  available.
- **`Header` / `Cookie` extractors need C++20.**
- **Synchronous file I/O** in `async_send_file`, `async_read_body_to_file` and
  streaming multipart uploads; it blocks the client/connection executor.
- **The HTTP reverse proxy buffers whole bodies** (64 MiB response cap).
- **CMake does not enable HTTP/3.** The exported target requires C++23 and
  links glaze, but the generated config lacks `find_dependency(glaze)`.
- **`simple_http_cpp17` CMake target** points at a `cpp17/include` directory
  that is not present in this repository; it is not installed or exported.
- **The unit-test runner does not emit an aggregate JSON report.** Results are
  conveyed through exit codes and per-suite logs.
- **Formatting is not enforced in CI.** `./format.sh --check` exists and is
  expected to be run manually before committing.

## License

MIT. See [LICENSE](LICENSE).
