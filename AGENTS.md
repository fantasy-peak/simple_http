# AGENTS.md — simple_http

面向参与本工程的 AI / 协作者的工程说明。内容以当前源码、构建脚本和测试为准；
与 `README.md`（英文默认）和 `README.zh-CN.md`（完整中文版）的分工是：README 面向
使用者，本文件面向**修改代码的人**，重点记录分层边界、并发契约、测试门线和已固化的
回归点。

如果文档与代码冲突，以代码为准，并顺手修正文档。

---

## 1. 这是什么

`simple_http` 是一个 **header-only 的 C++ HTTP 服务器 + 客户端库**，基于
`boost.asio` C++23 协程实现。

| 项 | 当前值 |
|---|---|
| 版本 | `0.9.0`（`include/simple_http/core/version.h`） |
| 形态 | 纯头文件；`include/` 下没有 `.cpp` |
| 入口头 | `#include <simple_http.h>` |
| 标准 | CMake 接口要求 C++23；仓库自带 `xmake.lua` 使用 `c++26` |
| 许可 | MIT |
| 依赖 | Boost（asio、regex）、OpenSSL、glaze、zlib、brotli；HTTP/3 额外 ngtcp2 + nghttp3；测试用 Catch2 |
| 构建 | xmake（主）；CMake 只安装 INTERFACE 目标供下游 `find_package` |

### 协议矩阵

| 能力 | 服务端 | 客户端 |
|---|---|---|
| HTTP/1.0 | 支持 | — |
| HTTP/1.1 | 支持 | 支持 |
| HTTP/2 over TLS（ALPN） | 支持 | 支持 |
| h2c Upgrade | 支持 | 支持（`H2cMode::Upgrade`） |
| h2c prior-knowledge | 支持 | 支持（`H2cMode::PriorKnowledge`） |
| HTTP/3 over QUIC | 可选（`SIMPLE_HTTP_ENABLE_HTTP3`） | 可选，且只显式指定 |
| WebSocket over HTTP/1.1 | 支持 | 支持 |
| WebSocket over HTTP/2（RFC 8441） | 支持 | 未实现 |
| WebSocket over HTTP/3 | 未实现 | 未实现 |
| SSE | 响应侧 helper | `SseStream` |

服务端还支持 UNIX domain socket、多监听器、TLS/mTLS、HTTP 反代、WebSocket 反代、
静态文件、OpenAPI 3.1 文档；客户端支持重定向、Cookie、出站代理（CONNECT / HTTPS /
SOCKS5）、自动解压、分层超时与可配置重试。

---

## 2. 目录结构

```
include/
  simple_http.h                    伞形头，聚合全部公共 API
  simple_http/
    core/                          协议无关基础设施
      version.h                    版本与 Server/User-Agent 产品串
      types.h                      error_code、Version、WriteMode、StreamStatus、Eof/Rst/Disconnect
      tls_version.h                TlsMinVersion
      http_method.h                Method 枚举 + is_idempotent
      http_status.h                状态码常量
      http_field.h                 字段名常量 + hop-by-hop 判定
      logging.h                    LogSink / set_log_sink / make_stdout_sink / SIMPLE_HTTP_*_LOG
      logging_json.h               make_json_sink（glaze）
      mime.h                       MIME 常量与扩展名映射
      http_date.h                  IMF-fixdate 格式化/解析、now_unix
      url_path.h                   路径解码与规范化（查找键）
      url_encoding.h               百分号编码 query_escape / path_escape
      accept_encoding.h            Accept-Encoding qvalue 解析
      validators.h                 ETag / 弱比较 / Range
      static_table.h               文档根 → 不可变查找表
      base64.h                     base64
      io_pool.h                    IoCtxPool（单线程 io_context 池）
      limits.h                     EngineLimits
      compression.h                CompressionConfig
      content_encoding.h           gzip/brotli 编解码
      address.h                    Unix/Inet 地址辅助
      resolve.h                    ResolveFn（客户端与 WS 反代 DNS 钩子）
    proto/                         版本无关 HTTP 模型
      headers.h                    Headers（get / get_all / add）
      body.h                       Body：推模式 channel / 拉模式 provider
      query.h                      QueryParams
      multipart.h                  multipart/form-data 解析 + MultipartForm 编码
      multipart_stream.h           增量 multipart 解析
      form.h                       read_urlencoded_body / read_multipart_body
      params.h                     path_params<T> / query_params<T> / parse_params<T>
      json.h                       read_json_body<T> / write_json（glaze）
      request.h                    Request（两端共用；含 AppStateView / app_state<T>()）
      response.h                   ResponseWriter（fluent 门面）
      response_writer.h            ResponseSink 抽象（引擎实现）
      compressing_writer.h         响应压缩装饰
      error_body.h                 JsonError / ProblemDetails + write_error / write_problem
      ws_frame.h                   WebSocket 帧编解码 + 握手 key
      ws_deflate.h                 permessage-deflate（RFC 7692）
      websocket.h                  WebSocket 句柄 + WsBackendImpl（读泵/写泵）
    transport/
      transport.h                  Transport concept + SslHandle
      tcp_transport.h              TcpStreamTransport（含零拷贝 scatter-gather）
      tls_context.h                TlsContext / TlsConfig（ALPN）
      tls_transport.h              TlsStreamTransport
    engine/
      dispatcher.h                 Dispatcher / WsLookup / WsHandlerFn / 反代目标类型
      ws_proxy_common.h            h1 Upgrade 与 h2 extended CONNECT 共用的 WS 上游握手
      h1/
        h1_parser.h                请求/响应头解析器
        h1_engine.h                HTTP/1.x 引擎
        ws_proxy.h                 WebSocket 字节隧道
      h2/
        h2_frame.h                 帧层
        hpack_*.h                  HPACK 静态表 / Huffman / 解码 / 编码
        h2_engine.h                HTTP/2 引擎
        h2_ws_transport.h          单条 h2 流 → WsBackendImpl Transport
      h3/
        h3_callbacks.h             nghttp3 回调适配
        h3_stream.h                请求流适配
        h3_engine.h                HTTP/3 引擎（ngtcp2 + nghttp3）
    handler/
      handler.h                    Handler / Next / Middleware / make_handler / invoke_handler
      validation.h                 Validator<T> / FieldError / 约束辅助函数
      extract.h                    Path/Query/Json/Form/State/Header/Cookie 提取器 + extract<...>()
      session.h                    SessionStore / MemorySessionStore / Session + session() 中间件
      csrf.h                       double-submit CSRF 中间件
      jwt.h                        JWT 验证（HS256/RS256/ES256/JWKS）+ require_scope/require_role
      jwks.h                       JWKS 解析（RSA/EC/oct）+ fetch_jwks_json
      builtin_middleware.h         request_id / access_log / recovery / basic_auth / real_ip /
                                   clean_path / strip_prefix / redirect_slashes / ws_origin /
                                   timeout / compress
      secure_headers.h             SecureHeadersConfig + secure_headers
      rate_limit.h                 TokenBucket / RateLimiter / rate_limit
      cors.h                       CorsConfig / make_cors_middleware
      conditionals.h               maybe_not_modified（handler 级 304）
      router.h                     Router（精确路由 + 段 trie + {*rest} + Host + 正则 + 反代 + 静态）
      http_proxy.h                 请求级 HTTP 反代
      static_files.h               静态文件服务
      file_response.h              send_file
      upload.h                     read_body_to_file
    net/
      connection.h                 serve_plaintext / serve_tls（协议嗅探）
      server.h                     Server / ServerConfig / Listener 配置 / 生命周期
    client/
      http.h                       公共客户端 API（平铺在 simple_http::）
      multipart_stream.h           MultipartStreamWriter / stream_file_body（流式上传）
      client_config.h              ClientConfig / TlsClientConfig / ProxyConfig / RetryPolicy /
                                   HttpVersionPolicy / H2cMode / client_errc
      client_stream.h              ResponseHead / ClientStream / ClientSession
      multiplexed_session.h        h2/h3 共用的多路复用骨架
      http_client.h                内部 ClientEngine（单连接模型核心）
      h1_client.h                  HTTP/1.1 会话
      h2_client.h                  HTTP/2 会话
      h3_client.h                  HTTP/3 会话（宏门控）
      tls_client.h                 客户端 TLS/ALPN/SNI/mTLS
      proxy.h                      出站代理（CONNECT / HTTPS / SOCKS5 / NO_PROXY）
      url.h                        绝对 URL 解析
      cookie_jar.h                 CookieJar
      decompressing_stream.h       响应解压装饰器
      sse.h                        SseParser / SseStream
      ws_client.h                  WebSocket 客户端（H1 Upgrade）
    quic/                          ngtcp2 封装（#ifdef SIMPLE_HTTP_ENABLE_HTTP3）
      protocol.h / connection.h / endpoint.h / ngtcp2_config.h /
      ngtcp2_crypto.h / tls.h
    openapi/
      openapi_doc.h                OAS 3.1 文档模型 + 手写 JSON 渲染
      openapi.h                    glaze 反射 schema、openapi::doc() 路由描述
```

其它目录：

- `test/`：统一入口 `test/run.sh`（`test/run.py`，分层 fast / standard / full）；
  按被测对象分层：
  - `unit/` Catch2 纯逻辑；`integration/` 进程内 C++ 集成；
  - `server/` 被测服务端 ← 独立客户端，`server/fixtures/` 是 C++ 夹具；
  - `client/` 被测客户端 → 独立服务端，`client/probes/` 是 C++ probe，
    `client/peers/` 是 Python 独立服务端；
  - `differential/` C++ ↔ Go 双向差分（`run.py` + `framework_diff.py` + `go/`）；
  - `external/conformance/`、`external/stress/`、`external/bench/` 外部工具套件；
  - `examples/`、`tools/`、`support/`、`tls_certificates/`。
  报告写到 `.cache/test-run/<timestamp>-<pid>/`（`report.json` + `junit.xml`）。
- `test/tls_certificates/`：测试证书（`./test/tls_certificates/...` 是相对路径契约）。
- `cmake/`：`simple_httpConfig.cmake.in`。
- **没有 `docs/`，也没有 `ROADMAP.md`**（已删除）；文档为 `README.md`（英文默认）、
  `README.zh-CN.md`（完整中文）与本文件。

---

## 3. 架构与硬约束

### 3.1 分层

```
handler / net / client 门面
        ↓ 只用 Request / ResponseWriter / Router / Body
proto（版本无关模型）
        ↓ ResponseSink
engine（h1 / h2 / h3，版本相关）
        ↓ Transport
transport（TCP / TLS / QUIC 字节流）
```

- **上层不得出现协议版本分支。** 需要版本差异时，把它放进 `engine/` 或对应的
  `ResponseSink` 实现；`ResponseWriter` 只是转发门面。
- 引擎通过 `Transport` 读写字节，不直接依赖 socket 类型。
- `Request` / `ResponseWriter` / `Router` / handler 对 HTTP 版本完全透明。

### 3.2 并发模型 A（必须遵守）

- 每条连接在 accept 时绑定到 `IoCtxPool` 中一个**单线程** `io_context`。
- 该连接上的引擎、writer、handler 全部只在这一个线程执行。
- 因此 `engine/` 下**没有 mutex / atomic**（唯一例外见下）。
- `ResponseWriter` / `WebSocket` 可以 `shared_ptr` 捕获并跨线程使用；每个操作先
  `co_await asio::dispatch(asio::bind_executor(executor, asio::use_awaitable))`
  hop 回连接 executor。各层都有同名 `hop()` 辅助函数。
- Handler 都在连接 executor 上 `co_spawn`；请求体消费钩子也 post 回该 executor。

刻意的跨连接共享例外（都在 handler 层，不在 `engine/`）：

- `RateLimiter` 的按 key 分桶加锁。
- `Router` 惰性构建中间件链的 atomic + mutex。
- 日志 sink 的 `atomic<shared_ptr<LogSink>>` 安装。
- `WebSocket::is_open()` 是唯一不 hop 的状态读取，因此 `m_open` 是 `atomic<bool>`。
- QUIC 路径的 `quic/endpoint.h` 中 `m_send_wake` 使用 `concurrent_channel`（与 h3
  其他无锁 channel 的约定不一致，修改时注意）。
- h3 的连接 executor 就是 endpoint 的 executor；`reuse_port` 默认开启，否则一个
  QUIC 监听的所有连接会挤在一条线程上。

### 3.3 反压

- HTTP/2 请求体**按流停放**：某条流消费不过来时只暂停该流，不能暂停整条连接。
- `END_STREAM` 在停放期间到达时记 `finish_pending`，排空后再 `body().finish()`。
- WebSocket 有**有界入站读泵**：应用在写被零窗口挡住时，socket 仍被读取进有界队列；
  队列满则停止读取，`read()` 腾出空间后恢复。
- 流式响应走写泵；写操作本身提供反压。
- 所有大小入口都有上限：`max_header_bytes`、`max_body_bytes`、
  `h2_max_header_list_size`、`ws_read_ahead_bytes`。

### 3.4 生命周期与所有权

- Engine 由 `shared_ptr` 持有，handler 协程捕获它，确保 handler 存活期间引擎不被销毁。
- h2 sink 持 `weak_ptr` 到引擎，每次操作 `lock()`；引擎销毁后返回错误而不是 UAF。
- WebSocket 写泵持 `shared_ptr` 到 backend；只要还有写在飞，transport/TLS 流就不会
  析构。watchdog 只持 `weak_ptr`。
- WebSocket 读泵 likewise 持 backend 引用，终止统一走 `stop_inbox()`；
  `~WebSocket` / `abort()` 会关闭 transport，解开卡在 `async_read_some` 的 reader。
- `Server::stop()` 有界等待已进入 handler 的请求（`shutdown_grace`），然后关闭剩余
  连接并停止 I/O 池。不要从信号处理函数里直接调用 `stop()`。

### 3.5 错误处理约定

- 可失败操作返回 `std::expected<T, error_code>`；客户端失败从不抛异常。
- 服务端 handler 抛出 `std::exception` 时由调用层转成 500；若响应头已发出则关闭连接。
- `ResponseSink` 的写操作返回 `[[nodiscard]] awaitable<error_code>`。
- 解析错误要区分：请求头过大 431、请求体过大 413、协议错误按版本返回 400 /
  RST_STREAM / GOAWAY。

### 3.6 日志

- 库只定义 `LogSink`（`enabled(level)` + `log(record)`），不依赖第三方日志库。
- `set_log_sink` 使用原子替换，运行中安全；`enabled()` 在格式化之前询问。
- 编译开关：`SIMPLE_HTTP_ENABLE_LOG`（默认 1）、`SIMPLE_HTTP_LOG_ACTIVE_LEVEL`。
- `access_log()` 输出结构化字段；`make_json_sink` 是内置 JSON lines sink。

---

## 4. 公共 API 速查

### 4.1 Server

```cpp
ServerConfig cfg;
cfg.listen = InetAddress{"0.0.0.0", 7788, false}; // 或 UnixAddress{"/run/app.sock"}
cfg.worker_threads = 4;
cfg.max_connections = 0;      // 0 = 不限；到上限用内核 backlog 背压
cfg.reuse_port = true;        // 每个 worker 一个 acceptor
cfg.tcp_nodelay = true;
cfg.plaintext_protocols = PlaintextProtocols::Both; // Http1 / Http2 / Both
cfg.shutdown_grace = std::chrono::milliseconds{5000};
cfg.limits = EngineLimits{};

Server server{cfg};
server.route(...);
server.use(...);
auto started = server.start(); // expected<StartReport, StartupError>
server.stop();
```

- `start(StartPolicy)`：`AllOrNothing`（默认）或 `BestEffort`。
- `run()` 是 `start()` 的 awaitable 包装，不驱动事件循环。
- 多监听：`cfg.listeners`，或
  `server.listen(InetAddress{...}).plaintext(...).tls(...).alpn(...)`。
- `Server::router()` 可共享给多个 `Server`。
- 观测：`port()`、`quic_port()`、`active_connections()`、`active_requests()`。

### 4.2 Handler / Middleware / Router

```cpp
using RequestPtr  = std::shared_ptr<Request>;
using ResponsePtr = std::shared_ptr<ResponseWriter>;

// awaitable<void>(RequestPtr, ResponsePtr)
// awaitable<void>(RequestPtr, ResponsePtr, SslHandle)
// awaitable<void>(RequestPtr, shared_ptr<WebSocket>)
```

中间件：

```cpp
using Next = std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle)>;
using Middleware =
    std::function<asio::awaitable<void>(RequestPtr, ResponsePtr, SslHandle, Next)>;
```

- `use()` 按注册顺序嵌套，最外层先跑；CORS 始终最外层。
- `group(prefix, mws, registrar)`：前缀 + 组中间件，可嵌套。
- per-route middleware 只包该路由。
- `req->set_state<T>()` / `get_state<T>()`：按类型键，last-set-wins。

内置中间件（除注明外均在 `simple_http::middleware`）：

`request_id()`、`access_log()`、`recovery()`、`basic_auth(user, pass)`、
`real_ip(trusted)`、`clean_path()`、`strip_prefix(prefix)`、`redirect_slashes()`、
`ws_origin(allowed, allow_missing=false)`、`timeout(d)`、`compress(cfg)`、
`decompress_request(cfg)`（opt-in 请求体解压）、`secure_headers(cfg)`、
`rate_limit(bucket)`、`rate_limit(limiter, key_fn)`；
`simple_http::make_cors_middleware(cfg)` 在顶层命名空间。

路由匹配顺序：

1. 精确路径（方法感知）
2. `{param}` 模板 trie
3. HTTP 反代路由
4. 本地正则
5. 静态文件
6. fallback
7. 内建 404

隐式规则：

- `any_methods`（空列表）= 任意方法，含扩展方法 token。
- GET 隐含 HEAD；GET 响应体由 writer 抑制。
- 路径存在、方法不符 → 405 + `Allow`；OPTIONS 自动 204 + `Allow`。
- 路径不存在 → 404 / fallback。
- 405 / 自动 OPTIONS 是普通 keep-alive 响应，引擎会 drain 未读请求体。
- WebSocket 查找：本地精确 ws → ws 反代 → 本地正则 ws。
- CORS 不覆盖 WebSocket 升级，使用 `ws_origin()`。

### 4.3 框架层（State / Extractor / Validation / Auth）

```cpp
// 共享应用状态：按类型键，shared_ptr 共享，不按请求复制；start() 前注册。
server.state(std::make_shared<AppState>(...));
const AppState *st = req->app_state<AppState>();

// 类型化提取器：失败自动 400/422/500，不进入 handler。
server.route({Method::Post}, "/users/{id}",
    extract<Path<UserParams>, Valid<Json<CreateUser>>>(
        [](const UserParams &p, CreateUser in, RequestPtr req, ResponsePtr res)
            -> asio::awaitable<void> { ... }));

// 校验：为类型特化 Validator<T>；失败 422，details 是字段错误数组。
template <> struct simple_http::Validator<CreateUser> { ... };

// Session / CSRF / JWT（均在 simple_http::middleware）。
server.use(middleware::session(store, SessionConfig{...}));
server.use(middleware::csrf(CsrfConfig{...}));
server.use(middleware::jwt(JwtConfig{.algorithms = {"RS256"}, .public_key_pem = pem, ...}));
server.use(middleware::require_scope("users:write"));

auto *s = session(*req);       // Session*（middleware 未挂载时为 nullptr）
const Principal *p = principal(*req);
```

- `extract<...>` 返回普通 `Handler`，不影响 route/group/middleware/OpenAPI。
- 同一次提取最多一个 body 提取器（`Json`/`Form`）。
- `SessionStore` 是 awaitable 接口；`MemorySessionStore` 带锁，Redis/DB 适配器放消费侧。
- JWT 强制算法 allowlist，拒绝 `none`；`exp` 默认必填；JWKS 支持静态/手动拉取文档
  （RSA/EC/oct，按 `kid` 选），不做签发/OAuth2/OIDC。
- Host 路由：`server.host(pattern, registrar)`；`*.example.com` 不匹配裸域或
  `evil-example.com`。Host 子 Router 共享 state，但 WebSocket 查找仍 host-agnostic，
  OpenAPI 文档也各自独立。
- 通配路径：`/files/{*rest}` 只能作为最后一段，捕获剩余路径（含 `/`）。

### 4.4 Request / ResponseWriter / Body

```cpp
req->method(); method_token(); version(); target(); path(); query();
req->query_params(); req->param("id"); req->header("host"); req->headers();
req->cookie("session"); req->peer_address(); req->body();
req->set_state<T>(...); req->get_state<T>();
req->cancel(); req->cancelled(); req->wait_cancelled();
```

`path()` 是 target 的原始 path 部分；需要解码/规范化时用 `clean_path()`。

读体：

```cpp
auto all = co_await req->body().read_all();  // expected<string, error_code>
for (;;) {
    auto frame = co_await req->body().read();
    if (!frame || frame->eof) break;
    use(frame->data);
}
```

响应：

```cpp
co_await res->status(200).content_type("text/plain").send("hello"); // 一次性
co_await res->status(200).begin();                                 // 流式
co_await res->write("chunk");
co_await res->finish();
```

其它：`header` / `replace_header`、`redirect`、`set_cookie`、`sse_begin` /
`sse_event` / `sse_comment`、`trailer` + `finish_trailers`、`send_continue`、
`send_bodyless`、`connected()`、`close()`、`version()`。

`send()` 自动补 `Date`、`Server`、`Content-Type`（204/304 不补 Content-Type）；
引擎会剥离 handler 设置的 hop-by-hop 字段（connection / keep-alive /
transfer-encoding / proxy-connection / upgrade）。

### 4.5 WebSocket

```cpp
server.ws_route("/chat",
    [](RequestPtr, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
        for (;;) {
            auto msg = co_await ws->read();
            if (!msg) break;
            if (co_await ws->write(msg->data, msg->text)) break;
        }
    });
```

- `read()` 返回 `expected<WsMessage, error_code>`，自动重组分片、Ping→Pong、
  收到 Close 回 Close。
- `write` / `write_text` / `write_binary` 按值收 payload；
  `write_view` / `*_view` 借用调用方缓冲，必须在 co_await 完成前保持有效。
- 写操作经内部写泵串行化。
- 读泵懒启动于首次 `read()`，socket 读取由独立 reader 持续排空进有界 `m_inbox`；
  队列上限来自 `EngineLimits::ws_read_ahead_bytes`（默认 4 MiB）。
- 协议错误 / Close / 非法 UTF-8 由 reader 记下关闭码，但 Close 帧要等应用下次
  `read()` 排空缓存后才发出（`flush_deferred_close`）。

### 4.6 Client

```cpp
Client http{executor, ClientConfig{}};

auto r = co_await http.get(url).send();
auto r2 = co_await http.post(url).json(obj).bearer_auth(token).send();
auto up = co_await http.open_stream(url, {.method = Method::Post});
auto ws = co_await http.open_websocket("wss://host/chat");
```

- `get` / `post` / `put` / `del` / `head` / `patch` / `request`。
- `RequestBuilder`：`query`、`header(s)`、`content_type`、`body`、`json<T>`、
  `multipart`、`basic_auth`、`bearer_auth`、`max_body_bytes`、`timeout`、`version`。
- `send()` 返回缓冲 `Response`，应用重定向 / Cookie / 重试 / 自动解压。
- `open_stream()` / `send_stream()` 是显式流，**不**自动跟随重定向、不应用 Cookie、
  不跑便捷层重试；自动解压仍生效（与 Go 的 `Response.Body` 一致）。
- 自动解压对齐 Go `Transport`：`auto_decompress` 默认 true，默认只广告/解码 gzip；
  调用方显式设置 `Accept-Encoding`、HEAD、带 Range 时不注入也不解码。br 需显式
  `accept_encodings = {"br","gzip"}`。解压后删除 `Content-Encoding`/`Content-Length`
  并置 `Response::uncompressed()`；解压装饰器会把请求 `finish_trailers()` 转发给内层。
- `Response`：`status`、`version`、`headers`、`header` / `header_owned`、`ok`、
  `bodyless`、`uncompressed`、`error_for_status`、`text` / `bytes` / `json<T>`、
  `read` / `read_all`。
- `Stream`：`write` / `finish`、`read_head`、`read` / `read_all`、`trailers`、
  `abort`；`read()` 受 body-idle 预算约束，`read_all()` 不套该 deadline。
- 客户端绑定单线程 executor（模型 A）；`io_context` 必须活过会话。
- 单连接模型：对第一个 origin 保持一条连接（h1 FIFO 门 / h2 多路复用），其他
  origin 使用一次性连接。
- 免费透明重拨：打开阶段与读响应阶段各一次；策略重试由 `ClientConfig::retry` 控制。
  状态码重试默认关闭（`retry_statuses` 为空，与 Go/reqwest 一致）；显式设 `{429, 503}` 后支持 `Retry-After`。
- 流式上传：`MultipartStreamWriter`（field/file/file_data）与 `stream_file_body`。
- 自动 100-continue：`StreamSpec::auto_expect_continue`；h1/h2 等待 interim，h3 直接
  发 body。最终拒绝返回 `client_errc::expectation_failed`。
- 请求 trailers：`Stream::finish_trailers()`，h1 chunked 终止符 / h2 trailing HEADERS；
  h3 返回 `trailers_not_supported`。服务端 `req->trailers()` 在 h1/h2 暴露。

### 4.7 数据与 OpenAPI

```cpp
read_urlencoded_body(req)         // expected<QueryParams, error_code>
read_multipart_body(req)          // expected<vector<MultipartPart>, error_code>
read_multipart_stream(body, boundary, cb)
read_json_body<T>(req)            // expected<T, error_code>
write_json(res, value, status)
path_params<T>(req) / query_params<T>(req) / parse_params<T>(q)
write_error(res, status, JsonError{...})
write_problem(res, status, ProblemDetails{...})
```

OpenAPI：

```cpp
server.route({Method::Get}, "/pets/{id}", handler,
    openapi::doc()
        .path_params<PetParams>()
        .response<Pet>()
        .summary("get a pet")
        .operation_id("getPet")
        .error<ErrorBody>(404, "no such pet"));

server.openapi().title("petshop").version("1.0.0");
server.serve_openapi("/openapi.json");
server.serve_swagger_ui("/swagger", "/openapi.json");
```

- 文档按路由级 `openapi::doc()` 元数据收集；没写描述的路由不进文档。
- 路径模板 `{name}` 必须等于 `path_params<T>` 的字段名，注册时校验。
- 无开关宏；glaze 是主库常规依赖。

---

## 5. 协议实现要点

### 5.1 HTTP/1.x

- 手写解析器（`engine/h1/h1_parser.h`），不依赖 beast。
- 只接受 `HTTP/1.0` / `HTTP/1.1` 请求行。
- 请求体 framing：chunked / Content-Length / 无。
- 响应 framing：一次性 Content-Length；流式在 HTTP/1.1 用 chunked，
  HTTP/1.0（以及 HEAD）用 close-delimited。
- HTTP/1.1 默认 keep-alive，HTTP/1.0 默认 close；`Connection` 头可覆盖。
- 请求目标按 RFC 9112 规范化：absolute-form 归约为 origin-form，CONNECT 保留
  authority-form；普通请求目标不是 origin-form 时 400。
- 严格拒绝 CL + TE、重复/非法 Content-Length 等双 framing。
- `Expect: 100-continue` 懒发或显式发，最多一次。
- 升级：WebSocket、h2c Upgrade（101 后重放 stream 1）。

### 5.2 HTTP/2

- 帧层、HPACK（静态表 / Huffman / 解码 / 编码）全部手写，不依赖 nghttp2。
- `read_loop || write_loop || watchdog`，连接状态只在单连接 executor 上。
- 支持 SETTINGS、PING、RST_STREAM、GOAWAY、WINDOW_UPDATE、CONTINUATION、padding；
  PRIORITY 只作建议；客户端发 PUSH_PROMISE 是 PROTOCOL_ERROR。
- 响应 trailers 用带 END_STREAM 的尾部 HEADERS。
- 一次性响应**不发 Content-Length**（END_STREAM 界定），HEAD 例外。
- RFC 8441 extended CONNECT（`:protocol: websocket`）默认开启，服务端支持。
- Rapid Reset 防护：同时执行的 handler 上限 = `h2_max_concurrent_streams`；
  超出进 `h2_max_queued_streams` 队列；队列溢出 → `GOAWAY(ENHANCE_YOUR_CALM)`。

### 5.3 HTTP/3

- `#ifdef SIMPLE_HTTP_ENABLE_HTTP3` 整体门控。
- 帧层、SETTINGS、GOAWAY、QPACK、blocked field sections、请求校验由 nghttp3 负责；
  QUIC 由 ngtcp2 负责。本库只保留适配层。
- 引擎实现 `quic::Protocol`，**没有写循环**；QUIC 连接负责拉数据。
- 没有自己的 idle watchdog；使用 QUIC 协商的 idle timeout。
- 服务端可 Alt-Svc 通告；支持连接迁移；0-RTT 可配置（默认关）。
- 服务端 HTTP/3 不支持 WebSocket；`Expect: 100-continue` 是 no-op（nghttp3 无 1xx API）。

### 5.4 WebSocket

- 帧编解码手写（`proto/ws_frame.h`），transport 无关。
- 服务端支持 h1 Upgrade 与 h2 extended CONNECT；h3 不支持。
- permessage-deflate（RFC 7692）服务端可协商，`EngineLimits::ws_compression`
  默认 false；客户端不协商。
- 读泵 / 写泵 / 有界入站队列是防止「串行 read→write + 对端不读」死锁的关键。

### 5.5 TLS / ALPN

- 服务端 `TlsConfig::min_version` 默认 TLS 1.3；设 `Tls12` 同时接受 1.2；
  1.0/1.1 永不允许。
- 服务端 TCP TLS ALPN 默认 `h2` + `http/1.1`；`h3` 只用于 QUIC。
- 客户端 `TlsClientConfig::min_version` 默认 TLS 1.2。
- mTLS：`mutual = true` + `ca_file`；handler 第三参数 `SslHandle` 可读对端证书。
- HTTP/3 的 QUIC 自带 TLS 1.3，ALPN 固定 `h3`。

### 5.6 EngineLimits 默认值

| 字段 | 默认 |
|---|---|
| `idle_timeout` | 120 s |
| `max_header_bytes` | 64 KiB（431） |
| `max_body_bytes` | 64 MiB（413） |
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

---

## 6. 构建与运行

### 6.1 xmake（主构建）

```sh
xmake build -j 12
xmake run test_unit
xmake run diff_openapi_server
xmake run fixture_conformance_server
```

- `xmake.lua`：`set_languages("c++26")`。
- 全局 `-O2 -Wextra`，关闭少量 GCC 误报；目前没有把 warning 当 error。
- 全局定义 `SIMPLE_HTTP_USE_BOOST_REGEX`。
- `SIMPLE_HTTP_ENABLE_HTTP3` 只加在需要它的目标（`fixture_conformance_server`、
  `diff_client_h3`、`diff_server_cpp`、`test_unit`）。
- 包来自私有仓库 `fantasy-peak/xmake-repo`：上游 ngtcp2 包不产 OpenSSL crypto
  helper，HTTP/3 需要私有仓库里的 `ENABLE_OPENSSL=ON` 构建。
- 依赖锁：`package.requires_lock`、`package.librarydeps.strict_compatibility`。
- `simple_http` 目标是 `set_kind("static")` 的空壳，只导出 include 和依赖。

### 6.2 CMake

```cmake
find_package(simple_http CONFIG REQUIRED)
target_link_libraries(app PRIVATE simple_http::simple_http)
```

当前真实状态：

- `add_library(simple_http INTERFACE)`，`cxx_std_23`。
- 版本从 `core/version.h` 读取，避免包版本漂移。
- 链接 Boost、OpenSSL、ZLIB、Brotli、glaze。
- **没有**链接 ngtcp2/nghttp3，所以 CMake 下游不会自动启用 HTTP/3。
- `cmake/simple_httpConfig.cmake.in` 没有 `find_dependency(glaze)`，下游可能需要
  自己 `find_package(glaze CONFIG REQUIRED)`。
- 根目录的 `compile_commands.json` 可能是旧文件（曾显示 `-std=c++23`）；
  以 `build/` 下 xmake 生成的文件与 `.deps` 里的实际 flags 为准。

### 6.3 编译宏

| 宏 | 默认 | 作用 |
|---|---|---|
| `SIMPLE_HTTP_ENABLE_HTTP3` | 未定义 | 编入 HTTP/3 与 QUIC 监听 |
| `SIMPLE_HTTP_USE_BOOST_REGEX` | 本仓库定义 | 路由正则使用 Boost.Regex |
| `SIMPLE_HTTP_ENABLE_LOG` | 1 | 日志总开关 |
| `SIMPLE_HTTP_LOG_ACTIVE_LEVEL` | 0 | 编译期最低日志级别 |

压缩、WebSocket、OpenAPI、UNIX socket 都没有开关宏。

---

## 7. 测试与验证

### 7.1 唯一入口与分层

```sh
test/run.sh fast        # unit
test/run.sh standard    # + openapi
test/run.sh full        # + client-diff / server-diff / conformance / stress
test/run.sh list
test/run.sh doctor
test/run.sh <suite>     # 单套件，例如 unit / client-diff / server-diff
test/run.sh unit '[h2]' # Catch2 过滤
```

| suite | tier | 内容 |
|---|---|---|
| `unit` | fast | 原 Catch2 纯逻辑测试（约 401 用例） |
| `openapi` | standard | OAS 3.1 元模式 + 文档/路由契约 + Swagger UI |
| `client-diff` | full | C++ 客户端 ↔ Go 客户端，同一个 Go 服务端 |
| `server-diff` | full | Go 客户端 → C++ 服务端 ↔ Go 服务端 |
| `conformance` | full | h2spec / h1spec / Autobahn / h3spec |
| `stress` | full | h2load / k6，失败数必须为 0 |

`test/run.sh` 默认 `--parallel 3`：`conformance`/`stress` 共用 7788–7792，固定为
同一条串行 lane；其余套件端口互不重叠（`concurrency` 用临时端口），可并行。
`--parallel 1` 完全串行。编译并行度用 `--jobs`（sanitizer 默认 7）。

C++ 二进制由 xmake 构建；Go 参考服务端与探针由各 runner 用 `go build` 构建
（`test/diff/go.mod` 固定 `x/net`、`gorilla/websocket`、`quic-go`）。
Python 侧只需要 `httpx`、`jsonschema`、`brotli`（`test/requirements.txt`）。

### 7.2 差分测试

`test/diff/` 分两个方向，二者都把 Go 标准库当作参考实现：

- **client**（`test/diff/client/`）：C++ 客户端与 Go `net/http` 客户端读同一份
  `cases/*.json`，请求同一个 Go 参考服务端（h1 `7810` / TLS h2 `7811` /
  h2c `7812` / h3 `7813` / WS `7810`）。服务端回显收到的 method/header/body，
  因此差异只能来自客户端。组：`basic`、`body`、`policy`、`ws`、`h3`。
- **server**（`test/diff/server/`）：同一个 Go 探针把请求分别发给 C++ 参考服务端
  （`7820`–`7823`）与 Go 参考服务端（`7830`–`7833`），每个案例输出
  `target=cpp` / `target=go` 两行。组：`routing`、`body`、`semantics`、
  `middleware`、`framework`、`static`、`ws`。

每个组是独立可执行程序（C++ 用 `DIFF_GROUP_*`，Go 用 `-ldflags -X main.defaultGroup`）。
观测是 JSONL，字段为 `case/target/status/version/headers/body/body_encoding/framing/error/notes`。

归一化与门禁都在 `test/diff/support/harness.py`：

- header 名小写，`allow`/`vary`/`cache-control` 按集合比较；
- 默认忽略 Date/Server/Content-Length/Transfer-Encoding/Connection/ETag/
  Last-Modified/Set-Cookie 等传输细节；
- body 支持 `exact` / `json` / `none`，可配置 `scrub` 正则；
- 未登记差异失败；`test/diff/known_diffs.json` 登记双方都合法的差异并写明理由；
- 每次运行写 `diff-report.md`（逐字段 cpp/go/verdict/reason）。

新增功能时：在 C++/Go 参考实现同步加行为 → 在对应组加用例 → 跑 diff → 只把
确属合法差异的项登记到 `known_diffs.json`。Go 无对应能力的功能标 `cpp-only`
并写单侧期望，不要伪造 Go 行为。

### 7.3 外部一致性与压力

被测 server 是 `test/fixtures/conformance_server.cpp`（目标
`fixture_conformance_server`），只保留外部套件需要的四个路由：

- `any /`：回显请求体（h1spec）
- `any /world`：回显 `?n=` 字节（h2spec、压力）
- `ws /echo`：逐字节回显（Autobahn）
- `ws /echo1k`：固定 1024 字节（压力）

监听 `7788`（嗅探+ws）、`7790`（纯 h2c）、`7791`（纯 h1）、`7792`（TCP+UDP h3）。
单协议端口是必需的：嗅探端口上「畸形 HTTP/2 前导」与「畸形 HTTP/1.x 请求行」
是同一批字节，h2spec 要 GOAWAY、h1spec 要 400。

```sh
test/external/conformance/run.sh
test/external/conformance/run.sh h2spec
test/external/stress/run.sh cpp h3 --no-conformance
```

压力门线与参数不变：POST 1 KiB、响应 2k/10k/20k、每档 300000 请求、4 线程、
200 客户端、20 流；WS 100 VUs × 600 iterations × 2000 消息。**唯一门线是失败数 0**。
脚本会先清理遗留服务并确认端口释放，否则 SO_REUSEPORT 会把 QUIC 连接表劈开。

### 7.4 OpenAPI

`test/diff/openapi/` 的 petshop 夹具（`:7795`）同时验证：

1. `/openapi.json` 通过官方 OAS 3.1 元模式（`schema/openapi-3.1.json`）；
2. 结构不变量：operationId 唯一、path 参数与模板一致且 required、每个声明响应
   有 description、`$ref` 全部可解析且无 `$defs` 泄漏；
3. 文档 ↔ 实际路由：每个 operation 的每个声明状态都有真实请求覆盖，校验状态码、
   Content-Type、响应 JSON Schema、声明响应头与 401 challenge；
4. `/swagger` 可访问、引用正确 spec、文档两次获取字节一致。

### 7.5 Sanitizer

```sh
test/run.sh sanitize asan   [suite...]   # ASan + LSan + UBSan
test/run.sh sanitize tsan   [suite...]   # TSan
test/run.sh sanitize ubsan  [suite...]   # 仅 UBSan
```

- 默认 suite：asan = unit/openapi/client-diff/server-diff/concurrency；
  tsan = unit/concurrency/server-diff；ubsan = unit/client-diff/server-diff。
- 默认 `--jobs 7`。unit 用 ASan 时 `-j14` 会 OOM；不要把它调回 12/14。
- 默认在 `.cache/sanitize/worktree-<san>` 建独立 worktree，日常 `build/` 不受影响；
  `--build-dir build-asan` 可复用已有目录，结束时恢复 `xmake f -c -o build`。
- `concurrency`（`test/sanitizer/concurrency.cpp`）覆盖多客户端、多线程、
  跨 executor `ResponseWriter` 写和日志 sink 替换；Go 参考实现不插桩。
- 报告扫描 ASan/LSan/TSan/UBSan 关键字；强制 `SIGKILL` 记为
  `STOP_TIMEOUT`（泄漏检查被跳过），不当作通过。
- 抑制规则保持最小：`test/sanitizer/{lsan,tsan,ubsan}.supp`。

### 7.6 差分比对原则

- 以 RFC 为准，不以「和 Go/nginx 不一样」判定为本库 bug。
- 已知合理差异：CL+TE 与 obs-fold 上更严格；h2 一次性响应省略 content-length；
  `Allow` 包含自动支持的 OPTIONS；br 压缩 Go 无对应。
- 真正要修的是双方都违反 RFC，或与独立实现互操作失败。
- 测试发现真实 bug 时优先修库（例如重定向丢 header、SSE 请求体未 finish、
  CookieJar 未把 loopback 视为安全源），而不是把它登记成已知差异。

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

必跑：

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
- 在 `test/unit/` 加纯逻辑覆盖；跨实现行为加到 `test/diff/` 对应功能组
  （客户端加 `client/cases/*.json` 与驱动，服务端加两侧路由与 Go 探针）。
- 新增差异必须登记到 `test/diff/known_diffs.json` 并写清 RFC 依据，否则 suite 失败。
- 不要引入协议版本分支到 `handler/` / `proto/` / `net/` / 客户端门面。

### 8.5 代码风格

- `.clang-format`：LLVM + `IndentWidth: 4`、`ColumnLimit: 120`。
- 提交前 `./format.sh`；CI 用 `./format.sh --check`。
- RAII、无裸 `new` / `delete`，所有权显式。
- 可失败操作返回 `expected` / `error_code`，不要用异常做控制流。
- 新代码尽量沿用现有命名与注释密度；协议行为要写清「为什么」。

---

## 9. 已固化的回归点（不要重犯）

这些是曾经踩过并已修复的坑，改动相关代码时必须保持：

1. **WebSocket 入站死锁**：串行 `read→write` 的 handler 在写被对端零窗口挡住时不再读，
   会与「猛发不读」的客户端互相压死。修法是内部有界读泵 + `m_inbox`。
2. **HTTP/2 全局暂停**：请求体 channel 满时曾暂停整条连接；现在是每流
   `paused_frames`，其他流继续。
3. **`END_STREAM` 与停放**：停放期间到达的 END_STREAM 必须记 `finish_pending`，
   排空后再 `body().finish()`，否则 `Body::read()` 提前 EOF 截断 body。
4. **`Body::feed` 语义**：收 `std::string&&` 并直接转发 `try_send`；asio 失败时参数
   原样保留，调用方可重试。不要提前包成 `Frame` 无条件 move。
5. **连接数上限**：到上限时 accept 循环不发起 `async_accept`，把连接留在内核 backlog；
   计数跨 acceptor 共享，释放发生在连接协程完成回调。资源类错误（EMFILE/ENOBUFS/
   ENOMEM）有指数退避，避免空转烧核。
6. **H1 响应 framing**：handler 设置的 `transfer-encoding` / `connection` 等
   hop-by-hop 字段必须剥离，framing 只由引擎决定；否则 CL + TE 双 framing。
7. **per-key 限流 Retry-After**：按该 key 的桶计算，和共享桶版本一致。
8. **204/304 默认 Content-Type**：只在有内容的状态上补 `text/plain`。
9. **HTTP/2 Rapid Reset**：并发 handler 上限 = `h2_max_concurrent_streams`；
   reset 的排队流要从队列移除；队列溢出 → ENHANCE_YOUR_CALM。
10. **h2 一次性响应**：不发 `content-length`（END_STREAM 界定），HEAD 例外。
11. **h3 响应队列**：按「已排队字节」封顶，不是按 in-flight 字节。
12. **解压流**：`DecompressingClientStream` 必须把响应 trailers 继续向上转发。
13. **CONNECT origin-form**：H1 解析器按 RFC 9112 直接 400；到达 handler 的 CONNECT
    由反代回 405。
14. **QUIC 发送上限**：`ClientConfig::quic_max_udp_payload_size` 默认 1200；
    VPN/隧道/WSL2 上调到 1472 会被截断，握手超时。
15. **TLS 默认值**：服务端默认 1.3-only，客户端默认 1.2；不要无声改回去。
16. **WebSocket Close 顺序**：协议错误/Close/非法 UTF-8 先记关闭码，Close 帧要等
    应用下次 `read()` 排空缓存后才发出，保证「先回显、后失败」。
17. **h2 `100-continue`**：handler 首次读 body 时懒发；h3 侧 no-op。
18. **客户端自动 100-continue**：h1/h2 先把 interim 100 交给 `write()`；h2 必须
    `clear_head_cache()`，否则后续 `read_head()` 会一直返回 100。h1 的
    `exchange_read` 半双工检查必须放行「Expect 已收到最终响应」的情况。
19. **h2 请求 trailers**：带 trailer 的最后一个 DATA **不能**带 END_STREAM；trailing
    HEADERS 同时带 END_STREAM + END_HEADERS，并在 body 队列排空后发送。
20. **h1 请求 trailers**：chunked 终止符是 `0\r\n<fields>\r\n`；服务端在 body EOF 前
    把字段写进 `Request::trailers()`。
21. **状态码重试**：`retry_statuses` 默认空（与 Go/reqwest 一致），显式开启后才按
    429/503 + `Retry-After` 重试；重试前响应体已消费。
22. **请求解压是 opt-in 且默认 Go 语义**：服务端只有挂载
    `middleware::decompress_request` 才解码；反代路由不挂载时原样转发压缩体与头。
    解码在 `Body::read()` 消费侧进行（h2/h3 可在 middleware 运行前已排队原始帧），
    `on_consumed` 必须按线上压缩字节归还流控；解码后超限 413、畸形/截断 400、
    不支持的 coding 415。`Body::enable_decoding` 只拒绝“已开始 read”的 body。
23. **客户端解压只在“自己请求的编码”上生效**：默认 `auto_decompress=true` + 仅 gzip；
    调用方显式设置 `Accept-Encoding`、HEAD、Range 时不注入也不解码。解压装饰器必须
    转发 `clear_head_cache()`（否则 100-continue 的 interim head 会循环）和
    `finish_trailers()`。
24. **重定向保留请求头**：`follow_redirects` 曾把 method token 与 headers `std::move`
    进当前 hop，递归的下一跳拿到空值；结果是 User-Agent/自定义头/Authorization
    在 3xx 后静默丢失。两处都必须是拷贝（跨 origin 的 auth/cookie 清除仍按规则做）。
25. **SSE 请求体必须 finish**：`SseStream::open` 走显式 Stream，请求体在
    `finish()` 前一直开着；不 finish 时等待响应头的服务端永远不返回。GET 的 SSE
    请求要在 `read_head()` 前 `finish()`。
26. **CookieJar 的 Secure 与 loopback**：RFC 6265bis / Go 把 localhost 与
    `127.0.0.0/8`/`::1` 视为可信源，Secure cookie 在 http 下也会发送；不要把它
    简化成「仅 https」。
27. **OpenSSL 惰性初始化**：所有 `ssl::context` 构造入口（`TlsContext`、
    `QuicTlsContext`、`make_client_ssl_context`）都必须先经过
    `transport/openssl_init.h` 的 `detail::ensure_openssl_initialized()`。
    多个线程首次并发 `SSL_CTX_new` 会在 OpenSSL 4 的 `ossl_ht_get` 里产生
    TSan race；这个 `std::call_once` 预热是进程级的，不是 SSL_CTX 单例。
    需要跨 Client 共享 context 时用 `make_client_tls_context()` +
    `TlsClientConfig::shared_context`，不要做全局单例。

---

## 10. 已知限制

- 客户端 `open_websocket` 只走 HTTP/1.1 Upgrade；不支持 h2/h3 WebSocket。
- 服务端 HTTP/3 不支持 WebSocket（RFC 9220 未实现）。
- 服务端 HTTP/3 的 `Expect: 100-continue` 是 no-op。
- 客户端不暴露 103 等其他 1xx；100-continue 在 h1/h2 暴露给自动流。
- HTTP/3 客户端只显式启用；不做 Alt-Svc / HTTPS RR 发现；不支持 0-RTT。
- 出站代理不覆盖 HTTP/3。
- `NO_PROXY` 不支持 CIDR / 通配。
- 客户端单连接模型：每个 origin 一条连接，其他 origin 一次性连接；没有传统连接池。
- 客户端 WebSocket 不协商 permessage-deflate。
- CORS 不处理 WebSocket 升级。
- Host 路由只作用于 HTTP dispatch；WebSocket 查找仍 host-agnostic，Host 子 Router 有独立
  OpenAPI 文档。
- JWT 只是验证器：不做令牌签发、OAuth2/OIDC 发现或 refresh-token 流程。JWKS 支持
  静态/手动拉取文档（`fetch_jwks_json`），不内置后台自动刷新；`aud`/`scope` 按字符串
  claim 解析，不支持 audience 数组。
- 请求 trailers 只支持 h1/h2；h3 返回 `trailers_not_supported`。
- 服务端请求解压只支持单个 `gzip`/`br` coding；`deflate`、多 coding 列表返回 415。
  默认不启用，需挂载 `middleware::decompress_request`。
- 客户端自动解压默认仅 gzip（对齐 Go）；br 需显式 `accept_encodings={"br","gzip"}`。
- 自动 100-continue 在 h1/h2 等待 interim；h3 直接发送 body（服务端无 1xx API）。
- Session 只内置进程内存储；Redis/DB 适配器由消费侧实现 `SessionStore`。CSRF 是
  header 型 double-submit，无法设置 header 的表单需 handler 自行校验字段。
- 校验通过 `Validator<T>` 按类型 opt-in，没有 tag/DSL 校验器。
- CMake 接口未接入 ngtcp2/nghttp3；HTTP/3 目前只由 xmake 构建提供。
- `simple_httpConfig.cmake.in` 缺 `find_dependency(glaze)`。
- 根目录 `compile_commands.json` 可能过期；以 `build/` 下的为准。

---

## 11. 常用命令速查

```sh
# 构建
xmake build -j 12
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
test/run.sh openapi
test/run.sh client-diff
test/run.sh server-diff
test/run.sh conformance
test/run.sh stress

# 直接运行差分 runner（Go 二进制由 runner 自行构建）
test/.venv/bin/python test/diff/client/run.py --groups basic,body,policy,ws,h3
test/.venv/bin/python test/diff/server/run.py --groups routing,body,semantics,middleware,framework,static,ws
test/.venv/bin/python test/diff/openapi/run.py

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
