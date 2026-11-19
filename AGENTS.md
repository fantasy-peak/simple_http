# AGENTS.md — simple_http

面向参与本工程的 AI / 协作者的工程说明。内容以**当前源码、构建脚本和测试**为准。
本工程当前的 `include/` 就是 **C++17 token-only 实现**：不要按旧的 C++23 协程主实现
来理解 API，也不要假设不存在的主分支结构。头文件注释与代码冲突时以代码为准，并顺手
修正注释。

---

## 1. 工程真实形态

| 项 | 当前值 |
|---|---|
| 版本 | `0.9.0`（`include/simple_http/core/version.h`） |
| 形态 | 纯头文件；`include/` 下没有 `.cpp` |
| 入口头 | `#include <simple_http.h>` |
| 最低标准 | C++17 |
| 异步模型 | Asio initiating function + 显式 completion token |
| 协程 | 仅在 `BOOST_ASIO_HAS_CO_AWAIT`（C++20+）下作为可选适配层 |
| JSON/typed params/OpenAPI/JWT | 由 `SIMPLE_HTTP_HAS_GLAZE` 门控，需 C++23 + glaze |
| 依赖 | Boost（Asio/System/Regex/Core）、OpenSSL、zlib、brotli；glaze 可选；HTTP/3 需 ngtcp2 + nghttp3 |
| 许可 | MIT |

### 1.1 异步 API 的核心约定

- 每个异步操作都是 Asio initiating function，**没有默认 completion token**：
  调用方必须显式传 callback、`asio::deferred`、`asio::detached`、
  `asio::as_tuple(asio::use_awaitable)` 等。
- 完成签名遵循 Asio 约定：`void(error_code)`、
  `void(error_code, T)`、`void(error_code, T1, T2)`。
- 虚拟边界（`ResponseSink`、`SessionStore`、`ResolveFn` 等）使用
  `asio::any_completion_handler` 做类型擦除，因此 associated executor /
  allocator / cancellation slot 不会在转发时丢失。
- 组合操作使用 `asio::async_compose` 状态机；不要引入自定义协程桥接。
- 库内部不以异常做控制流；同步可失败 API 返回 `error_code` 或
  `boost::system::result<T>`（别名 `simple_http::result<T>`）。

### 1.2 协议矩阵

| 能力 | 服务端 | 客户端 |
|---|---|---|
| HTTP/1.0 | 支持 | —（客户端只发 1.1） |
| HTTP/1.1 | 支持 | 支持 |
| HTTP/2 over TLS | 支持（ALPN） | 支持（ALPN） |
| h2c Upgrade | 支持 | **未接线** |
| h2c prior knowledge | 支持 | 支持（明文 `HttpVersionPolicy::Http2`） |
| HTTP/3 over QUIC | 可选（`SIMPLE_HTTP_ENABLE_HTTP3`） | 可选，且仅显式 `HttpVersionPolicy::Http3` |
| WebSocket over HTTP/1.1 | 支持 | 支持 |
| WebSocket over HTTP/2（RFC 8441） | 支持 | 未实现 |
| WebSocket over HTTP/3 | 未实现 | 未实现 |
| SSE | 响应 helper | `SseStream` |
| UNIX domain socket | **未实现**（配置类型保留） | — |

### 1.3 当前不存在的能力

- 没有名为 `HttpClient` 的类，没有 `client/http_client.h`。
- 没有 `Client::async_get/async_post/.../async_send_stream`；客户端入口是
  `client.get(url).send(token)` 或 `Client::async_send(...)`。
- 没有 C++23 协程主实现；协程只是可选 handler/middleware 适配。
- 没有服务端 UNIX-domain listener；`UnixAddress` 在 `start()` 中会被拒绝。
- 没有 HTTP/2 / HTTP/3 server push。
- HTTP/3 没有 WebSocket、请求 trailers、`Expect: 100-continue`、0-RTT。
- 客户端没有传统连接池；每个 `Client` 只缓存一条到某个 origin 的连接。

---

## 2. 目录结构

```
include/
  simple_http.h                     伞形头（聚合全部公共 API）
  simple_http/
    core/                           协议无关基础设施
      accept_encoding.h             Accept-Encoding qvalue
      address.h                     peer 地址展示（IPv4-mapped 处理）
      base64.h
      compat.h                      C++17 兼容：ByteSpan、starts_with、executor_initiation
      compression.h                 CompressionConfig / RequestDecompressionConfig
      content_encoding.h            gzip/br 编解码、协商
      format.h                      `{}` 子集格式化（替代 std::format）
      glaze_support.h               SIMPLE_HTTP_HAS_GLAZE 门控
      http_date.h                   IMF-fixdate
      http_field.h                  字段名常量、hop-by-hop
      http_method.h                 Method + is_idempotent
      http_status.h                 状态码常量
      io_pool.h                     IoCtxPool（单线程 io_context 池）
      limits.h                      EngineLimits
      logging.h                     LogSink / LogLevel / LogRecord / 宏
      mime.h
      resolve.h                     ResolveFn（客户端与 WS 反代 DNS 钩子）
      result.h                      boost::system::result 别名
      source_location.h             C++17 source_location 替身
      static_table.h                静态文件查找表
      tls_version.h                 TlsMinVersion
      types.h                       error_code、Version、Method 辅助、tcl 校验
      url_encoding.h                query_escape / path_escape
      url_path.h                    decode_and_normalize
      validators.h                  ETag / 弱比较 / Range
      version.h                     版本与 Server/User-Agent 产品串
    proto/                          版本无关 HTTP 模型
      body.h                        Body：推/拉双模式、透明解码
      compressing_writer.h          响应压缩装饰器
      error_body.h                  JsonError / ProblemDetails
      form.h                        urlencoded / multipart body 读取
      headers.h                     Headers
      json.h                        glaze JSON 读写（门控）
      multipart.h                   整块 multipart 解析 + MultipartForm 编码
      multipart_stream.h            增量 multipart
      params.h                      path_params / query_params（门控）
      query.h                       QueryParams
      request.h                     Request（两端共用）
      response.h                    ResponseWriter（fluent 门面）
      response_writer.h             ResponseSink 抽象
      websocket.h                   WebSocket + WsBackendImpl
      ws_deflate.h                  permessage-deflate
      ws_frame.h                    WebSocket 帧编解码
    transport/
      transport.h                   Transport 鸭子接口 + SslHandle
      serialized_writer.h           单连接串行写队列（h1/WS）
      tcp_transport.h               TcpTransport
      tls_context.h                 TlsContext / TlsConfig（ALPN）
      tls_transport.h               TlsTransport / NestedTlsTransport
      openssl_init.h                OpenSSL 进程级一次性初始化
    engine/
      dispatcher.h                  Handler/Middleware 适配、代理 seam
      ws_proxy_common.h             h1 Upgrade 与 h2 extended CONNECT 共用握手
      h1/ h1_parser.h h1_engine.h ws_proxy.h
      h2/ h2_frame.h hpack_*.h h2_engine.h h2_ws_transport.h
      h3/ h3_callbacks.h h3_stream.h h3_engine.h（宏门控）
    handler/
      handler.h                     Next/Middleware/make_middleware
      validation.h                  Validator<T> / FieldError / 约束辅助
      extract.h                     Path/Query/Json/Form/State/Header/Cookie + extract
      session.h                     SessionStore / MemorySessionStore / Session
      csrf.h                        double-submit CSRF
      jwt.h / jwks.h                JWT 验证与 JWKS
      builtin_middleware.h          内置中间件
      secure_headers.h
      rate_limit.h
      cors.h
      conditionals.h
      router.h
      http_proxy.h
      static_files.h
      file_response.h               async_send_file
      upload.h                      async_read_body_to_file
    net/
      connection.h                  PlaintextProtocols
      server.h                      Server / ServerConfig / 监听生命周期
    client/
      http.h                        Client / RequestBuilder / Response / Stream
      client_config.h               ClientConfig / client_errc / RetryPolicy / ProxyConfig
      client_stream.h               ClientStream / ResponseHead
      h1_client.h / h2_client.h / h3_client.h
      multiplexed_session.h         h2/h3 共用骨架
      tls_client.h                  客户端 TLS/ALPN/SNI
      proxy.h                       出站代理（CONNECT / HTTPS / SOCKS5 / NO_PROXY）
      url.h                         绝对 URL 解析
      cookie_jar.h
      decompressing_stream.h
      sse.h                         SseParser / SseStream
      ws_client.h                   WebSocket 客户端（H1 Upgrade）
      multipart_stream.h            MultipartStreamWriter / async_stream_file_body
    openapi/
      openapi_doc.h                 OAS 3.1 模型 + JSON 渲染
      openapi.h                     glaze 反射 schema / doc()（门控）
    quic/                           ngtcp2 封装（宏门控）
test/
  unit/                             Catch2 单测（约 511 个 TEST_CASE）
  cpp17/smoke.cpp                   以 C++17 编译并运行公共 API
  diff/client/                      C++ 客户端 ↔ Go net/http 客户端
  diff/server/                      Go 探针 → C++ server ↔ Go server + h2raw
  diff/openapi/                     OAS 3.1 契约 + Swagger UI
  diff/support/harness.py           归一化、比较、known_diffs
  fixtures/conformance_server.cpp   外部合规/压力专用 server
  external/conformance/             h2spec / h1spec / h3spec / Autobahn
  external/stress/                  h2load / k6
  sanitizer/                        concurrency harness + suppressions
  fuzz/                             fuzz_* 目标 + corpus
  tls_certificates/                 测试证书（相对仓库根路径是契约）
  coverage.sh / sanitize.py / run.py / run.sh
```

`include.old/`、`build-*`、`v2ray-cpp/`、`review/`、`REVIEW-*.md`、`bin/` 不是
库的公共接口，不要据此写文档或改代码。

---

## 3. 架构与硬约束

### 3.1 分层

```
handler / net / client 门面
        ↓ 只用 Request / ResponseWriter / Router / Body
proto（版本无关模型）
        ↓ ResponseSink
engine（h1 / h2 / h3）
        ↓ Transport
transport（TCP / TLS / QUIC 字节流）
```

- 上层不得出现协议版本分支。版本差异放在 `engine/` 或对应的
  `ResponseSink` 实现里；`ResponseWriter` 只是转发门面。
- 引擎只通过 `Transport` 读写字节，不直接依赖 socket 类型。
- `Request` / `ResponseWriter` / `Router` / handler 对 HTTP 版本透明。

### 3.2 并发模型 A（必须遵守）

- `IoCtxPool` 有 `worker_threads` 个**单线程** `io_context`，每个 worker 拥有一个
  acceptor；同一 listener 的 acceptor 用 `SO_REUSEPORT` 共享端口，由内核分派新连接。
- acceptor 在哪个 worker 的 context 上，它接受的 socket 就绑定到哪个 context：
  连接不会在 worker 之间迁移。
- connection closer 注册表按 worker 分片，只在所属 worker 线程读写；`stop()`
  通过向每个 worker `post` 一次来关闭其名下的连接。
- 该连接上的引擎、writer、handler、`Body`、`WebSocket` 全部只在这一个线程执行。
- 因此 `engine/` 下没有 mutex/atomic 保护连接状态。
- **handler 中所有操作都是 owner-thread only**：`Request`（`cancel()` /
  `cancelled()` 除外）、`Body`、`ResponseWriter`、`WebSocket`，以及路由、中间件、
  `extract`、session/CSRF/JWT 等辅助对象，都只能在处理该请求的 executor 线程调用，
  **不支持跨线程操作**。调用方保证所有 `req`/`res`/`Body`/`WebSocket` 操作
  （包括 `is_open()` 与析构）都在 socket 的 executor 上；这些类型没有运行期线程
  检查，违反契约属于未定义行为。
- 引擎的 `ResponseSink` 实现也不再做防御性 hop：`ResponseWriter` 到达时已经在
  连接 executor 上，sink 直接执行对应操作。
- **`Client` 同样绑定单线程 executor**：构造时传入的 executor 就是它唯一的执行
  线程；`async_send` / `async_open_stream` / `async_open_websocket` 以及后续
  `Stream` 操作都必须在 own executor 上调用，不支持跨线程。连接缓存内部的
  `dispatch`/互斥只是实现细节，不构成跨线程 API 承诺。
- 需要换线程时调用方显式 `asio::post(req->executor(), ...)`（客户端用
  `asio::post(client.executor(), ...)`）；不要依赖库内 hop 把调用方协程恢复回来。
  例如 `SessionStore` 可以在自己的线程完成回调，但 `session` 中间件会先
  `dispatch` 回连接 executor，再触碰 request/response。
- 刻意的跨连接共享例外（都在 handler 层，不在 `engine/`）：
  - `RateLimiter` 按 key 分桶加锁；
  - `Router` 惰性构建链的 atomic + mutex；
  - 日志 sink 的原子替换；
  - `WebSocket::is_open()` 的 atomic 状态；
  - `Server` 的连接计数、lifecycle registry、core 指针快照；
  - `MemorySessionStore` / `CookieJar` 的内部 mutex。

### 3.3 反压

- HTTP/2 请求体**按流停放**：某条流 channel 满时只暂停该流，其他流继续。
- 停放期间到达的 `END_STREAM` 记 `finish_pending`，排空后再 `body().finish()`。
- HTTP/2 响应输出高/低水位：1 MiB / 256 KiB；写循环每轮最多 framed 256 KiB。
- HTTP/3 用 **`out_bytes`（未 ACK 的 enqueued 字节）** 做反压，不是
  `out_write - out_ack`。
- WebSocket 有**有界入站读泵**：`m_inbox` 上限来自
  `EngineLimits::ws_read_ahead_bytes`（默认 4 MiB）。
- h1 请求体是 pull 模式：handler 不读，socket 就不读。
- 所有入口都有上限：`max_header_bytes`、`max_body_bytes`、
  `h2_max_header_list_size`、`ws_read_ahead_bytes`、SSE 行长、multipart part 大小。

### 3.4 生命周期与所有权

- 引擎用 `shared_ptr` 持有；handler 捕获它，保证 handler 存活期间引擎不析构。
- h2 sink 持 `weak_ptr` 到引擎，每次操作 `lock()`；引擎销毁后返回错误而不是 UAF。
- WebSocket 写泵持 `shared_ptr` 到 backend；`~WebSocket` 调 `abort()`，
  防止 detached 写泵永久持有 backend。
- WebSocket reader 持有 backend 引用，终止统一走关闭逻辑；`abort()` 关闭 transport
  以解开卡在 `async_read_some` 的 reader。
- `Server::stop()` 有界等待已进入 handler 的请求（`shutdown_grace`），然后关闭剩余
  连接并停止 I/O 池。不要从信号处理函数里直接调用 `stop()`。
- 异步回调不得持裸 `this`；`Client` 用 `keep_alive()` 捕获（堆对象共享所有权，
  栈对象是非拥有别名，依赖 awaitable 契约）。

### 3.5 错误处理

- 同步可失败 API 返回 `result<T>` / `error_code`；异步失败通过 completion 的
  `error_code`。
- 客户端失败从不抛异常；TLS 配置等启动期错误可能抛 `std::runtime_error`。
- 服务端 handler 抛 `std::exception`：未开始响应 → `reset_for_error()` + 500；
  已开始 → `async_close()`。
- 协议错误要区分：请求头过大 431、请求体过大 413、协议错误按版本返回 400 /
  RST_STREAM / GOAWAY / h3 reset。

### 3.6 日志

- 库只定义 `LogSink`（`enabled(level)` + `log(record)`），不依赖第三方日志库。
- `LogRecord` 有 `level/category/source_location/message/attrs`；`Attr` 的字符串
  只在 sink 调用期间有效，`LogFields` 的值由调用方拥有。
- `set_log_sink` 原子替换；`enabled()` 在格式化之前询问。
- 编译开关：`SIMPLE_HTTP_ENABLE_LOG`（默认 1）、`SIMPLE_HTTP_LOG_ACTIVE_LEVEL`。
- `middleware::access_log()` 输出结构化字段；`LogFields` 会合并进 access 记录。

---

## 4. 公共 API 速查

### 4.1 Server

```cpp
ServerConfig cfg;
cfg.listen = InetAddress{"0.0.0.0", 7788};
cfg.worker_threads = 4;
cfg.max_connections = 0;          // 0 = 不限；到上限留在内核 backlog
cfg.tcp_nodelay = true;
cfg.plaintext_protocols = PlaintextProtocols::Both;
cfg.shutdown_grace = std::chrono::milliseconds{5000};
cfg.limits = EngineLimits{};

Server server{cfg};
server.route(...);
server.use(...);
auto started = server.start();    // result<StartReport, StartupError>
server.stop();
```

- accept 拓扑固定：每个 worker 一个 acceptor，socket 在 accept 线程上直接服务；
  没有单 acceptor 轮询模式，也没有独立的 acceptor 线程/context。
- `start(StartPolicy)`：`AllOrNothing`（默认）或 `BestEffort`。
- 多监听：`cfg.listeners`，或 `server.listen(TcpListenerConfig{...})`。
- 每个 listener 可带自己的 `EngineLimits`、`max_connections`、`tls`、
  `plaintext`、`alpn`、`socket_setup`、`listener_setup`。
- `Server::start()` 自己启动线程；没有 `run()` / `join()`。
- 观测：`port()`、`quic_port()`（h3）、`active_connections()`、`active_requests()`。
- `UnixAddress` 会被拒绝（`server_errc::invalid_config`）。

`PlaintextProtocols` 与内部 listener 策略的映射：

| 公开值 | 内部行为 |
|---|---|
| `Http1` | 纯 HTTP/1.x |
| `Http2` | h2 prior knowledge |
| `Both` | 嗅探 preface；非 h2 走 h1，并允许 `Upgrade: h2c` |
| `TcpListenerConfig::plaintext = Http1WithH2cUpgrade` | 纯 h1 + h2c Upgrade |

TLS 监听永远不允许 h2c Upgrade；协议由 ALPN 决定。`TlsHttp::Http2Only` 在没有
h2 ALPN 时关闭连接，不回退 h1。

### 4.2 Handler / Middleware / Router

```cpp
using RequestPtr  = std::shared_ptr<Request>;
using ResponsePtr = std::shared_ptr<ResponseWriter>;
using HandlerCompletion = asio::any_completion_handler<void()>;
using CallbackHandler =
    std::function<void(RequestPtr, ResponsePtr, SslHandle, HandlerCompletion)>;

#if defined(BOOST_ASIO_HAS_CO_AWAIT)
using CoroHandler = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle)>;
using Handler = std::variant<CallbackHandler, CoroHandler>;
#else
using Handler = CallbackHandler;
#endif
```

`make_handler` 接受：

- 已构造的 `Handler`；
- `(RequestPtr, ResponsePtr[, SslHandle]) -> void`；
- `(RequestPtr, ResponsePtr[, SslHandle], HandlerCompletion) -> void`；
- C++20：`(RequestPtr, ResponsePtr[, SslHandle]) -> asio::awaitable<void>`。

中间件：

```cpp
using Next = CallbackHandler;
using Middleware =
    std::function<void(RequestPtr, ResponsePtr, SslHandle, Next, HandlerCompletion)>;
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
using CoroNext = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle)>;
using CoroMiddleware =
    std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle, CoroNext)>;
#endif
```

- `use()` / `make_middleware()` 接受回调式或协程式；`make_middlewares(...)`
  用于 `group()` / `route()` / `ws_route()` 的 `std::vector<Middleware>` 参数。
- `use()` 按注册顺序嵌套，最外层先跑；CORS 始终最外层。
- `group(prefix, mws, registrar)`：前缀 + 组中间件，可嵌套。
- per-route middleware 只包该路由。
- `req->set_state<T>()` / `get_state<T>()`：按类型键，last-set-wins。

内置中间件（`simple_http::middleware`）：

`request_id()`、`access_log()`、`recovery()`、`basic_auth(user, pass)`、
`real_ip(trusted)`、`clean_path()`、`strip_prefix(prefix)`、
`redirect_slashes()`、`ws_origin(allowed, allow_missing=false)`、
`timeout(d)`、`compress(cfg)`、`decompress_request(cfg)`、
`secure_headers(cfg)`、`rate_limit(bucket)`、`rate_limit(limiter, key_fn)`、
`session(store, cfg)`、`csrf(cfg)`、`jwt(cfg)`、`require_scope(scope)`、
`require_role(role)`；CORS 用 `simple_http::make_cors_middleware(cfg)` 或
`Server::cors(cfg)`。

路由匹配顺序（`Router::dispatch_impl`）：

1. Host 子 Router（命中后父 Router 的其余阶段都不参与）
2. 精确路径（方法感知）
3. `{param}` 模板 trie（含末尾 `{*rest}`）
4. 本地正则（第一个匹配）
5. HTTP 反代（精确后正则）
6. 静态文件阶段
7. fallback
8. 内建 404

隐式规则：

- 路径被 exact/template/regex 认领但方法不符：立即 405 + `Allow`，`OPTIONS`
  自动 204 + `Allow`；不会继续落到 proxy/static/fallback。
- GET 隐含 HEAD；注册 GET 不占用 HEAD 槽，显式 HEAD 可以后注册并优先。
- 空 methods = 任意方法，含扩展 token；显式注册 `Method::Unknown` 被拒绝。
- `Allow` 始终包含 `OPTIONS`。
- WebSocket 查找顺序：本地 ws exact → ws 反代 → 本地 ws regex。
- `static_files` 不能在 group 内注册；带 middleware 的 group 拒绝 `ws_proxy`；
  `http_proxy` 会继承 group middleware。
- Host 只作用于 HTTP dispatch；WebSocket 查找仍 host-agnostic。

### 4.3 框架层

```cpp
server.state(std::make_shared<AppState>(...));
const AppState *st = req->app_state<AppState>();

server.route({Method::Post}, "/users/{id}",
    extract<Path<UserParams>, Valid<Json<CreateUser>>>(
        [](const UserParams &p, CreateUser in, RequestPtr req, ResponsePtr res)
            -> asio::awaitable<void> { ... }));
```

- `extract<...>` 返回普通 `Handler`，不影响 route/group/middleware/OpenAPI。
- 提取值按模板参数顺序放在 handler 参数最前面，然后才是 `RequestPtr`、
  `ResponsePtr`、可选 `SslHandle`、可选 `HandlerCompletion`。
- 同一次提取最多一个 body extractor（`Json` / `Form`）。
- `Validator<T>` 的实际形状是
  `static ValidationResult validate(const T&)`，**不是** `std::expected`。
  `ValidationResult` 有 `.errors`、`has_value()`、`operator bool`、`error()`。
- 校验辅助：`required_field`、`min_length`、`max_length`、`range`、`one_of`、
  `push_if`、`field_errors_json`。

glaze 门控：

- `Path` / `Query` / `Json` / `Form` 的 `Extractor` 特化在
  `SIMPLE_HTTP_HAS_GLAZE` 内；plain C++17 下这些类型不能实际使用。
- `State<T>`、`Valid<E>` 不受门控。
- `Header<T, "name">` / `Cookie<T, "name">` 需要 C++20 class-type NTTP。
- JWT/JWKS 与 OpenAPI 整体在门控内。

Session / CSRF / JWT：

- `SessionStore::load/save/destroy` 使用 `any_completion_handler`，`save` 按值
  接收 `SessionData`；`MemorySessionStore` 加锁，达到上限时删除
  `unordered_map::begin()`（不是严格 LRU）。
- `SessionConfig` 默认：`cookie_name="sid"`、`path="/"`、`same_site="Lax"`、
  `secure=true`、`http_only=true`、`rolling=true`、`max_age=86400s`；没有 domain 字段。
- CSRF 是 header 型 double-submit（默认 `x-csrf-token`），只检查 header，
  不读取表单字段；safe 方法：GET/HEAD/OPTIONS/TRACE。
- JWT 只支持 HS256/RS256/ES256，拒绝 `alg: none`，默认 `require_exp=true`；
  `aud` 是单字符串；不做签发、OIDC discovery 或后台 JWKS 刷新。

### 4.4 Request / ResponseWriter / Body

```cpp
req->method(); method_token(); version(); target(); path(); query();
req->query_params(); req->param("id"); req->header("host"); req->headers();
req->trailers(); req->cookie("session"); req->peer_address();
req->body(); req->executor(); req->cancellation_slot();
req->cancel(); req->cancelled();
req->set_state<T>(...); req->get_state<T>(); req->app_state<T>();
```

取消：

```cpp
req->cancel();                    // 幂等、任意线程安全；只发出 terminal cancellation
req->cancelled();                 // 原子读取
auto reg = req->cancellation_registration(); // 首选：完成后自动注销
auto slot = reg.slot();
req->cancellation_slot();         // 兼容入口；signal 保留到 Request 析构
```

`cancellation_registration()` 返回的对象必须活到绑定操作完成（通常 move 进
completion handler）；它析构时把子 signal 从请求的 fan-out 列表移除，避免
反复取槽导致列表无界增长。

绑定了请求槽的库内操作遵循 Asio per-operation cancellation：挂起的读、写和
h2/h3 反压等待都以 `asio::error::operation_aborted` 完成。HTTP/1.x 的写一旦可能
有字节上线就无法撤回，取消它会关闭连接（该连接不会复用）。

读体：

```cpp
req->body().async_read(token);      // void(error_code, ReadResult)
req->body().async_read_all(token);  // void(error_code, std::string)
```

`ReadResult{data, eof}` 显式区分 EOF 与空 chunk。`Body::feed(std::string&&)`
返回 false 时参数保持原样，调用方可重试；`finish()` / `fail()` 不可丢，
channel 满时用 out-of-band pending 状态。

响应：

```cpp
res->status(200).content_type("text/plain");
res->async_send("hello", token);        // one-shot
res->async_begin(token);                // streaming
res->async_write("chunk", token);
res->async_finish("last", token);
```

其它：`header` / `replace_header` / `redirect` / `set_cookie` /
`async_sse_begin` / `async_sse_event` / `async_sse_comment` /
`trailer` + `async_finish_trailers` / `async_send_continue` /
`async_send_bodyless` / `async_connected` / `async_close` / `version()`。

规则：

- 一个 `ResponseWriter` 只发送一个响应头；`send` / `send_bodyless` / `begin`
  共享 `m_started` 检查，第二次返回 `invalid_argument`。
- `status()` 拒绝 100..599 之外的值。
- `header()` / `set_cookie()` / `trailer()` 做 token、控制字节、cookie-octet
  校验；非法字段置 `m_field_rejected`，最终写失败。
- `async_send_bodyless` 只允许 HEAD / 204 / 304。
- `async_begin` 拒绝 204 / 304 和 1xx。
- `async_send` 自动补 `Date`、`Server`、`Content-Type`（204/304 不补
  Content-Type）；引擎剥离 handler 设置的 hop-by-hop 字段。
- SSE helper 会对 event/id/retry 中的 CR/LF/NUL 做替换，防止注入。

### 4.5 WebSocket

```cpp
server.ws_route("/chat",
    [](RequestPtr, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws->async_read(asio::use_awaitable);
            if (!msg) break;
            co_await ws->async_write(msg->data, msg->text, asio::use_awaitable);
        }
    });
```

- `async_read` → `void(error_code, WsMessage)`，自动重组分片、Ping→Pong、
  收到 Close 回 Close、文本增量 UTF-8 校验。
- `async_write_text` / `async_write_binary` 按值收 payload；
  `async_write_text_view` / `async_write_binary_view` / `async_write_view`
  借用调用方缓冲，必须在操作完成前保持有效。
- 写操作经内部写泵串行化；读泵在首次 `read()` 时懒启动，socket 读取由独立
  reader 持续排空进有界 `m_inbox`。
- 协议错误 / Close / 非法 UTF-8 由 reader 记下关闭码，但 Close 帧要等应用下次
  `read()` 排空缓存后才发出（`flush_deferred_close`）。
- 服务端支持 h1 Upgrade 与 h2 extended CONNECT；h3 不支持。
- `EngineLimits::ws_compression` 默认 false；客户端不协商 permessage-deflate。
- 客户端只用 HTTP/1.1 Upgrade（`Client::async_open_websocket`）。

### 4.6 Client

```cpp
Client client{executor, ClientConfig{}};

auto r = co_await client.get(url).send(asio::use_awaitable);
auto r2 = co_await client.post(url).content_type("application/json")
                       .body(json).bearer_auth(token)
                       .send(asio::use_awaitable);

auto s = co_await client.async_open_stream(url, StreamSpec{.method = Method::Post}, 0,
                                           asio::use_awaitable);
auto ws = co_await client.async_open_websocket("wss://host/chat", WebSocketSpec{},
                                               asio::use_awaitable);
```

- `get` / `post` / `put` / `del` / `head` / `patch` / `request`。
- `RequestBuilder`：`query`、`header(s)`、`content_type`、`basic_auth`、
  `bearer_auth`、`body`、`max_body_bytes`、`timeout`、`version`、`send`。
  **没有** `.json(...)` 便捷方法。
- `send()` 返回缓冲 `Response`，应用重定向 / Cookie / 状态码重试 / 自动解压。
- `async_open_stream()` 是显式流：**不**跟随重定向、不应用 Cookie、不重试；
  自动解压仍生效。
- `Response`：`status`、`version`、`headers`、`header` / `header_owned`、
  `ok`、`bodyless`、`uncompressed`、`error_for_status`、`text` / `bytes`、
  `async_read_all`。`text()` 与 `bytes()` 是同一个字符串。
- `Stream`：`async_write` / `async_finish` / `async_finish_trailers` /
  `async_read_head` / `async_read` / `async_read_all` / `async_ensure_continue` /
  `head()` / `trailers()` / `finished()` / `abort()`。
- 客户端绑定单线程 executor（模型 A）：所有 initiating function 与后续
  `Stream` 操作都必须在构造时的 executor 上调用，不支持跨线程；`io_context`
  必须活过会话。
- 单连接模型：对第一个 origin 保持一条连接（h1 FIFO / h2 多路复用），其他
  origin 使用一次性连接。`Client::close()` 关闭缓存会话。
- 自动解压对齐 Go：`auto_decompress=true` 且只广告 gzip；调用方显式设置
  `Accept-Encoding`、HEAD、带 Range 时不注入也不解码。br 需显式
  `accept_encodings={"br","gzip"}`。
- 状态码重试默认关闭（`retry_statuses` 为空）；显式设置后按 429/503 +
  `Retry-After` 重试。`RetryPolicy::condition` / `require_replayable` 当前**未使用**。
- 明文 + `HttpVersionPolicy::Http2` 走 prior knowledge；`H2cMode::Upgrade`
  当前**未接线**。
- HTTP/3 只在 `SIMPLE_HTTP_ENABLE_HTTP3` 下可用，且必须显式
  `HttpVersionPolicy::Http3` + `https://`；不支持代理、请求 trailers、1xx。
- 取消：`send` / `async_open_stream` / `async_open_websocket` 以及
  `Stream::async_read` / `async_read_head` / `async_write` 都绑定调用方的
  cancellation slot。`asio::cancel_after` 可以在 DNS/dial/TLS/读/写任一阶段中止，
  返回 `operation_aborted`。客户端取消不复用被中止的交换：写取消会关闭 transport。

### 4.7 数据与 OpenAPI

```cpp
async_read_json_body<T>(req, token);                 // void(error_code, result<T>)
async_write_json(res, value, status, token);         // void(error_code)
read_urlencoded_body(req, token);                    // void(error_code, QueryParams)
read_multipart_body(req, token);                     // void(error_code, vector<MultipartPart>)
read_multipart_stream(body, boundary, cb, token);    // void(error_code)
async_write_error(res, status, JsonError, token);    // void(error_code)
async_write_problem(res, status, ProblemDetails, token);
```

OpenAPI：

```cpp
server.route({Method::Get}, "/pets/{id}", handler,
    openapi::doc()
        .path_params<PetParams>()
        .response<Pet>(200)
        .summary("get a pet")
        .operation_id("getPet")
        .error<ErrorBody>(404, "no such pet"));

server.openapi().title("petshop").version("1.0.0");
server.serve_openapi("/openapi.json");
server.serve_swagger_ui("/swagger", "/openapi.json");
```

- 文档按路由级 `openapi::doc()` 元数据收集；没有描述的路由不进文档
  （`NoDoc`）。
- 路径模板 `{name}` 必须等于 `path_params<T>` 的字段名，注册时校验。
- `route_regex` 没有 `Doc` 参数，正则路由不能附 OpenAPI descriptor。
- `verify_openapi()` 只检查 exact/template，不检查 regex/proxy/static。
- Swagger UI 使用 CDN，不打包本地资源。

---

## 5. 协议实现要点

### 5.1 HTTP/1.x

- 手写解析器（`engine/h1/h1_parser.h`），不依赖 beast。
- 只接受 `HTTP/1.0` / `HTTP/1.1` 请求行。
- 请求 framing：chunked / Content-Length / 无；HTTP/1.0 不接受 chunked。
- 响应 framing：一次性 Content-Length；流式 HTTP/1.1 用 chunked，
  HTTP/1.0（以及 HEAD）用 close-delimited。
- 严格拒绝 CL + TE、重复/非法 Content-Length、HTTP/1.0 + TE、
  不支持的 transfer coding（501）、重复 chunked。
- `Expect: 100-continue` 仅 HTTP/1.1；惰性发、最多一次；最终响应已开始时不再发 100。
- 请求 trailers：chunked 终止符 `0\r\n<fields>\r\n`；每个消息重置 trailer 字节预算。
- body 用 pull provider；handler 返回后 drain 未读 body 以复用连接。
- 升级：WebSocket、h2c Upgrade（101 后把原请求重放为 stream 1）。
- h1 的响应字节和 WebSocket 101 都经每连接一个 `SerializedWriter`。

### 5.2 HTTP/2

- 帧层、HPACK 全部手写，不依赖 nghttp2。
- `read_loop || write_loop || watchdog`，连接状态只在单连接 executor 上。
- SETTINGS、PING、RST_STREAM、GOAWAY、WINDOW_UPDATE、CONTINUATION、padding；
  PRIORITY 只作建议；客户端发 PUSH_PROMISE 是 PROTOCOL_ERROR。
- 请求在 END_HEADERS 后即 dispatch，handler 与后续 DATA 并发。
- 请求字段校验（§8.2.1/§8.3.1）：普通字段名必须是 token；`:method` 必须是
  token；`:path` 必须是 origin-form 或 `*` 且不含 `#`/SP；`host` 重复或与
  `:authority` 不一致是流级 PROTOCOL_ERROR。
- 响应 trailers：trailing HEADERS + END_STREAM；最后 DATA 不带 END_STREAM。
- 一次性响应不发 Content-Length（END_STREAM 界定），HEAD 例外。
- RFC 8441 extended CONNECT 默认开启（`h2_enable_connect_protocol`）。
- Rapid Reset 防护：同时执行的 handler 上限 = `h2_max_concurrent_streams`；
  超出进 `h2_max_queued_streams` 队列；队列溢出 → `GOAWAY(ENHANCE_YOUR_CALM)`。
- 关闭流按 RFC 9113 §5.1 三分法处理；固定 256 项 ring 记录关闭原因。
- 不实现 server push。

### 5.3 HTTP/3

- `#ifdef SIMPLE_HTTP_ENABLE_HTTP3` 整体门控。
- 帧层、SETTINGS、GOAWAY、QPACK、blocked field sections、请求校验由 nghttp3
  负责；QUIC 由 ngtcp2 负责。本库只保留适配层。
- 引擎实现 `quic::Protocol`，没有自己的写循环；QUIC 连接负责拉数据。
- 没有自己的 idle watchdog；使用 QUIC 协商的 idle timeout。
- 请求在 headers 完成时 dispatch，不等 END_STREAM；每 stream 一个 handler。
- Body channel 满时停放该流；END_STREAM 记 `finish_pending`。
- 消费式流控：读走字节后回补 stream/connection offset；handler 提前返回时归还
  未读 credit。
- 服务端不支持 WebSocket；`Expect: 100-continue` 是 no-op；请求 trailers 不暴露。
- 响应 trailers 支持。
- 客户端不支持代理、请求 trailers、1xx；0-RTT 未启用。
- 服务端可 Alt-Svc 通告；QUIC 缺 `quic_transport_parameters` 时按 RFC 9001
  §8.2 关闭（`CONNECTION_CLOSE 0x016d`）。

### 5.4 WebSocket

- 帧编解码手写（`proto/ws_frame.h`），transport 无关。
- 服务端支持 h1 Upgrade 与 h2 extended CONNECT；h3 不支持。
- permessage-deflate（RFC 7692）服务端可协商，默认 false。
- 读泵 / 写泵 / 有界入站队列是防止「串行 read→write + 对端不读」死锁的关键。
- 压缩帧在解压前不做 UTF-8 增量校验；空消息不压缩；mask key 必须来自 CSPRNG。
- 64 位 payload 长度最高位必须为 0；部分帧 payload 只交付一次。

### 5.5 TLS / ALPN

- 服务端 `TlsConfig::min_version` 默认 TLS 1.3；设 `Tls12` 同时接受 1.2；
  1.0/1.1 永不允许。
- 服务端 TCP TLS ALPN 默认 `h2` + `http/1.1`；`h3` 只用于 QUIC。
- `TlsHttp::Http2Only` 必须要求 ALPN 匹配（`require_alpn_match`），否则
  握手成功但回退 h1；服务端在握手完成后发现没有 h2 会关闭连接。
- 客户端 `TlsClientConfig::min_version` 默认 TLS 1.2。
- mTLS：`mutual = true` + `ca_file`；handler 第三参数 `SslHandle` 可读对端证书。
- HTTP/3 的 QUIC 自带 TLS 1.3，ALPN 固定 `h3`。
- 所有 `ssl::context` 构造入口都必须先经过
  `transport/openssl_init.h` 的 `ensure_openssl_initialized()`，避免多线程首次
  并发 `SSL_CTX_new` 的 TSan race。

### 5.6 EngineLimits 默认值

| 字段 | 默认 |
|---|---|
| `idle_timeout` | 120 s（≤ 0 禁用） |
| `max_header_bytes` | 64 KiB（431） |
| `max_body_bytes` | 64 MiB（413）；0 = 不限制，h1/h2/h3 一致 |
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

客户端 `ClientConfig::limits` 把 `h2_initial_window` 覆盖为 1 MiB。

---

## 6. 构建与运行

### 6.1 xmake（主构建）

```sh
xmake f -m release
xmake build -j 12

xmake build -j 12 test_unit
xmake build -j 12 fixture_conformance_server
xmake build -j 12 diff_server_cpp
xmake build -j 12 diff_openapi_server
xmake run test_unit
```

- `set_languages("c++23")`：库与测试默认按 C++23 编译；glaze 辅助因此可用。
- `test_cpp17_smoke` 单独 `set_languages("c++17")`，验证 C++17 公共 API。
- 全局 `-O2 -Wextra`，关闭少量 GCC 误报；全局定义
  `SIMPLE_HTTP_USE_BOOST_REGEX`。
- `SIMPLE_HTTP_ENABLE_HTTP3` 只加在需要它的目标
  （`test_unit`、`fixture_conformance_server`、`diff_client_h3`、
  `diff_server_cpp`）。
- 依赖来自私有 xmake 仓库 `fantasy-peak/xmake-repo`；ngtcp2 必须是
  `ENABLE_OPENSSL=ON` 构建。
- 测试目标都是 `set_default(false)`，裸 `xmake build` 不会构建它们。
- 测试目标 `set_rundir(".")`，因为 TLS 证书路径是
  `./test/tls_certificates/...`。

### 6.2 CMake 的真实状态

```cmake
find_package(simple_http CONFIG REQUIRED)
target_link_libraries(app PRIVATE simple_http::simple_http)
```

当前 `CMakeLists.txt`：

- `simple_http` 是 `INTERFACE` 目标，`cxx_std_23`；
- 链接 Boost、OpenSSL、ZLIB、Brotli、glaze；
- **没有** ngtcp2/nghttp3，也没有 `SIMPLE_HTTP_ENABLE_HTTP3`；
- 另有一个 `simple_http_cpp17` INTERFACE 目标，指向
  `${CMAKE_CURRENT_SOURCE_DIR}/cpp17/include`，但该目录**不存在**；
  它也未被安装/导出；
- `cmake/simple_httpConfig.cmake.in` 没有 `find_dependency(glaze)`，
  尽管导出的 target 公开链接 `glaze::glaze`；
- 没有 `enable_testing()` / `add_test()`。

改 CMake 时要么把这些修好，要么在文档里明确标注，不要假装它们已经可用。

### 6.3 编译宏

| 宏 | 默认 | 作用 |
|---|---|---|
| `SIMPLE_HTTP_ENABLE_HTTP3` | 未定义 | 编入 HTTP/3 与 QUIC 监听 |
| `SIMPLE_HTTP_USE_BOOST_REGEX` | 本仓库定义 | 路由正则使用 Boost.Regex |
| `SIMPLE_HTTP_HAS_GLAZE` | 自动检测 | glaze 辅助（C++23 + glaze） |
| `SIMPLE_HTTP_ENABLE_LOG` | 1 | 日志总开关 |
| `SIMPLE_HTTP_LOG_ACTIVE_LEVEL` | 0 | 编译期最低日志级别 |
| `BOOST_ASIO_HAS_CO_AWAIT` | Boost 决定 | 协程 handler/middleware 适配 |

压缩、WebSocket、OpenAPI、UNIX socket 都没有独立开关宏。

---

## 7. 测试与验证

### 7.1 唯一入口与分层

```sh
test/run.sh fast        # unit + cpp17
test/run.sh standard    # fast + openapi
test/run.sh full        # 全部 8 个 suite
test/run.sh list
test/run.sh doctor
test/run.sh <suite>
test/run.sh unit '[h2]'
```

| suite | tier | 内容 |
|---|---|---|
| `unit` | fast | Catch2 单测/回归（约 511 个 `TEST_CASE`） |
| `cpp17` | fast | `test/cpp17/smoke.cpp` 以 `-std=c++17` 编译并运行 |
| `openapi` | standard | OAS 3.1 元模式 + 文档/路由契约 + Swagger UI |
| `client-diff` | full | C++ client ↔ Go `net/http` client |
| `server-diff` | full | Go 探针 → C++ server ↔ Go server；含 `h2raw` |
| `concurrency` | full | 多客户端/多线程/teardown harness |
| `conformance` | full | h2spec / h1spec / h3spec / Autobahn |
| `stress` | full | h2load / k6 |

`fast = unit + cpp17`；`standard = fast + openapi`；`full = 全部`。

运行器行为：

- 默认位置参数是 `fast`；也可以直接给 suite 名或 `list`/`doctor`。
- `--suites a,b,c` 直接指定 suite；`--jobs` 默认 12；`--parallel` 默认 3。
- `conformance` 与 `stress` 共用 7788–7792，固定同一条串行 lane。
- `full` 默认严格依赖：缺外部工具 → 退出码 3；`--allow-skip` 放宽。
- 缺测试二进制是 suite FAIL，不是 SKIP。
- 退出码：0 通过；1 失败；2 用法错误；3 环境/构建错误。
- 报告写到 `.cache/test-run/<timestamp>-<pid>/`；每个 suite 一个 `.log` 和
  `build.log`。统一 runner **不**产出聚合 JSON。
- suite 环境变量：`SIMPLE_HTTP_TEST_BIN_DIR`、`SIMPLE_HTTP_TEST_WORKDIR`、
  `SIMPLE_HTTP_TEST_SUITE`。

### 7.2 差分测试

- **client-diff**（`test/diff/client/`）：C++ 与 Go 客户端请求同一个 Go 参考
  服务端；服务端回显 method/header/body，因此差异只能来自客户端。组：
  `basic`、`body`、`policy`、`ws`、`h3`。端口 7810–7817。
- **server-diff**（`test/diff/server/`）：同一个 Go 探针分别打 C++ 与 Go 参考
  服务端（7820–7825 / 7830–7834）。组：`routing`、`body`、`semantics`、
  `middleware`、`framework`、`static`、`ws`、`raw`、`h2raw`。
  `h2raw` 是 C++ 单侧原始 HTTP/2 / h2c / WebSocket 断言，输出 PASS/FAIL。
- **归一化**（`test/diff/support/harness.py`）：header 名小写；
  `allow`/`vary`/`accept-encoding`/`cache-control` 按集合比较；默认忽略
  Date/Server/Content-Length/Transfer-Encoding/Connection/ETag/Last-Modified/
  Set-Cookie 等；body 支持 `exact`/`json`/`none` + `scrub`。
- 客户端差分默认比较 `version` 与 `framing`；错误按稳定 slug 比较。
- 未登记差异失败；`test/diff/known_diffs.json` 当前登记：
  - client：`policy.redirect_referer`（body）、`policy.redirect_cross_origin`
    （body）、`h3.status_204`（framing）；
  - server：`raw.cl_te`、`raw.duplicate_cl`、`raw.obs_fold`（status）、
    `body.request_trailers_h3`（body）。
- 新增功能时：C++ 与 Go 参考实现同步加行为 → 加用例 → 跑 diff → 只登记确属
  合法差异的项，并写清 RFC 依据。Go 无对应能力时标 `cpp-only`。

### 7.3 外部一致性与压力

被测 server 是 `test/fixtures/conformance_server.cpp`，监听：

- `7788`：嗅探 h1+h2c，WebSocket `/echo`、`/echo1k`，启用 permessage-deflate；
- `7790`：纯 h2c；
- `7791`：纯 h1；
- `7792`：TCP + UDP HTTP/3。

单协议端口是必需的：嗅探端口上「畸形 h2 前导」与「畸形 h1 请求行」是同一批
字节，h2spec 要 GOAWAY、h1spec 要 400。

```sh
test/external/conformance/run.sh
test/external/conformance/run.sh h2spec
test/external/stress/run.sh cpp h3 --no-conformance
test/external/stress/run.sh both
```

门线：

- h2spec：`failed == 0` 且 `passed >= 146`；
- h1spec：总用例 33，必须全部通过；
- h3spec：总用例 49，`passed >= 47`；
- Autobahn：`FAILED == 0` 且 `NON-STRICT == 0`；
- 压力：连接复用 200/200；每个响应档位 `failed == 0` 且 `errored == 0`；
  h1 k6 `errors == 0`；WebSocket `recv == ITERS × WANT`（默认 1,200,000）。

`CONFORMANCE_HTTP3=0` 跳过 7792/h3spec；`STRESS_HTTP3=0` 跳过 h3 档。
脚本会先清理遗留服务和确认端口释放，否则 SO_REUSEPORT 会把 QUIC 连接表劈开。

### 7.4 OpenAPI

`test/diff/openapi/` 的 petshop 夹具（`:7795`）验证：

1. 文档通过本地 OAS 3.1 元模式；
2. 结构不变量：operationId 唯一、path 参数与模板一致且 required、每个声明响应
   有 description、`$ref` 可解析且无 `$defs` 泄漏；
3. 文档 ↔ 实际路由：每个 operation 的每个声明状态都有真实请求覆盖，校验状态码、
   Content-Type、响应 JSON Schema、声明响应头与 401 challenge；
4. `/swagger` 可访问、引用正确 spec、文档两次获取字节一致。

### 7.5 Sanitizer

```sh
test/run.sh sanitize asan [suite...]   # ASan + LSan + UBSan
test/run.sh sanitize tsan [suite...]   # TSan
test/run.sh sanitize ubsan [suite...]  # 仅 UBSan
```

默认 suite：

| sanitizer | 默认 suites |
|---|---|
| asan | `unit openapi client-diff server-diff concurrency` |
| tsan | `unit concurrency server-diff` |
| ubsan | `unit client-diff server-diff` |

- 默认 `--jobs 7`；ASan 编译 unit 时 `-j14` 会 OOM。
- 默认在 `.cache/sanitize/worktree-<san>` 建独立 worktree；
  `--build-dir build-asan` 可复用已有目录，结束时恢复默认 xmake 配置。
- 报告：`.cache/sanitize/<san>/<timestamp>/sanitize-report.{md,json}`。
- 强制 `SIGKILL` 记为 `STOP_TIMEOUT`，不当作通过。
- 抑制文件 `test/sanitizer/{lsan,tsan,ubsan}.supp` 目前只有注释。

### 7.6 Fuzzing

```sh
test/fuzz/run.sh
test/fuzz/run.sh --libfuzzer --time 60
```

- 六个目标：`fuzz_h1`、`fuzz_h1_body`、`fuzz_h2`、`fuzz_ws`、
  `fuzz_ws_deflate`、`fuzz_multipart`。
- 默认 GCC + ASan/UBSan，由 `standalone_main.cpp` 做确定性变异；
  `--libfuzzer` 使用 clang（优先 `/usr/lib/llvm-24`）。
- 目标必须确定性、无 I/O/网络；所有循环和分配受输入大小约束。
- 发现 crash 先最小化、加入 corpus，再修解析器；不要把它放进抑制列表。

### 7.7 Coverage

```sh
test/coverage.sh
COVERAGE_SUITES="unit server-diff" test/coverage.sh
COVERAGE_FAIL_UNDER=70 test/coverage.sh
```

- 独立构建 `build-cov`（`--coverage -O0 -g`），默认 suites：
  `unit openapi server-diff client-diff concurrency`。
- 输出 `.cache/coverage/coverage.{txt,html}`；只有设置 `COVERAGE_FAIL_UNDER`
  才有门线。
- 默认不包含 conformance/stress；结束时恢复默认 xmake 配置。

### 7.8 CI

`.github/workflows/ci.yml` 在 push/PR 上：

```sh
xmake f -y -m release
test/run.sh standard --jobs 4
test/run.sh client-diff --jobs 4
test/run.sh server-diff --jobs 4
test/run.sh concurrency --jobs 4
```

CI **不**跑：conformance/stress、sanitizer、coverage、fuzz、format check、
clang-tidy、CMake 构建。

---

## 8. 修改指南

### 8.1 改协议层 / 传输层

必跑：

```sh
test/external/conformance/run.sh
test/external/stress/run.sh
```

若涉及 h2/h3/WS，再补：

```sh
test/run.sh unit
test/run.sh server-diff
```

### 8.2 改客户端

```sh
test/run.sh client-diff
```

涉及 h3 时确认 `diff_client_h3` 构建成功；无法构建时该组 SKIP，其他组仍跑。

### 8.3 改服务端 handler / 路由

```sh
test/run.sh unit
test/run.sh server-diff
```

### 8.4 加新 API / 行为

- 同步更新 `README.md`、`README.zh-CN.md` 与本文件。
- 在 `test/unit/` 加纯逻辑覆盖；跨实现行为加到 `test/diff/` 对应功能组。
- 新增差异必须登记到 `test/diff/known_diffs.json` 并写清 RFC 依据。
- 不要引入协议版本分支到 `handler/` / `proto/` / `net/` / 客户端门面。
- 新异步操作必须是 Asio initiating function；不要在库内部引入协程。
- 头文件注释与代码不一致时，以代码为准并修正注释。

### 8.5 代码风格

- `.clang-format`：LLVM + `IndentWidth: 4`、`ColumnLimit: 120`。
- `./format.sh` 就地格式化 `include/` 与 `test/`；`./format.sh --check` 检查。
  CI 不调用它。
- RAII、无裸 `new`/`delete`，所有权显式。
- 可失败操作返回 `result` / `error_code`；不要用异常做控制流。
- 新代码沿用现有命名与注释密度；协议行为要写清「为什么」。

---

## 9. 已固化回归点（改动相关代码时必须保持）

以下均来自当前 `include/` 与 `test/` 的代码/断言；编号只为引用方便。

1. **`Body::finish()` / `fail()` 不可丢**：channel 满时记录 out-of-band
   `m_pending_eof` / `m_pending_error`，排空后再交付。否则 reader 永久挂起，
   h2 还会泄漏 connection window。`proto/body.h:110-133,338-375`；
   `test/unit/test_proto.cpp:166-208`。
2. **h2 END_STREAM 与停放**：停放期间到达的 END_STREAM 记 `finish_pending`，
   排空后再 `body().finish()`，否则 `Body::read()` 提前 EOF 截断 body。
   `engine/h2/h2_engine.h:787-791,2145-2156`；`test/unit/test_h2_engine.cpp:559-597`。
3. **h2 只暂停单条流**：某条流 body channel 满时，DATA 进该流
   `paused_frames`，不暂停整条连接；connection window 不回补形成总内存上界。
   `h2_engine.h:763-791,2121-2131`；`test_h2_engine.cpp:527-557`。
4. **h2 连接接收窗口固定从 65535 起**：`SETTINGS_INITIAL_WINDOW_SIZE` 只影响
   stream window；配置更小时也不能把 connection window 收缩到 65535 以下。
   `h2_engine.h:340-345,837-845`；`test/unit/test_review_regressions.cpp:1004-1066`。
5. **h2 关闭流三分法**（RFC 9113 §5.1）：本端 RST 后最小处理并丢弃；对端 RST /
   双方 END_STREAM 后可 `STREAM_CLOSED`；从未打开的 skipped id 是
   `GOAWAY(PROTOCOL_ERROR)`。固定 256 项 ring 记录关闭原因。
   `h2_engine.h:652-682,2160-2185`；`test_h2_engine.cpp:260-327`。
6. **h2 split header block 穿过 reset**：stream 被 reset/erase 后，尚未收完的
   HEADERS/CONTINUATION 仍必须喂给 HPACK decoder，否则动态表失步。
   `h2_engine.h:615-619,1600-1669`；`test/unit/test_trailers.cpp:577+`。
7. **h2 响应 trailers**：trailing HEADERS 携带 END_STREAM，最后 DATA 不携带；
   必须在 body 队列排空后发送。`h2_engine.h:504-523,716-720,2498-2563`；
   `test_trailers.cpp:178-250`。
8. **h2 早响应归还流控 credit**：401/429/405 等提前结束时，必须归还未读 body 的
   stream + connection credit，不能等 END_STREAM。`h2_engine.h:2686-2739`；
   `test_h2_engine.cpp:717-754`。
9. **h2 half-closed(remote) 上的 DATA 先归还 connection credit 再 reset**，否则
   每帧泄漏一个 frame_len，最终 `GOAWAY(FLOW_CONTROL_ERROR)`。
   `h2_engine.h:2004-2021`。
10. **h2 Rapid Reset**：并发 handler 上限 = `h2_max_concurrent_streams`；
    reset 的排队流从队列移除；队列溢出 → `GOAWAY(ENHANCE_YOUR_CALM)`。
    `h2_engine.h:2592-2631`；`core/limits.h:80-103`；`test_h2_engine.cpp:165-210`。
11. **h2 写前检查 `stream_writable()`**：peer reset 或已发 END_STREAM 后再写是
    连接级协议错误；HEADERS+END_STREAM 也必须记录 `end_stream_sent`。
    `h2_engine.h:431-476,545-553`。
12. **h1 framing 严格性**：TE 是 coding list；chunked 必须唯一且最后；TE+CL、
    重复 CL、HTTP/1.0 + TE 都是 400；不支持的 coding 是 501。
    `h1_engine.h:1380-1459`；`test/unit/test_h1_body.cpp:218-238`。
13. **h1 trailer 字节预算按消息复位**：`m_trailer_bytes` 在
    `setup_body_framing` 归零；不能连接级累计。`h1_engine.h:742-747`；
    `test_h1_body.cpp:176-216`。
14. **h1 空 chunk 是 no-op**：不能下传成 `0\r\n\r\n`，否则提前终止 chunked body。
    `h1_engine.h:365-387`；`test_trailers.cpp:511-533`。
15. **h1 的 101 必须串行化**：WebSocket 101 与普通响应字节共用每连接
    `SerializedWriter`，不能另起第二个 transport write。
    `h1_engine.h:152-165,253-279,1295-1326`；
    `test/unit/test_h1_ws_serialization.cpp:124-167`。
16. **SerializedWriter 队列条目在完成前不移出**：scatter-gather 描述符指向
    deque 元素；写期间不能 `pop_front`、不能 erase 中间元素、不能 move 出去。
    失败清理只能从队尾 `pop_back`。回调不得强引用 writer owner。
    `transport/serialized_writer.h:5-19,168-176,219-234`；
    `test/unit/test_serialized_writer.cpp:103-233`。
17. **`async_error_only` 复制 buffer owner**：只 move 进局部会在 partial write
    时 UAF；必须把 `Start` 复制进 pending completion。
    `transport/transport.h:89-139`；`test_review_regressions.cpp:1190-1204`。
18. **scatter-gather 传 `std::array` 本身，不传 span**：Asio 只复制序列对象；
    传 span 会在第二次 `async_write_some` 读取已销毁栈帧。
    `transport/transport.h:43-87`；`test/cpp17/smoke.cpp` 的 `test_scatter_write`。
19. **取消 signal 必须 fan-out**：`Request::cancellation_slot()` 每次返回独立
    `cancellation_signal`，`cancel()` 向全部广播。共享单 signal 会被内层 bind
    覆盖外层 `cancellation_state`，清理时写悬空 impl → SIGSEGV。
    `proto/request.h:354-435`；`test/unit/test_cancellation.cpp:49-89`。
20. **WebSocket 有界读泵**：`m_inbox` 上限来自 `ws_read_ahead_bytes`；串行
    read→write 的 handler 不会与「猛发不读」的对端互相压死。
    `proto/websocket.h:19-27,370-397,739`；`test/unit/test_ws_codec.cpp:914-952`。
21. **WebSocket Close 顺序**：协议错误/Close/非法 UTF-8 先记关闭码，Close 帧要等
    应用下次 `read()` 排空缓存后才发出；h1/h2 升级路径必须让 WebSocket handle
    活到 Close 写完。`websocket.h:661-680`；`h1_engine.h:1315-1325`。
22. **WebSocket 64 位长度最高位必须为 0**：否则 `pos + payload_len` 回绕，
    `std::string` 分配 UINT64_MAX 并抛 `length_error`。
    `proto/ws_frame.h:227-234`；`test_ws_codec.cpp:1001-1020`。
23. **WebSocket 部分帧 payload 只交付一次**：不能每次 reread 重置
    `m_partial_off`；否则 UTF-8 校验重复喂入且大帧 O(n²)。
    `ws_frame.h:256-272,303-325`；`test_ws_codec.cpp:210-234`。
24. **压缩帧在解压前不做 UTF-8 增量校验**：否则会把 deflate 字节判成非法 UTF-8
    并 1007。`websocket.h:431-451`；`ws_frame.h:335-340`；`test_ws_codec.cpp:781-800`。
25. **空 WebSocket 消息不压缩**：RFC 7692 §7.2.3.5；避免空消息 marker 差异。
    `ws_deflate.h:308-325`；`test_ws_codec.cpp:667-696`。
26. **WebSocket mask key 必须来自 CSPRNG**（RAND_bytes），不能用可预测序列。
    `ws_frame.h:49-56`；`test_ws_codec.cpp:954-961`。
27. **一个响应只有一个头**：`send` / `send_bodyless` / `begin` 共享
    `m_started`；`status()` 拒绝 100..599 之外的值；`send_bodyless` 只允许
    HEAD/204/304；`async_begin` 拒绝 204/304 和 1xx。
    `proto/response.h:54-66,186-224,399-422`；
    `test_review_regressions.cpp:944-1002,1109-1146`。
28. **handler 同步 throw 必须完成**：未开始响应 → `reset_for_error()` + 500；
    已开始 → close。completion 必须恰一次，否则 h1 stall、h2/h3 泄漏并发槽。
    `engine/dispatcher.h:230-267,315-375`；
    `test_review_regressions.cpp:105-123,887-925`。
29. **Request state 用 deque**：`get_state<T>()` 返回指针后，后续 `set_state()`
    不得使已有元素失效；路径参数用 offset+length，target 重写/Request 移动后仍有效。
    `proto/request.h:441-459,532-539`；`test_review_regressions.cpp:1068-1101`。
30. **`CompressingResponseSink` 持 weak Request**：强引用会在 session middleware
    把 `ResponseWriter` 存进 request state 时形成 request/response/sink 环。
    `proto/compressing_writer.h:223-232,351-357`；
    `test_review_regressions.cpp:75-103`。
31. **未完成的流式 h1 响应不得复用连接**：begin 后无 finish 时
    `m_response_complete` 为 false，关闭连接，否则下一个响应会被当成上一个
    chunked body。`h1_engine.h:476-478,531`；`test_trailers.cpp:535-555`。
32. **h2 响应输出反压与 compaction**：高/低水位 1 MiB / 256 KiB；队列必须
    compaction，否则持续 refill 的 streaming 响应保留所有历史 chunk。
    `h2_engine.h:84-123,556-590,684-713,773-786`。
33. **h3 反压用 `out_bytes`**：`out_write - out_ack` 只表示“已交出但未 ACK”；
    对端流控关闭时它接近 0，`out_q` 会无界增长。`h3_stream.h:64-71`。
34. **h3 请求体通道满必须停放，不能 reset**：`Body::feed()` 返回 false 只表示
    通道满且参数可重试；DATA 进该流 `paused_frames`，排空后回灌；END_STREAM
    期间有停放帧时记 `finish_pending`。`h3_engine.h:1088-1114,1503-1524`。
35. **h3 单向流阻塞也必须通知 nghttp3**：control / QPACK encoder/decoder 流
    不在 `m_streams` 里；`on_stream_blocked` 必须对所有流调用
    `nghttp3_conn_block_stream()`，否则 `next_stream_data()` 反复返回同一段字节。
    `h3_engine.h:679-686`。
36. **h3 非法请求头不得先派发 handler**；handler 提前返回时归还未读 body 的
    connection/stream credit。`h3_engine.h:1105-1114,1349-1394,1526-1537`。
37. **OpenSSL 惰性初始化**：所有 `ssl::context` 构造入口必须经过
    `ensure_openssl_initialized()`；多线程首次并发 `SSL_CTX_new` 会在 OpenSSL 4
    的 `ossl_ht_get` 里产生 TSan race。需要共享 context 时用
    `TlsClientConfig::shared_context`，不要做全局单例。
    `transport/openssl_init.h`；`tls_context.h:118-124`。
38. **`IoCtxPool::at()` 返回稳定引用**：pool 大小在构造时固定、之后不再
    增删 context，`at()` 返回 `asio::io_context &`；没有 main context，
    `at()` 只接受 `[0, size())`。`core/io_pool.h`；
    `test_review_regressions.cpp`。
39. **`TlsContext` 不可移动**：OpenSSL ALPN callback 持有 `&m_alpn_state`；
    移动会让指针悬空。`transport/tls_context.h:98-106`。
40. **异步 `ResolveFn` completion 必须 marshal 回调用方 executor**：hook 可能
    同步或从其他线程完成。`core/resolve.h:82-94`；
    `test_review_regressions.cpp:624-666`。
41. **h1 WS route 同步 throw 不能逃出 `io_context::run()`**：未 handoff → 500；
    已 handoff → 不再写 HTTP 错误。`h1_engine.h:1132-1146,1195-1203`；
    `test_h1_ws_serialization.cpp:169-200`。
42. **`async_send_file` 先 open 再写头**：`prepare()` 只做 stat/上限检查；
    open 失败不得在响应对象上留下 content-type/content-length。
    `handler/file_response.h:161-170`。
43. **multipart part 参数拒绝 CR/LF/NUL**：name/filename/content_type/boundary
    会注入 part 头。`proto/multipart.h`；`client/multipart_stream.h`。
44. **CookieJar 拒绝单标签 Domain 与 IP 字面量**：否则 cookie 会泄漏到所有同后缀
    站点；完整 PSL 通过 `CookieJar(PublicSuffixFn)` 注入。loopback 视为安全源。
    `client/cookie_jar.h`。
45. **CORS 空 allowlist + credentials 必须 fail-closed**：空 `allow_origins` 加
    `allow_credentials` 不发 CORS 头；显式 `"*"` 才走镜像逻辑。
    `handler/cors.h:179-203,285-370`。
46. **h2 字段语法与 Host/:authority**：普通字段名必须是 token；值不得含 CTL；
    `:method` 必须是 token；`:path` 必须是 origin-form 或 `*`；`host` 重复或与
    `:authority` 不一致是流级 PROTOCOL_ERROR。`h2_engine.h:1698-1935`；
    `test_h2_engine.cpp:643-713`。
47. **错误码类别统一**：客户端自决错误在 `"simple_http.client"` category；
    server 配置错误在 `"simple_http.server"`；transport 错误保持 asio
    `error_code`。不要混用 generic 与 system 类别。
48. **客户端请求头在序列化处最后校验**：h1/h2/h3 的请求头编码统一拒绝非法字段名
    与含 CTL 的值，返回 `client_errc::invalid_header`。
49. **h1 客户端 chunk 大小必须溢出检查并流式交付**：`size = size*16+d` 要比较
    `(max-d)/16`；不能等整个 chunk 进入 `m_in` 再返回。
    `client/h1_client.h`；`test_review_regressions.cpp` 的 h1 client 用例。
50. **h1 客户端响应头要和请求侧一样严格**：只接受 CRLF，拒绝裸 LF/CR、obs-fold、
    值内 CTL、重复/溢出 Content-Length、TE+CL；状态码 3 位 100..599；接受
    HTTP/1.0 并设置 `bodyless`。`client/h1_client.h`。
51. **服务端取消遵循 Asio per-operation cancellation**：`Body::async_read()` 把
    channel 的 `channel_cancelled` 归一为 `operation_aborted`；h1 pull 的 transport
    读写、h2/h3 的 `await_out_space` 都透传调用方 cancellation slot；h1 排队 pull
    在启动前被取消时以 `operation_aborted` 完成，且一个 slot 同时只有一个消费者。
    `core/cancel.h`、`proto/body.h`、`engine/h1/h1_engine.h`、`h2_engine.h`、
    `h3_engine.h`；`test/unit/test_server_cancellation_verify.cpp`。
52. **h1 写取消即关闭连接**：`Http1ResponseSink::wrap()` 在关联槽上安装处理器；
    未开始的写以 `operation_aborted` 完成，已经上线的 chunked 帧无法撤回，因此
    必须关闭 transport 且不得复用连接。
53. **h1 body 失败码要保持**：`BodyState::failure` 记录首个失败（取消/断开），
    失败后的读返回同一个错误而不是干净 `eof`；排队读不能把取消当成正常结束。
    `h1_engine.h` 的 `body_failed` / `pull_body_chunk_impl`。
54. **客户端取消透传调用方槽**：`Client::async_send` / `async_open_stream` /
    `async_open_websocket` 和 `Stream::read_head_with` / `read_with` 把 caller slot
    转发给内层 dial/read；`HopState`/`HopCancel::abort_transport` 让
    DNS/dial/TLS 阶段可被 timer 中止。完成必须 post 回调用方 executor，不能在
    emit 内销毁 cancellation_state。`client/http.h`、`client/h1_client.h`；
    `test/unit/test_client_units.cpp` 的 `[client][cancel]`。
55. **取消适配器统一入口**：改变 completion handler 形状只能用
    `forward_cancel_slot()`；自定义槽处理器用 `install_cancel_handler()` /
    `SlotGuard`；从取消处理器完成调用方用 `post_cancelled()`。禁止把
    `any_completion_handler` 包进普通 lambda（会丢 executor/allocator/slot）。
    请求子槽优先用 `cancellation_registration()` 并在完成时释放，
    `active_cancellation_slots()` 必须回到 0。`core/cancel.h`；
    `test/unit/test_cancellation.cpp` 的 registration 用例。

---

## 10. 已知限制与「注释过时」清单

### 10.1 已知限制

- 客户端 `H2cMode::Upgrade` 未接线；`ClientConfig::default_h2c` 未被读取。
  明文 `HttpVersionPolicy::Http2` 走 prior knowledge。
- `RetryPolicy::condition` / `require_replayable` 未使用；只有状态码重试。
- 重定向默认关闭（`max_redirects = 0`）。
- 客户端没有传统连接池；只缓存一个 origin 的连接。
- 客户端 `Response::text()` 与 `bytes()` 相同。
- h1 客户端没有独立 idle timeout；h2 用 `ClientConfig::idle_timeout`。
- HTTP/3 客户端不支持代理、请求 trailers、1xx；服务端不支持 WS、request
  trailers、100-continue、push、0-RTT。
- 不支持 UNIX-domain 服务端监听。
- 没有 `SO_REUSEPORT` 的平台只启动一个 acceptor（固定在第 0 个 worker），
  所有连接都由该 worker 的线程服务。
- `NO_PROXY` 不支持 CIDR / 通配。
- `CookieJar` 不内置 PSL 数据。
- CORS 不覆盖 WebSocket 升级；WebSocket 用 `ws_origin`。
- Host 路由只作用于 HTTP dispatch；WebSocket 查找仍 host-agnostic。
- JWT 只是验证器；`aud` 单字符串；无自动 JWKS 刷新。
- 请求解压默认关闭，需挂 `middleware::decompress_request`；只支持单个
  gzip/br coding。
- 客户端自动解压默认只 gzip；br 需显式 `accept_encodings`。
- `async_send_file` / `async_read_body_to_file` / streaming multipart 文件读取
  是同步 I/O。
- HTTP 反代缓冲完整请求/响应（响应 cap 64 MiB）；`HttpProxyTarget::h2c`、
  `h2c_prior_knowledge`、`max_attempts` 当前未使用。
- CMake 未接入 ngtcp2/nghttp3；`simple_http_cpp17` 指向不存在的目录；
  config 缺 `find_dependency(glaze)`。
- 统一测试 runner 不产出聚合 JSON。
- CI 不跑格式检查、clang-tidy、CMake 构建、conformance/stress。

### 10.2 当前代码中已过时的注释（改到相关位置时顺手修正）

- `include/simple_http.h` 说 “HTTP/3/QUIC is not part of this port”，但 h3 引擎
  与 QUIC 监听实际存在（宏门控）。
- `engine/h2/h2_engine.h` 某处注释说 dispatch 等 END_STREAM；实际在
  END_HEADERS 后即 dispatch。
- `client/sse.h` 注释写 `client.async_open_sse(...)`；实际是
  `SseStream::async_open(client, ...)`。
- `client/multipart_stream.h` 注释写 `client.async_open_multipart(...)`；
  实际是 `MultipartStreamWriter::async_open(client, ...)`。
- `client/h3_client.h` 注释指向不存在的 `client/http_client.h`。
- `handler/validation.h` 顶部示例用 `std::expected<void, vector<FieldError>>`；
  实际接口是 `static ValidationResult validate(const T&)`。
- `openapi/openapi.h` 说 “There is no compile-time on/off switch”；实际整文件在
  `SIMPLE_HTTP_HAS_GLAZE` 内。
- `core/compression.h` 说 `EngineLimits` 可以携带 `CompressionConfig`；实际
  没有该成员。
- `handler/session.h` 说 save 失败会记录日志；实际代码忽略 error_code。
- `CMakeLists.txt` 的 `simple_http_cpp17` 目标指向不存在的 `cpp17/include`。

---

## 11. 常用命令速查

```sh
# 构建
xmake f -m release
xmake build -j 12
xmake build -j 12 test_unit
xmake build -j 12 fixture_conformance_server
xmake build -j 12 diff_server_cpp
xmake build -j 12 diff_openapi_server

# 自测（唯一入口）
test/run.sh fast
test/run.sh standard
test/run.sh full
test/run.sh list
test/run.sh doctor

# 单套件
test/run.sh unit
test/run.sh unit '[h2]'
test/run.sh cpp17
test/run.sh openapi
test/run.sh client-diff
test/run.sh server-diff
test/run.sh concurrency
test/run.sh conformance
test/run.sh stress

# 直接运行差分 runner（Go 二进制由 runner 自行构建）
test/.venv/bin/python test/diff/client/run.py --groups basic,body,policy,ws,h3
test/.venv/bin/python test/diff/server/run.py --groups routing,body,semantics,middleware,framework,static,ws,raw,h2raw
test/.venv/bin/python test/diff/openapi/run.py

# 覆盖率
test/coverage.sh
COVERAGE_SUITES="unit server-diff" test/coverage.sh

# Fuzzing
test/fuzz/run.sh
test/fuzz/run.sh --libfuzzer --time 60

# Sanitizer（默认 -j7；独立 worktree）
test/run.sh sanitize asan
test/run.sh sanitize asan unit --build-dir build-asan --jobs 7
test/run.sh sanitize tsan
test/run.sh sanitize ubsan unit

# 外部一致性 / 压力
test/external/conformance/run.sh
test/external/conformance/run.sh h2spec
test/external/stress/run.sh cpp h3 --no-conformance
test/external/stress/run.sh both

# 格式
./format.sh
./format.sh --check
```
