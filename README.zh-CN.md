# simple_http

**一个纯头文件、基于 Boost.Asio C++ 协程的 HTTP 服务器与客户端库。**
支持 HTTP/1.0、HTTP/1.1、HTTP/2（h2 / h2c）、可选的 HTTP/3（QUIC），以及 WebSocket。

> A header-only HTTP server and client library for modern C++, built on `boost.asio`
> C++23 coroutines. It speaks HTTP/1.0, HTTP/1.1, HTTP/2 (h2/h2c), optional HTTP/3
> over QUIC, and WebSocket.

- **当前版本**：`0.9.0`（见 `include/simple_http/core/version.h`）
- **语言标准**：库接口要求 C++23；仓库自带的 xmake 构建配置使用 C++26
- **许可证**：MIT
- **入口头**：`#include <simple_http.h>`

[中文](#toc) · [English](README.md)

---

<a id="toc"></a>
## 目录

- [这是什么](#what-is-this)
- [特性概览](#features)
- [快速开始](#quick-start)
- [框架层](#framework-layer)
- [构建、依赖与编译宏](#build)
- [服务端指南](#server-guide)
- [客户端指南](#client-guide)
- [架构与并发模型](#architecture)
- [测试与验证](#testing)
- [已知限制](#known-limitations)
- [代码风格与贡献](#contributing)

<a id="what-is-this"></a>
## 这是什么 · What is this

`simple_http` 把 HTTP 的各个部分拆成若干层，全部实现都在 `include/` 下的头文件里，
没有 `.cpp` 源文件：

| 层 | 目录 | 职责 |
|---|---|---|
| `core` | `include/simple_http/core/` | 与协议无关的基础设施：类型、状态码、日期、URL 编码、压缩、日志、I/O 池、限制项 |
| `proto` | `include/simple_http/proto/` | 版本无关的 `Request` / `ResponseWriter` / `Body` 模型，表单、JSON、multipart、WebSocket 消息层 |
| `transport` | `include/simple_http/transport/` | 字节流抽象：明文 TCP、TLS（含 ALPN / mTLS） |
| `engine` | `include/simple_http/engine/` | 各协议版本引擎：`h1/`、`h2/`、`h3/`；它们实现同一套 `ResponseSink`，由 `ResponseWriter` 门面转发 |
| `handler` | `include/simple_http/handler/` | Handler 类型系统、路由、中间件、App State、提取器/校验、Session/CSRF/JWT、静态文件、反代、限流、CORS、安全头 |
| `net` | `include/simple_http/net/` | 监听、协议嗅探、`Server` 门面 |
| `client` | `include/simple_http/client/` | 出站客户端：HTTP/1.1、HTTP/2、可选的 HTTP/3、WebSocket、SSE、代理、Cookie |
| `quic` | `include/simple_http/quic/` | QUIC 传输封装（ngtcp2），仅在 `SIMPLE_HTTP_ENABLE_HTTP3` 下编入 |
| `openapi` | `include/simple_http/openapi/` | 按路由收集的 OpenAPI 3.1 文档与 Swagger UI |

上层（`proto` / `handler` / `net` / `client` 门面）不出现 `if (version == ...)` 这类分支；
版本差异被收敛在 `engine/` 和具体的 `ResponseSink` 实现里，`ResponseWriter` 只做转发。

### 设计要点

1. **协程贯穿始终。** Handler 是 `asio::awaitable<void>`；读请求体、写响应、等待取消
   都是可 `co_await` 的异步操作，天然具备反压。
2. **一个模型覆盖所有版本。** 业务代码只面对 `Request` / `ResponseWriter` / `Router`，
   不关心底下是 HTTP/1.x、HTTP/2 还是 HTTP/3。
3. **传输与协议解耦。** 引擎通过 `Transport` 读写字节，不直接依赖具体 socket 类型。
4. **并发模型 A。** 每条连接绑定到 `IoCtxPool` 中一个单线程 `io_context`；该连接上的
   引擎、writer、handler 都只在这一个线程运行，因此 `engine/` 内没有 mutex / atomic。
   `ResponseWriter` / `WebSocket` 可以跨线程持有，写操作会先 hop 回连接所属 executor。
5. **显式反压。** HTTP/2 请求体按流停放，WebSocket 有有界入站读泵，流式响应走写泵；
   没有一处允许对端把内存推成无界。
6. **日志与后端解耦。** 库只定义 `LogSink`，不依赖任何第三方日志库；适配代码由使用者
   在自己的工程里实现。

<a id="features"></a>
## 特性概览 · Features

### 服务端能力

- HTTP/1.0、HTTP/1.1、HTTP/2（TLS ALPN、h2c `Upgrade`、h2c prior-knowledge）。
- HTTP/3 over QUIC（需要 `SIMPLE_HTTP_ENABLE_HTTP3`，依赖 ngtcp2 + nghttp3）；
  支持 Alt-Svc 通告、连接迁移与可配置 0-RTT。
- WebSocket：HTTP/1.1 `Upgrade`，以及 HTTP/2 extended CONNECT（RFC 8441）。
- 方法感知路由：精确路径、`{param}` 段 trie、`{*rest}` 通配、正则路由、Host 作用域、
  `any_methods`、fallback。
- 框架层：类型化 App State、编译期提取器、校验、Session、CSRF、JWT
  （HS256/RS256/ES256）。
- 中间件链：全局、组、单路由三级作用域；CORS、超时、恢复、认证、访问日志、
  request-id、real-ip、限流、安全头、响应压缩。
- 流式响应、SSE、响应尾部字段（trailers）。
- 请求取消与流式友好超时（响应头前绝对、响应头后空闲）。
- 静态文件（ETag / Range / 304 / SPA fallback / 预压缩 `.br` / `.gz`）、
  `send_file`、流式上传落盘。
- 表单、JSON、multipart（整块与流式）、类型化 path/query 参数。
- HTTP 反向代理与 WebSocket 字节级反向代理。
- OpenAPI 3.1 文档 + Swagger UI（按路由元数据收集）。
- TLS 1.3 默认、可选 TLS 1.2、mTLS、ALPN、UNIX domain socket、多监听器。
- HTTP/2 Rapid Reset（CVE-2023-44487）防护、请求头/体大小上限、空闲超时。

### 客户端能力

- HTTP/1.1 与 HTTP/2（HTTPS 走 ALPN；明文走 h2c Upgrade 或 prior-knowledge）。
- HTTP/3（`HttpVersionPolicy::Http3`，显式启用；QUIC 自带 TLS 1.3）。
- 便捷 API（`get` / `post` / … / `send`）与显式全双工 `Stream`。
- 流式 multipart / 文件体上传，不把整个 body 放进内存。
- 自动 `Expect: 100-continue`、请求 trailers、429/503 `Retry-After` 重试。
- 重定向、CookieJar、Basic/Bearer 认证、自动解压、缓冲式 multipart 上行。
- 出站代理：HTTP CONNECT、HTTPS 代理、SOCKS5，支持环境变量与 `NO_PROXY`。
- 分层超时（连接、响应头 TTFB、响应体空闲）、免费透明重拨 + 可配置重试。
- 服务端/客户端 WebSocket 共用同一套消息层；客户端 WebSocket 走 HTTP/1.1 Upgrade。
- SSE 客户端（`SseStream`）。

<a id="quick-start"></a>
## 快速开始 · Quick Start

### 最小服务器

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
                     co_await res->status(200).content_type(mime::text_plain).send("Hello World!");
                 });

    auto started = server.start(); // std::expected<StartReport, StartupError>
    if (!started) {
        for (const auto &failure : started.error().failures)
            std::println(stderr, "{}: {}", failure.name, failure.error.message());
        return 1;
    }

    // 工作线程在内部 I/O 池里；主线程只等待信号。
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (g_stop == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    return 0;
}
```

`Server` 构造时就会创建 I/O 线程池；`start()` 绑定监听器并启动 accept 循环。
`stop()` 会关闭监听、等待已进入 handler 的请求（受 `shutdown_grace` 限制）、
再关闭剩余连接并停止线程池。

### 路由、参数与中间件

```cpp
Server server{cfg};

server.use(middleware::request_id());
server.use(middleware::access_log());
server.use(middleware::recovery());

server.route({Method::Get}, "/users/{id}",
             [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                 auto id = req->param("id"); // std::optional<std::string_view>
                 if (!id) {
                     co_await res->status(400).send("bad id");
                     co_return;
                 }
                 co_await res->status(200).send("user " + std::string{*id});
             });

server.group("/api", {middleware::basic_auth("svc", "s3cret")}, [](Router &api) {
    api.route({Method::Get, Method::Post}, "/items",
              [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
                  co_await res->status(200).send("items");
              });
});

server.fallback([](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
    co_await res->status(404).send("not found");
});
```

路由的隐式规则：

- 注册 `{Method::Get}` 自动允许 `HEAD`，writer 会抑制响应体。
- 路径存在但方法不匹配 → `405` + `Allow`；`OPTIONS` 自动 `204` + `Allow`。
- 路径不存在 → `404`（或 fallback）。
- 405 / 自动 OPTIONS 都是普通 keep-alive 响应，引擎会 drain 未读请求体后继续复用连接。

### WebSocket

```cpp
server.ws_route("/chat",
    [](RequestPtr, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws->read(); // std::expected<WsMessage, error_code>
            if (!msg)
                break; // 对端关闭或出错
            if (auto ec = co_await ws->write("echo: " + msg->data, msg->text))
                break;
        }
        co_return; // 返回后引擎发送 Close 并优雅关闭
    });
```

### 客户端示例

```cpp
asio::awaitable<void> fetch() {
    Client http{co_await asio::this_coro::executor}; // 绑定到单线程 executor

    auto r = co_await http.get("https://example.com/").send();
    if (!r)
        co_return;
    std::println("status={}", r->status());
    auto body = co_await r->read_all();
}
```

全双工流式请求：

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

### OpenAPI 与 Swagger UI

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
        co_await res->status(200).content_type(mime::app_json).send(body ? *body : "{}");
    },
    openapi::doc()
        .path_params<PetParams>()
        .response<Pet>()
        .summary("get a pet")
        .operation_id("getPet")
        .error<ErrorBody>(404, "no such pet"));

server.serve_openapi("/openapi.json");
server.serve_swagger_ui("/swagger", "/openapi.json");
```

### 编译与运行

```sh
xmake build -j12
xmake run server
```

`test/server.cpp` 会启动多个监听器：`:7788` 明文嗅探、`:7789` TLS/mTLS（同时
UDP QUIC）、`:7790` 纯 h2c、`:7791` 纯 h1、`:7792` TCP+UDP 双传输、`:7793`
大窗口 h2、`:7794` builder 配置的纯 h1。完整路由示例也在其中。

<a id="framework-layer"></a>
## 框架层 · Framework layer

`handler/` 层现在包含一组小型应用框架能力：共享状态、类型化提取、校验、Session/
CSRF/JWT、Host 作用域和通配路由。全部 opt-in，且只存在于 handler 层，协议引擎无感知。

### App State

```cpp
struct AppState {
    std::shared_ptr<Db> db;
};

server.state(std::make_shared<AppState>(AppState{db}));

server.route({Method::Get}, "/users", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
    const AppState *state = req->app_state<AppState>();
    if (!state) {
        co_await res->status(500).send("state missing");
        co_return;
    }
    co_await res->status(200).send(co_await state->db->query(...));
});
```

状态按静态类型键存放在 Router 中，按 `shared_ptr` 共享，不按请求复制。必须在
`start()` 前注册；Host 子 Router 共享同一份状态。

### 类型化提取器与校验

```cpp
struct UserParams { int id{}; };
struct CreateUser { std::string name; int age{}; };

server.route({Method::Get}, "/users/{id}",
    extract<Path<UserParams>, State<AppState>>(
        [](const UserParams &p, AppState *state, RequestPtr, ResponsePtr res)
            -> asio::awaitable<void> {
            co_await res->status(200).send(std::to_string(p.id));
        }));

server.route({Method::Post}, "/users",
    extract<Valid<Json<CreateUser>>>(
        [](CreateUser in, RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            co_await res->status(201).send(in.name);
        }));
```

提取器：`Path<T>`、`Query<T>`、`Json<T>`、`Form<T>`、`State<T>`、`Header<T, "name">`、`Cookie<T, "name">`；同一次 `extract<...>`
最多一个 body 提取器（`Json`/`Form`）。失败自动 400/422/500，不会进入 handler。

校验通过 `Validator<T>` 特化 opt-in：

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

失败返回 422，字段错误数组放在 JSON `details` 中。

### Session 与 CSRF

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
            s->regenerate_id();     // 防会话固定
            s->set("uid", "42");
        }
        co_await res->status(204).send("");
    });
```

Cookie 只存不透明 id，数据在 `SessionStore` 里；接口是 awaitable 的，Redis/数据库
适配器由使用者在消费侧实现。默认 `MemorySessionStore` 是进程内、带锁实现。CSRF 使用
double-submit：安全请求建立 cookie，非安全请求必须在 header 中回显。无法设置 header 的
HTML 表单需要在 handler 里自行校验隐藏字段或写适配器。

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
    const Principal *p = simple_http::principal(*req);
    co_await res->status(200).send(p ? p->subject : "");
});
```

验证器强制算法 allowlist（拒绝 `none`），用 OpenSSL 校验 HS256/RS256/ES256 签名，
并校验 `exp`（默认必填）、`nbf`、`iss`、`aud`，成功后发布 `Principal`（subject /
scopes / roles）。JWKS 支持静态文档与手动拉取（RSA / EC P-256 / oct key，按 `kid` 选）：

```cpp
auto jwks_body = co_await fetch_jwks_json(client, jwks_url);
server.use(middleware::jwt(JwtConfig{
    .algorithms = {"RS256", "ES256"},
    .jwks_json = jwks_body ? *jwks_body : std::string{},
    .issuer = "https://issuer.example",
    .audience = "my-api",
}));
```

文档在 middleware 构造时解析一次；自动后台刷新不在这一层。令牌签发与 OAuth2/OIDC
发现也未包含。

### Host 路由与通配路径

```cpp
server.host("api.example.com", [](Router &api) {
    api.route({Method::Get}, "/users", users_handler);
});
server.host("*.example.com", [](Router &sub) {
    sub.route({Method::Get}, "/", subdomain_handler);
});

server.route({Method::Get}, "/files/{*rest}",
    [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
        co_await res->status(200).send(std::string{req->param("rest").value_or("")});
    });
```

精确 Host 优先于通配后缀，最长后缀优先；`*.example.com` 不匹配 `example.com` 或
`evil-example.com`。不带端口的 pattern 忽略请求端口，带 `:port` 时也比较端口。
`{*rest}` 只能出现在最后一段，捕获剩余路径（含 `/`）。Host 路由目前只作用于 HTTP
dispatch，WebSocket 查找仍是 host-agnostic。

<a id="build"></a>
## 构建、依赖与编译宏 · Build

### 依赖

| 依赖 | 用途 |
|---|---|
| Boost（asio、regex） | 异步 I/O 与正则 |
| OpenSSL | TLS / mTLS / QUIC TLS |
| glaze | JSON 反射与 OpenAPI schema（主库常规依赖） |
| zlib、brotli | 响应压缩与 WebSocket permessage-deflate（始终编入） |
| ngtcp2、nghttp3 | 仅 HTTP/3（QUIC/QPACK/帧层） |
| Catch2 | 仅单元测试 |

压缩与 WebSocket 扩展**没有开关宏**：zlib/brotli 是常规依赖，是否生效由运行时配置决定
（`middleware::compress`、`EngineLimits::ws_compression`、`ClientConfig::auto_decompress`）。

### xmake

仓库使用 xmake（`xmake.lua`）。包来自私有仓库 `fantasy-peak/xmake-repo`——上游
ngtcp2 包的 OpenSSL crypto helper 不可用，HTTP/3 需要该仓库里的构建。构建配置启用了
依赖锁（`package.requires_lock`）。

```sh
xmake build -j12              # 构建；同时刷新 build/compile_commands.json
xmake run server              # 示例服务器
xmake run unittest            # 单元测试
xmake run client              # 客户端整合自检
xmake run regression          # 服务端对抗性回归
xmake run loadgen -- ...      # 压测工具
```

`simple_http` 目标本身是 `set_kind("static")` 的空壳，只把 `include/` 与依赖以 `public`
方式导出；真正的内容全部在头文件里。

### CMake

`CMakeLists.txt` 安装一个 INTERFACE 目标，供下游 `find_package`：

```cmake
find_package(simple_http CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE simple_http::simple_http)
```

CMake 接口声明 `cxx_std_23`，并携带 Boost、OpenSSL、glaze、zlib、brotli 依赖。
注意以下三点：

- 仓库内 `xmake.lua` 当前设置的是 `c++26`，CMake 侧要求的是 C++23；两者都能构建，
  但下游若用 CMake，请按 C++23 或更高标准编译。
- CMake 的 INTERFACE 目标**没有**链接 ngtcp2/nghttp3，因此 CMake 下游不会启用
  HTTP/3；需要 HTTP/3 时请使用本仓库的 xmake 构建，或自行把这两个依赖接上。
- 导出的 `simple_httpTargets.cmake` 链接了 `glaze::glaze`，但
  `cmake/simple_httpConfig.cmake.in` 没有 `find_dependency(glaze)`；下游可能需要
  自己 `find_package(glaze CONFIG REQUIRED)`。

### 编译宏

| 宏 | 默认 | 说明 |
|---|---|---|
| `SIMPLE_HTTP_ENABLE_HTTP3` | 未定义 | 编入 HTTP/3 引擎与 QUIC 监听；未定义时 `ServerConfig::quic` 等字段不存在 |
| `SIMPLE_HTTP_USE_BOOST_REGEX` | 本仓库定义 | 路由正则使用 Boost.Regex，否则使用 `std::regex` |
| `SIMPLE_HTTP_ENABLE_LOG` | `1` | 日志总开关；设 `0` 时日志宏展开为 `((void)0)` |
| `SIMPLE_HTTP_LOG_ACTIVE_LEVEL` | `0`（Trace） | 低于该级别的日志在编译期丢弃 |

UNIX domain socket、WebSocket、OpenAPI 都没有开关宏：
UNIX socket 可用性由 `BOOST_ASIO_HAS_LOCAL_SOCKETS` 自动决定；OpenAPI 常编，只有写了
`openapi::doc()` 的路由才进文档。

<a id="server-guide"></a>
## 服务端指南 · Server Guide

### ServerConfig 与监听器

单监听器的便捷字段：

```cpp
ServerConfig cfg;
cfg.listen = InetAddress{"0.0.0.0", 7788, false}; // 或 UnixAddress{"/run/app.sock"}
cfg.worker_threads = 4;
cfg.max_connections = 0;      // 0 = 不限；到上限时用内核 backlog 背压
cfg.reuse_port = true;        // SO_REUSEPORT：每个 worker 一个 acceptor
cfg.tcp_nodelay = true;
cfg.plaintext_protocols = PlaintextProtocols::Both; // Http1 / Http2 / Both（嗅探）
cfg.shutdown_grace = std::chrono::milliseconds{5000};
cfg.limits = EngineLimits{};  // 协议引擎调参，见下

cfg.socket_setup   = [](asio::ip::tcp::socket &s) { /* 每连接 */ };
cfg.listener_setup = [](asio::ip::tcp::acceptor &a) { /* 每监听 socket，bind 后 listen 前 */ };
```

多监听器可以直接写 `cfg.listeners`，或用 builder：

```cpp
server.listen(InetAddress{"0.0.0.0", 80})
      .name("http")
      .plaintext(PlaintextHttp::Http1WithH2cUpgrade);

server.listen(InetAddress{"0.0.0.0", 443})
      .tls(TlsConfig{.cert_chain_file = "cert.pem", .private_key_file = "key.pem"})
      .alpn(TlsHttp::Http1AndHttp2);
```

`start()` 返回 `std::expected<StartReport, StartupError>`；`run()` 是同一操作的
`asio::awaitable` 版本：

- `StartPolicy::AllOrNothing`（默认）：任一监听失败则整体回滚。
- `StartPolicy::BestEffort`：保留已绑定的监听器，失败项写入 `report.failures`。

`run()` 只是 `start()` 的协程包装，不会自己驱动事件循环；工作线程由内部 I/O 池运行。

### TLS / mTLS

```cpp
cfg.tls = TlsConfig{
    .cert_chain_file = "server_cert.pem",
    .private_key_file = "server_key.pem",
    .mutual = true,
    .ca_file = "ca_cert.pem",
    .min_version = TlsMinVersion::Tls13, // 默认；Tls12 可同时接受 TLS 1.2
};
```

- 服务端默认只接受 TLS 1.3；需要兼容旧客户端时显式设 `TlsMinVersion::Tls12`。
- TLS 监听默认 ALPN 为 `h2` + `http/1.1`。
- 需要客户端证书的 handler 可以接收第三个参数 `SslHandle`：

```cpp
server.route({Method::Get}, "/whoami",
    [](RequestPtr, ResponsePtr res, SslHandle ssl) -> asio::awaitable<void> {
        if (!ssl) {
            co_await res->status(400).send("plaintext");
            co_return;
        }
        X509 *cert = SSL_get1_peer_certificate(*ssl);
        if (cert)
            X509_free(cert);
        co_await res->status(200).send("ok");
    });
```

### Handler 形态

Handler 必须是返回 `asio::awaitable<void>` 的协程，按实参个数自动选择：

```cpp
using RequestPtr  = std::shared_ptr<Request>;
using ResponsePtr = std::shared_ptr<ResponseWriter>;

// 普通
awaitable<void>(RequestPtr, ResponsePtr);
// 带 TLS 句柄
awaitable<void>(RequestPtr, ResponsePtr, SslHandle);
// WebSocket
awaitable<void>(RequestPtr, shared_ptr<WebSocket>);
```

Handler 抛出异常时引擎会返回 500；若响应头已经发出，则关闭连接而不是写第二个状态行。
`middleware::recovery()` 可以把它变成链上的统一处理。

### 中间件

中间件形状对齐 Go `net/http` / axum `from_fn`：

```cpp
server.use([](RequestPtr req, ResponsePtr res, SslHandle ssl,
              Next next) -> asio::awaitable<void> {
    auto t0 = std::chrono::steady_clock::now();          // before
    co_await next(std::move(req), std::move(res), ssl);  // 继续链路
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    SIMPLE_HTTP_INFO_LOG("status={} elapsed={}ms", res->status(), ms); // after
});
```

- 不调用 `next()` 即短路。
- `use()` 按注册顺序嵌套，最外层先跑；CORS 始终位于最外层。
- 组中间件只作用于 `group()` 内注册的路由；per-route 中间件只包这一条路由。
- 每请求类型化状态：`req->set_state<T>(...)` / `req->get_state<T>()`，按类型键，
  last-set-wins。

内置中间件（除特别注明外，均在 `simple_http::middleware`）：

| 工厂 | 作用 |
|---|---|
| `request_id()` | 复用或生成 `X-Request-Id`，写回响应并放进 `RequestId` state |
| `access_log()` | 链结束后记录方法、路径、状态、耗时、peer、request-id 等 |
| `recovery()` | 捕获下游异常；未发头回 500，已发头关闭连接 |
| `basic_auth(user, pass)` | HTTP Basic；失败 401 + `WWW-Authenticate` |
| `real_ip(trusted)` | 仅在直连 peer 可信时解析 `X-Real-IP` / `X-Forwarded-For`，放进 `ClientIp` state |
| `clean_path()` | 百分号解码并规范化 `//`、`/./`；拒绝 `..` / 畸形路径（400） |
| `strip_prefix("/api")` | 去掉路径前缀，保留 query |
| `redirect_slashes()` | 去掉多余尾斜杠并 301 |
| `ws_origin(allowed, allow_missing=false)` | 校验 WebSocket 的 `Origin`（CSWSH 防护） |
| `timeout(d)` | 流式友好超时：响应头前绝对时限，响应头后空闲时限；到期 504 或中止 |
| `compress(cfg)` | 挂载即启用响应压缩；按 Accept-Encoding / 类型 / 大小判定 |
| `secure_headers(cfg)` | CSP / X-Frame-Options / nosniff / Referrer-Policy / HSTS 等 |
| `rate_limit(bucket)` | 共享令牌桶；超限 429 + `Retry-After` |
| `rate_limit(limiter, key_fn)` | 按 key 分桶；带 key 上限与 LRU 淘汰 |
| `simple_http::make_cors_middleware(cfg)` | 把 CORS 策略作为普通中间件挂到组/单路由（顶层命名空间） |

`server.cors(CorsConfig{...})` 是 CORS 的专用入口，始终把策略放在最外层。

### 路由匹配顺序

一次请求按以下顺序匹配（与 nginx 的大方向一致）：

1. 精确路径（方法感知）
2. `{param}` 模板 trie
3. HTTP 反向代理路由
4. 本地正则路由
5. 静态文件阶段
6. fallback
7. 内建 404

细节：

- `any_methods` 是空方法列表，表示任意方法（包括扩展方法 token）。
- 同一 `(path, method)` 重复注册时，先注册者生效。
- `route_regex` 使用整段路径匹配；组内注册时前缀会被转义。
- 405 的 `Allow` 会合并所有匹配该路径的方法，`OPTIONS` 总是包含在内。
- WebSocket 查找顺序是：本地精确 ws 路由 → ws 反代 → 本地正则 ws 路由。
- CORS 不覆盖 WebSocket 升级；ws 路由的 Origin 校验请用 `ws_origin()`。

### Request

```cpp
req->method();            // Method
req->method_token();      // 原始方法 token（支持扩展方法）
req->version();           // Version::Http1 / Http11 / Http2 / Http3
req->target();            // 请求目标
req->path();              // target 的 path 部分（原始字节；规范化请用 clean_path()）
req->query();             // 原始 query
req->query_params();      // QueryParams
req->param("id");         // 模板路由捕获
req->header("host");      // optional<string_view>
req->headers();           // Headers（支持 get_all）
req->cookie("session");   // optional<string_view>
req->peer_address();      // 对端地址
req->body();              // Body 流
req->set_state<T>(...);   // 每请求类型化 state
req->get_state<T>();
req->cancel();            // 协作式取消
req->cancelled();
req->wait_cancelled();    // 在等待者自己的 executor 上恢复
```

读请求体：

```cpp
// 一次性读完
auto body = co_await req->body().read_all(); // expected<string, error_code>

// 逐块读取
for (;;) {
    auto frame = co_await req->body().read();
    if (!frame) break;
    if (frame->eof) break;
    use(frame->data);
}
```

`max_body_bytes` 超限时引擎回 413；`max_header_bytes` 超限回 431。

### ResponseWriter

`ResponsePtr` 是 `std::shared_ptr<ResponseWriter>`。响应有两种形状：

```cpp
// 一次性
co_await res->status(200).content_type("text/plain").send("hello");

// 流式（HTTP/1.1 为 chunked，HTTP/2 为 DATA）
co_await res->status(200).begin();
co_await res->write("first ");
co_await res->finish("last");
```

其他常用能力：

```cpp
res->header("x-trace", "abc");          // 追加
res->replace_header("x-frame-options", "SAMEORIGIN"); // 同名替换

co_await res->redirect("https://example.com/new", status::temporary_redirect).send("");

res->set_cookie("session", "abc", std::chrono::seconds{3600},
                "/", "", true, true, "Lax");

// 响应尾部字段（RFC 9110 §6.5）：必须用在流式响应上
co_await res->status(200).begin();
co_await res->write("body");
res->trailer("x-checksum", "abc123");
co_await res->finish_trailers();

// SSE
co_await res->sse_begin();
co_await res->sse_event(R"({"ok":true})", "message", "42");
co_await res->sse_comment("keep-alive");
```

`send()` 会自动补 `Date`、`Server`、`Content-Type`（204/304 不补 Content-Type）。
Handler 自己设置的 `connection` / `transfer-encoding` 等 hop-by-hop 字段会被引擎剥离，
framing 只由引擎决定。

### 取消与超时

请求级取消是**协作式**的：

- `req->cancel()` / `req->cancelled()` / `req->cancel_token()` 线程安全。
- `req->wait_cancelled()` 在等待者自己的 executor 上恢复。
- HTTP/2、HTTP/3 的 RST_STREAM、连接关闭会触发取消；HTTP/1.1 只能在下次读写时观察到断开。
- 不观察取消的 handler 不会被强行停止。

`middleware::timeout(d)` 建在同一套信号上，且对流式响应友好：

- 响应头发出前：绝对时限，`d` 内必须开始响应，否则 504。
- 响应头发出后：空闲时限，每次写都会重置；持续输出的 SSE / 下载 / 流式反代不会被误杀。
- WebSocket 握手直接放行。

### 静态文件、文件响应与上传

```cpp
StaticFilesConfig cfg;
cfg.table.root = "./web/dist";
cfg.table.immutable_prefixes = {"/assets/"}; // 带内容哈希的构建产物
cfg.spa_fallback = "index.html";             // 客户端路由；"" = 关闭

auto site = std::make_shared<StaticFiles>(std::move(cfg));
std::string err;
if (!site->load(err)) {
    std::println(stderr, "static root error: {}", err);
    return;
}
server.static_files(std::move(site));
```

静态服务支持 `ETag`、`Last-Modified`、`If-None-Match` / `If-Modified-Since` → 304、
`Range` → 206 / 416、预压缩 `.br` / `.gz` 兄弟文件、`Vary: Accept-Encoding`、
目录索引、SPA fallback 和路径逃逸防护。SPA fallback 默认只对最后一段不含 `.` 的路径
生效，所以 `/user/123` 会回 `index.html`，而缺失的 `/app.js` 仍然是 404。

发送单个文件（64 KiB 分块，不把整文件读入内存）：

```cpp
(void)co_await send_file(*res, "./data/report.pdf");
```

上传落盘：

```cpp
auto n = co_await read_body_to_file(req->body(), "./upload.bin", 64ull * 1024 * 1024);
if (!n) {
    co_await write_error(res, status::bad_request,
                         {.code = "upload_failed", .message = n.error().message()});
    co_return;
}
co_await res->status(201).send(std::to_string(*n));
```

### 表单、JSON、参数与 multipart

```cpp
// URL-encoded body
auto form = co_await read_urlencoded_body(*req); // expected<QueryParams, error_code>

// multipart/form-data（整块）
auto parts = co_await read_multipart_body(*req); // expected<vector<MultipartPart>, error_code>

// 流式 multipart：文件 part 直接写盘，不把整个 body 放进内存
MultipartStreamCallbacks cb;
cb.on_part_begin = [&](const MultipartPart &p) { /* 打开文件 */ return true; };
cb.on_part_data  = [&](std::string_view d) { /* 写入 */ return true; };
cb.on_part_end   = [&] { /* 关闭文件 */ };
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

// 类型化参数（字段名 == 路由模板名 / query key）
struct UserParams { std::int64_t id{}; };
auto p = path_params<UserParams>(*req);   // optional<UserParams>

struct ListQuery { std::optional<std::string> q; int page{1}; };
auto q = query_params<ListQuery>(*req);   // optional<ListQuery>
```

`parse_params<T>(q)` 可把任意 `QueryParams q`（包括解析后的 urlencoded body）按字段名
解析进结构体。

统一错误响应：

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

### CORS、安全头、限流与压缩

```cpp
// CORS：预检 204，且不会进入路由
server.cors(CorsConfig{
    .allow_origins = {"https://app.example.com", "https://*.example.com"},
    .allow_credentials = true,
    .allow_methods = {"GET", "POST", "DELETE"},
    .allow_headers = {"content-type", "authorization"},
    .expose_headers = {"x-request-id"},
    .max_age = std::chrono::seconds{600},
});

// 安全响应头
server.use(middleware::secure_headers(middleware::SecureHeadersConfig{
    .content_security_policy = "default-src 'self'",
    .hsts_max_age = std::chrono::seconds{31536000},
}));

// 共享令牌桶：100 req/s，burst 200
server.use(middleware::rate_limit(middleware::make_token_bucket(100, 200)));

// 按 key 分桶：10 req/s，burst 20，最多 10000 个 key
auto limiter = std::make_shared<middleware::RateLimiter>(10, 20, 10000);
server.use(middleware::rate_limit(limiter, [](const RequestPtr &req) {
    return std::string{req->peer_address()};
}));

// 响应压缩：挂载即启用；组/单路由作用域同理
server.use(middleware::compress(CompressionConfig{
    .min_bytes = 1024,
    .gzip_level = 6,
    .brotli_quality = 5,
    .compress_streamed = true,
}));
```

CORS 的 `allow_origins` 支持精确 origin 与子域通配 `https://*.example.com`；
`allow_credentials=true` 时不会回 `*`，而是回显实际 Origin。

### 反向代理

```cpp
// 请求级 HTTP 反代：每请求一条上游连接，响应流式回传
server.http_proxy("/api", "backend.internal", 8080);

server.http_proxy("/v2", HttpProxyTarget{
    .host = "10.0.0.5",
    .port = 8443,
    .tls = true,                 // TLS 上游；ALPN 可自动选 h2
    .h2c = false,                // 明文上游可走 Upgrade
    .h2c_prior_knowledge = false,
    .preserve_host = false,
    .max_attempts = 3,
});

// 正则路由 + 捕获组重写：/v1/<rest> -> 上游 /$1
server.http_proxy_regex("^/v1/(.*)$", "10.0.0.5", 9000, "/$1");

// WebSocket 字节级隧道（校验 101 后原样拼接）
server.ws_proxy("/ws", "backend.internal", 9000);
```

反代行为：

- 过滤 hop-by-hop 字段，自证 `X-Forwarded-For` / `-Proto` / `-Host`。
- 幂等请求可重试（`max_attempts`）；请求取消会中止上游交换。
- `CONNECT` 请求回 405。
- 上游走 client 层，TLS 上游可通过 ALPN 使用 HTTP/2；明文上游可选 h2c Upgrade 或
  prior-knowledge。
- 默认不读取环境代理；需要时设 `HttpProxyTarget::use_env_proxy = true`，或在路由的
  `ClientConfig` 里显式配置。
- WS 反代只支持 HTTP/1.1 明文上游；HTTP/1.1 Upgrade 与 HTTP/2 extended CONNECT
  共用同一套上游握手。

### 日志

库不直接写流，只把记录交给 `LogSink`：

```cpp
#include <simple_http.h>

set_log_sink(make_stdout_sink(LogLevel::Info)); // 文本
// set_log_sink(make_json_sink(LogLevel::Info)); // JSON lines（stderr）

SIMPLE_HTTP_INFO_LOG("backend {} connected", peer);
```

`LogSink` 的接口只有 `enabled(level)` 与 `log(record)`；记录里同时带人类可读 `message`
和结构化 `attrs`。安装是原子的，运行中替换安全；`SIMPLE_HTTP_ENABLE_LOG=0` 可整体编出。

### EngineLimits

```cpp
EngineLimits limits;
limits.idle_timeout = std::chrono::seconds{120};
limits.max_header_bytes = 64 * 1024;   // 超出 431
limits.max_body_bytes = 64ull * 1024 * 1024; // 超出 413
limits.ws_read_ahead_bytes = 4 * 1024 * 1024;
limits.ws_compression = false;
limits.h2_max_concurrent_streams = 200;
limits.h2_max_frame_size = 16384;
limits.h2_initial_window = 65535;
limits.h2_enable_connect_protocol = true; // RFC 8441
limits.h2_max_header_list_size = 0;       // 0 = 跟随 max_header_bytes
limits.h2_max_queued_streams = 0;         // 0 = 跟随并发上限
limits.h3_max_concurrent_streams = 0;     // 0 = 跟随 h2 值
```

HTTP/2 Rapid Reset 防护：同时在跑的 handler 数量被限制为
`h2_max_concurrent_streams`，超出的流进入 `h2_max_queued_streams` 队列，队列溢出则
`GOAWAY(ENHANCE_YOUR_CALM)`。

<a id="client-guide"></a>
## 客户端指南 · Client Guide

### 创建 Client

```cpp
asio::io_context io;
auto guard = asio::make_work_guard(io);
std::thread t{[&] { io.run(); }};

Client http{io.get_executor(), ClientConfig{}};
```

`Client` 强制绑定单线程 executor（模型 A）：构造后所有操作都在这一个线程上运行，
引擎内无锁。要求 `io_context` 活得比 Client 及其会话更久。

配置在构造时复制进 Client，之后修改 `ClientConfig` 不会影响已有实例。TLS 配置非法
（例如只给客户端证书、没给私钥）时构造会抛出 `std::runtime_error`。

### 便捷 API

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

方法：`get` / `post` / `put` / `del` / `head` / `patch` / `request`。
`RequestBuilder` 支持 `query`、`header(s)`、`content_type`、`body`、`json<T>`、
`multipart(MultipartForm)`、`basic_auth`、`bearer_auth`、`max_body_bytes`、`timeout`、
`version`。`send()` 返回缓冲响应；`send_stream()` 返回显式 `Stream`。

也可以直接构造两端共用的 `Request` 再 `client.send(req)`（Go `http.Client.Do` 形态）。

### 流式 multipart 与文件上传

`MultipartForm` 是整块缓冲路径。大文件用 `MultipartStreamWriter` 逐 part 写出：

```cpp
auto up = co_await MultipartStreamWriter::open(
    client, "https://host/upload", {.boundary = "B"});
co_await up->field("title", "hello");
co_await up->file("upload", "notes.txt", "./notes.txt"); // 从磁盘分块读取
co_await up->finish();
```

裸文件体上传用 `stream_file_body`：

```cpp
auto up = co_await client.open_stream("https://host/upload",
                                      {.method = Method::Put});
co_await stream_file_body(*up, "./blob.bin");
co_await up->finish();
```

两者都按 64 KiB 分块，走普通流式请求体（HTTP/1.1 chunked，HTTP/2/3 DATA）。

### Expect: 100-continue 与请求 trailers

```cpp
auto up = co_await client.open_stream("https://host/upload",
    {.method = Method::Post, .auto_expect_continue = true,
     .continue_timeout = std::chrono::seconds{1}});
if (co_await up->write(body) == client_errc::expectation_failed) {
    auto head = co_await up->read_head(); // 对端拒绝了 body，最终响应可读
}
```

请求 trailers 用 `finish_trailers`；需要先在请求头写 `Trailer`：

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

服务端在 body 结束后通过 `req->trailers()` 读取（HTTP/1.1 与 HTTP/2）。

### 显式 Stream

```cpp
auto up = co_await http.open_stream("https://host/upload",
                                    {.method = Method::Post,
                                     .response_head_timeout = std::chrono::seconds{5},
                                     .body_idle_timeout = std::chrono::seconds{30}});

co_await up->write(chunk);       // HTTP/1.1：chunked；HTTP/2：DATA
co_await up->finish();           // 结束请求体

auto head = co_await up->read_head(); // TTFB 预算在这里
while (auto c = co_await up->read()) { // 每次读受 body-idle 预算约束
    if (c->eof) break;
    consume(c->data);
}
const Headers &trailers = up->trailers();
```

HTTP/1.1 是半双工：`finish()` 之后才能开始读响应；在请求体结束前读响应会立即得到
`client_errc::half_duplex`，而不是挂起。HTTP/2 的发送与接收队列独立，可以交织。

注意 `open_stream()` / `send_stream()` 是最底层的显式流：**不会**自动跟随重定向、
不会自动应用 CookieJar、也不会跑便捷层的重试策略。需要这些行为时用 `send()`。

`Stream::read()` 每次受 `body_idle_timeout` 约束；`Stream::read_all()` 不套这个
deadline，只受调用方传入的 `max_bytes` 限制。

### 配置

```cpp
ClientConfig cfg;
cfg.connect_timeout = std::chrono::seconds{10};        // TCP / 代理隧道 / TLS 握手各自预算
cfg.request_timeout = std::chrono::seconds{60};        // 便捷 send 的总预算
cfg.response_head_timeout = std::chrono::seconds{30};  // TTFB
cfg.body_idle_timeout = std::chrono::seconds{0};       // 0 = 体可无限空闲
cfg.max_redirects = 5;                                 // 0 = 不跟随
cfg.cookie_jar = std::make_shared<CookieJar>();
cfg.auto_decompress = true;                            // 自动解压并发送 Accept-Encoding
cfg.default_version = HttpVersionPolicy::Auto;         // Auto / Http11 / Http2 / Http3
cfg.default_h2c = H2cMode::Upgrade;                    // Upgrade / PriorKnowledge / Off
cfg.retry.max_retries = 2;
cfg.retry.initial_backoff = std::chrono::milliseconds{100};
cfg.proxy = ProxyConfig{.kind = ProxyKind::Socks5, .host = "127.0.0.1", .port = 1080};
```

TLS 客户端配置：

```cpp
cfg.tls = TlsClientConfig{
    .verify_peer = true,
    .verify_host = true,
    .ca_file = "ca.pem",
    .cert_chain_file = "client_cert.pem", // mTLS 时与私钥一起设置
    .private_key_file = "client_key.pem",
    .min_version = TlsMinVersion::Tls12,  // 客户端默认 1.2；服务端默认 1.3
};
```

### 重定向、Cookie 与认证

- 301/302/303 把 POST/PUT/DELETE 改成无 body 的 GET；307/308 保留方法和 body。
- 拒绝 `https -> http` 降级（`client_errc::redirect_to_insecure`）。
- `max_redirects` 默认 0（不跟随）。
- `CookieJar` 按 Domain / Path / Secure / Expires 过滤，重定向链共享。
- `basic_auth` / `bearer_auth` 是 `RequestBuilder` 的便捷方法。

### 重试

- **免费透明重拨**：复用的 keep-alive 连接被证明已失效时，自动换新连接重发。打开交换
  与读响应两个阶段各有一次免费重放，不计入 `max_retries`。
- **策略重试**：`ClientConfig::retry` 配置最大次数、指数退避与条件回调；默认重试
  `stream_refused`、幂等方法的传输失败、以及请求尚未发出时的拨号失败。
- **状态码重试**：`retry.retry_statuses` 默认空（与 Go/reqwest 一致，需显式开启）；
  设为 `{429, 503}` 后，在 `max_retries > 0` 时对可重放、幂等的请求生效。`Retry-After`（秒或 IMF-fixdate）会替代该次退避；重试前
  会先读完响应体。`retry_status_on_non_idempotent` 可让 POST 类方法也参与。
- 重试总是重新拨号；同一次交换的 open 与读响应共享一个 retry budget，并受
  `request_timeout` 约束（重定向的每一跳各自获得完整的 `request_timeout`）。
- 流式 body 已被消费、无法重放的请求默认不重试（`require_replayable`）。

### 出站代理

```cpp
cfg.proxy = ProxyConfig{
    .kind = ProxyKind::Https,       // Http / Https / Socks5
    .host = "proxy.internal",
    .port = 3128,
    .username = "user",
    .password = "pass",
};
cfg.proxy_from_env = true;          // 未显式设置时读 HTTPS_PROXY / HTTP_PROXY / ALL_PROXY
cfg.no_proxy = {"localhost", ".internal", "127.0.0.1:8080"};
```

- HTTP 代理使用 CONNECT；HTTPS 代理是 TLS over TLS，使用独立的代理 ssl::context
  （不会向代理出示客户端证书）。
- SOCKS5 支持 RFC 1929 用户名/密码认证；目标域名交给代理远程解析。
- `NO_PROXY` 支持 `*`、精确 host、域名后缀、`host:port`、IP 字面量；不支持 CIDR/通配。
- HTTP/3 恒直连（QUIC 需要 CONNECT-UDP/MASQUE）。

### HTTP/3

```cpp
ClientConfig cfg;
cfg.default_version = HttpVersionPolicy::Http3; // URL 必须是 https://
Client h3{io.get_executor(), cfg};
```

- 只做显式启用：不读取 Alt-Svc / HTTPS RR、不做 0-RTT、不做 h3 WebSocket。
- 同一 origin 保持一条 QUIC 连接，多路复用请求流。
- 未定义 `SIMPLE_HTTP_ENABLE_HTTP3` 时，该策略在拨号处返回
  `client_errc::version_not_negotiated`。
- `quic_max_udp_payload_size` 默认 1200（QUIC 最小 MTU）；干净以太网可调到 1472，
  VPN/隧道/WSL2 上保持 1200。

### WebSocket 与 SSE 客户端

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

客户端 WebSocket 走 HTTP/1.1 Upgrade（校验 101 与 `Sec-WebSocket-Accept`），复用服务端
`WsBackendImpl`；读泵、写泵、有界入站队列与服务端完全一致。

```cpp
auto sse = co_await SseStream::open(http, "https://host/events");
if (!sse)
    co_return;
while (auto ev = co_await sse->next()) {
    handle(ev->event, ev->data, ev->id);
}
// 重连时把 sse->last_event_id() 放进 Last-Event-ID
```

`SseStream::open` 要求响应状态为 `200`，且默认要求 `Content-Type` 含
`text/event-stream`（`SseOptions::require_event_stream`）；断线重连策略由调用方决定。

<a id="architecture"></a>
## 架构与并发模型 · Architecture

### 一次请求的路径

```
socket 字节
  └─ Transport（TCP / TLS / QUIC 流）
      └─ 版本引擎（h1 / h2 / h3）
          ├─ 解析 head / body framing
          ├─ 构造 Request，写入体流
          ├─ Dispatcher（Router）
          │    ├─ 全局中间件 → 组中间件 → per-route 中间件
          │    ├─ 路由匹配 / 静态文件 / 反代
          │    └─ Handler（协程）
          └─ ResponseWriter → 版本化 ResponseSink → Transport
```

- `ResponseWriter` 是版本无关的门面；每个引擎提供自己的 `ResponseSink` 实现。
- 请求体在 HTTP/1.x 是拉模式（handler 读时从 socket 取），HTTP/2 是推模式
  （读循环把 DATA 放进每流队列，handler 从队列消费）。两者的 `Body` 接口一致。
- HTTP/3 的帧层、QPACK、SETTINGS、丢包恢复由 ngtcp2/nghttp3 负责；本库只保留
  它们与 `Dispatcher` 之间的适配层。

### 并发模型 A

- 每条连接在 accept 时绑定到 `IoCtxPool` 中一个单线程 `io_context`。
- 该连接上的所有引擎/writer/handler 工作都只在这个线程上执行，因此无需加锁。
- `ResponseWriter` / `WebSocket` 可以 `shared_ptr` 捕获并跨线程使用；每个写操作先
  `co_await asio::dispatch(asio::bind_executor(executor, ...))` hop 回连接线程。
- `engine/` 下没有 mutex / atomic。
- 刻意的例外都在 handler 层：`RateLimiter` 的按 key 分桶加锁、Router 惰性构建
  中间件链、日志 sink 的原子替换。
- HTTP/3 的连接 executor 是 endpoint 的 executor；`reuse_port` 默认开启，否则一个
  QUIC 监听的所有连接会挤在一条线程上。

### 反压

- HTTP/2 请求体按流停放：某条流消费不过来时只暂停该流，不阻塞整条连接。
- WebSocket 有界入站读泵：应用在写被对端零窗口挡住时，socket 仍会被读取进有界队列，
  队列满则停止读取；`read()` 腾出空间后继续。
- 流式响应走写泵，写操作本身提供反压。
- `max_body_bytes` / `max_header_bytes` / `h2_max_header_list_size` 都设上限。

### 优雅关闭

- `Server::stop()` 关闭监听器，等待已进入 handler 的请求（最多 `shutdown_grace`），
  然后关闭剩余连接并停止 I/O 池。
- 未关闭的连接、空闲 keep-alive、长连接 WebSocket 都受同一份 grace 约束。
- 信号处理函数里不要直接调用 `stop()`；设一个 `sig_atomic_t` 标志，由主循环调用。

### 日志与可观测性

- `LogSink` 是唯一日志接口；`make_stdout_sink` / `make_stderr_sink` / `make_json_sink`
  是内置实现。
- `access_log()` 输出方法、路径、状态、耗时、peer、request-id 等字段。
- `Server::active_connections()` / `active_requests()` / `port()` / `quic_port()`
  可运行时观测。

<a id="testing"></a>
## 测试与验证 · Testing

项目同时使用本库自测和独立实现交叉验证。后者很重要：如果客户端和服务端共享同一个
规范误读，两者互测会互相抵消。

### 本地测试目标

```sh
xmake build unittest && xmake run unittest      # Catch2：389 个用例，65298 条断言
xmake build regression && xmake run regression  # 裸 socket 畸形/边界请求
xmake build client && xmake run client          # 客户端整合自检：245 checks
xmake build server && xmake run python-tests    # Python 第三方客户端：113 checks
```

单元测试涵盖 core、proto、h1 解析器、h2 帧 / HPACK / Huffman、WebSocket 帧与消息层、
路由、Host/通配路由、App State、提取器、校验、Session、CSRF、JWT、CORS、限流、
静态文件、OpenAPI、客户端 URL / 配置 / TLS 参数 / 代理等。
按标签过滤：`xmake run unittest "[h2]"`。

### 交叉验证

| 方向 | 驱动 | 内容 |
|---|---|---|
| 服务端 ← 独立客户端 | `test/python/run.py`（httpx / hyper-h2 / websockets / aioquic） | 113 checks：h1 / h2 / h3 / WS / trailer / CORS / 回归 |
| 服务端 ← 独立客户端 | `test/python/rfc8441_client.py` | 85 checks：HTTP/2 extended CONNECT 与 h2 WebSocket 反代 |
| 客户端 → 独立服务端 | `test/python/run_client_cross.py` | 43 checks（stdlib http.server / websockets）+ 6 checks（aioquic h3） |
| 服务端 ← Autobahn | Docker `crossbario/autobahn-testsuite` | WebSocket 全量用例；要求 FAILED 0、NON-STRICT 0 |
| 服务端 ← h2spec | Go 版 h2spec | `:7790`，146/146 |
| 服务端 ← h1spec | Deno 版 h1spec | `:7791`，33/33 |
| 服务端 ← h3spec | h3spec | `udp :7792`，49 例中通过 ≥47（2 个已知 TLS 8.2 假阳性） |

Python 依赖在 `test/python/requirements.txt` 中固定版本；首次使用先建虚拟环境：

```sh
python3 -m venv test/python/.venv
test/python/.venv/bin/pip install -r test/python/requirements.txt
```

### 外部一致性套件

```sh
test/conformance/run.sh          # h2spec / h1spec / autobahn / h3spec 全部
test/conformance/run.sh h2spec   # 只跑一个
```

端口不能混用：`7790` 是纯 h2c，`7791` 是纯 h1，`7788` 是嗅探 + WebSocket，
`7792` 是 TCP + UDP 双传输。单协议端口是必需的：在嗅探端口上，“畸形 HTTP/2 前导”
和“畸形 HTTP/1.x 请求行”是同一批字节，而 h2spec 要 GOAWAY、h1spec 要 400。

h2spec / h1spec / h3spec / Autobahn 的工具不在仓库里；脚本会自检环境并打印准备命令
（h2spec 预编译二进制、Deno/Docker 版 h1spec、Autobahn Docker 镜像、h3spec 二进制）。

### 压力与连接生命周期

```sh
test/stress/run.sh [cpp|rust|both] [h1|h2|h3|ws]...
test/stress/run.sh both          # 默认：两个实现、四档全跑
test/stress/run.sh cpp h3        # 只压 C++ 的 h3
```

脚本会先强制清理遗留测试服务并确认 TCP/UDP 端口释放（否则 SO_REUSEPORT 会把 QUIC
连接表劈开），再按需跑一致性套件，最后压测。门线只有一条：**失败数必须为 0**；
吞吐只报告，不写死，因为依赖机器。

依赖 `h2load` 与 `k6`；h3 档还要求 `h2load` 本身带 ngtcp2/nghttp3 支持。`cpp` 目标
使用端口 `7788`–`7792`，`rust` 目标使用 `7888`–`7892`。

### 压测工具

```sh
xmake build loadgen
xmake run loadgen http://127.0.0.1:7791/world -p h1 -n 300000 -c 100
xmake run loadgen http://127.0.0.1:7790/world -p h2 -n 600000 -c 20 -m 40
xmake run loadgen ws://127.0.0.1:7788/echo1k -p ws -n 200000 -c 64
```

`-p` 可选 `h1` / `pub` / `h2` / `ws`；输出对齐 h2load 的
`finished in Xs, N req/s, M MB/s` 与延迟分位。

### 其他检查目标

- `xmake run readme_examples`：编译检查 README 中的代码样例。
- `xmake run openapi_demo` + `test/openapi_verify.py`：OpenAPI 端到端（demo 监听
  `:7795`，`/swagger` 浏览）。
- `xmake run middleware_server`：在 `:7796`（以及 TLS 1.2 的 `:7797`）挂载全部内置
  中间件，供与 Go 参考实现做差分。
- `xmake run client-cross-python`：客户端对独立 Python 服务端的交叉验证。
- `xmake run h3_public_check -- https://cloudflare-quic.com/`：手工公网 HTTP/3 冒烟。
- `xmake run thread_check`：客户端线程模型与销毁路径压测（配合 sanitizer 使用）。
- `test/framework_diff.sh` / `xmake run framework-diff`：同时启动 C++ 框架参考服务器与
  等价的 Go `net/http` 服务器，对比 64 项可观测行为——App State、提取器、校验、
  Session、CSRF、JWT（HS256/RS256/ES256）、Host 路由和通配路径。
- `test/client_diff.sh` / `xmake run client-diff`：C++ 客户端打 Go 参考服务端、Go 客户端
  打同一个 Go 服务端、Go 客户端再打 C++ 框架服务端；三份 PASS/FAIL 输出逐项 diff，
  覆盖流式 multipart/文件上传、自动 100-continue、请求 trailers、429/Retry-After、
  JWKS 拉取。

<a id="known-limitations"></a>
## 已知限制 · Known limitations

- 服务端 HTTP/2 WebSocket（RFC 8441）已实现；**客户端** `open_websocket` 仍只走
  HTTP/1.1 Upgrade，尚未支持 h2 / h3 WebSocket。
- 服务端 HTTP/3 不提供 WebSocket（RFC 9220 的 extended CONNECT 未实现）。
- nghttp3 v1.18 没有 1xx 提交 API，**服务端 HTTP/3** 的 `Expect: 100-continue`
  目前是 no-op。
- HTTP/3 客户端只支持显式指定，不做 Alt-Svc / HTTPS RR 自动发现，不支持 0-RTT。
- 出站代理不覆盖 HTTP/3（QUIC 需要 CONNECT-UDP/MASQUE）。
- `NO_PROXY` 不支持 CIDR 与通配符。
- 客户端采用“每个 origin 一条连接”的模型：对第一目标保持一条连接，其他 origin 使用
  一次性连接；没有传统连接池。
- 客户端 WebSocket 不协商 permessage-deflate（服务端已实现 RFC 7692）。
- 客户端在 HTTP/1.1 与 HTTP/2 暴露 interim `100-continue`；其他 1xx（如 103 Early
  Hints）仍会丢弃。
- CORS 中间件不处理 WebSocket 升级；请使用 `ws_origin()` 做 Origin 校验。
- Host 路由只作用于 HTTP dispatch；当前 WebSocket 引擎查找是 host-agnostic 的。
  Host 子 Router 有独立的 OpenAPI 文档。
- JWT 只是验证器：不做令牌签发、OAuth2/OIDC 发现或 refresh-token 流程。JWKS 支持
  静态/手动拉取文档，不内置后台自动刷新。`aud`/`scope` 按字符串 claim 解析，不支持
  audience 数组。
- 请求 trailers 支持 HTTP/1.1 与 HTTP/2；HTTP/3 返回
  `client_errc::trailers_not_supported`。
- 自动 `Expect: 100-continue` 在 HTTP/1.1 与 HTTP/2 等待 interim 响应；HTTP/3 直接
  发送 body（nghttp3 无 1xx 提交 API）。
- Session 只内置进程内存储；Redis/数据库适配器由消费侧实现 `SessionStore`。CSRF 是
  header 型 double-submit；无法设置 header 的表单需要在 handler 里校验字段。
- 校验通过 `Validator<T>` 按类型 opt-in，没有 tag/DSL 校验器。
- HTTP/2 一次性响应刻意不发 `Content-Length`（由 END_STREAM 界定，HEAD 除外）。
- CMake 的 INTERFACE 目标未接入 ngtcp2/nghttp3，因此 HTTP/3 目前只由 xmake 构建提供。
- 服务端默认 TLS 1.3-only；需要 TLS 1.2 时显式设置 `TlsConfig::min_version`。
- HTTP/3 构建依赖私有 xmake 仓库中的 ngtcp2（带 OpenSSL crypto helper）与 nghttp3。

<a id="contributing"></a>
## 代码风格与贡献

- `.clang-format`：LLVM 基线 + `IndentWidth: 4`、`ColumnLimit: 120`。
- 提交前运行 `./format.sh`；CI 可跑 `./format.sh --check`。
- 分层约束：上层（`proto` / `handler` / `net` / `client` 门面）不得出现协议版本分支，
  版本差异只放进 `engine/` 与对应的 `ResponseSink` 实现。
- RAII、无裸 `new` / `delete`，所有权显式。
- 改协议/传输层后跑 `test/conformance/run.sh`；改流的生命周期后额外跑
  `test/stress/run.sh`。
- 新的公开 API 或行为变化，请同步更新 `README.md`、`README.zh-CN.md` 与 `AGENTS.md`。

<a id="license"></a>
## 许可证

MIT License，见 [LICENSE](LICENSE)。
