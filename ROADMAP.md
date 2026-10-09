# ROADMAP

本项目的中长期 backlog —— 对照主流 Web 框架（Go `net/http`/chi/gin/echo、Rust
axum/tower、reqwest/tungstenite）的能力面整理。分四档：

- **框架核心**：主流框架开箱自带或高频率能力，本库尚未对齐 —— 优先级最高；
- **协议扩展**：需要动协议实现的缺口（成本最高）；
- **生态库边界**：Go/Rust 框架核心通常也不自带，由第三方中间件 crate/包提供 ——
  原则是**不进内核**，除非有特殊理由；
- **体验/运维**：小成本、锦上添花。

每一项都是"确认过现状后列出来的"，开工前先重读对应头文件确认它确实还没有。

> 新会话接手提示：**客户端公共 API 已平铺到 `simple_http::` 顶层**（`Client`/`RequestBuilder`/`Stream`/`ClientResponse`，
> 全部定义在 `client/http.h`），与服务端共用同一批类型：`Request` 是两端共用的请求消息（服务端引擎 fill + handler
> 读；客户端构建 + 引擎发送），`Body` 是统一读写流（服务端请求体读 / 客户端请求体与响应读共用）。命名对齐 Go
> net/http：服务端写句柄 `ResponseWriter`、引擎抽象 `ResponseSink`、客户端读响应 `Response`。`detail::ClientEngine`
> 等会话类型在 `simple_http::detail`（内部）。新增功能一律在 `simple_http::` 顶层做。

## 已完成：客户端 API 重构（2026-10 收官）

**公共客户端 API 平铺在 `simple_http::`（`include/simple_http/client/http.h`）**，命名对齐 Go net/http：
- `Client`（连接不可见：池/复用/重试/重定向/Cookie 内部；`get/post/put/del/head/patch/request(url)` + `open_stream(url, StreamSpec)` + 显式 `send(shared_ptr<Request>)`（Go `Do(req)`）+ `open_websocket(url, spec)` + 内部通道 `open_target` 供反代）+ `stats()`。
- `RequestBuilder`：`.query/.header/.headers/.content_type/.basic_auth/.bearer_auth/.body/.json<T>/.multipart(MultipartForm)/.timeout/.max_body_bytes`；`.send()`（缓冲便捷，内部构造 `Request` 后走 `Client::send`）或 `.send_stream()`（流式）。
- `Stream`：**全双工**（写端=请求体 write/finish；读端=响应 read_head/read/read_all/head，h2 可交织）——**WebSocket 客户端的载体**；分层超时（见下）。`abort()`。
- `Response`：buffered（便捷 send）与 streaming 两形态；`status/version/headers/header/header_owned/ok/bodyless/error_for_status`；`text/json<T>/bytes/read/read_all`；**`body()` 统一读流**（Go `resp.Body`）。`StreamSpec`/`WebSocketSpec` 为逐请求参数。
- 错误：`expected<T, error_code>`；层次：`response_head_timeout`/`body_idle_timeout`/`http_status` 等。
- **分层超时已落地**：`ClientConfig::response_head_timeout`（TTFB）/`body_idle_timeout`（体静默）→ `Stream::read_head/read`（`detail::deadline`）；`StreamSpec` 可覆盖；便捷 `send()` 保留整体 `request_timeout`。
- **`Request` 两端共用**（`proto/request.h`）：服务端引擎填充（`set_method`/`set_target`/`mutable_headers`/`body().feed`）、handler 读（`method/path/query/header/body/param/state`）；客户端构建（`set_url`、`set_body`、`close`/`stream_body`、`basic_auth`）+ 引擎发送。`clone_for_replay`（= Go `GetBody`）供策略重试与 h2c upgrade 重放。会话接口收 `shared_ptr<Request>`，与服务端 `RequestPtr` 对称。
- **旧类型已移除**：`HttpClient`（连接引擎入 `simple_http::detail::ClientEngine`，便捷层迁入 `Client`）、`RequestSpec`（并入 `Request`）均删除；`client/client.h` 聚合头删除。
- **客户端 WebSocket**（`client/ws_client.h`）：`Client::open_websocket(url, spec)` → `expected<shared_ptr<WebSocket>>`；经由 `ClientEngine::dial_transport`（裸建连，不开 HTTP 会话）+ HTTP/1.1 Upgrade 握手（校验 101 + `Sec-WebSocket-Accept`），复用服务端 `WsBackendImpl`/`WebSocket`（方向参数：服务端解析掩码帧、客户端解析无掩码帧、客户端写帧掩码）。
- 测试：`test/client.cpp`（151 checks，含 `suite_websocket`/`suite_multipart`/`suite_retry`/`suite_reverse_proxy`），`test/client_cross.cpp`+`test/ws_client_cross.cpp` 为**双向 Python 交叉验证**（stdlib http.server / websockets 库对客户端；`xmake run client-cross-python`）。

**剩余功能 backlog（回接上文各档）**：出站代理、SNI 多证书、请求体缓存读取、
per-route 体积上限、进程内测试工具、请求 dump、
AccessLog 结构化。

## 框架核心（建议做）

| 能力 | 对标 | 现状与方案 |
|---|---|---|
| **HTTP/2 上的 WebSocket（RFC 8441）** | axum/tower `ws` over h2 | ✅ 已完成（服务端）：h2 引擎宣告 `SETTINGS_ENABLE_CONNECT_PROTOCOL`，处理 `CONNECT + :protocol: websocket`（校验 method/`:scheme`/`:path`，凭据不足则 PROTOCOL_ERROR 重置流；未路由则 404），回 `200` 后由 `engine/h2/h2_ws_transport.h` 把单条流适配成 `WsBackendImpl` 的 Transport，复用全部帧编解码/掩码/流控/permessage-deflate；半关闭（客户端 END_STREAM）后仍可回写。验证：`test/python/rfc8441_client.py`（hyper-h2，77 checks，含 h2c/TLS/`wss`/deflate/分片/极限/多路复用）。客户端 h2 WebSocket 仍未做 |
| **出站代理** | reqwest `Proxy`、Go `http.ProxyFromEnvironment` | `HTTP_PROXY`/`HTTPS_PROXY` 环境变量 + `ClientConfig::proxy`；HTTP 代理用 `CONNECT` 隧道（TLS）或绝对-form 请求（明文）；SOCKS5 可选。涉及连接层（`acquire` 处把代理端当作对端握手） |
| **客户端构造 multipart 上传** | reqwest `.multipart()`、Go `mime/multipart` | ✅ 已完成（`proto/multipart.h` 的 `MultipartForm`——boundary 生成、part 头（name/filename 引号转义，Go escapeQuotes 语义）、3 参 `file()` 按扩展名自动 Content-Type、`release_body()` 免拷贝；`RequestBuilder::multipart(form)` 进 `send()`/`send_stream()` 便捷层。测试：`suite_multipart`（9 checks）+ 单测 `[forms]`） |
| **URL 百分号编码工具** | Go `net/url.QueryEscape` / `PathEscape`、axum 无（serde） | ✅ 已完成（`core/url_encoding.h`：`query_escape` / `path_escape`） |
| **SNI 多证书 / 按主机选证书** | Go `tls.Config.GetCertificate`、nginx `ssl_certificate` 按 server_name | 目前 `ServerConfig::tls` 单证书。方案：`TlsConfig` 加 `std::function<std::shared_ptr<...>(std::string hostname)>` 钩子，握手回调里按 SNI 取证书 |
| **请求体缓存读取** | Go 中间件 `io.NopCloser` 回填；tower-http 的 RequestBodyLimit | **中间件读过的 body，handler 就读不到了**（Body 是单向 channel）。方案：`Body` 加读取缓存（`rewind()`/自动缓存已读字节，超上限即停缓存），或提供 `read_all_cached` helper |
| **per-route / per-group 请求体大小上限** | axum `DefaultBodyLimit` layer、Go `http.MaxBytesHandler` | 目前 `EngineLimits::max_body_bytes` 全局。方案：注册/中间件级覆盖（上传端点放大、其余收紧），存进 Request/按路由查到 |
| **可配置重试策略** | reqwest 生态 `reqwest-retry` | ✅ 已完成（`ClientConfig::retry`：`max_retries`/指数退避（初始/上限/乘数）/条件回调（默认：stream_refused、幂等方法、免费重拨后仍失败的覆盖场景、拨号未发）；`require_replayable` 挡住流式 body；引擎 open 阶段与便捷层读阶段共享一个 budget，**整个请求（含退避）受 `request_timeout` 总 deadline 约束**；另有一次**免费透明重拨**（kept 连接传输层失败自愈，不消耗 budget）。测试：`suite_retry` + `[client]` 单测） |
| **请求 ID 进访问日志** | tower-http `TraceLayer` 把 `x-request-id` 打进行日志 | ✅ 已完成（`access_log` 读 `req->get_state<RequestId>()`，有则 `peer [id] "GET ..."`） |

## 连接与传输（中等优先级）

| 能力 | 对标 | 现状与方案 |
|---|---|---|
| **客户端 per-host 并发连接上限** | Go `http.Transport.MaxConnsPerHost` | ✅ 以别的办法完成了（2026 单连接简化）：每个 `Client` 对首个目标 origin 只保持**一条** TCP 连接，顺序/低并发复用；连接忙时并发请求各拨一次性连接；其它 origin 走一次性连接。连接断开免费重拨一次。测试：`suite_retry`/`suite_shared_client`/`suite_reverse_proxy`/loadgen |
| **同源高并发 h1 的排队模型** | （内部） | `pub` 面在每线程 ≥8 路同源并发、持续负载下仍有残余级联失败（每次免费重拨只抵扣一次瞬时错误，连接在并发下被失败请求的收尾 close 打断）。方向：把 h1 并发从「拨号自愈」收敛为「同一条连接上排队」，需要门释放与持有者决策完全同步（token 化已落地）+ 失败度量的重拨预算；当前以「忙则拨号」为可工作形态 |
| **客户端 ETag/If-None-Match 条件请求 helper** | Go `http.Header.Add("If-None-Match")` 手动；reqwest 无 | `ClientConfig`/`Request` 便捷：上次 ETag → `If-None-Match`，304 时可直接用缓存 |
| **HTTP/3 客户端** | reqwest `http3` feature（冷门） | 服务端有 H3，客户端没有。成本最高，需求最冷，除非下游要「同一进程既是 H3 server 又是 H3 client」再动 |

## 协议扩展（成本最高，需动引擎/帧层）

| 能力 | 对标 | 备注 |
|---|---|---|
| **HTTP/2 上的 WebSocket（RFC 8441）** | axum/tower `ws` over h2 | `h2_engine` 未见 `CONNECT + :protocol: websocket` 处理；AGENTS 已注明"尚未接入" |
| **WebSocket permessage-deflate** | Autobahn 停 216 个 UNIMPLEMENTED | RFC 7692 扩展协商 + 压缩帧数据 |
| **multipart 流式处理** | axum `Multipart` 不整块入内存 | 目前 `parse_multipart` 整段缓冲（单 part 有上限兜底）；如需大文件上传流式落盘再动 |
| **HTTP/3 0-RTT / 连接迁移** | ngtcp2 能力 | 0-RTT 默认关（安全）；迁移是新连接的优化项 |

## 生态库边界（默认不做，除非下游要求）

- **认证/授权**：JWT、Session、CSRF —— Go `gorilla/session`、`golang-jwt`；Rust `axum-extra`/`jsonwebtoken`
- **OpenTelemetry / tracing / metrics** —— Go `otelhttp`；Rust `tower-http` TraceLayer + `metrics`
- **模板渲染** —— Go `html/template`；Rust `askama`/`tera`
- **请求验证器** —— Go `validator`；Rust `validator` crate（`read_json_body<T>` 读出后可用任意外挂）
- **限流进阶** —— 固定/滑动窗口、分布式（Redis）计数；现有 `rate_limit.h` 是令牌桶（全局或按 key）
- **请求体解压（deflate 上传）** —— 安全敏感，多数框架默认禁；不跟

## 体验/运维（小成本，随用随加）

- `real_ip` 可信代理支持 **CIDR** 范围 —— ✅ 已完成（`10.0.0.0/8`、`2001:db8::/32`，v4+v6）
- handler 级条件请求通用 helper —— ✅ 已完成（`handler/conditionals.h` 的 `maybe_not_modified`：If-None-Match/If-Modified-Since → 304 bodyless，设置 ETag/Last-Modified）
- `Server::stop` 的超时上界 —— ✅ 已有（`IoCtxPool::stop` 的 `kDrainGrace=2s` 强制 + 本次给 acceptor-close 等待也加 2s 兜底）；如需可配时长再加参数
- 进程内测试工具公开（`test/` 的 `FakeResponseWriter`/`MockTransport` 是内部替身，未作为库 API 暴露；对齐 Go `httptest`）
- 请求/响应 dump helper（调试，对齐 `httputil.DumpRequest`）
- `AccessLog` 结构化（JSON 行 / 可配格式）

## 已完成（历史对照，已对齐水平）

中间件三作用域（use/group/per-route）+ 状态传递、CORS（通配子域+自定义判定）、
request_id/access_log/recovery/basic_auth/real_ip/clean_path/strip_prefix/redirect_slashes、
**secure_headers（安全响应头基线：HSTS 仅 TLS / CSP / X-Frame-Options / nosniff /
Referrer-Policy，`Response::replace_header` 覆盖）**、
限流（令牌桶/per-key）、query/form/multipart/json 解析、类型化参数提取
（path_params/query_params）、redirect、SSE、Cookie 读写+jar、客户端重定向（含跨源丢敏感头、
超限报错）+basic_auth、OpenAPI 3.1、压缩、graceful stop、405/OPTIONS、静态文件 ETag/Range/304。