# simple_http

**一个纯头文件、面向 C++17 的 HTTP 服务器与客户端库，基于 Boost.Asio。**

`simple_http` 在一个与 HTTP 版本无关的请求/响应模型之上，实现了 HTTP/1.0、
HTTP/1.1、HTTP/2（TLS、h2c Upgrade、h2c prior knowledge）、可选的基于 QUIC 的
HTTP/3，以及 WebSocket —— 服务器端与客户端都覆盖。

每个异步操作都是原生的 Asio initiating function，并要求调用方显式传入 completion
token。C++17 下可以使用回调、`asio::deferred`、`asio::detached` 和
`asio::as_tuple`；当翻译单元以 C++20 或更高标准编译时，还可以使用
`asio::use_awaitable` 与协程 handler。库是纯头文件的：`include/` 下没有任何
`.cpp` 文件。

- **版本：** 0.9.0
- **许可证：** MIT
- **最低标准：** C++17
- **伞形头：** `#include <simple_http.h>`

---

## 目录

- [为什么选择 simple_http](#为什么选择-simple_http)
- [协议支持](#协议支持)
- [依赖与标准要求](#依赖与标准要求)
- [构建与安装](#构建与安装)
- [快速开始：服务器](#快速开始服务器)
- [快速开始：客户端](#快速开始客户端)
- [路由与中间件](#路由与中间件)
- [Request、Response 与 Body](#requestresponse-与-body)
- [WebSocket](#websocket)
- [JSON、表单、类型化提取、校验与认证](#json表单类型化提取校验与认证)
- [客户端参考](#客户端参考)
- [反向代理、静态文件与 OpenAPI](#反向代理静态文件与-openapi)
- [限制参数](#限制参数)
- [测试](#测试)
- [已知限制](#已知限制)

---

## 为什么选择 simple_http

- **连接两端使用同一套模型。** 服务端请求与客户端请求是同一个 `Request` 类型；
  响应统一走 `ResponseWriter` 门面。handler 不需要按 HTTP 版本分支。
- **不强制使用协程。** 公共 API 是 token 形式的：C++17 程序可以全程使用回调；
  C++20/23 程序可以按需使用 `asio::use_awaitable` 或协程 handler，而不需要改变库。
- **有界且具备反压。** 请求头/请求体上限、HTTP/2 每流流控、有界 WebSocket
  read-ahead 队列，以及输出高低水位，都是设计的一部分，而不是事后补丁。
- **单线程连接亲和。** 每条被接受的连接固定到一个单线程 `io_context`。
  该连接的引擎、writer 和 handler 都在同一个线程上运行，因此协议路径内部不需要
  mutex。
- **异步入口可跨线程发起。** 绑定到服务端连接的 `Body` / `ResponseWriter` /
  `WebSocket` 异步操作会先 `dispatch` 到连接的 owner executor，再通过
  `Lifecycle` 检查连接/流是否仍可用；同步入口（`feed`/`finish`/`fail`、
  fluent setter）仍要求 owner 线程。连接关闭后，未开始的异步操作以
  `not_connected` 完成，已开始的通过取消完成。
- **不用异常做控制流。** 可失败操作返回 `error_code` 或
  `boost::system::result`。客户端失败从不抛出异常。
- **边缘可插拔。** DNS 解析、日志、TLS context 构造、会话存储与路由查找都可以注入。

## 协议支持

### 服务端

| 能力 | 状态 |
|---|---|
| HTTP/1.0、HTTP/1.1 | 支持 |
| HTTP/2 over TLS（ALPN） | 支持 |
| h2c Upgrade（HTTP/1.1 `Upgrade: h2c`） | 支持 |
| h2c prior knowledge | 支持 |
| HTTP/3 over QUIC | 可选（`SIMPLE_HTTP_ENABLE_HTTP3`） |
| WebSocket over HTTP/1.1 | 支持 |
| WebSocket over HTTP/2（RFC 8441 extended CONNECT） | 支持 |
| WebSocket over HTTP/3 | 不支持 |
| Server push（HTTP/2 或 HTTP/3） | 不支持 |
| SSE 响应 helper | 支持 |
| IPv4 / IPv6 / 双栈 | 支持 |
| UNIX domain listener | 尚未实现（仅保留配置类型） |

### 客户端

| 能力 | 状态 |
|---|---|
| HTTP/1.1（明文、TLS） | 支持 |
| HTTP/2 over TLS（ALPN） | 支持 |
| h2c prior knowledge | 支持（`http://` URL + `HttpVersionPolicy::Http2`） |
| h2c `Upgrade` | 尚未接线（`H2cMode` 存在，但客户端始终使用 prior knowledge） |
| HTTP/3 over QUIC | 可选，且只能显式指定（`HttpVersionPolicy::Http3`） |
| WebSocket over HTTP/1.1 | 支持 |
| WebSocket over HTTP/2 或 HTTP/3 | 不支持 |
| 重定向 | 支持，默认关闭（`max_redirects = 0`） |
| Cookie jar | 支持 |
| 出站代理 | HTTP CONNECT、HTTPS CONNECT、SOCKS5 |
| gzip / brotli 响应解压 | 支持（默认只广告 gzip） |
| 流式请求上传 | 支持 |
| SSE | 支持 |
| 请求 trailers | HTTP/1.1、HTTP/2；HTTP/3 不支持 |
| 自动 `Expect: 100-continue` | HTTP/1.1、HTTP/2 |

## 依赖与标准要求

| 依赖 | 用途 |
|---|---|
| Boost（Asio、System、Regex、Core） | I/O、错误模型、路由正则 |
| OpenSSL | TLS、证书校验、mTLS、QUIC TLS |
| zlib、brotli | gzip/br 内容编码、WebSocket permessage-deflate |
| glaze | JSON 反射、类型化 params/extractor、JWT/JWKS、OpenAPI |
| ngtcp2 + nghttp3 | 可选的 HTTP/3 与 QUIC |

### C++ 标准与特性门控

- **C++17** 足以使用 core、proto、transport、HTTP/1.x、HTTP/2、WebSocket、
  路由、中间件、静态文件、反向代理与客户端。
- **C++20 或更高** 额外启用协程 handler 与 `asio::use_awaitable`
  （`BOOST_ASIO_HAS_CO_AWAIT`）。
- **C++23 + glaze** 启用 glaze 辅助：JSON 反射、类型化
  `path_params` / `query_params`、`Path` / `Query` / `Json` / `Form`
  extractor、JWT/JWKS 与 OpenAPI 文档模型。

门控宏是 `SIMPLE_HTTP_HAS_GLAZE`（`core/glaze_support.h`）。当
`__cplusplus > 202002L` 且存在 `<glaze/glaze.hpp>` 时自动为 `1`，否则为 `0`。
可以用 `-DSIMPLE_HTTP_HAS_GLAZE=0` 显式关闭，或用 `=1` 强制打开。门控按翻译单元
求值；由于被门控的 API 是纯头文件模板，同一程序里混用 C++17 与 C++23 翻译单元是
受支持的。

`Header<T, "name">` 与 `Cookie<T, "name">` extractor 额外要求 C++20 的
class-type 非类型模板参数。

## 构建与安装

`simple_http` 是纯头文件的：把 `include/` 加入 include 路径就是最小集成方式。

### xmake

仓库的 `xmake.lua` 会构建纯头文件目标与全部测试二进制。它期望以下包：
`boost`、`openssl4`、`glaze`、`zlib`、`brotli`，以及可选的
`ngtcp2` / `nghttp3`。

```sh
xmake f -m release
xmake build -j 12
```

库的使用者只需要 include 目录与链接依赖。测试目标声明为
`set_default(false)`，所以裸 `xmake build` 不会构建它们；需要按名字构建，例如
`xmake build -j 12 test_unit`。

### CMake

```cmake
find_package(simple_http CONFIG REQUIRED)
target_link_libraries(app PRIVATE simple_http::simple_http)
```

当前 CMake 接口目标要求 C++23，并链接 Boost、OpenSSL、zlib、brotli 与 glaze。
它**没有**链接 ngtcp2/nghttp3，因此通过 CMake 不会启用 HTTP/3。不使用 glaze 辅助
的严格 C++17 消费者可以直接包含头文件；当前 CMake 的粗糙之处见
[已知限制](#已知限制)。

## 快速开始：服务器

### C++17，回调风格

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

`Server::start()` 会自行启动工作线程。`Server::stop()` 会遵守
`ServerConfig::shutdown_grace`，取消剩余连接，并 join I/O 线程。

### C++20/23，协程 handler

```cpp
server.route({Method::Get}, "/hello",
             [](RequestPtr, ResponsePtr res) -> boost::asio::awaitable<void> {
                 res->status(status::ok).content_type("text/plain");
                 co_await res->async_send("hello, world\n", boost::asio::use_awaitable);
             });
```

接受的 handler 形状会被自动归一化：

- `void(RequestPtr, ResponsePtr)` —— 同步，立即完成；
- `void(RequestPtr, ResponsePtr, SslHandle)`；
- `void(RequestPtr, ResponsePtr, HandlerCompletion)`；
- `void(RequestPtr, ResponsePtr, SslHandle, HandlerCompletion)`；
- C++20 构建中的 `awaitable<void>(RequestPtr, ResponsePtr[, SslHandle])`。

handler 抛出异常时：如果响应尚未开始，转为 500；如果响应头已经发出，则关闭连接。

### 监听选项

```cpp
ServerConfig cfg;

TcpListenerConfig plain;
plain.address = InetAddress{"0.0.0.0", 8080};
plain.plaintext = PlaintextHttp::Http1AndHttp2;   // 嗅探 + 允许 h2c Upgrade
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

常用的 `ServerConfig` 字段：

| 字段 | 默认值 | 含义 |
|---|---|---|
| `worker_threads` | 4 | 单线程 `io_context` 工作线程数 |
| `max_connections` | 0 | 0 = 不限制；超出的连接留在内核 backlog |
| `reuse_port` | true | 每个 worker 一个 acceptor，使用 `SO_REUSEPORT` |
| `shutdown_grace` | 5000 ms | 关闭前允许在飞 handler 完成的时间 |
| `tcp_nodelay` | true | 复制到隐式 listener |
| `plaintext_protocols` | `Both` | `Http1`、`Http2`（prior knowledge）或 `Both` |
| `limits` | `EngineLimits{}` | 所有 listener 共享的协议限制 |

`start(StartPolicy::AllOrNothing)` 是默认值；`BestEffort` 会保留绑定成功的
listener，并在 `StartReport` 中报告失败项。

## 快速开始：客户端

### C++17，回调风格

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

### C++20/23，awaitable 风格

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

客户端对每个 origin 保留一条可复用连接（HTTP/1.1 在这条连接上排队，HTTP/2 在其上
多路复用）。发往其他 origin 的请求会建立一条在交换结束后关闭的一次性连接。如果
`io_context` 上还有 keep-alive 会话，应在销毁它之前调用 `Client::close()`。

## 路由与中间件

```cpp
server.route({Method::Get}, "/hello", handler);                 // 精确路径
server.route({Method::Get, Method::Post}, "/items", handler);   // 多个方法
server.route({}, "/anything", handler);                         // 任意方法
server.route_regex({Method::Get}, R"(/files/(\d+))", handler);  // 正则
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

路由匹配顺序：

1. Host 子 Router（`Host` 头命中已注册 pattern 时）
2. 精确路径（方法感知）
3. `{param}` 模板 trie，包括作为最后一段的 `{*rest}`
4. 本地正则
5. HTTP 反向代理路由
6. 静态文件阶段
7. fallback handler
8. 内建 404

隐式规则：

- 路径命中了 exact/template/regex 路由但方法不允许时，返回 405 + `Allow`；
  `OPTIONS` 则返回 204 + `Allow`。**不会**继续落到 proxy/static/fallback。
- 注册 `GET` 会隐式提供 `HEAD`；writer 会抑制响应体。
- 空方法列表匹配任意方法，包括扩展方法 token。
- `Allow` 始终包含 `OPTIONS`。

### 中间件

中间件采用 Go `net/http` / tower 的形状。回调形式在 C++17 下可用：

```cpp
server.use([](RequestPtr req, ResponsePtr res, SslHandle ssl,
              Next next, HandlerCompletion done) {
    const auto t0 = std::chrono::steady_clock::now();
    next(std::move(req), std::move(res), ssl,
         [t0, done = std::move(done)]() mutable {
             // 在链的其余部分之后运行
             (void)t0;
             std::move(done)();
         });
});
```

在 C++20 构建中，返回
`awaitable<void>(RequestPtr, ResponsePtr, SslHandle, CoroNext)` 的协程中间件也会被
接受。`make_middleware`、`make_middlewares` 与 `compose_middleware` 会归一化混用
形式。

内置中间件（`simple_http::middleware`）：

`request_id()`、`access_log()`、`recovery()`、`basic_auth(user, pass)`、
`real_ip(trusted)`、`clean_path()`、`strip_prefix(prefix)`、
`redirect_slashes()`、`ws_origin(allowed, allow_missing = false)`、
`timeout(deadline)`、`compress(cfg)`、`decompress_request(cfg)`、
`secure_headers(cfg)`、`rate_limit(bucket)`、`rate_limit(limiter, key_fn)`、
`session(store, cfg)`、`csrf(cfg)`、`jwt(cfg)`、`require_scope(scope)`、
`require_role(role)`。

CORS 通过 `server.cors(cfg)` 或 `simple_http::make_cors_middleware(cfg)` 安装，
并且始终作为最外层中间件运行。空 `allow_origins` 搭配
`allow_credentials = true` 是 fail-closed，不会发出任何 CORS 头。

## Request、Response 与 Body

### Request

```cpp
req->method();              // Method
req->method_token();        // 原始 token（保留扩展方法）
req->version();             // Version::Http1 / Http11 / Http2 / Http3
req->target();              // request-target
req->path();                // path 部分，未解码
req->query();               // 原始 query 字符串
req->query_params();        // 解析后的 QueryParams（惰性）
req->param("id");           // 路径参数（如果引擎已填充）
req->header("host");        // optional<string_view>
req->headers();             // Headers
req->trailers();            // 请求 trailers（HTTP/1.1 与 HTTP/2）
req->cookie("sid");
req->peer_address();
req->app_state<T>();        // Router 上注册的共享应用状态
req->get_state<T>() / set_state<T>(...);  // 每请求状态
```

使用 token 读取请求体：

```cpp
// 完成签名：void(error_code, std::string)
auto [ec, body] = co_await req->body().async_read_all(
    boost::asio::as_tuple(boost::asio::use_awaitable));

// 完成签名：void(error_code, ReadResult)；ReadResult{data, eof}
auto [ec2, chunk] = co_await req->body().async_read(
    boost::asio::as_tuple(boost::asio::use_awaitable));
```

HTTP/1.x 请求体是 pull 模式：handler 不读，引擎就不会读 socket。HTTP/2 与 HTTP/3
的请求体被推入有界 channel；channel 满时只停放对应流。

### Response

同步 setter 可链式调用：

```cpp
res->status(200)
   .content_type("application/json")
   .header("x-trace-id", id)
   .set_cookie("sid", value, std::chrono::seconds{3600},
               "/", {}, true, true, "Lax");
```

一次性发送或流式发送：

```cpp
// 一个完整响应。完成签名：void(error_code)。
co_await res->async_send("hello\n", token);

// 流式发送。每个操作完成签名：void(error_code)。
co_await res->async_begin(token);
co_await res->async_write("chunk", token);
co_await res->async_finish("last chunk", token);

// trailers 替代最后一次 finish。
res->trailer("x-checksum", value);
co_await res->async_finish_trailers(token);

// 无 body 且无 body framing：仅 HEAD、204、304。
co_await res->async_send_bodyless(token);

// interim 响应。
co_await res->async_send_continue(token);

// 连接状态。
co_await res->async_connected(token);   // 完成签名：void(error_code, bool)
co_await res->async_close(token);
```

辅助接口：`redirect(location, code)`、`async_sse_begin`、`async_sse_event`、
`async_sse_comment`、`version()`、`started()`、`reset_for_error()`、
`activity()`。

writer 强制的规则：

- 一个 `ResponseWriter` 只发送一个响应头。第二次调用
  `async_send` / `async_send_bodyless` / `async_begin` 返回
  `invalid_argument`。
- 状态码必须是 100–599。非法状态码、头名/头值、cookie 或 trailer 会把响应标记为
  拒绝，并让写入失败。
- 会自动补充 `Date`、`Server`，以及（对有内容的状态）`Content-Type`。
- `async_send_bodyless` 只接受 HEAD、204、304。keep-alive HTTP/1.1 连接上的无 body
  200 会造成 response smuggling。

HTTP/1.x 的流式响应使用 chunked 编码；HTTP/1.0 与 HEAD 使用 close-delimited
framing。HTTP/2 与 HTTP/3 使用 DATA 帧，trailers 由尾部字段段承载。

## WebSocket

服务端：

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

客户端：

```cpp
simple_http::WebSocketSpec spec;
spec.origin = "http://127.0.0.1:8080";

auto [ec, ws] = co_await client.async_open_websocket(
    "ws://127.0.0.1:8080/chat", spec,
    boost::asio::as_tuple(boost::asio::use_awaitable));
if (!ec)
    co_await ws->async_write_text("hello", boost::asio::use_awaitable);
```

`WebSocket` 操作：`async_read`、`async_write_text`、`async_write_binary`、
`async_write(data, text)`、`*_view` 变体、`async_ping`、`async_close`、
`async_enable_write_compression`、`async_set_compression_level`、
`compression_negotiated()`、`is_open()`。

服务端注意事项：

- `ws_route` handler 可以是回调，也可以是协程。
- 支持 HTTP/1.1 Upgrade 与 HTTP/2 RFC 8441 extended CONNECT。
- `EngineLimits::ws_compression` 启用 permessage-deflate 协商。
- 内部有界 read-ahead 队列避免了经典死锁：应用执行 `read → write`，而对端只发不读。
- `write_view` / `*_view` 在操作完成前借用调用方缓冲；按值变体会复制。

## JSON、表单、类型化提取、校验与认证

> 除 `State<T>`、`Valid<E>` 与校验辅助外，本节 API 都要求
> `SIMPLE_HTTP_HAS_GLAZE=1`（C++23 + glaze）。

### JSON

```cpp
// 完成签名：void(error_code, result<T>)
auto [ec, parsed] = co_await simple_http::async_read_json_body<CreateUser>(
    *req, boost::asio::as_tuple(boost::asio::use_awaitable));

// 完成签名：void(error_code)
co_await simple_http::async_write_json(res, user, simple_http::status::created,
                                       boost::asio::use_awaitable);
```

`async_read_json_body` 对畸形或类型不匹配的 JSON 返回 `invalid_argument`；
由 handler 决定这是 400 还是 422。

与 glaze 一起使用的类型需要 `glz::meta<T>` 特化（或 glaze 的反射配置），
与其他任何 glaze 程序一致。

### 表单与 multipart

```cpp
// 完成签名：void(error_code, QueryParams)
co_await read_urlencoded_body(*req, token);

// 完成签名：void(error_code, vector<MultipartPart>)
co_await read_multipart_body(*req, token);

// 通过回调进行增量解析。完成签名：void(error_code)
co_await read_multipart_stream(req->body(), boundary, callbacks, token);
```

### 类型化提取

```cpp
struct UserParams { std::string id; };

server.route({Method::Get}, "/users/{id}",
             extract<Path<UserParams>>(
                 [](const UserParams &p, RequestPtr, ResponsePtr res, HandlerCompletion done) {
                     res->async_send("user " + p.id, finish_with(std::move(done)));
                 }));
```

extractor：`Path<T>`、`Query<T>`、`Json<T>`、`Form<T>`、`State<T>`、
`Header<T, "name">`（C++20）、`Cookie<T, "name">`（C++20），以及
`Valid<Extractor>`。

提取失败不会进入 handler：

| 失败 | 状态码 |
|---|---|
| 缺失/非法 path 或 query | 400 |
| 畸形 JSON/表单 | 400 |
| 请求体超过限制 | 413 |
| 缺失应用状态 | 500 |
| `Valid<T>` 拒绝值 | 422 |

一次 `extract<...>` 中最多只能出现一个 body extractor（`Json` 或 `Form`）。

### 校验

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

`Valid<T>` extractor 失败时产生 422，并带有 JSON `details` 数组，元素形如
`{field, code, message}`。校验辅助函数是 `required_field`、`min_length`、
`max_length`、`range`、`one_of` 与 `push_if`。

### Session、CSRF 与 JWT

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

- `SessionStore` 是异步的：`load`、`save`、`destroy` 接收 completion handler。
  `MemorySessionStore` 是内置的进程内存储。
- Session cookie 只携带不透明 id；默认 `secure = true`、`http_only = true`、
  `rolling = true`。
- CSRF 是基于 header 的 double-submit（默认 `x-csrf-token`）。它检查 header，
  不检查表单字段。
- JWT 支持 HS256、RS256、ES256，可使用静态 PEM key 或 JWKS 文档。会拒绝
  `alg: none`，默认要求 `exp`，配置了 issuer/audience 时进行校验。它是验证器，
  不是授权服务器：不负责签发 token、OIDC discovery 或 refresh 流程。
- `fetch_jwks_json(client, url, token)` 拉取一次 JWKS；应用把结果放进
  `JwtConfig::jwks_json`。

## 客户端参考

### 请求构造

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

`RequestBuilder` 提供 `query`、`header`、`headers`、`content_type`、
`basic_auth`、`bearer_auth`、`body`、`max_body_bytes`、`timeout` 与
`version`。当前实现**没有** `.json(...)` 便捷方法——请使用
`content_type("application/json")` 加 `body(serialized_json)`。

### 缓冲响应

`Response` 暴露 `status()`、`version()`、`headers()`、`header()`、
`header_owned()`、`ok()`、`bodyless()`、`uncompressed()`、`error_for_status()`、
`text()`、`bytes()` 与 `async_read_all(max_bytes, token)`。缓冲
`Response::text()` 与 `Response::bytes()` 返回同一个字符串。

### 流式请求/响应

```cpp
simple_http::StreamSpec spec;
spec.method = simple_http::Method::Post;
spec.headers.add("content-type", "application/octet-stream");

// 完成签名：void(error_code, shared_ptr<Stream>)
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

`Stream` 还暴露 `async_finish_trailers`、`async_read`、
`async_ensure_continue`、`head()`、`trailers()`、`finished()` 与 `abort()`。
显式 stream 路径不跟随重定向、不应用 cookie、不重试。HTTP/1.1 是半双工：必须先
结束请求体，再读取响应。

### SSE

```cpp
auto [ec, sse] = co_await simple_http::SseStream::async_open(
    client, "http://127.0.0.1:8080/events", simple_http::SseOptions{},
    boost::asio::as_tuple(boost::asio::use_awaitable));

for (;;) {
    auto [nec, event] = co_await sse->async_next(
        boost::asio::as_tuple(boost::asio::use_awaitable));
    if (nec)   // 正常结束时为 boost::asio::error::eof
        break;
    std::cout << event.event << ": " << event.data << '\n';
}
```

### 流式 multipart 上传

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

服务端的 multipart `filename` 是不可信输入。把它拼到目录之前，先用
`simple_http::sanitize_filename()` 归约为单一路径分量；返回空字符串时应自行生成
文件名。

### 客户端配置

| 字段 | 默认值 |
|---|---|
| `TlsClientConfig::verify_peer` / `verify_host` | true / true |
| `TlsClientConfig::min_version` | TLS 1.2 |
| `ClientConfig::default_version` | `Auto` |
| `connect_timeout` | 10 s |
| `idle_timeout` | 120 s |
| `request_timeout` | 60 s |
| `response_head_timeout` | 30 s |
| `body_idle_timeout` | 0（不限制） |
| `max_redirects` | 0（不跟随） |
| `auto_decompress` | true，广告 `gzip` |
| `accept_encodings` | `{"gzip"}` |
| `retry` | 不重试 |
| `limits.h2_initial_window` | 1 MiB（客户端覆盖） |
| `quic_max_udp_payload_size` | 1200 |
| `proxy` / `proxy_from_env` | 无 / true |
| `tcp_nodelay` / `tcp_keepalive` | true / false |

自动解压与 Go 的 `http.Transport` 一致：只有当客户端自己注入了
`Accept-Encoding`（调用方没有设置）、方法不是 HEAD、且请求没有 `Range` 时才生效。
设置 `accept_encodings = {"br", "gzip"}` 可以启用 brotli。

重试是 opt-in 的。`RetryPolicy::retry_statuses`（例如 `{429, 503}`）启用状态码
重试，并支持 `Retry-After`。`condition` / `require_replayable` 字段存在，但当前
缓冲路径不会读取它们。

### 出站代理

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

支持的类型是 HTTP CONNECT、HTTPS CONNECT 与 SOCKS5（可选用户名/密码）。
`proxy_from_env = true` 会在拨号时读取 `HTTPS_PROXY`、`HTTP_PROXY`、
`ALL_PROXY` 与 `NO_PROXY`。`NO_PROXY` 支持 `*`、精确 host、域后缀与
`host:port`；不支持 CIDR 与 glob 形式。HTTP/3 直接拨号，不走代理隧道。

### Cookie

```cpp
auto jar = std::make_shared<simple_http::CookieJar>();
ClientConfig cfg;
cfg.cookie_jar = jar;
cfg.max_redirects = 5;
```

`CookieJar` 执行 Domain/Path/Secure/Expires 规则、`__Host-`/`__Secure-` 前缀规则，
并把 loopback 视为安全源。如果没有提供 `PublicSuffixFn`，会拒绝单标签 `Domain`
与 IP 字面量。

## 反向代理、静态文件与 OpenAPI

### HTTP 反向代理

```cpp
simple_http::HttpProxyTarget target;
target.host = "127.0.0.1";
target.port = 9000;
target.preserve_host = false;

server.http_proxy("/api", target);
```

`HttpProxyTarget` 还提供 `rewrite_path`、`tls`、`host_header`、
`preserve_inbound_forwarding`、`hooks`（`rewrite`、`modify_response`、
`error_handler`）与 `use_env_proxy`。请求路径会按配置重写后转发；hop-by-hop 字段
以及 `Connection` 指名的字段会在两侧被剥离。当前实现会缓冲完整请求体和响应体
（响应上限 64 MiB）。

### 静态文件

```cpp
simple_http::StaticFilesConfig static_cfg;
static_cfg.table.root = "./public";

auto site = std::make_shared<simple_http::StaticFiles>(std::move(static_cfg));
std::string error;
if (!site->load(error))
    throw std::runtime_error(error);

server.static_files(site);
```

`StaticFiles` 支持 index 文件、SPA fallback、预压缩 gzip/brotli 变体、`Range`
（identity 表示）、`ETag` / `Last-Modified` / 条件请求，以及按扩展名推断 MIME。
它服务 GET 与 HEAD；文件不存在时交给 Router 的 fallback/404。

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

只有注册时附带 `openapi::doc()` 描述的路由才会出现在文档中。路径模板名必须与
`path_params<T>` 的字段名一致；不一致会在注册时被拒绝。`serve_swagger_ui`
提供一个小的 HTML 页面，从 CDN 加载 Swagger UI。

## 限制参数

`EngineLimits` 由所有服务端引擎共享，可以按 listener 覆盖。

| 字段 | 默认值 |
|---|---|
| `idle_timeout` | 120 s（≤ 0 禁用） |
| `max_header_bytes` | 64 KiB → 431 |
| `max_body_bytes` | 64 MiB → 413；0 表示不限制 |
| `ws_read_ahead_bytes` | 4 MiB |
| `ws_compression` | false |
| `h2_max_concurrent_streams` | 200 |
| `h2_max_frame_size` | 16384 |
| `h2_initial_window` | 65535 |
| `h2_enable_connect_protocol` | true |
| `h2_max_header_list_size` | 0 → 跟随 `max_header_bytes` |
| `h2_max_queued_streams` | 0 → 跟随 `h2_max_concurrent_streams` |
| `h3_max_concurrent_streams` | 0 → 跟随 `h2_max_concurrent_streams` |
| `h3_alt_svc` | true（仅 HTTP/3 构建） |
| `h3_alt_svc_max_age` | 86400 |

## 测试

唯一的测试入口是 `test/run.sh`（内部驱动 `test/run.py`）。xmake 构建 C++ 测试
二进制；Go 参考服务端与探针由各自的 diff runner 构建。

```sh
test/run.sh fast        # unit + cpp17
test/run.sh standard    # fast + openapi
test/run.sh full        # 全部 suite
test/run.sh list
test/run.sh doctor
test/run.sh unit '[h2]' # Catch2 过滤
test/run.sh client-diff
test/run.sh server-diff
```

| Suite | Tier | 内容 |
|---|---|---|
| `unit` | fast | Catch2 单测/回归测试 |
| `cpp17` | fast | 以 C++17 编译并运行公共 API |
| `openapi` | standard | OAS 3.1 元模式、文档/路由契约、Swagger UI |
| `client-diff` | full | C++ 客户端与 Go `net/http` 客户端对同一个 Go 服务端 |
| `server-diff` | full | Go 探针分别打 C++ 与 Go 参考服务端 |
| `concurrency` | full | 多客户端/多线程/拆卸 harness |
| `conformance` | full | h2spec、h1spec、h3spec、Autobahn |
| `stress` | full | h2load 与 k6，要求零失败 |

`--parallel N` 默认 3。`conformance` 与 `stress` 共用 7788–7792 端口，固定在同一条
串行 lane 中运行。`--jobs` 控制编译并行度（默认 12）。`full` 默认把缺失的外部
工具当作错误，除非传入 `--allow-skip`。报告与每个 suite 的日志写到
`.cache/test-run/<timestamp>-<pid>/`。

额外套件：

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

端口、diff 分组、归一化规则与工具前置条件见 `test/README.md`。

## 已知限制

- **客户端 h2c Upgrade 尚未接线。** `H2cMode::Upgrade` 与
  `ClientConfig::default_h2c` 存在，但客户端当前对明文 HTTP/2 使用 prior
  knowledge（`HttpVersionPolicy::Http2`），其余情况使用 HTTP/1.1。
- **客户端重试只基于状态码。** 缓冲实现不会读取 `RetryPolicy::condition` 与
  `require_replayable`。
- **重定向默认关闭。** 需要设置 `ClientConfig::max_redirects`。
- **没有 UNIX domain 服务端 listener。** `UnixAddress` 是保留类型，启动时会被
  拒绝；transport 类型存在，但没有 listener 使用它们。
- **没有 HTTP/3 WebSocket 或 extended CONNECT。**
- **HTTP/3 不暴露请求 trailers 与 `Expect: 100-continue`。** 响应 trailers 支持。
- **HTTP/2 与 HTTP/3 都没有 server push**；HTTP/3 客户端没有 0-RTT。
- **HTTP/3 只能显式启用。** 客户端不读取 Alt-Svc 或 HTTPS RR。
- **出站代理不覆盖 HTTP/3。**
- **`NO_PROXY` 不支持 CIDR 或 glob。**
- **客户端每个 origin 只保留一条连接**，不是通用连接池。发往第二个 origin 的请求
  使用一次性连接。
- **`CookieJar` 不内置 PSL 数据。** 需要完整公共后缀检查时注入
  `PublicSuffixFn`；默认拒绝单标签域与 IP 字面量。
- **glaze 辅助需要 C++23。** 在真正的 C++17 翻译单元中，
  `Path` / `Query` / `Json` / `Form` 提取、类型化 params、JWT/JWKS 与 OpenAPI
  不可用。`State<T>`、`Valid<E>` 与校验辅助仍然可用。
- **`Header` / `Cookie` extractor 需要 C++20。**
- **同步文件 I/O** 出现在 `async_send_file`、`async_read_body_to_file` 与流式
  multipart 上传中；它会阻塞客户端/连接 executor。
- **HTTP 反向代理会缓冲完整 body**（响应上限 64 MiB）。
- **CMake 不启用 HTTP/3。** 导出的目标要求 C++23 并链接 glaze，但生成的 config
  缺少 `find_dependency(glaze)`。
- **`simple_http_cpp17` CMake 目标**指向本仓库中不存在的 `cpp17/include`
  目录；它未被安装或导出。
- **单测 runner 不产出聚合 JSON 报告。** 结果通过退出码与每个 suite 的日志传达。
- **CI 不强制格式检查。** `./format.sh --check` 存在，但需要在提交前手动运行。

## 许可证

MIT。见 [LICENSE](LICENSE)。
