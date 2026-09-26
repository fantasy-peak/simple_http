# AGENTS.md — simple_http

面向参与本工程的 AI/协作者的快速上手说明。全部内容基于源码整理。

## 这是什么

`simple_http` 是一个 **header-only 的 C++ HTTP 服务器 + 客户端库**，基于
`boost.asio` C++20 协程实现。服务端支持 HTTP/1.0、HTTP/1.1、HTTP/2（h2 / h2c /
prior-knowledge）、WebSocket；客户端（`client/` 层）支持 http/https × HTTP/1.1/HTTP/2，
通过 ALPN（TLS）或 h2c（明文，Upgrade 或 prior-knowledge）自动协商。HTTP/3 预留了
骨架（默认关闭）。

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
      mime.h               MIME 类型
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
      h3/                  HTTP/3 骨架（#ifdef SIMPLE_HTTP_ENABLE_HTTP3，默认空）
    handler/               handler 类型系统 + 路由
      handler.h            Handler 类型别名与 make_handler/invoke_handler
      router.h             Router（含一条反代路由用的 HttpClient、反代匹配）
      http_proxy.h         请求级反代：上游走 client 层（连接池/TLS/h2），响应流式回传
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
```

其它目录：

- `test/`：`server.cpp` 是示例服务器；`client.cpp` 是客户端整合自检；`server_regression.cpp`
  是服务端对抗性回归；`unit/` 是 Catch2 单元测试；`manual_http1_keepalive.py` 是可选的**手工**
  交叉验证；`tls_certificates/` 是测试证书（`server.cpp.test` 是未参与构建的旧快照）。
- 没有 `docs/` 目录：架构说明就在本文件与各头文件顶部注释里。

## 架构要点（来自代码与设计）

1. **协程贯穿始终**：handler 是 `asio::awaitable<void>`，读请求体、写响应
   都是 `co_await` 的异步操作，天然反压。
2. **一个抽象收敛协议版本**：`Request`/`Response`/`Router`/handler 完全与
   HTTP 版本无关，版本差异只藏在 `engine/` 与 `ResponseWriter` 实现里，上层
   没有 `if (version==…)`。
3. **传输与协议解耦**：引擎通过 `Transport` 抽象读写字节，不直接依赖具体
   socket 类型（TCP 明文 / TLS，未来 QUIC）。
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
server.route("/world", [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
    co_await res->status(200).send("hello");
});
server.route_regex("/api/.*", handler);
server.ws_route("/chat", ws_handler);
server.fallback(not_found_handler);

server.start();      // 同步：启动所有监听并阻塞至绑定完成，返回 bool
// 或 co_await server.run();  // 协程版
server.stop();
```

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
  自动选 h2 / HTTP/1.1（`h2c` 则对明文后端尝试一次 Upgrade）。后端 TLS 的信任策略来自
  `ServerConfig::proxy_client`（CA/客户端证书/是否校验）。WebSocket 反代
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
- `SIMPLE_HTTP_ENABLE_HTTP3`（默认未定义）：HTTP/3 骨架开关。

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
  客户端 URL/配置/TLS 参数/错误码。`test/unit/test_support.h` 放共用测试替身。按标签过滤：
  `xmake run unittest "[h2]"`。
- **`test/server_regression.cpp`**：起进程内服务器（明文 + mTLS），用**裸 socket** 发畸形/边界
  请求并断言线上行为——400/431/413、CL+TE 冲突、坏 chunked、trailers、流水线、HEAD/204、
  keep-alive 与空闲超时、h2 的 SETTINGS/ACK、PING、GOAWAY（帧过大/流 0 DATA/偶数流 id/SETTINGS
  长度/流控越界）、h2c 升级、WebSocket 握手与分片/Ping/Close、TLS 的 mTLS 与 TLS1.3 要求。
- **`test/client.cpp`**：客户端整合自检（协议矩阵、流式、64 路并发、连接池、TLS 校验、
  反代、错误注入）。

三套都是 C++、零 Python 依赖，也不用手工起服务（各自在进程内起监听）。`test/` 下另有
`manual_http1_keepalive.py`：一个可选的**手工**交叉验证（用第三方 `requests` 客户端打示例
服务器，需 `pip install requests` + `xmake run server`），不属于自动套件；`server.cpp` 是示例
服务器，`tls_certificates/` 是测试证书，`server.cpp.test` 是旧快照（未参与构建）。

### 外部一致性套件（每次改动后必跑）

三套跨实现的一致性套件，独立于 simple_http 自己的客户端——和 `test/python/` 同一个理由，
但更权威：它们是各自协议的参考级套件，只在服务端真的错的地方分歧。**任何触及协议层的改动
做完后都要跑一次**：

```sh
test/conformance/run.sh          # 三套依次跑；非零退出码 = 有套件未达标或环境不全
test/conformance/run.sh h2spec   # 只跑指定的（h2spec / h1spec / autobahn）
```

| 套件 | 打哪个端口 | 覆盖 | 基线 |
|---|---|---|---|
| **h2spec** | `:7790`（纯 h2c） | HTTP/2 帧层、流状态机、HPACK | **146/146**（`-S` strict 147/147） |
| **h1spec** | `:7791`（纯 h1） | HTTP/1.1 请求行与字段解析、分片到达 | **33/33** |
| **Autobahn** | `:7788`（嗅探） | WebSocket / RFC 6455 | 517 用例：**FAILED 0、NON-STRICT 0** |

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

## 代码风格

- `.clang-format`：基于 Google 风格改造，`ColumnLimit: 120`，`IndentWidth: 4`，
  指针/引用左对齐（`int* p` / `int& r`），大括号换行（class/function/struct 后
  换行），`namespace` 不缩进并加尾注释。提交前请 clang-format。
- 编译告警按错误处理：`set_warnings("all", "error")`，不要留 warning。
- 遵守分层：上层（proto/handler/net）不得出现协议版本分支，版本差异只放进
  `engine/` 和对应的 `ResponseWriter` 实现。
- RAII、无裸 `new`/`delete`；所有权显式。
