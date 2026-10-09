# AGENTS.md — simple_http

面向参与本工程的 AI/协作者的快速上手说明。全部内容基于当前源码整理。

## 这是什么

`simple_http` 是一个 **header-only 的 C++ HTTP 服务器 + 客户端库**，基于
`boost.asio` C++23 协程实现。服务端支持 HTTP/1.0、HTTP/1.1、HTTP/2（h2 / h2c /
prior-knowledge）、HTTP/3（QUIC，默认关闭）、WebSocket；客户端（`client/` 层）支持
http/https × HTTP/1.1/HTTP/2，通过 ALPN（TLS）或 h2c（明文，Upgrade 或
prior-knowledge）自动协商。

关键事实（来自代码）：

- **纯头文件库**：所有实现都在 `include/` 下的 `.h` 里，没有 `.cpp` 源文件。`xmake.lua` 的
  `target("simple_http")` 是个 `set_kind("static")` 的空壳目标，只把 `include/` 与依赖以
  `public` 方式带出去（`CMakeLists.txt` 同理由下游 `find_package` 使用）。
- **手写协议编解码**：HTTP/1.x、HTTP/2 帧层、HPACK、WebSocket 帧均在
  `include/simple_http/engine/` 与 `proto/` 内手写实现（不依赖 beast/nghttp2；`Status`/`Field`
  常量在 `core/http_status.h`、`core/http_field.h`，`Method` 在 `core/http_method.h`）。
  仅依赖 `boost.asio`（裸 TCP socket 与 `asio::ssl::stream`）+ OpenSSL。**HTTP/3 是例外**：
  帧层/QPACK/SETTINGS/丢包恢复等来自 **ngtcp2 + nghttp3**，本库只保留引擎与
  `Dispatcher` 之间的适配层。
- **C++23**：`xmake.lua` 中 `set_languages("c++23")`。
- **单一入口头**：`#include "simple_http.h"` 聚合全部公共 API。

## 目录结构

代码统一在 `include/simple_http/` 下按层组织（见 `simple_http.h` 的聚合顺序）：

```
include/
  simple_http.h            伞形头，聚合所有层
  simple_http/
    core/                  协议无关基础设施
      version.h            Version 枚举 (Http1/Http11/Http2/Http3)
      types.h              error_code、Version、WriteMode、StreamStatus、Eof/Rst/Disconnect
      http_method.h        Method 枚举
      http_status.h        状态码常量 + reason_phrase
      logging.h            日志 facade（LogSink / set_log_sink / make_stdout_sink / SIMPLE_HTTP_*_LOG）
      mime.h               MIME 类型常量 + by_extension（扩展名→类型）
      http_date.h          IMF-fixdate 格式化/解析 + now_unix（手写，不用 strftime）
      url_path.h           请求路径解码与规范化（查找键）+ under_prefix
      url_encoding.h       百分号编码（query_escape / path_escape，Go net/url 对标）
      accept_encoding.h    Accept-Encoding qvalue 解析（独立于编码器层）
      validators.h         ETag / 弱比较 / Range 解析
      static_table.h       文档根 → 不可变查找表（静态服务的协议无关半边）
      base64.h             base64（含 WebSocket 握手用标准 base64）
      io_pool.h            IoCtxPool（单线程 io_context 池，并发模型 A）
      limits.h             EngineLimits（超时/大小上限/HTTP2 窗口/ws_read_ahead_bytes 等）
      compression.h        压缩配置（CompressionConfig，宏门控）
      content_encoding.h   gzip/brotli 编解码（宏门控）
    proto/                 版本无关的 HTTP 模型
      headers.h            Headers
      body.h               Body（流式请求体；推模式 channel / HTTP/1 拉模式）
      query.h              QueryParams：query-string/urlencoded 解析（Go r.URL.Query / axum Query<T>）
      multipart.h          multipart/form-data 解析 + 客户端编码 MultipartForm
      form.h               read_urlencoded_body / read_multipart_body 高层读取
      params.h             path_params<T> / query_params<T> / parse_params<T>（axum 提取器，glaze 反射）
      json.h               read_json_body<T> / write_json(res, T, status)（glaze 反射，默认可用）
      request.h            Request（两端共用；set_body 保留 m_body_source 供重放）
      response.h           Response（fluent；redirect / SSE 封装）
      response_writer.h    ResponseWriter（版本收敛的纯虚接口）
      compressing_writer.h 响应体压缩装饰（宏门控）
      ws_frame.h           WebSocket 帧编解码 + 握手 key（手写）
      websocket.h          WebSocket 句柄 + WsBackendImpl（读泵 + 写泵）
    transport/             字节流传输抽象
      transport.h          Transport concept + SslHandle
      tcp_transport.h      TcpStreamTransport（裸 TCP；async_write_seq 零拷贝 scatter-gather）
      tls_context.h        TlsContext / TlsConfig（OpenSSL + ALPN）
      tls_transport.h      TlsStreamTransport（ssl::stream）
    engine/                各版本协议引擎（实现 ResponseWriter）
      dispatcher.h         Dispatcher / WsLookup / WsHandlerFn 类型（引擎↔handler 的窄接缝）
      h1/                  HTTP/1.x 引擎 + 请求头解析器 h1_parser.h（含 WebSocket 升级握手、
                           h2c 升级）；ws_proxy.h 是字节级 WebSocket 隧道
      h2/                  HTTP/2 引擎 + 帧层 + HPACK（h2_frame / hpack_*）；读/写分离
                           （read_loop || write_loop），请求体按流停放
      h3/                  HTTP/3 引擎——nghttp3 与本库 Dispatcher 之间的适配层：
                           h3_engine.h / h3_stream.h / h3_callbacks.h
                           （#ifdef SIMPLE_HTTP_ENABLE_HTTP3）
    handler/               handler 类型系统 + 路由
      handler.h            Handler/Next/Middleware 类型别名、make_handler/invoke_handler/compose_middleware
      builtin_middleware.h request_id / access_log / recovery / basic_auth / real_ip / clean_path /
                           strip_prefix / redirect_slashes
      secure_headers.h     安全响应头中间件 SecureHeadersConfig
      rate_limit.h         TokenBucket / RateLimiter + rate_limit(middleware)（429 + Retry-After）
      cors.h               内建 CORS：CorsConfig 编译成 Middleware（预检 204，不进路由）
      conditionals.h       handler 级条件请求 304
      router.h             Router（方法感知精确路由 + 段 trie + 反代 + 静态阶段；group/per-route）
      http_proxy.h         请求级反代：每请求一条上游连接（client 层），响应流式回传
      static_files.h       静态文件服务（static_table.h 的 serving 半边 + SPA fallback）
    quic/                  QUIC 传输（#ifdef SIMPLE_HTTP_ENABLE_HTTP3）——ngtcp2 的包装
      protocol.h / connection.h / endpoint.h / ngtcp2_config.h / ngtcp2_crypto.h / tls.h
    net/                   监听与连接协议检测
      connection.h         serve_plaintext / serve_tls（协议检测）
      server.h             Server 门面 + ServerConfig / Listen / InetAddress / UnixAddress / QuicAddress
    client/                出站 HTTP 客户端（与 server 对称的一层）
      http.h               公共客户端 API（平铺在 `simple_http::` 顶层）：Client / RequestBuilder /
                           Stream / Response / StreamSpec / WebSocketSpec
      ws_client.h          WebSocket 客户端：dial_transport 裸建连 + Upgrade 握手，复用 WsBackendImpl
      client_config.h      ClientConfig / TlsClientConfig / client_errc / HttpVersionPolicy / H2cMode
      url.h                绝对 URL 解析（parse_url；ws:// wss:// 归一后复用）
      client_stream.h      ResponseHead / ClientStream / ClientSession（内部引擎接口）
      tls_client.h         客户端 ssl::context 与握手（SNI / ALPN / 主机名校验 / mTLS）
      h1_client.h          HTTP/1.1 会话（一次一交换，keep-alive、chunked、h2c Upgrade）
      h2_client.h          HTTP/2 会话（多路复用、流控、SETTINGS/PING/GOAWAY、h2c 播种流 1）
      cookie_jar.h         CookieJar（Domain/Path/Secure/Expires 过滤）
      http_client.h        内部连接引擎 `detail::ClientEngine`（单连接模型核心）
      decompressing_stream.h  响应体解压装饰器（read() 层解码，重写 head）
    openapi/               OpenAPI 3.1 文档 + Swagger UI（常编；文档按路由级 openapi::doc() 描述收集）
      openapi_doc.h        OAS 3.1 文档模型 + 手写 JSON 渲染器 + components/schemas 去重
      openapi.h            glaze 反射→内联 schema、openapi::doc() 路由描述 /
                           query<T>/resp<T>/response_header<T> 等
```

其它目录：

- `test/`：`server.cpp` 是示例服务器；`client.cpp` 是客户端整合自检；`server_regression.cpp`
  是服务端对抗性回归；`unit/` 是 Catch2 单元测试；`loadgen.cpp` 是基于本库客户端的压测工具；
  `thread_check.cpp` 是客户端线程模型压测；`openapi_demo.cpp` + `openapi_verify.py` 是
  OpenAPI 端到端；还有若干 `*_check.cpp`（body_cap / h1_duplex / h1_pull_lifetime /
  h2_limit / read_all_timeout / retry / ws_cross / client_cross）配合 `test/python/` 下的
  独立 Python 服务端驱动；`conformance/` 与 `stress/` 是两套外部驱动脚本；
  `tls_certificates/` 是测试证书。
- `ROADMAP.md`：对照开源框架的 backlog（新增差异项先记到它下面再动工；当前工作区里可能被
  删除/改写，以 `git status` 为准）。
- 没有 `docs/` 目录：架构说明就在本文件与各头文件顶部注释里。

## 架构要点（来自代码与设计）

1. **协程贯穿始终**：handler 是 `asio::awaitable<void>`，读请求体、写响应都是
   `co_await` 的异步操作，天然反压。
2. **一个抽象收敛协议版本**：`Request`/`Response`/`Router`/handler 完全与
   HTTP 版本无关，版本差异只藏在 `engine/` 与 `ResponseWriter` 实现里，上层
   没有 `if (version==…)`。
3. **传输与协议解耦**：引擎通过 `Transport` 抽象读写字节，不直接依赖具体
   socket 类型（TCP 明文 / TLS / QUIC 流）。
4. **并发模型 A（单线程 io_context 绑定）**：每个连接在 accept 时被绑定到
   `IoCtxPool` 中一个单线程 `io_context`，该连接的所有引擎/writer 工作都只在这一个
   线程上运行，因此天然串行、无需锁。绑定的方式取决于 `ServerConfig::reuse_port`：
   默认（false）由唯一的 acceptor 在 `accept_loop` 里用 `m_pool->next_ptr()`
   轮转分配；开启时每个 worker context 各持一个 acceptor（SO_REUSEPORT），连接留在
   accept 它的那个 context 上。`Response`/`WebSocket` 可被 handler 用
   `shared_ptr` 捕获并跨线程/跨协程使用，写操作会先 hop 回连接所属 executor。
5. **日志与后端解耦**：库只定义 `LogSink`（`level` / `where` / `category` / `message`，
   不含任何第三方类型），是否保留某条记录由 sink 的 `enabled()` 决定，且在格式化**之前**
   询问。适配第三方日志库的代码写在**消费者侧**（README 的 Logging 节有 spdlog 示例）。
   安装 sink 用 `set_log_sink`（`std::atomic<std::shared_ptr<>>`，运行中替换安全）。
6. **反压是显式的、按流/按队列封顶的**：HTTP/2 请求体按流停放（见下），WebSocket 有
   有界入站读泵（见下），静态/流式响应走 write pump。没有一处允许对端把内存推成无界。

## 公共 API（以源码为准）

Handler 是协程，通过实参个数在编译期自动选择形态（`handler.h`）：

```cpp
using RequestPtr  = std::shared_ptr<Request>;
using ResponsePtr = std::shared_ptr<Response>;

// 普通         : awaitable<void>(RequestPtr, ResponsePtr)
// 带 TLS 句柄  : awaitable<void>(RequestPtr, ResponsePtr, SslHandle)
// WebSocket    : awaitable<void>(RequestPtr, std::shared_ptr<WebSocket>)
```

中间件（`handler.h` 的 `Middleware`/`Next`，对标 Go net/http / axum from_fn）：

```cpp
// 全局：use() 按注册顺序嵌套，最外层先跑。next() 之前是 before 阶段，
// 之后恢复是 after 阶段（可读 res->status()）；不调 next() 即短路。
server.use([](RequestPtr req, ResponsePtr res, SslHandle ssl, Next next) -> asio::awaitable<void> {
    co_await next(std::move(req), std::move(res), ssl);   // 继续链路（→ 路由 → handler）
    log(res->status());                                    // after：观察响应
});
// 每请求类型化 state（Go context / tower Extensions）：
req->set_state(Principal{...});  // 中间件写
req->get_state<Principal>();     // handler 读；last-set-wins，按 static 类型键

// 组（chi/gin Group / axum nest）：前缀 + 组中间件只作用于组内路由，可嵌套，
// 组内 use() 只作用该组；per-route 中间件只包这一条路由的 handler。
server.group("/api", {auth_mw}, [](Router &api) {
    api.route({Get}, "/users", h);                            // → /api/users
    api.route({Post}, "/users", {rate_limit_mw}, create);     // 再加 per-route 中间件
});
// 内置中间件：middleware::request_id() / access_log() / recovery() / basic_auth()
//              / real_ip(trusted) / clean_path() / strip_prefix("/x") / redirect_slashes()
//              / secure_headers(SecureHeadersConfig)（安全响应头基线，HSTS 仅 TLS）
server.use(simple_http::middleware::request_id());
```

`Server` 门面（`net/server.h`）为 fluent 风格，路由方法转发给内部 `Router`：

```cpp
ServerConfig cfg;
cfg.listen = InetAddress{"0.0.0.0", 7788, false};  // Listen = variant<InetAddress, UnixAddress>
// 双栈写 InetAddress{"::", port, false}；UNIX socket 写 UnixAddress{"/run/app.sock"}——
// 两者互斥由类型保证，写不出"同时给端口和路径"的配置。
cfg.worker_threads = 4;
// cfg.tls = TlsConfig{ .cert_chain_file=..., .private_key_file=...,
//                      .mutual=true, .ca_file=... };  // 可选 TLS/mTLS
// cfg.limits = EngineLimits{...};                     // 可选引擎调参
// cfg.reuse_port = true; cfg.tcp_nodelay = true; cfg.plaintext_protocols = ...;
// #ifdef SIMPLE_HTTP_ENABLE_HTTP3: cfg.quic = QuicAddress{"0.0.0.0", 7792, false};

Server server{cfg};
server.route({Method::Get}, "/world", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
    co_await res->status(200).send("hello");
});
// 路由 = (method, path) 对，方法先行。方法集可以来自任意可迭代容器。
server.route({Method::Post, Method::Put}, "/users", handler);
server.route(any_methods, "/webhook", webhook_handler);   // 任意方法
server.route_regex({Method::Get}, "/api/.*", handler);
server.ws_route("/chat", ws_handler);
server.fallback(not_found_handler);

server.start();      // 同步：启动所有监听并阻塞至绑定完成，返回 bool
// 或 co_await server.run();  // 协程版
server.stop();
```

路由层是**方法感知的**（`router.h`）：精确路由按 `(method, path)` 存；**含 `{name}` 的
路径是段 trie**——`route({Method::Get}, "/a/{x}", h)` 即可路由 `/a/5`，handler 用
`req->param("x")` 取捕获（OpenAPI 的 `path_params<T>()` 只是按类型解析进结构体）。两条
隐式规则跟着主流框架走：

- **GET 隐含 HEAD**：注册 `{Method::Get}` 自动允许 HEAD，writer 抑制 body。
- **405 + Allow（及自动 OPTIONS）**：path 存在但方法不符 → 405，`Allow` 列出全部方法；
  OPTIONS 自动 204 + Allow。path 不存在仍是 404。405/OPTIONS 是**普通 keep-alive 响应**
  （引擎 drain 未读请求体后继续复用）。

CORS 是策略入口（`handler/cors.h`）：`CorsConfig` 编译成 `Middleware` 存进 `m_cors`，
始终置于 dispatch 链路头部。带 `Origin` + `Access-Control-Request-Method` 的 OPTIONS 在
中间件内应答 204 并短路。字段语义见 README 的 CORS 段。`allow_origins` 支持子域通配，
另有 `allow_origin_fn`；`make_cors_middleware()` 可混进组/单路由。

`Response`（fluent，一次性或流式）：

```cpp
co_await res->status(200).content_type("text/plain").send(body);  // 一次性
co_await res->status(200).begin();  co_await res->write(chunk);  co_await res->finish(last);  // 流式
```

`Request` 常用：`method() / path() / query() / query_params() / cookie(name) / version()
/ header(name) / body().read() / read_all()`。`Response` 常用：`status/header/content_type`、
`send`、`begin/write/finish`、`redirect`、`set_cookie`（Go `http.SetCookie` 命名参数）、
`replace_header`（同名替换）、SSE 三件套 `sse_begin/sse_event/sse_comment`。

表单与查询：`read_urlencoded_body(req)`、`read_multipart_body(req)`、`read_json_body<T>` /
`write_json`。客户端上行 multipart 用 `proto/multipart.h` 的 `MultipartForm`
（`.field` / `.file`，`release_body()` 免拷贝）。类型化参数：`path_params<T>` /
`query_params<T>` / `parse_params<T>`。同名头多值：`headers().get_all(name)`。

限流（`handler/rate_limit.h`）：`make_token_bucket(rate, burst)` + `rate_limit(bucket)`；
按 key 分桶 `RateLimiter(rate, burst, max_keys)` + `rate_limit(limiter, key_fn)`。超限 429 +
`Retry-After`。

### WebSocket

`WebSocket`（`proto/websocket.h`，handler 收到 `shared_ptr<WebSocket>`）：

```cpp
server.ws_route("/chat", [](RequestPtr req, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
    for (;;) {
        auto msg = co_await ws->read();          // expected<WsMessage, error_code>
        if (!msg) break;                         // 对端关闭或出错
        if (co_await ws->write("echo: " + msg->data, msg->text)) break;
    }
    co_return;  // 返回后引擎自动发 Close 帧并优雅关闭
});
```

实现要点（`ws_frame.h` + `websocket.h`）：

- 帧编解码手写；消息保持在内存，单帧与分片重组累计大小受 `EngineLimits::max_body_bytes`
  约束。`read()` 返回 `WsMessage{data, text}`，自动重组 continuation、Ping→Pong、收到
  Close 回 Close。
- `write`/`write_text`/`write_binary` 按值收 payload（move 进队列，构建后延迟 await 也
  安全）；`write_view`/`*_view` **借用**调用方缓冲，在返回前写完，只需缓冲活过 co_await。
- **写泵 `run_writer`**：所有写经一个 detached 协程串行化；泵持 `shared_ptr` 到 backend，
  写未完则 transport/TLS 流不析构。watchdog 只持 `weak_ptr`，连接结束即释放。
- **读泵（内部，`run_reader` 懒启动于首次 `read()`）**：socket 的读取**不再由应用 `read()`
  驱动**，而由一条独立 reader 协程持续排空进**有界入站队列** `m_inbox`，`read()` 变成出队。
  这样「串行 read→write」的 echo handler 在写被对端零窗口挡住时，socket 仍在读，不会与
  「猛发不读」的客户端互相压死（曾每个 k6 轮次偶发丢一条连接、卡到客户端 20s 超时）。
  队列上限 `m_inbox_limit`（字节，含每条消息固定开销）就是背压：满则 reader 停读，`read()`
  腾出空间再续。上限来自 `EngineLimits::ws_read_ahead_bytes`（默认 4 MiB，0 取库默认），
  客户端侧对应 `WebSocketSpec::read_ahead_bytes`。
- **终止/关闭的顺序**：协议错误、收到的 Close、非法 UTF-8 都由 reader 记下关闭码
  （`m_close_code`）并停止读取，但**不立即 `close_with`**——Close 帧由应用下次 `read()`
  在排空全部已缓存消息后才发出（`flush_deferred_close`），从而「先回显、后失败」的顺序与
  旧的按需读实现完全一致（Autobahn 因此保持 OK 298 / NON-STRICT 0）。
- 拆除路径统一 `stop_inbox()`；`abort()`（`~WebSocket`）会关 transport 以解开停在
  `async_read_some` 的 reader，否则 backend（reader 持引用）无法销毁。

升级握手在 HTTP/1.1 引擎内完成（`Upgrade: websocket` → 101）；ws 走明文、wss 走 TLS
（ALPN 非 h2 时落到 H1 引擎）。**服务端 HTTP/2 的 WebSocket（RFC 8441）已接入**：h2
引擎宣告 `SETTINGS_ENABLE_CONNECT_PROTOCOL`，接受 `:method = CONNECT` +
`:protocol = websocket` 的 extended CONNECT（`:scheme`/`:path` 必需），回 `200` 后把该
DATA 流当作 WebSocket 隧道（`engine/h2/h2_ws_transport.h` 把单条 h2 流适配成
`WsBackendImpl` 的 Transport，帧编解码/掩码/流控/deflate 全部复用）。**客户端侧的
`Client::open_websocket` 仍走 HTTP/1.1 Upgrade**（h2 客户端 WebSocket 是后续项）。

## 协议升级路径（均在 H1 引擎解析完请求头后判定）

- **WebSocket 升级**：`Upgrade: websocket` + `Sec-WebSocket-Key`，命中 ws 路由则发
  101 并交给 WebSocket 层。
- **h2c 明文升级**：`Upgrade: h2c` + `HTTP2-Settings`，发 101 后交给
  `Http2Engine::run_h2c`，把原请求重放为 stream 1，连接后续以 h2 继续。
- **h2 extended CONNECT（RFC 8441）**：在 **h2 引擎内**判定——`:method = CONNECT` +
  `:protocol = websocket`，校验 `:scheme`/`:path` 后按 nginx 序查找：本地精确 ws 路由 →
  `ws_proxy` 反代 → 本地正则 ws 路由。本地路由走上 ws 中间件链，终端的 `WsUpgrade` 回
  `200` 并建立流级隧道；`ws_proxy` 在前端 h2 与后端 HTTP/1.1 之间做**握手翻译**（向后端
  合成 `Sec-WebSocket-Key` 发起 Upgrade，等 101 后回 h2 `200`，保留 query 字符串，转发
  后端协商的 `Sec-WebSocket-Extensions` 等头部，随后字节级拼接）。`:protocol` 非
  websocket 走普通 dispatch。
- 升级成功后 transport 交由对应协议层接管，H1 引擎不再关闭它（`m_upgraded`）。

完整用法参考 `test/server.cpp`。

### OpenAPI（`openapi/` 层，常编、按路由收集）

**glaze**（主库默认依赖，`glaze_json_schema` 注解提供字段级
description/enum）。带 `openapi::doc()...` 描述的注册在注册时把
`(方法, path)`、body schema、参数、安全、错误响应收进一份 OAS 3.1 文档，
serve 出去给 CDN 版 swagger-ui 浏览。**没有编译开关**：文档是按路由的元数据，
不写描述的路由不进文档、零开销，不调 `serve_openapi` 就不发文档：

```cpp
server.openapi().title("petshop").version("1.0.0").server("http://127.0.0.1:7795");
server.route({Method::Get}, "/pets/{id}", handler,
    openapi::doc()
        .path_params<PetParams>()
        .response<Pet>()
        .summary("get a pet").tag("pet").operation_id("getPet")
        .query<std::string>("q", {.description = "..."})
        .error<ErrorBody>(404, "no such pet"));
// 请求体 + 201 + 安全 + 响应头 + 多条错误响应：
server.route({Method::Post}, "/owners/{owner_id}/pets", handler,
    openapi::doc()
        .path_params<OwnerParams>()
        .request_body<CreatePetReq>()
        .response<Pet>(201)
        .security({"bearer"})
        .response_header<std::string>("X-Request-Id", "the request id")
        .error<ErrorBody>(400, "invalid body")
        .error<ErrorBody>(404, "no such owner"));
// Params 单一声源：模板 {name} 必须等于结构体字段名（注册时校验）
auto p = openapi::path_params<OrderItemParams>(*req);   // 缺/坏 → nullopt
auto in = co_await openapi::read_body<CreateUserReq>(*req, err); // 400/422

server.serve_openapi("/openapi.json");
server.serve_swagger_ui("/swagger", "/openapi.json");
```

要点：单一 `route()`，schema 是尾参描述对象、由命名方法构建（不靠模板实参
个数区分语义）；schema 手写、swagger 安全（**不用** `glz::write_json_schema`；
重复对象收拢进 `components.schemas` 再 `$ref`）；路径模板 = 段 trie；405 保持
连接。完整用例见 `test/openapi_demo.cpp` + `test/openapi_verify.py`（41 项断言）。

### 客户端（`client/` 层）

**公共客户端 API 平铺在 `simple_http::` 顶层（`client/http.h`）**：

```cpp
simple_http::Client client{ex, cfg};   // ex = 单线程 executor（模型 A），io_context 必须活过会话
auto r = co_await client.post("https://host/items").json(CreateItem{...}).bearer_auth(jwt).send();
auto up = co_await client.open_stream("https://host/upload", {.method = Method::Post}); // 全双工
co_await up->write(chunk); co_await up->finish();
auto head = co_await up->read_head(); while (auto c = co_await up->read()) { … }
auto req = std::make_shared<Request>(Version::Http11, ex); req->set_method(Method::Get);
auto r2 = co_await client.send(std::move(req));            // Go http.Client.Do
simple_http::MultipartForm form; form.field("title","hi"); form.file("upload","a.txt",data);
auto r3 = co_await client.post(url).multipart(std::move(form)).send();
auto ws = co_await client.open_websocket("wss://host/chat", {.origin = "https://app"});
```

分层超时：`ClientConfig::response_head_timeout`（TTFB）与 `body_idle_timeout`（体静默）落在
显式 `Stream` 上，`StreamSpec` 可逐请求覆盖；便捷 `send()` 缓冲路径保留整体
`request_timeout`。`Response::header_owned()` 给出无悬垂风险的头取值。

要点：

- **便捷层重定向与 Cookie**：`max_redirects`（0 = 不跟随），301/302/303 转 GET、307/308
  保留方法与 body，拒绝 https→http 降级，Location 支持四种形态；`cookie_jar` 按
  Domain/Path/Secure/Expires 过滤回放。`basic_auth` / `bearer_auth`。
- **单连接模型**：一个 `Client` 对它的第一个 origin 只保持一条 TCP 连接（h1 FIFO 门
  `ExchangeGate`，h2 多路复用）；打到其它 origin 用一次性连接。门在交换完成时释放，
  不依赖调用方何时丢弃 stream。
- **读写形状**：请求体 `write()`/`finish()`（h1 chunked / h2 DATA）；响应体 `read()` 返回
  `ReadResult`（data 或 eof），失败是 error_code（`RST_STREAM` → `stream_reset`，
  中途断开 → `connection_reset`）。`read_head()` 幂等。
- **重试**：**免费透明重拨一次**（kept 连接传输层失败）与**策略重试**（`ClientConfig::retry`，
  退避 + 条件回调，默认只重试 stream_refused/幂等方法/免费重拨后仍失败）；引擎 open 与
  便捷层读响应共享一个 retry budget，整个请求受 `request_timeout` 总 deadline 约束。
- **线程模型**：构造期强制 pinned 单线程 executor，引擎内零锁。契约：io_context 必须活过
  它绑定的会话（退出 worker 前先 drain）。
- **客户端 WebSocket**：`open_websocket` 经 `dial_transport` 裸建连 + HTTP/1.1 Upgrade 握手
  （校验 101 + `Sec-WebSocket-Accept`），复用服务端 `WsBackendImpl`；帧方向由参数区分。
- **反代**：`Server::http_proxy*` 由 `handler/http_proxy.h` 处理，上游走 client 层
  （TLS/ALPN 选 h2、`h2c` 尝试一次 Upgrade）。每条反代路由自带一个 `ClientConfig`；
  按 nginx 默认形态**每请求新建一个 Client**（pinned 到当前连接 executor），交换结束即释放。
  WebSocket 反代（`ws_proxy*`）是字节级隧道。

完整用法与边界用例参考 `test/client.cpp`（自检，`xmake run client`，161 checks，含
`suite_websocket`/`suite_multipart`/`suite_retry`/`suite_reverse_proxy`）。**双向 Python
交叉验证**：`test/python/run.py` 用第三方客户端驱动 C++ 服务端（httpx/h2/websockets，
54 checks）；`test/python/run_client_cross.py` 用独立服务端驱动 C++ 客户端。

### 压测工具（loadgen，用本库自己的客户端打负载）

`test/loadgen.cpp` 用 simple_http 的客户端压任意端点，输出对齐 h2load 的
`finished in Xs, N req/s, M MB/s` + 状态桶 + 延迟分位：

```sh
xmake build loadgen && xmake run loadgen http://127.0.0.1:7791/world -p h1 -n 300000 -c 100
xmake run loadgen http://127.0.0.1:7790/world -p h2  -n 600000 -c 20 -m 40   # h2load -c×-m 模型
xmake run loadgen ws://127.0.0.1:7788/echo1k -p ws -n 200000 -c 64
```

`h1` 走会话直持；`pub` 走公开 `Client`；`h2` 走 `-c` 连接 × `-m` 流；`ws` 走
`open_websocket`。`-t N` 多 context（每线程一个 io_context + 自己的 Client）。参照数据与
已知限制见源码注释与 ROADMAP「同源高并发 h1」。`detail::ClientEngine::connect` 返回调用方
独占的会话（owner 自管生命周期，不回池、不空闲回收）。

## 构建与运行

工程用 **xmake**（`xmake.lua`）。同时存在 `CMakeLists.txt`（仅把库作为 INTERFACE 目标
安装，供下游 `find_package`）。日常开发用 xmake。

依赖（`add_requires`）：`boost`（asio+regex）、`openssl3`、`nghttp2`、`ngtcp2`、
`nghttp3`、`glaze`；**仅测试目标用** `catch2`；压缩目标额外用 `zlib`/`brotli`。启用了
reproducibility 锁（`package.requires_lock`）。

编译期宏（`xmake.lua` 中 `add_defines`）：

- `SIMPLE_HTTP_USE_BOOST_REGEX`：正则用 boost.regex。
- UNIX domain socket **没有开关宏**：可用性由 `BOOST_ASIO_HAS_LOCAL_SOCKETS` 自动决定。
  配置写 `cfg.listen = UnixAddress{"/run/app.sock"}`；绑定前会 unlink 旧 socket 文件。
  WebSocket 同样无开关宏，始终编入。
- `SIMPLE_HTTP_ENABLE_LOG`：日志总开关（`core/logging.h` 默认 1；本仓库的 `xmake.lua`
  **不设**这个宏，下游如 v2ray-cpp 才设成 0）。设为 0 时 `SIMPLE_HTTP_*_LOG` 展开为
  `((void)0)`，注意别让变量只在日志里被用到。
- `SIMPLE_HTTP_LOG_ACTIVE_LEVEL`（默认 0 = Trace）：低于该级别的记录在**编译期**被丢弃。
- `SIMPLE_HTTP_ENABLE_HTTP3`（默认未定义）：编入 HTTP/3 引擎与 QUIC 监听，需 ngtcp2 +
  nghttp3。未定义时 `ServerConfig::quic` 与 QUIC 调优字段**本身不存在**。xmake 的包来自
  私有仓库 `fantasy-peak/xmake-repo`（上游 ngtcp2 是 `-DENABLE_OPENSSL=OFF`，不产 crypto
  helper，没有它就没有 TLS）。**不要链 `/opt/h3/lib` 的预编译库**（对系统 OpenSSL 编，
  与本仓库的 openssl3 ABI 风险）。
- OpenAPI **没有开关宏**：glaze 是主库默认依赖（`proto/json.h` 已无条件引入），文档
  按路由级 `openapi::doc()...` 描述收集，常编（见 OpenAPI 一节）。

gzip/brotli 响应压缩与 WebSocket permessage-deflate **没有开关宏**：zlib 与 brotli
是主库的常规依赖，永远编入；是否生效是运行期配置——响应压缩按 Go 框架的形态用
**中间件挂载**（`server.use(group/route).compress(cfg)`，即 chi/gin/echo 的
`middleware.Compress/Gzip`，挂载=对该作用域开启，逐请求再按 Accept-Encoding/大小/类型
判定）；WebSocket permessage-deflate 由 `EngineLimits::ws_compression` 协商、连接级
用 `enable_write_compression`/`set_compression_level` 调；客户端 `ClientConfig::auto_decompress`。

常用命令：

```sh
xmake build -j12         # 构建（自动更新 build/compile_commands.json）
xmake run server         # 运行示例服务器（明文 :7788，TLS :7789，h2c :7790，h1 :7791，
                         #  大窗口 h2 :7793，双传输 tcp+udp :7792）
```

### 测试

四套自检（前三套默认不参与 all，`set_default(false)`）：

```sh
xmake build unittest && xmake run unittest      # 纯逻辑单元测试（Catch2，秒级；257 用例）
xmake build regression && xmake run regression  # 服务端对抗性回归（裸 socket）
xmake build client && xmake run client          # 客户端整合自检（PASS/FAIL + 退出码；161 checks）
xmake build server && xmake run python-tests    # 第三方客户端驱动服务端（54 checks）
```

- **`test/python/`**：用 **httpx / hyper-h2 / websockets** 这三个**独立实现**驱动
  `test/server.cpp` 起的服务端。理由是 C++ 套件用的是本库自己的客户端，客户端与服务端
  **共享的规范误读会互相抵消**。依赖用虚拟环境管理（`test/python/requirements.txt`，版本
  已 pin），首跑需先建 venv。
- **`test/unit/`（Catch2）**：core / proto / h1 解析器 / h2 帧·HPACK·Huffman / WebSocket
  帧与消息层（MockTransport 驱动）/ 路由 / CORS / 客户端 URL·配置·TLS 参数·错误码。
  按标签过滤：`xmake run unittest "[h2]"`。
- **`test/server_regression.cpp`**：裸 socket 发畸形/边界请求并断言线上行为。
- **`test/client.cpp`**：客户端整合自检（协议矩阵、流式、并发、单连接复用、TLS、反代、
  可配置重试、错误注入）。
- **`test/openapi_demo.cpp` + `test/openapi_verify.py`**：OpenAPI 端到端（41 项断言）。

### 压力测试统一入口（改动后必跑的一条龙）

协议层/传输层改动落地后，**只执行这一个脚本**即完成「清理 → 符合性 → 压力」全流程：

```sh
test/stress/run.sh [cpp|rust|both] [h1|h2|h3|ws]...
test/stress/run.sh both          # cpp + rust 四档全跑（默认 both）
test/stress/run.sh cpp h3        # 只跑 cpp 的 h3 档
```

脚本内建，顺序固定：**强制清理**（杀光遗留测试服务并验证 TCP/UDP 端口释放；不清理会让
SO_REUSEPORT 把 QUIC 连接表劈成两半）→ **符合性**（`RUN_CONFORMANCE=1` 时先跑
`test/conformance/run.sh`，全绿才继续）→ **压力**（h1/ws 走 k6，h2c/h3 与复用门线走
h2load）。规格：请求 POST 1KB body，响应三档 2k/10k/20k，每档 20 万请求，**失败数必须为
0**。头部「测试参数配置」可随时改。

### 外部一致性套件（每次改动后必跑）

```sh
test/conformance/run.sh          # 三套依次跑；非零退出 = 有套件未达标或环境不全
test/conformance/run.sh h2spec   # 只跑指定的（h2spec / h1spec / autobahn / h3spec）
```

| 套件 | 打哪个端口 | 基线 |
|---|---|---|
| **h2spec** | `:7790`（纯 h2c） | **146/146**（`-S` strict 147/147） |
| **h1spec** | `:7791`（纯 h1） | **33/33** |
| **Autobahn** | `:7788`（嗅探） | 517 用例：**FAILED 0、NON-STRICT 0** |
| **h3spec** | `udp :7792`（纯 h3） | **49 用例：通过 ≥47** |

- **端口不能混用**：`7790`/`7791` 是单协议端点，嗅探端口上「畸形 h2 前导」与「畸形 h1
  请求行」是同一批字节，h2spec 要 GOAWAY、h1spec 要 400。
- 依赖不在仓库里（许可证/体积），脚本自检并打印准备命令：h2spec 预编译二进制（v2.6.0 由
  Go 1.12.7 构建，**不支持 TLS 1.3**，只测明文）；h1spec 无许可证第三方脚本；Autobahn 走
  Docker。
- **UNIMPLEMENTED / INFORMATIONAL 不算失败**，但 **NON-STRICT 算**。

### 负载与连接生命周期套件（改传输层或流的生老病死之后必跑）

一致性套件每例从干净连接开始，看不见「同一条连接上第 N 个请求」。`test/stress/run.sh` 专问
那件事：复用门线（一条连接串行 200 请求必须全成）+ 吞吐（只报告，不写死，因为依赖机器）。
门线只有一条：**失败数必须为 0**。`-t/-c/-m/-n` 等见脚本头部配置。QUIC 侧要开
`ServerConfig::reuse_port`，否则只跑在机器一小部分上。

### 丢包与乱序下的复现（手工，需要 root）

QUIC 恢复路径只在丢包时被走到，一致性套件与负载套件都在无丢包回环上跑。手法：`tc qdisc
add dev <iface> root netem loss 3%` 注入丢包，`h2load --alpn-list=h3 …` 压，**先确认 netem
生效**（接口名 WSL2 是 `loopback0`，普通 Linux 是 `lo`），**必须重复跑**（方差 4 倍量级），
并**找一个对照实现**。用完 `tc qdisc del`。ngtcp2 版实测 3%/10% 丢包下每轮 20000/20000
全成、0 失败。

## 代码风格

- `.clang-format`：LLVM 基线 + `IndentWidth: 4`、`ColumnLimit: 120`。提交前跑
  `./format.sh`（或 `./format.sh --check`，CI 用）。
- 编译告警按错误处理（`set_warnings("all", "error")`），不要留 warning。
- 遵守分层：上层（proto/handler/net）不得出现协议版本分支，版本差异只放进
  `engine/` 和对应的 `ResponseWriter` 实现。
- RAII、无裸 `new`/`delete`；所有权显式。

## 近期修复（供参考，避免重犯）

- **WebSocket 入站死锁**：串行 read→write 的 handler 在写被对端零窗口挡住时不再读，与
  「猛发不读」的客户端互相压死。修法：内部有界读泵（见上）。复现：k6 ws 单连接偶发卡到
  20s 超时、少 2000 条。
- **HTTP/2 全局暂停**：请求体 channel 满时曾置**连接级** `m_body_paused`，一条流就能拖住
  整条连接的所有流。修法：改为每流 `paused_frames` 停放 + 继续解析（连接接收窗口仍是总量
  上限）；`END_STREAM` 在停放期间到达时记 `finish_pending`，排空后才 `body().finish()`
  （否则 `Body::read()` 会提前 EOF 截断 body——ASan 下 `/upload` 2000 字节收到 1025 才暴露）。
  复现已固化为 `test/python/test_regressions.py`。
- **`Body::feed` 语义**：改为收 `std::string&&` 并直接转发 `try_send`——asio 只在成功分支
  移动参数，失败时参数原样保留，调用方可重试；提前包成 `Frame` 会无条件 move、失败即丢。
