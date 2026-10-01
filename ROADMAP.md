# ROADMAP

本项目的中长期 backlog —— 对照主流 Web 框架（Go `net/http`/chi/gin/echo、Rust
axum/tower、reqwest/tungstenite）的能力面整理。分四档：

- **框架核心**：主流框架开箱自带或高频率能力，本库尚未对齐 —— 优先级最高；
- **协议扩展**：需要动协议实现的缺口（成本最高）；
- **生态库边界**：Go/Rust 框架核心通常也不自带，由第三方中间件 crate/包提供 ——
  原则是**不进内核**，除非有特殊理由；
- **体验/运维**：小成本、锦上添花。

每一项都是"确认过现状后列出来的"，开工前先重读对应头文件确认它确实还没有。

> 新会话接手提示：**客户端公共 API 已全面切到 `simple_http::http`（`client/http.h`）**——详见下文
> 「已完成：客户端 API 重构」。新增功能一律在 `http::` 上做（`Stream` 是未来 WS 客户端底座、分层超时
> 已落地）；旧客户端类型仅为内部引擎，勿新增使用点。当前工作区（git 分支 `test`）大量改动未提交。

## 已完成：客户端 API 重构（2026-10 收官）

**新公共客户端 API = `simple_http::http`（`include/simple_http/client/http.h`）**
- `http::Client`（连接不可见：池/复用/重试/重定向/Cookie 内部；`get/post/put/del/head/patch/request(url)` + `open_stream(url, StreamSpec)` + 内部通道 `open_target` 供反代）+ `stats()`。
- `http::RequestBuilder`：`.query/.header/.headers/.content_type/.basic_auth/.bearer_auth/.body/.json<T>/.timeout/.max_body_bytes`；`.send()`（缓冲便捷）或 `.send_stream()`（流式）。
- `http::Stream`：**全双工**（写端=请求体 write/finish；读端=响应 read_head/read/read_all/head，h2 可交织）——**未来 WebSocket 客户端的底座**；分层超时（见下）。`abort()`。
- `http::Response`：buffered（便捷 send）与 streaming 两形态；`status/version/headers/header/header_owned/ok/bodyless/error_for_status`；`text/json<T>/bytes/read/read_all`。
- 错误：`expected<T, error_code>`；分层：`response_head_timeout`/`body_idle_timeout`/`http_status` 等。
- **分层超时已落地**：`ClientConfig::response_head_timeout`（TTFB）/`body_idle_timeout`（体静默）→ `Stream::read_head/read`（`http::detail::deadline`）；`StreamSpec` 可覆盖；便捷 `send()` 保留整体 `request_timeout`。
- **旧类型（`HttpClient`/`ClientSession`/`ClientStream`/`RequestSpec`/`ClientTarget`/`ClientResponse`）为内部引擎**：伞形头不聚合 `client/client.h`；不在新代码新增使用点；未做 `http::detail` 命名空间搬家（收益低风险中，暂缓）。
- 测试：`test/client.cpp` 全部走新 API（122 checks），`suite_http_api` 是新 API 自检（builder 链/query 编码/json/auth/error_for_status/open_stream 双向/分块读）。

**剩余功能 backlog（回接上文各档）**：客户端 WebSocket（= `Stream` 底座 + `ws_frame` 复用）、
客户端 multipart 构造、出站代理、SNI 多证书、请求体缓存读取、per-route 体积上限、可配置重试、
per-host 并发上限、ETag/304 helper、进程内测试工具、请求 dump、`real_ip` CIDR 已有、AccessLog 结构化。

## 框架核心（建议做）

| 能力 | 对标 | 现状与方案 |
|---|---|---|
| **客户端 WebSocket** | Go `gorilla/websocket`、Rust `tungstenite` | 服务端有 `ws_route`/`ws_proxy`，但 `client/` 层**没有任何 WS 客户端**。方案：复用 `proto/ws_frame.h` 的帧编解码 + client 会话层建连（TLS/明文、h1 Upgrade），新 `client/ws_client.h`，API 对齐服务端 `WebSocket` 句柄（read/write/close） |
| **出站代理** | reqwest `Proxy`、Go `http.ProxyFromEnvironment` | `HTTP_PROXY`/`HTTPS_PROXY` 环境变量 + `ClientConfig::proxy`；HTTP 代理用 `CONNECT` 隧道（TLS）或绝对-form 请求（明文）；SOCKS5 可选。涉及连接层（`acquire` 处把代理端当作对端握手） |
| **客户端构造 multipart 上传** | reqwest `.multipart()`、Go `mime/multipart` | 服务端能解析 multipart（`read_multipart_body`），客户端没有构造器（反代/文件上传到上游也用得上）。方案：`SimpleMultipartBuilder` → `RequestSpec`（boundary 生成、part 头、大小上限） |
| **URL 百分号编码工具** | Go `net/url.QueryEscape` / `PathEscape`、axum 无（serde） | ✅ 已完成（`core/url_encoding.h`：`query_escape` / `path_escape`） |
| **SNI 多证书 / 按主机选证书** | Go `tls.Config.GetCertificate`、nginx `ssl_certificate` 按 server_name | 目前 `ServerConfig::tls` 单证书。方案：`TlsConfig` 加 `std::function<std::shared_ptr<...>(std::string hostname)>` 钩子，握手回调里按 SNI 取证书 |
| **请求体缓存读取** | Go 中间件 `io.NopCloser` 回填；tower-http 的 RequestBodyLimit | **中间件读过的 body，handler 就读不到了**（Body 是单向 channel）。方案：`Body` 加读取缓存（`rewind()`/自动缓存已读字节，超上限即停缓存），或提供 `read_all_cached` helper |
| **per-route / per-group 请求体大小上限** | axum `DefaultBodyLimit` layer、Go `http.MaxBytesHandler` | 目前 `EngineLimits::max_body_bytes` 全局。方案：注册/中间件级覆盖（上传端点放大、其余收紧），存进 Request/按路由查到 |
| **可配置重试策略** | reqwest 生态 `reqwest-retry` | 目前只有「池中连接陈旧→重试一次」固定规则。方案：`ClientConfig` 加 `retry`（次数/退避/条件回调），幂等判定已有（`is_idempotent`） |
| **请求 ID 进访问日志** | tower-http `TraceLayer` 把 `x-request-id` 打进行日志 | ✅ 已完成（`access_log` 读 `req->get_state<RequestId>()`，有则 `peer [id] "GET ..."`） |

## 连接与传输（中等优先级）

| 能力 | 对标 | 现状与方案 |
|---|---|---|
| **客户端 per-host 并发连接上限** | Go `http.Transport.MaxConnsPerHost` | 池是「空闲数上限」，没有「并发连接总数上限」。方案：`ClientConfig` 加 `max_conns_per_host`（容器计数，超出排队/失败） |
| **客户端 ETag/If-None-Match 条件请求 helper** | Go `http.Header.Add("If-None-Match")` 手动；reqwest 无 | `ClientConfig`/`RequestSpec` 便捷：上次 ETag → `If-None-Match`，304 时可直接用缓存 |
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