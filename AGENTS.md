# AGENTS.md — simple_http

面向参与本工程的 AI/协作者的快速上手说明。全部内容基于源码整理。

## 这是什么

`simple_http` 是一个 **header-only 的 C++ HTTP 服务器 + 客户端库**，基于
`boost.asio` C++20 协程实现。服务端支持 HTTP/1.0、HTTP/1.1、HTTP/2（h2 / h2c /
prior-knowledge）、HTTP/3（QUIC）、WebSocket；客户端（`client/` 层）支持
http/https × HTTP/1.1/HTTP/2，通过 ALPN（TLS）或 h2c（明文，Upgrade 或
prior-knowledge）自动协商。**HTTP/3 只有服务端**，且默认关闭
（`SIMPLE_HTTP_ENABLE_HTTP3`）。

关键事实（来自代码）：

- **纯头文件库**：所有实现都在 `include/` 下的 `.h` 里，没有 `.cpp` 源文件。`xmake.lua` 的
  `target("simple_http")` 是个 `set_kind("static")` 的空壳目标，只把 `include/` 与依赖以
  `public` 方式带出去（`CMakeLists.txt` 同理由下游 `find_package` 使用）。
- **手写协议编解码**：不依赖 beast/nghttp2（连头文件都不包含；`Status`/`Field`
  常量在 `core/http_status.h`、`core/http_field.h`，`Method` 在 `core/http_method.h`）。
  HTTP/1.x、HTTP/2 帧层、HPACK、
  WebSocket 帧均在 `include/simple_http/engine/` 与 `proto/` 内手写实现。仅
  依赖 `boost.asio`（裸 TCP socket 与 `asio::ssl::stream`）+ OpenSSL。
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
      logging.h            日志 facade（LogSink / set_log_sink / SIMPLE_HTTP_*_LOG）
      mime.h               MIME 类型常量 + by_extension（扩展名→类型）
      http_date.h          IMF-fixdate 格式化/解析 + now_unix（手写，不用 strftime）
      url_path.h           请求路径解码与规范化（查找键）+ under_prefix
      accept_encoding.h    Accept-Encoding qvalue 解析（独立于编码器层）
      validators.h         ETag / 弱比较 / Range 解析
      static_table.h       文档根 → 不可变查找表（静态服务的协议无关半边）
      base64.h             base64（含 WebSocket 握手用标准 base64）
      io_pool.h            IoCtxPool（单线程 io_context 池，并发模型 A）
      limits.h             EngineLimits（超时/大小上限/HTTP2 窗口等）
    proto/                 版本无关的 HTTP 模型
      headers.h            Headers
      body.h               Body（流式请求体）
      request.h            Request
      response.h           Response（fluent，面向用户）
      response_writer.h    ResponseWriter（版本收敛的纯虚接口）
      ws_frame.h           WebSocket 帧编解码 + 握手 key（手写）
      websocket.h          WebSocket 句柄（transport-agnostic，含写泵）
    transport/             字节流传输抽象
      transport.h          Transport concept + SslHandle
      tcp_transport.h      TcpStreamTransport（裸 TCP）
      tls_context.h        TlsContext / TlsConfig（OpenSSL + ALPN）
      tls_transport.h      TlsStreamTransport（ssl::stream）
    engine/                各版本协议引擎（实现 ResponseWriter）
      dispatcher.h         Dispatcher / WsLookup / WsHandlerFn 类型（引擎↔handler 的窄接缝）
      h1/                  HTTP/1.x 引擎 + 请求头解析器 h1_parser.h（含 WebSocket 升级握手、
                           h2c 升级）；ws_proxy.h 是字节级 WebSocket 隧道（与 handler/http_proxy.h
                           的请求级反代相对）
      h2/                  HTTP/2 引擎 + 帧层 + HPACK（h2_frame / hpack_*）
      h3/                  HTTP/3 引擎——nghttp3（帧层 + QPACK）与本库 Dispatcher 之间的
                           适配层：h3_engine.h（引擎 + Http3ResponseWriter）、
                           h3_stream.h（每流的收发队列与唤醒）、h3_callbacks.h（nghttp3
                           回调表）。行数比手写时少一半，因为帧层、QPACK、SETTINGS/GOAWAY
                           与请求合法性校验都归 nghttp3 了
                           （#ifdef SIMPLE_HTTP_ENABLE_HTTP3）
    handler/               handler 类型系统 + 路由
      handler.h            Handler 类型别名与 make_handler/invoke_handler
      cors.h               内建 CORS：CorsConfig 编译成 Filter（预检在 filter 内应答 204，不进路由）
      router.h             Router（含反代用的 HttpClient、反代匹配、静态阶段）
      http_proxy.h         请求级反代：上游走 client 层（连接池/TLS/h2），响应流式回传
      static_files.h       静态文件服务（static_table.h 的 serving 半边 + SPA fallback）
    quic/                  QUIC 传输（#ifdef SIMPLE_HTTP_ENABLE_HTTP3）——ngtcp2 的包装，
                           不是一份 QUIC 实现：包解析、密钥派生、ACK、丢包恢复、拥塞控制、
                           流状态机与流控都在 ngtcp2 里
      protocol.h           quic::Protocol：连接↔协议引擎的窄接缝（事件上报 +
                           next_stream_data()：由连接从引擎"拉"要发的字节）
      connection.h         QuicConnection——驱动 ngtcp2：收发、定时器、回调表
      endpoint.h           QuicEndpoint——一个 UDP socket 与它上面所有连接；按 CID 解复用，
                           外加 Version Negotiation / Retry / stateless reset 三件无状态答复
      ngtcp2_config.h      QuicConnectionConfig → ngtcp2_settings / transport_params 的映射，
                           以及哪些旋钮在 ngtcp2 下没有对应物
      ngtcp2_crypto.h      每连接的 SSL 与 ngtcp2_crypto_ossl_ctx（TLS 握手由它驱动）
      tls.h                QuicTlsContext（SSL_CTX + ALPN + mTLS）与 h3 ALPN 常量
    net/                   监听与连接协议检测
      connection.h         serve_plaintext / serve_tls（协议检测）
      server.h             Server 门面 + ServerConfig / Listen
    client/                出站 HTTP 客户端（与 server 对称的一层）
      client_config.h      ClientConfig / ClientTarget / TlsClientConfig / client_errc
      url.h                绝对 URL 解析（parse_url）
      client_stream.h      RequestSpec / ResponseHead / ClientStream / ClientSession
      tls_client.h         客户端 ssl::context 与握手（SNI / ALPN / 主机名校验 / mTLS）
      h1_client.h          HTTP/1.1 会话（一次一交换，keep-alive、chunked、h2c Upgrade）
      h2_client.h          HTTP/2 会话（多路复用、流控、SETTINGS/PING/GOAWAY、h2c 播种流 1）
      client_pool.h        空闲连接池（每个 HttpClient 一个，按 executor/origin 分区）
      http_client.h        HttpClient 门面：connect/open_stream + get/post/request
      client.h             聚合头
    openapi/               OpenAPI 3.1 文档 + Swagger UI（#ifdef SIMPLE_HTTP_ENABLE_OPENAPI）
      openapi_doc.h        OAS 3.1 文档模型 + 手写 JSON 渲染器 + components/schemas 去重
                           （glaze-free：只有发射逻辑，schema 以 raw JSON 接入）
      openapi.h            glaze 反射→内联 schema、openapi::query<T>/resp<T>/response_header<T>、
                           path_params<T>（axum Path 提取器）、read_body<TReq>（400/422 校验）
```

其它目录：

- `test/`：`server.cpp` 是示例服务器；`client.cpp` 是客户端整合自检；`server_regression.cpp`
  是服务端对抗性回归；`unit/` 是 Catch2 单元测试；`conformance/` 与 `stress/` 是两套**外部**
  驱动脚本（见下两节）；`python/` 是第三方客户端套件；`manual_http1_keepalive.py` 是可选的
  **手工**交叉验证；`tls_certificates/` 是测试证书（`server.cpp.test` 是未参与构建的旧快照）。
- 没有 `docs/` 目录：架构说明就在本文件与各头文件顶部注释里。

## 架构要点（来自代码与设计）

1. **协程贯穿始终**：handler 是 `asio::awaitable<void>`，读请求体、写响应
   都是 `co_await` 的异步操作，天然反压。
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
   询问。适配 spdlog 或公司日志库的代码写在**消费者侧**（可复制示例见 README 的 Logging 节，
   实际用例见 `v2ray-cpp/src/flux.cpp`）——把适配器放进库里会把第三方头文件带进每个消费者的
   include 树，还得跟着对方的版本变化走。安装 sink 用 `set_log_sink`，运行中替换是安全的
   （`std::atomic<std::shared_ptr<>>`），不再有 `LOG_CB` 那种全局 `std::function` 的竞争。

## 公共 API（以源码为准）

Handler 是协程，通过实参个数在编译期自动选择形态（`handler.h`）：

```cpp
using RequestPtr  = std::shared_ptr<Request>;
using ResponsePtr = std::shared_ptr<Response>;

// 普通         : awaitable<void>(RequestPtr, ResponsePtr)
// 带 TLS 句柄  : awaitable<void>(RequestPtr, ResponsePtr, SslHandle)
// WebSocket    : awaitable<void>(RequestPtr, std::shared_ptr<WebSocket>)
// 过滤器       : awaitable<bool>(RequestPtr, ResponsePtr)  // 返回 false 短路
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

Server server{cfg};
server.route({Method::Get}, "/world", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
    co_await res->status(200).send("hello");
});
// 路由 = (method, path) 对，方法先行。方法集可以来自任意可迭代容器——配置解析
// 出来的 std::vector<Method> 直接传。空集（any_methods）表示「任意方法」。
server.route({Method::Post, Method::Put}, "/users", handler);
server.route(any_methods, "/webhook", webhook_handler);   // 任意方法
server.route_regex({Method::Get}, "/api/.*", handler);
server.ws_route("/chat", ws_handler);
server.fallback(not_found_handler);

server.start();      // 同步：启动所有监听并阻塞至绑定完成，返回 bool
// 或 co_await server.run();  // 协程版
server.stop();
```

路由层是**方法感知**的（`router.h`）：精确路由按 `(method, path)` 存（每 path 一张按方法索引的表，`method_bits` 折叠）；**含 `{name}` 的路径是段 trie**——普通 `route({Method::Get}, "/a/{x}", h)` 不需要任何 OpenAPI 形态即可路由 `/a/5`，handler 用 `req->param("x")` 取捕获（OpenAPI 的 `path_params<T>()` 只是把捕获按类型解析进结构体，是上层增强）。两条隐式规则跟着主流框架走（Flask / axum / Go 1.22 / Spring）：

- **GET 隐含 HEAD**：注册 `{Method::Get}` 自动允许 HEAD，走同一 handler，writer 抑制 body（write 层本就按 HEAD 行为抑制，有回归测试）。
- **405 + Allow（及自动 OPTIONS）**：path 存在但方法不符 → 405，`Allow` 头列出该 path 全部方法（含自动应答的 OPTIONS）；OPTIONS 本身则自动回 204 + Allow。语义照抄 static 阶段的「resolved first, then rejected」——path 不存在仍是 404，不是 405。405/OPTIONS 是**普通 keep-alive 响应**（对齐 Go ServeMux / axum / Spring：返回后连接不断，引擎 drain 未读请求体后继续复用；曾有实现一度关闭连接，已改为主流行为）。`Method::Unknown`（扩展方法 token）不可注册，但 `any_methods` 路由会接住它。

CORS 是策略入口（`handler/cors.h`）：`CorsConfig` 编译成 `Filter` 存进 `m_cors`。

```cpp
// 带 Origin + Access-Control-Request-Method 的 OPTIONS 在 filter 内应答 204（无 body、
// 无 framing）并短路，所以预检永远不会落到路由上——引擎层不特判 OPTIONS，交给路由只会
// 得到 404，浏览器据此判定预检失败。
server.cors(CorsConfig{.allow_origins = {"https://app.example"}});  // {} = 任意 origin
```

`CorsConfig` 的字段语义见 `README.md` 的 CORS 段。`CorsConfig` 表达不了的策略（按路径
分白名单、运行期查列表、PNA 预检）放 `before` filter——注意 `before` 对每个请求都跑，
需自行判 `Origin` 并自己加 `Vary: Origin`；只想收窄内建行为可用 `make_cors_filter()`。

三条容易踩的边界：被拒 origin 的预检同样回 204（只是不带 CORS 头，这样浏览器发出的
OPTIONS 不会在日志里变成 404）；实际请求不会因此被拦（CORS 是浏览器闸门而非鉴权）；
WebSocket 升级走 `WsLookup`、不经 dispatch，不受此影响。

`Response`（fluent，一次性或流式）：

```cpp
co_await res->status(200).content_type("text/plain").send(body);  // 一次性
// 流式：
co_await res->status(200).begin();
co_await res->write(chunk);
co_await res->finish(last);
```

`Request` 常用：`req->method()`（返回 `Method`）、`req->path()`、`req->query()`、
`req->version()`、`req->header(name)`、`req->body().read()` / `read_all()`。

`WebSocket`（`proto/websocket.h`，handler 收到 `shared_ptr<WebSocket>`）：

```cpp
server.ws_route("/chat", [](RequestPtr req, std::shared_ptr<WebSocket> ws) -> asio::awaitable<void> {
    for (;;) {
        auto msg = co_await ws->read();          // expected<WsMessage, error_code>
        if (!msg) break;                         // 对端关闭或出错
        // msg->data 是消息内容，msg->text 标识 text/binary
        if (co_await ws->write("echo: " + msg->data, msg->text)) break;
    }
    co_return;  // 返回后引擎自动发 Close 帧并优雅关闭
});
```

WebSocket 实现要点（`ws_frame.h` + `websocket.h`）：

- 帧编解码手写实现，消息保持在内存（不落临时文件）；单帧与分片重组累计大小都受
  `EngineLimits::max_body_bytes` 约束。
- `read()` 返回 `WsMessage{data, text}`（类型随返回值交付，不再有 `got_text()`）；
  自动重组 continuation 分片、Ping→Pong、收到 Close 回 Close。
- `write_text`/`write_binary`/`write`/`close` 安全用于任意协程：每个操作先 hop
  回连接 executor（并发模型 A），写操作再经内部写泵（`run_writer`）串行化。
- 升级握手在 HTTP/1.1 引擎内完成（`Upgrade: websocket` → 101）；ws 走明文、
  wss 走 TLS（ALPN 非 h2 时落到 H1 引擎）。**HTTP/2 的 WebSocket（RFC 8441）尚未接入。**

关于协议升级路径（均在 H1 引擎 `serve_loop` 解析完请求头后判定）：

- **WebSocket 升级**：`Upgrade: websocket` + `Sec-WebSocket-Key`，命中 ws 路由则发
  101 并交给 WebSocket 层。
- **h2c 明文升级**：`Upgrade: h2c` + `HTTP2-Settings`，发 101（`Connection: Upgrade` /
  `Upgrade: h2c`）后交给 `Http2Engine::run_h2c`，把原请求重放为 HTTP/2 stream 1，
  连接后续以 h2 继续。带 body 的升级请求（如 POST）其 body 会一并重放到 stream 1。
- 升级成功后 transport 交由对应协议层接管，H1 引擎不再关闭它（`m_upgraded`）。

完整用法参考 `test/server.cpp`，其中演示了一次性响应、双向流式、TLS 客户端证书
检查、大 body、延迟响应、异常处理、HTTP/2 全双工、WebSocket 回显等。

### OpenAPI（`openapi/` 层，`SIMPLE_HTTP_ENABLE_OPENAPI`）

宏门控 + **glaze**（下游自备，header-only；`glaze_json_schema` 注解提供字段级
description/enum）。typed 路由在注册时把 `(方法, path)`、body schema、参数、安全、
错误响应收进一份 OAS 3.1 文档，serve 出去给 CDN 版 swagger-ui 浏览：

```cpp
server.openapi().title("petshop").version("1.0.0").server("http://127.0.0.1:7795")
    .security_scheme("bearer", openapi::bearer());   // Authorize 按钮的来源

// 无请求体：<Res> 一参；带 body：<Req, Res> 两参
server.route<Pet>({Method::Get}, "/pets/{id}", handler,
    openapi::OperationInfo{.summary = "get a pet", .tag = "pet", .operation_id = "getPet"},
    openapi::query<std::string>("q", {.description = "..."}),   // query/path/header 参数
    openapi::resp<ErrorBody>(404, "no such pet"));              // 任意多个错误响应
// Params 单一声源：模板 {name} 必须等于结构体字段名（注册时校验，不符即跳过）
server.route<OrderItemParams, openapi::NoBody, Order>({Method::Get},
    "/orders/{order_id}/items/{item_id}", handler);
// 运行时取参：req->param("id")（string_view）或按类型解析进结构体
auto p = openapi::path_params<OrderItemParams>(*req);   // 缺/坏 → nullopt, handler 定 404
// 请求体校验：严格读（error_on_missing_keys）失败再宽松读——400 畸形 / 422 缺必填
auto in = co_await openapi::read_body<CreateUserReq>(*req, err);

server.serve_openapi("/openapi.json");    // 启动同时对账：文档说过但路由没服务的 → warning
server.serve_swagger_ui("/swagger", "/openapi.json");   // CDN 页，离线换 static_files
```

要点：
- **schema 是手写的、swagger 安全的**：glaze 反射给名字/类型，输出内联 JSON Schema；
  **不用** `glz::write_json_schema`（它吐 `$defs` + `#/$defs` 引用，写进 OAS 中段会让
  swagger-ui 解析崩溃，抓过一次）。重复的对象类型会收拢进 `components.schemas` 并以
  `$ref` 引用（文档根级，swagger 可解析）。
- **路径模板 = 段 trie**（`router.h`）：`{name}` 匹配恰好一个非空段，正则在之后。
  模板之间共享匿名 param 边、名字挂在终点（`/owners/{owner}/pets/{pet_id}` 与
  `/owners/{owner_id}/pets` 可共存），同形路由的名字必须一致否则跳过。
- **405/405 保持连接**：method 不符回 405 + `Allow`（并集，含自动 OPTIONS），
  连接**不断**（对齐 Go ServeMux / axum；HTTP/1.1 下引擎 drain body 后继续 keep-alive）。
- 完整用法参考 `test/openapi_demo.cpp`（含四个刁钻用例：混合标量类型、布尔严格、
  字面量压模板、正文+参数+安全叠加）+ `test/openapi_verify.py` 的 41 项断言。

### 客户端（`client/` 层）

与 handler 一样是协程，但方向相反：内部分**会话层**（连接 + 流）与**便捷层**
（一次调用拿到完整响应）。协议由客户端自己协商：TLS 走 ALPN（`h2` / `http/1.1`），
明文走 h2c（`H2cMode::Upgrade` 单次 Upgrade 往返，或 `PriorKnowledge` 直接发 preface）；
协商不到要求的版本返回 `client_errc::version_not_negotiated`，不会静默降级。

```cpp
simple_http::HttpClient http;                       // 策略：TLS/超时/限制/连接池
auto r = co_await http.get("https://host/path");    // expected<ClientResponse, error_code>
auto p = co_await http.post("http://host/echo", "body", simple_http::mime::text_plain);

simple_http::ClientTarget target;                   // 多 origin：按 target 分别建连/复用
target.host = "10.0.0.5"; target.port = 8080; target.use_tls = true;
auto session = co_await http.connect(target);       // expected<shared_ptr<ClientSession>>
auto stream  = co_await (*session)->open_stream(spec);   // h2 可并发多流
co_await (*stream)->write(chunk);                   // 中间块
co_await (*stream)->finish(last_chunk);             // 结束请求体
auto head = co_await (*stream)->read_head();        // ResponseHead：status/version/headers/bodyless
while (auto chunk = co_await (*stream)->read()) {   // 响应体：data 或 eof（复用 ReadResult）
    if (chunk->eof) break;
    /* chunk->data */
}
// 头有缓存：read_head() 幂等，read() 之后仍可 st->status() / st->head()
```

要点：
- **一次一交换 vs 多路复用**：h1 会话同一时刻只服务一个交换（再开返回
  `session_busy`），h2 会话可同时开任意多流；两种版本的 `ClientStream` API 完全一致。
- **读写形状与服务端对称**：请求体 `write()`/`finish()`（h1 走 chunked、h2 走 DATA 帧）；
  响应体 `read()` 返回 `ReadResult`（data 或 eof），失败是 error_code——
  `RST_STREAM` → `stream_reset`，连接中途断开/截断 → `connection_reset`。响应头由
  `read_head()` 显式读取（幂等、有缓存），不会被当成 body 事件。
- **背压**：h2 响应体按“应用消费多少补多少 WINDOW_UPDATE”，出站 DATA 受双窗口 + 水位
  约束；h1 直接拉取 socket，天然反压。流式上传用 chunked（h1）/ DATA（h2）。
- **可复用性**：只有响应长度明确（Content-Length / chunked）且连接未要求关闭时，
  h1 会话才回池；h2 在无在途流且未 GOAWAY 时回池。池按 (executor, scheme, host,
  port, 版本策略, tag) 分区，每客户端独立（避免把不同 TLS 校验策略的连接串用）。
- **重试**：仅当失败来自**池中**连接的传输错误（对端已关闭）且请求体可重放时，才在
  新连接上重试一次；协议/策略类错误直接上报。
- **所有权**：`ClientStream` 强引用会话；h2 会话的读写循环自持（空闲池中会话由池持有，
  池 armed 的 idle 计时器到期即关闭）。h2c 升级成功后由 successor 持有前一个（池句柄）
  会话，避免悬空。
- **线程模型**：沿用「模型 A」——会话绑定一个 executor；所有流操作先
  `dispatch` 回该 executor，`HttpClient` 可跨线程共享（池有锁）。
- **反代**：`Server::http_proxy*` 注册的路由由 `handler/http_proxy.h` 处理，上游复用 client 层
  （连接池、陈旧连接重试一次、h2 多路复用）；`HttpProxyTarget::tls` 让后端走 HTTPS 并由 ALPN
  自动选 h2 / HTTP/1.1（`h2c` 则对明文后端尝试一次 Upgrade）。**每条反代路由自带一个
  `ClientConfig`**（`http_proxy(path, target, client_cfg)`，缺省适合公网后端）——CA/客户端证书/
  是否校验/超时/池大小都是这条路由自己的，不同后端各自独立（内部 mTLS 后端和公网后端绝不共享
  凭据或连接池），所以它不属于 `ServerConfig`，也不设全局。WebSocket 反代
  （`ws_proxy*`）是字节级隧道，与 client 层无关。

完整用法与边界用例参考 `test/client.cpp`（自检程序，`xmake run client`）。

## 构建与运行

工程用 **xmake** 构建（`xmake.lua`）。同时存在 `CMakeLists.txt`（仅把库作为
INTERFACE 目标安装，供下游 `find_package`）。日常开发用 xmake。

依赖（`add_requires`）：`boost`（asio+regex）、`openssl3`，以及**仅测试目标用**的 `catch2`
（`unittest` / `regression`，二者 `set_default(false)`）。启用了 reproducibility 锁
（`package.requires_lock`，新增依赖会改动 `xmake-requires.lock`）。

编译期宏（`xmake.lua` 中 `add_defines`）：

- `SIMPLE_HTTP_USE_BOOST_REGEX`：正则用 boost.regex。
- UNIX domain socket **没有开关宏**：可用性由 `BOOST_ASIO_HAS_LOCAL_SOCKETS` 自动决定
  （`local::stream_protocol` 是 Asio 的一部分，零额外依赖，消费者无需定义任何东西）。
  配置写 `cfg.listen = UnixAddress{"/run/app.sock"}`；绑定前会 unlink 旧 socket 文件，
  否则重启必然 EADDRINUSE。WebSocket 同样无开关宏，始终编入。
- `SIMPLE_HTTP_ENABLE_LOG`：日志总开关（`core/logging.h` 默认 1；本仓库的 `xmake.lua` **不设**
  这个宏，下游如 v2ray-cpp 才设成 0；设为 0 时 `SIMPLE_HTTP_*_LOG` 展开为 `((void)0)`，
  注意别让变量只在日志里被用到，否则触发 `-Wunused-variable`）。
- `SIMPLE_HTTP_LOG_ACTIVE_LEVEL`（默认 0 = Trace）：低于该级别的记录在**编译期**被丢弃
  （只留 Warn 及以上就配 3，只留 Error 及以上配 4）。比 `SIMPLE_HTTP_ENABLE_LOG=0` 更细：
  既能留下错误日志，又不为被丢弃的级别生成任何代码。
- `SIMPLE_HTTP_ENABLE_HTTP3`（默认未定义）：编入 HTTP/3 引擎与 QUIC 监听。未定义时
  `ServerConfig::quic` 与 QUIC 调优字段**本身不存在**，所以不定义它的构建不可能要到一个
  UDP 侧。
  打开它就要编入 **ngtcp2 + nghttp3**（两个 C 库，只有这条路径用）。这曾是反过来的：QUIC
  与 HTTP/3 都是手写的，依赖里明确不含 ngtcp2。改的原因是手写一份 RFC 9000 的代价——
  丢包恢复、PTO、流生命周期这些只在丢包与"同一条连接的第 N 个请求"下才暴露的地方，出过
  两次真问题（见下面两节）。xmake 的包来自私有仓库 `fantasy-peak/xmake-repo`：上游
  xmake-repo 的 ngtcp2 是 `-DENABLE_OPENSSL=OFF` 构建的，**不产 crypto helper**，没有它
  就没有 TLS。`nghttp3` 用上游包即可。**不要链 `/opt/h3/lib` 的预编译库**——那是对系统
  OpenSSL 3.5.5 编的，本仓库用 openssl3 3.6.3，混链是 ABI 风险。
- `SIMPLE_HTTP_ENABLE_OPENAPI`（默认未定义）：编入 OpenAPI 3.1 文档生成 + Swagger UI
  服务（`openapi/` 层与 `Router::route<Params, Second, Res>` / `serve_openapi` 等 API）。
  需要下游自己把 **glaze** 放到 include 路径（header-only）；主库 target 不依赖它，镜子
  压缩的 opt-in 先例——只在本仓库的 `openapi_demo` 与 `unittest` target 启用。schema
  用 glaze 反射与 `glaze_json_schema` 注解生成，但**输出是手写内联、无 `$ref`/`$defs`**
  （`$defs` 写进 OAS 中段会让 swagger-ui 解析崩溃，抓过一次），对象类型收拢进
  `components/schemas` 再以 `$ref` 引用（文档根级，swagger 可解析）。宏未定义时这些
  API 与字段**不存在**。

常用命令：

```sh
xmake build -j12         # 构建（自动更新 build/compile_commands.json）
xmake run server         # 运行示例服务器（明文 :7788，TLS :7789）
```

### 测试

四套自检（前三套默认不参与 all，`set_default(false)`）：

```sh
xmake build unittest && xmake run unittest      # 纯逻辑单元测试（Catch2，秒级）
xmake build regression && xmake run regression  # 服务端对抗性回归（裸 socket）
xmake build client && xmake run client          # 客户端整合自检（PASS/FAIL + 退出码）
xmake build server && xmake run python-tests    # 第三方客户端驱动服务端（见下）
```

- **`test/python/`**：用 **httpx / hyper-h2 / websockets** 这三个**独立实现**驱动
  `test/server.cpp` 起的服务端。存在的理由是：前三套用的都是 simple_http **自己的客户端**，
  客户端与服务端**共享的规范误读会互相抵消**——两边一起错，测试照样绿。独立实现只在服务端
  真的错的地方分歧。它已经抓到过：handler 抛异常会**断开连接**而不是回 500（C++ 客户端只报
  一个裸断开，套件里也没人问过 500）。依赖用虚拟环境管理（`test/python/requirements.txt`，
  版本已 pin），首跑需先建 venv；CI 会跑这套。

- **`test/unit/`（Catch2）**：不碰 socket 的单元测试——core（types/method/status/base64/logging/
  io_pool）、proto（Headers/Body/Request/Response + 假 ResponseWriter）、h1 请求与响应解析器、
  h2 帧/HPACK/Huffman、WebSocket 帧与消息层（用 `MockTransport` 驱动）、路由匹配与 `$1` 重写、
  CORS 策略（预检应答、origin 匹配、回显上限）、客户端 URL/配置/TLS 参数/错误码。
  `test/unit/test_support.h` 放共用测试替身。按标签过滤：
  `xmake run unittest "[h2]"`。
- **`test/server_regression.cpp`**：起进程内服务器（明文 + mTLS），用**裸 socket** 发畸形/边界
  请求并断言线上行为——400/431/413、CL+TE 冲突、坏 chunked、trailers、流水线、HEAD/204、
  keep-alive 与空闲超时、h2 的 SETTINGS/ACK、PING、GOAWAY（帧过大/流 0 DATA/偶数流 id/SETTINGS
  长度/流控越界）、h2c 升级、WebSocket 握手与分片/Ping/Close、TLS 的 mTLS 与 TLS1.3 要求。
- **`test/client.cpp`**：客户端整合自检（协议矩阵、流式、64 路并发、连接池、TLS 校验、
  反代、错误注入）。
- **`test/openapi_demo.cpp` + `test/openapi_verify.py`**：OpenAPI 功能的端到端演示与
  验证。demo 是一个带模板路由（`/pets/{id}`、双捕获 `/orders/{order_id}/items/{item_id}`）、
  Params 单一声源、请求/响应头、安全、400/422 校验的真实 API，只编译于
  `SIMPLE_HTTP_ENABLE_OPENAPI`；verify 用 httpx 发真实请求断言文档结构、路径参数按类型
  解析（string/double/bool 精确往返）、405 keep-alive（裸 socket 同连接连发两请求）、
  与 swagger-ui 可渲染（`$ref` 指向 components 无解析错误）。运行：
  `xmake build openapi_demo && test/python/.venv/bin/python test/openapi_verify.py`；
  浏览器打开 `http://127.0.0.1:7795/swagger`。

三套都是 C++、零 Python 依赖，也不用手工起服务（各自在进程内起监听）。`test/` 下另有
`manual_http1_keepalive.py`：一个可选的**手工**交叉验证（用第三方 `requests` 客户端打示例
服务器，需 `pip install requests` + `xmake run server`），不属于自动套件；`server.cpp` 是示例
服务器，`tls_certificates/` 是测试证书，`server.cpp.test` 是旧快照（未参与构建）。

### 压力测试统一入口（改动后必跑的一条龙）

协议层/传输层的改动落地后，**只执行这一个脚本**即完成「清理 → 符合性 → 压力」全流程：

```sh
test/stress/run.sh [cpp|rust|both] [h1|h2|h3|ws]...
# 例：test/stress/run.sh both          # cpp + rust 四档全跑（默认 both）
#     test/stress/run.sh cpp h3        # 只跑 cpp 的 h3 档
```

脚本内建，顺序固定：

1. **强制清理**：杀光遗留测试服务（simple_http 的 release/debug server、rust_http_server、
   server_dbg），SIGTERM 后 SIGKILL 兜底，并验证 TCP/UDP `7788-7792` / `7888-7892` 端口释放。
   不清理的坑：`7792` UDP 有 SO_REUSEPORT，残留进程会把新服务器的 QUIC 连接表按包的哈希
   劈成两半——实测 20 万请求 ~800 failed，`ss` 还看不出异常。
2. **符合性测试**：目标含 `cpp` 时先跑 `test/conformance/run.sh`（基线：h2spec 146/146、
   h1spec 33/33、h3spec 47/49、Autobahn FAILED 0 / NON-STRICT 0），逐套件通过数照打并给出
   「✓ 符合性通过（退出码 0）」明文判定；未达基线即中止，不进压力档。
   `RUN_CONFORMANCE=0` 可跳过（配置块里改）。
3. **压力测试**（四档；C++ 与 Rust 必须同一工具才可比）：
   | 档 | 工具 | 说明 |
   |---|---|---|
   | h1 / ws 负载 | **k6** | h1 打 `/world?n=`（POST 1KB）；ws 打 `/echo1k`（请求 512B → 响应 1KB，k6_ws.js） |
   | h2c / h3 负载 | **h2load**（`/opt/h3/bin/h2load`，系统自带无 QUIC 不可用） | k6 不支持 h2c 与 h3 |
   | 复用门线（全协议） | h2load | 一条连接串行 200 请求必须 200/200；k6 断线自动重连，表达不了该语义 |
   - 规格：**请求 POST 1KB body，响应三档 2k/10k/20k**（`?n=`），每档 20 万请求，
     **失败数必须为 0**（k6 档 `errors=0`）；失败不是方差，是事故。
4. **头部「测试参数配置」可随时改**：`LOAD_CLIENTS`（客户端数）、`LOAD_STREAMS`（每连接
   并发流数）、`LOAD_REQUESTS`、`RESP_TIERS`、`WS_MSG_SIZE`/`WS_RESP_SIZE`/`WS_MSG_VUS` 等。

`rust_http_server` 仓库的 `test/stress/run.sh` 已改为 shim，等价于 `run.sh rust ...`；
C++ 与 Rust 的对比由 `run.sh both` 一次给出（同一窗口、两端同工具、全部门线绿才算达标）。

### 外部一致性套件（每次改动后必跑）

三套跨实现的一致性套件，独立于 simple_http 自己的客户端——和 `test/python/` 同一个理由，
但更权威：它们是各自协议的参考级套件，只在服务端真的错的地方分歧。**任何触及协议层的改动
做完后都要跑一次**（已由统一入口 `test/stress/run.sh` 在目标含 cpp 时自动先跑本套件，
也可按需单独执行下面的命令）：

```sh
test/conformance/run.sh          # 三套依次跑；非零退出码 = 有套件未达标或环境不全
test/conformance/run.sh h2spec   # 只跑指定的（h2spec / h1spec / autobahn）
```

| 套件 | 打哪个端口 | 覆盖 | 基线 |
|---|---|---|---|
| **h2spec** | `:7790`（纯 h2c） | HTTP/2 帧层、流状态机、HPACK | **146/146**（`-S` strict 147/147） |
| **h1spec** | `:7791`（纯 h1） | HTTP/1.1 请求行与字段解析、分片到达 | **33/33** |
| **Autobahn** | `:7788`（嗅探） | WebSocket / RFC 6455 | 517 用例：**FAILED 0、NON-STRICT 0** |
| **h3spec** | `udp :7792`（纯 h3） | QUIC 传输 + HTTP/3 错误路径 | **49 用例：总数 49、通过 ≥47** |

- **端口不能混用。** `7790`/`7791` 是 `ServerConfig::plaintext_protocols` 声明的单协议端点
  （见 `net/connection.h`）：嗅探端口上"畸形的 HTTP/2 前导"和"畸形的 HTTP/1.x 请求行"是
  同一批字节，而 h2spec 要 GOAWAY、h1spec 要 **400**。在嗅探端口上 h2spec 天然是 145/146，
  那不是缺陷。
- **三套的依赖都不在仓库里**（许可证/体积原因），脚本会自检并把准备命令打出来，缺什么不会
  静默跳过：h2spec 是预编译二进制（自行下载；注意 v2.6.0 由 Go 1.12.7 构建，**不支持 TLS 1.3**，
  所以它只测明文端口）；h1spec 是**无许可证**的第三方脚本，运行时缓存到 `.cache/conformance/`，
  不进仓库；Autobahn 走 Docker 镜像。
- **Autobahn 的 UNIMPLEMENTED 不算失败**：那 216 个全是 `12.x`/`13.x` 的 permessage-deflate
  （未实现的扩展）。**INFORMATIONAL 也不算**（`7.1.6` 和 `7.13.1/7.13.2` 是规范明说"未定义"的）。
  但 **NON-STRICT 算**——它是"通过了但偏离 SHOULD"，这个仓库的立场是不要（`6.4.3`/`6.4.4`
  的 UTF-8 fail-fast 就是这么修掉的）。
- **它们抓到过什么**（自研套件一概看不见）：RSV 位不校验、text 消息完全不校验 UTF-8、
  close code 不校验、协议错误不发 Close 帧只静默断连、Host 缺失/重复不检查、字段名与字段值
  的字符集不检查，以及"靠请求行形状猜协议"这个做法本身与 HTTP/1.1 语义的冲突。

### 负载与连接生命周期套件（改传输层或流的生老病死之后必跑）

一致性套件每个用例都从一条**干净的连接**开始，正好把连接的生命周期整个跳过去——它看不见
「同一条连接上第 N 个请求」。这一套专问那件事：

```sh
test/stress/run.sh          # 三档依次跑；非零退出码 = 有未达标或环境不全
test/stress/run.sh h3       # 只跑指定的（h1 / h2 / h3）
```

| 档 | 打哪个端口 | 复用门线 | 吞吐 |
|---|---|---|---|
| **h1** | `:7791`（纯 h1） | 一条连接串行 200 个请求，**必须 200/200** | 只报告 |
| **h2** | `:7790`（纯 h2c） | 同上 | 只报告 |
| **h3** | `udp :7792`（双传输端点的 QUIC 侧） | 同上 | 只报告 |

- **门线只有一条：失败数必须为 0。** 吞吐**不写死**：req/s 依赖机器（核数、内存带宽、
  回环实现），写死一个数会在别的机器上误报；而「一条连接上连发 200 个请求，少一个就是
  连接生命周期出了问题」是与机器无关的确定性判断。
- **`7789` 不要拿来做负载**：它是 mTLS 端口，`h2load` 不带客户端证书（它的选项里就没有
  这一项），ALPN 之后走不下去。
- **h3 需要一份编了 ngtcp2/nghttp3 的 `h2load`。** 系统包里的那份**没编**，压 h3 会退化成
  TCP TLS 然后 ALPN 谈崩（`No supported protocol was negotiated`）。脚本**不看版本号**——
  `h2load --version` 编不编都打印同一行——而是在服务器起来之后**实测一发**；缺了会打印
  自编配方并以非零退出。自编的装到别处时用 `H2LOAD=` 指过来（非静态链接还要给
  `LD_LIBRARY_PATH`）。
- **压测要在 sanitizer 下跑一遍。** CI 的 ASan+UBSan job 只跑 `unittest`/`regression`/`client`
  ——**h3 那条路径它一条都覆盖不到**（项目没有 h3 客户端，也没有 h3 单测）。做法：按 gcc.yaml
  的参数构建（`--cxxflags='-fsanitize=address,undefined -fno-omit-frame-pointer'`
  `--ldflags='-fsanitize=address,undefined'`，**别加 `--toolchain=gcc`**，那会触发 boost 包重装），
  然后 `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`
  跑 `test/stress/run.sh`。
  **LeakSanitizer 只在 `exit()` 时报告**，所以示例服务器必须能优雅退出——`test/server.cpp`
  为此装了 SIGINT/SIGTERM 处理器（处理器只置 `sig_atomic_t` 标志，主循环看到了才调
  `Server::stop()`，因为 stop 本身不是 async-signal-safe）。硬杀进程的话泄漏检测等于没跑；
  在 gdb 下调 `__lsan_do_leak_check()` 也不行，LSan 明确不支持 ptrace。
  **它抓到过**：连接与引擎之间的 `shared_ptr` **引用环**——`set_protocol(shared_from_this())`
  让连接持有引擎、引擎又持有连接，于是 200 个连接连同它们的 ngtcp2 conn、nghttp3 conn 与
  SSL 全都不析构（24.3 MB / 30058 次分配）。修法是把连接那侧改成 `std::weak_ptr` 并逐点
  `lock()`；**不能**在 `close_now()` 里 `reset()`——`on_stream_data` 的调用栈里就是引擎，
  reset 会把正在执行的对象析构掉。
- **QUIC 侧要开 `ServerConfig::reuse_port`，否则它会跑在机器的一小部分上。** 不开时 QUIC
  监听只有**一个** UDP socket、落在**一个** worker context 上（`server.h` 里
  `sockets = reuse_port ? pool->size() : 1`），而同一台服务器上 h2c 的连接是被轮转分散到
  整个池子的——于是 h3 等于拿一个核去打人家的四个核。实测
  `-t 4 -n 200000 -c 200 -m 20`：**7.2k → 21k req/s（约 3×），p99 700ms → 110ms**；
  开了之后还反超 ngtcp2 官方参考服务端（21k vs 13.5k，它是单 socket）。TCP 侧几乎不受
  影响（98.6k → 94.2k，噪声边缘）。`test/server.cpp` 的 dual 端点已按此配置。
- **它抓到过两次，都是"同一条连接上第 N 个请求"这一类**，而三套一致性套件与四套自研
  套件一概看不见：
  1. *手写实现*：HTTP/3 在「上一条流 retire 之后再开新流」时把新流**静默吞掉**。判据是
     「id 低于见过的最大 id 就当成已 retire 的重传」，可**帧的到达顺序不是流被打开的
     顺序**——承载 QPACK 动态表插入的 encoder 流（客户端 uni 流 6）落在先到的 decoder 流
     之后，于是被丢弃、从未创建，引用动态表的字段段永远 Blocked，请求无限期挂起。表现是
     `-n 10 -c 1 -m 1` 只成 1 个、卡满 30 秒。
  2. *ngtcp2 版*：客户端跑完 `initial_max_streams_bidi`（100）个请求后再也开不了新流——
     ngtcp2 **不会自动归还** MAX_STREAMS，只有它自己没通过 `stream_open` 报告过的流才自动
     加。服务端必须在流关闭时调 `ngtcp2_conn_extend_max_streams_bidi`。表现极具特征：
     复用档恰好 **100/200**，负载档 18 万失败。修在 `quic/connection.h` 的 `cb_stream_close`。
  两次都只在复用档暴露，这正是这一档存在的理由——门线是「一条连接串行 200 个请求必须
  全成」，少一个就是连接生命周期出了问题。

### 丢包与乱序下的复现（手工，需要 root）

一致性套件和负载套件都在**无丢包**的回环上跑，而 QUIC 的恢复路径只在丢包时才被走到——
有一类问题它们看不见。手写实现时抓到过：**PTO 探测包不携带重传数据**。服务端的首飞
（ServerHello）整体丢失时，丢包检测的阈值没有「确认」可作基准，那个包就永远留在在飞队列
里、既不判丢也不重传；客户端收不到 ServerHello，便以翻倍间隔重传 ClientHello 五十秒以上，
而服务端每次都成功解密它、只回 ACK-only 包（ACK-only 不引出 ACK），两边各自退避到死。
两万请求里会有几千到一万四千个失败（就看哪些连接掷到丢首飞），而同一台机器上的另一个
QUIC 实现 0 失败。

那份实现已经不在了（恢复逻辑现在归 ngtcp2），**这一节的手法仍然要留着**：丢包路径是换
实现时最容易被新引入的代码弄坏、又最不容易被发现的地方——它不在这三套一致性套件和负载
套件里任何一档的射程内。

**ngtcp2 版实测（2026-09-27，`-t 4 -n 20000 -c 10 -m 20`）**：无丢包 20257 req/s；
`netem loss 3%` 三轮、`loss 10%` 两轮，**每轮都是 20000/20000 全成、0 失败**。吞吐随丢包率
单调下滑（3% 约 1.6 万，10% 约 0.5–1 万），这既是"netem 真的生效"的证据，也说明环境确实
够狠。同一台机器上官方 `osslserver` 在 3% 下同样 0 失败，可作对照。对比手写实现当年
「两万请求里几千到一万四千个失败」，**这个 bug 不是被修好了，而是随着恢复逻辑一起交出去、
结构上不可能再出现**。

在回环上注入丢包（**改的是整台机器的回环，用完必须撤掉**）：

```sh
sudo tc qdisc add dev loopback0 root netem loss 3%    # 接口名见下
h2load --alpn-list=h3 -t 4 -n 20000 -c 10 -m 20 https://127.0.0.1:7792/world
sudo tc qdisc del dev loopback0 root
```

- **先确认 netem 真的生效。** 0% 和 5% 的吞吐一样就说明接口加错了，而这个错误不会报
  任何警。接口名要看平台：WSL2 的回环流量走 `loopback0`（`lo` 上加了等于没加，抓包也
  只能看到 `loopback0`），普通 Linux 上是 `lo`。
- **必须重复跑。** 这个场景的方差在 4 倍量级：哪些连接丢掉首飞是掷骰子，同一份二进制在
  3% 下的失败数会在 0 到 14000 之间跳。判断一个改动有没有用，用**交替 A/B**（改前改后
  轮流跑同样多轮）比中位数，而不是各跑一次比大小——本仓库在这上面栽过：一个改动曾被
  单次运行「证明」有效 4–10×，重复测量后两组分布几乎完全重叠。
- **找一个对照实现。** 同一台机器上起一个别的 QUIC 服务端，用同一个 `h2load` 打同样的
  参数，能立刻分开「这是本库的问题」和「这是环境的账」——上面那个 bug 就是靠它定位的。
  同一个手法在 TLS 上也用过一次，结论恰好相反（那次是环境）。

## 代码风格

- `.clang-format`：LLVM 基线（`BasedOnStyle: LLVM`）+ 两个覆盖：`IndentWidth: 4`、
  `ColumnLimit: 120`。要按当前 clang-format 版本展开全部选项
  `clang-format --style=file --dump-config`。提交前请 clang-format。
- 编译告警按错误处理：`set_warnings("all", "error")`，不要留 warning。
- 遵守分层：上层（proto/handler/net）不得出现协议版本分支，版本差异只放进
  `engine/` 和对应的 `ResponseWriter` 实现。
- RAII、无裸 `new`/`delete`；所有权显式。
