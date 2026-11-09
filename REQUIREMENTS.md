# cpp17 迁移需求（对话纪要）

日期：2026-11-06
分支：`asio-internal`

## 1. 核心目标

1. 继续推进 `cpp17/`：把仓库根目录的 C++23 协程版 `simple_http` 适配成并行的
   **C++17 token-only** 版本。
2. **以 `simple_http`（根 `include/`）为唯一基准线**：协议实现直接参考、移植根实现
3. **最终执行方式**：清空 `cpp17/` 目录，重新从当前 `simple_http` 生成 C++17 版本。

## 2. 硬性技术要求

### 2.1 彻底去协程

`cpp17/include/` 中不得出现：

- `asio::awaitable`
- `co_await`
- `co_return`
- `co_yield`
- `co_spawn`

唯一例外：`engine/dispatcher.h` 中对 `Handler` / `WsHandler` 的 C++20 callable
自动适配（feature-guarded）。

### 2.2 全部 Asio 原生化

- 每个异步操作必须是 Asio 原生 initiation：
  - `asio::async_initiate<Token, Signature>` + `asio::async_compose` 状态机，
    或等价的手写回调状态机。
- handler 的 associated executor / allocator / cancellation slot 必须原样转发。
- 完全参照 Asio 库内部的实现方式（`any_completion_handler`、组合操作、
  `bind_cancellation_slot`、`bind_executor` 等）。
- 不引入自定义协程桥接。

### 2.3 Token 自动适配

- 同一 API 由调用方 token 决定是 callback 还是协程：
  - 回调、`asio::deferred`、`asio::detached` 始终可用；
  - C++20 的 `ascii::use_awaitable` 只是调用侧可选 token，核心不得依赖。
- 无默认 token，调用方显式传入。

### 2.4 Asio 取消

- 使用 `asio::cancellation_slot` / `asio::cancellation_signal`。
- 请求级取消通过 `Request::cancellation_slot()` 绑到待定操作。
- 不再使用 `std::stop_source` / 自建 channel 唤醒。

### 2.5 无异常设计

- 返回值可重新设计，统一用 `error_code` / `expected` 表达失败。
- 库内部不抛异常，不用异常做控制流。

## 3. 生命周期约束（回调化重点）

1. 每个在飞回调持有强 `shared_ptr` 保活（连接、writer、backend、状态机）。
2. 长生命周期注册/反向引用使用 `weak_ptr`，避免 UAF 与循环引用。
3. 完成回调可能释放最后一个引用，因此回调内不能依赖裸 `this`。
4. 写队列、读泵、watchdog 等异步组件必须持有自身 `shared_ptr`。

## 4. 其它约定

- `logging_json` 暂时删除，以后再说。
- 需要运行合规测试与压力测试，并与现有 `simple_http` 对比性能是否退化。
- 遵守 `AGENTS.md` 的 `cpp17/` 铁规则与验收命令。

## 4.1 glaze 门控（C++17 下的处理）

glaze 7.x 需要 C++23（concepts、`std::byteswap`），无法在真正的 C++17 翻译单元里
包含。处理方式是**编译期特性门**，而不是修改 glaze：

- `cpp17/include/simple_http/core/glaze_support.h` 定义
  `SIMPLE_HTTP_CPP17_ENABLE_GLAZE`：当 `__cplusplus > 202002L` 且
  `__has_include(<glaze/glaze.hpp>)` 时为 1，否则为 0。外部可显式
  `-DSIMPLE_HTTP_CPP17_ENABLE_GLAZE=0/1` 覆盖。
- 以下头文件整体包在 `#if SIMPLE_HTTP_CPP17_ENABLE_GLAZE` 中，C++17 下为空：
  `proto/json.h`、`proto/params.h`、`handler/extract.h`、`handler/jwt.h`、
  `handler/jwks.h`、`openapi/openapi.h`。
- `extract.h` 的 `Header<T, "name">` / `Cookie<T, "name">` 使用字符串字面量作非类型
  模板参数（C++20 能力），另行以 `SIMPLE_HTTP_CPP17_STRING_NTTP` 门控；其余提取器
  （`Path` / `Query` / `State` / `Valid<...>`）跟随 glaze 门控。
- 同一套 `cpp17/include` 被 C++17 翻译单元包含时看不到这些类型；被 C++23 翻译单元
  包含时自动启用。两个构建都由测试覆盖：
  - `test_cpp17_smoke`：`-std=c++17`；
  - `test_cpp17_glaze`：`-std=c++23` + glaze，覆盖 typed params、JSON、
    OpenAPI schema、JWT/JWKS。

## 4.2 当前移植状态

已移植并通过测试：

- core / proto / transport 全量（不含 `logging_json.h`，按约定暂缓）；
- HTTP/1.0、HTTP/1.1、HTTP/2 服务端（含 h2c Upgrade、RFC 8441 扩展 CONNECT、
  WebSocket 本地路由与字节级反代）；
- handler 层：Router（精确/模板/正则/静态文件/Host/反代）、内置中间件、CORS、
  secure headers、rate limit、session、CSRF、validation、conditionals、
  `extract<...>`、JWT/JWKS、`send_file`、`read_body_to_file`；
- OpenAPI 文档模型与 `openapi::doc()`（glaze 门控）；
- 客户端：HTTP/1.1、HTTP/2、WebSocket（H1 Upgrade）、重定向、Cookie、SSE、
  流式上传（multipart）、自动解压、出站代理（CONNECT / HTTPS / SOCKS5）；
- 服务端 WebSocket 反代的 h1 与 h2 前端；
- 测试：`test/run.sh` 的 `cpp17`、`cpp17-http`、`cpp17-client`、`cpp17-glaze`；
- 外部合规（对 `fixture_conformance_server_cpp17` 实测）：
  - h2spec `146/146`；
  - h1spec `33/33`；
  - Autobahn `517` 用例，`OK 514`、`FAILED 0`、`NON-STRICT 0`、`INFORMATIONAL 3`。

- 外部压力（对 `fixture_conformance_server_cpp17`，h1/h2/ws）：
  - h1：3 档响应体，0 错误；
  - h2c：3 档 × 300000 请求，0 错误；
  - WebSocket `echo1k`：1,200,000 条回显，0 失败。

外部合规命令（无 HTTP/3 的服务端需要 `CONFORMANCE_HTTP3=0`）：

```sh
xmake build fixture_conformance_server_cpp17
SERVER_BIN=$PWD/build/linux/x86_64/release/fixture_conformance_server_cpp17 \
CONFORMANCE_HTTP3=0 test/external/conformance/run.sh h2spec h1spec autobahn
```

压力测试（h3 档由 `STRESS_HTTP3=0` 自动跳过）：

```sh
SERVER_BIN=$PWD/build/linux/x86_64/release/fixture_conformance_server_cpp17 \
STRESS_HTTP3=0 RUN_CONFORMANCE=0 \
test/external/stress/run.sh cpp h1 h2 ws --no-conformance
```

### 4.2.1 已固化回归点

- **scatter-gather 写的 buffer 生命周期**：`transport/transport.h` 的
  `async_stream_write_seq` 曾把栈上的 `std::array<asio::const_buffer, N>` 以
  `boost::span` 传给 `asio::async_write`。Asio 只复制序列对象，span 仍是视图；
  一次写完时看不出问题，超过 socket 缓冲（实测 WebSocket 回显 128 KiB 后）需要
  多轮 `async_write_some` 时，续写会读取已销毁栈帧里的 buffer 描述符，表现为
  静默截断/卡死。现在直接传 `std::array` 本身，Asio 按值复制元素。
  `test/cpp17/smoke.cpp` 的 `test_scatter_write` 用 4 KiB socket 缓冲 + 256 KiB
  两段写 + 看门狗固化了该回归。

明确未移植：

- HTTP/3 / QUIC（`engine/h3/`、`quic/`）：cpp17 目标没有链接 ngtcp2/nghttp3。
- `core/logging_json.h`（需求约定删除）。
- 根实现的内部 `client/http_client.h` 在 cpp17 中合并进 `client/http.h`。

### 4.2.2 2026-11-08：token 实现补齐（根 `include/`）

本轮把 token 实现推进到可通过全部主测试套件，并补上移植时遗漏的行为：

- 取消：`Request::cancellation_slot()` 改为每次分配独立 signal，`cancel()`
  广播；修复同一 slot 被内外层同时绑定导致的 `cancellation_state` UAF。
  h1 的 socket 读显式转发 cancellation slot，`response_head_timeout` /
  `body_idle_timeout` / `request_timeout` 真正生效。
- 客户端：`dial`/`dial_via_proxy` 接回 `ResolveFn` 与 `connect_timeout`；
  h1 的 `Expect: 100-continue` 把 interim 100 作为 head 上抛；303/301/302
  降级 GET 时清除 `content-length`；状态码重试（`retry_statuses` +
  `Retry-After`）与 `max_redirects` 超限错误补齐。
- 服务端：h2c Upgrade 移植回 h1/h2（101 后把原请求重放为 stream 1），
  TLS 监听禁用；请求解压超限映射 413；`JsonError` 省略空 `details`；
  `message_size` / `invalid_argument` 统一使用 asio system 类别。
- h3 客户端：`dial_h3` 保证临时 `SSL_CTX` 活到 `SSL_new`。
- 验证（2026-11-08）：`test_unit` 419 用例全绿；client-diff 5 组全绿
  （basic 21 / body 9 / policy 24 + 2 known / ws 3 / h3 6 + 1 known）；
  server-diff 9 组全绿（routing 24 / body 23 / semantics 12 / middleware 14 /
  framework 30 / static 12 / ws 6 / raw 7 + 3 known / h2raw 12）；
  openapi 193 checks 全绿；h2spec 146/146、h1spec 33/33、h3spec 47/49
  （基线 47）、Autobahn 517（FAILED 0）；stress h1/h2c/h3/ws 全部 0 失败。

## 5. 验收

```sh
grep -rn "co_await\|co_return\|co_yield\|co_spawn\|asio::awaitable" cpp17/include \
    | grep -v "engine/dispatcher.h"
# 必须无输出

# C++17 编译 + smoke
最后可以编译当前test 目录下的单元测试， 初期阶段可以使用简单例子， 编译过就行
```

最终目标：

- `cpp17/include` 能以 `-std=c++17` 编译；
- token 接口同时支持回调与 `use_awaitable`；
- Asio 取消可生效；
- h2spec / h1spec / Autobahn / stress 对照 `simple_http` 基线不退化。
