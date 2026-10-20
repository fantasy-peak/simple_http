# simple_http 代码审查与改进清单

> 审查对象：当前工作区 `include/simple_http/`、`test/`、`xmake.lua`、`CMakeLists.txt`。
> 方法：读代码 + 在本机启动 `test/server`，用 curl / nghttp / aioquic / 库自带 Client 交叉验证。
> 结论按严重度分组；“实测”表示有可复现的线上行为，“代码”表示由源码直接确认。

## 摘要

协议引擎和并发模型是强项：h1/h2/h3、WebSocket、多监听、取消与背压都有针对性实现，
外部一致性基线也很扎实（h2spec 146/146、h1spec 33/33、Autobahn 517 用例 0 失败、
h3spec 通过 ≥47/49）。

主要短板不在帧层，集中在：

1. 少数安全/协议语义 bug（`Date`、`real_ip`、`CookieJar`、WebSocket 握手超时、h1 framing 解析）；
2. 运维语义（`Server::stop` 不是优雅关闭、静态大文件、Slowloris 只有 idle timeout）；
3. 客户端/反代连接模型（单连接、每请求新建上游连接）；
4. 构建与打包（CMake 实际不可用、告警未按错误处理、依赖冗余、无 CI/fuzz）。

## P0：确定性问题（建议优先修复）

### 1. 动态响应缺 `Date`

`proto/response.h` 的 `apply_defaults()` 只补 `content-type` 和 `server`，没有 `date`。
实测 `/world` 在 h1、h2c、h3 三种协议下都没有 `Date`；静态文件服务反而手动补了。

RFC 9110 §6.6.1 要求 origin server 在绝大多数响应中发送 `Date`。修复：在
`apply_defaults()` 里补 `http_date(now_unix())`，已有 `Date` 时不覆盖。

**状态：✅ 已修复（见文末验证）。**

### 2. `real_ip` 信任链错误

`handler/builtin_middleware.h` 的 `real_ip`：

- `X-Real-IP` 无条件采用，完全不检查直连 peer 是否在 `trusted_proxies` 内；
- `X-Forwarded-For` 只按列表筛选 hop，没有先验证直连 peer 可信。

实测：`trusted={10.0.0.0/8}`、直连 peer=`127.0.0.1`、请求带 `X-Real-IP: 1.2.3.4`，
`ClientIp` 仍被设为 `1.2.3.4`。默认 `rate_limit` 的 key 优先使用 `ClientIp`，因此可被
伪造绕过。

修复：空 `trusted_proxies` 视为“不信任任何转发头”（fail closed）；只有直连 peer 被
列表覆盖时才读取 `X-Real-IP`/`X-Forwarded-For`；XFF 仍按自右向左取第一个非可信 hop。

**状态：✅ 已修复（见文末验证）。**

### 3. `CookieJar` 的 host-only / 替换 / 删除

实测：

```text
host-only cookie -> subdomain : [hostonly=secret]   # 不应发送
after replacement: [sid=one; sid=two]                # 同名未替换
after deletion:    [sid=one; sid=two]                # Max-Age=0 未删除
```

- host-only cookie 被当成 Domain cookie，泄漏到子域；
- 同 `(name, domain, path)` 的 `Set-Cookie` 只追加不替换；
- `Max-Age=0` / 过期删除不会移除旧 cookie；
- `Domain` 属性不校验是否 domain-match 请求 host；
- `Domain=co.uk` 这类 public-suffix 仍会被接受（头文件已声明不做 PSL，但前四项是 bug）。

修复：给 `Cookie` 增加 `host_only`；`store()` 先删除同键旧值，过期 cookie 只删不加；
`Domain` 必须匹配请求 host；`Path` 非法时回退默认目录；`Max-Age` 优先于 `Expires`。

**状态：✅ 已修复（见文末验证）。**

### 4. `Client::open_websocket` 握手没有超时

`client/ws_client.h` 的 `ws_upgrade()` 在读取 101 响应头时没有 deadline；
`connect_timeout` 只覆盖 TCP/TLS，`idle_timeout` 要握手成功后才交给 `WsBackendImpl`。

实测：对“接受 TCP 但永不回响应”的服务端，`connect_timeout=2s`、`idle_timeout=2s`，
`open_websocket()` 7 秒仍不返回。

修复：给整个 Upgrade 握手加 `ClientConfig::response_head_timeout` 预算，超时回
`client_errc::response_head_timeout` 并关闭 transport。

**状态：✅ 已修复（见文末验证）。**

### 5. HTTP/1 framing 用子串匹配

`h1_engine.h` 用 `icontains(*te, "chunked")`、`icontains(*conn, "close")`。实测：

| 请求 | simple_http | nginx 1.31.7 |
|---|---|---|
| `Transfer-Encoding: notchunked` + CL | 被当成 chunked，连接直接断 | `501` |
| `Transfer-Encoding: gzip, chunked` | 接受，但不会解 gzip | `501` |
| `Transfer-Encoding: chunked, gzip` | 接受（chunked 不是最后的 coding） | `501` |
| `Connection: not-close` | 误当 close，1 个响应后断开 | — |
| `Connection: xkeep-alive` (HTTP/1.0) | 误当 keep-alive | — |

这既是语义错误，也是请求走私的经典形状。修复：

- 按逗号拆分、去 OWS、大小写不敏感地**精确匹配** token；
- `Connection` 支持多行；
- `Transfer-Encoding` 只接受单独的 `chunked`（且必须是最后一个 coding）；
- 不支持的 transfer coding 回 `501`；
- `chunked` 不是最后一个、空 coding、重复 `chunked` 回 `400`；
- `Transfer-Encoding` + `Content-Length` 同时存在回 `400`（对齐 nginx，消除 CL.TE 分歧）。

**状态：✅ 已修复（见文末验证）。**

### 6. `Server::stop()` 不是优雅关闭

`net/server.h` 在 drain 前调用 `shutdown_connections()`，直接关闭所有 live transport；
2 秒只是协程展开上界。实测 `GET /delay?ms=5000` 发出 1 秒后 `SIGTERM`，curl 收到
`Empty reply from server`。

建议：可配置 drain（先停 accept → 等在途请求完成或超时 → 再关连接）；h2/h3 先发 GOAWAY。

**状态：✅ 已修复（见文末第二批验证）。**

### 7. 静态大文件阻塞 + Range 整文件读

`core/static_table.h::read_body()` 对超过预载阈值的文件用同步 `std::ifstream` 全量读；
`handler/static_files.h` 先 `read_body` 再 `substr(range)`。后果：阻塞 io_context worker；
大文件 1 字节 Range 也会读整个文件进内存。建议异步随机读 + 只读 range。

**状态：待修复。**

### 8. Slowloris 只有 idle timeout

h1/h2 每次读都刷新 idle deadline，没有“从首字节起算”的绝对 header/preface 超时。
攻击者每 119 秒发 1 字节即可长期占用连接。建议增加 `header_timeout`。

**状态：待修复。**

### 9. 反代/客户端连接模型

- `handler/http_proxy.h` 每请求新建 upstream `Client`，无上游 keep-alive 池；
- `Client` 每个 origin 只保留一条连接，h1 并发忙时其他请求各自拨一次性连接；
  ROADMAP 已承认高并发残余失败。

**状态：已知限制，后续评估。**

### 10. `RateLimiter` 无回收，`rate=0` UB

- `max_keys` 满后新 key 永久 429，空闲 bucket 不淘汰；
- `TokenBucket::seconds_until_token()` 在 `rate=0` 时除零（浮点 inf 转 int64）。

**状态：✅ 已修复（见文末第二批验证）。**

## P1：API / 语义改进

- `PlaintextProtocols::Http1` 仍允许 h2c Upgrade；真正 h1-only 是
  `PlaintextHttp::Http1Only`。测试服务器启动日志把 `:7791` 叫 “h1-only” 会误导。
- `Expect: 100-continue` 不自动回 100；handler 直接读 body 可能与严格客户端互等。
  h2/h3 的 `send_continue()` 还是 no-op。
- absolute-form 请求目标（`GET http://host/path HTTP/1.1`）实测 404；`OPTIONS *` 也 404。
- 客户端错误分类：h2 prior-knowledge 打到 h1 端口报 `ECONNRESET`，而不是
  `version_not_negotiated`/`protocol_error`；h2 客户端未显式校验服务器首帧必须是 SETTINGS。
- TLS 服务端固定 1.3 且不可配置；与 nginx 默认 1.2+1.3 不同（安全取向，需文档说明）。
- `reuse_port` 默认 `true`，与 nginx 默认相反。
- 请求 trailer 不能由客户端发送；h1 请求 trailer 不暴露给 handler；multipart 整段缓冲。
- `basic_auth` 用普通 `==` 比较，非常量时间。

## P2：构建 / 打包 / 质量

- **CMake 实际不可用**：`find_package(Brotli REQUIRED)` 在 Ubuntu 上没有
  `BrotliConfig.cmake`（实测 configure 失败）；`cmake/simple_httpConfig.cmake.in`
  缺 `find_dependency(glaze)`，但导出 target 链接 `glaze::glaze`；CMake 不支持 HTTP/3。
- **xmake**：`set_warnings("all", "error")` 被注释；`-O2` 对所有模式无条件生效；
  `nghttp2` 是未使用依赖（二进制中 0 个符号）；`glaze` 重复 `add_requires`。
- **没有 CI / fuzz**：conformance/stress/Python 交叉脚本很强，但没有 CI 工作流、
  sanitizer 矩阵和解析器 fuzz 语料。

## 已确认的强项

- h1/h2/h3 帧层与 HPACK/WebSocket 编解码手写且通过外部套件；
- Rapid Reset、CONTINUATION flood、请求体按流停放、WebSocket 有界读泵；
- `Request::cancel` + `middleware::timeout` 的协作式取消；
- 多监听/每监听配置、h2c/h3/Alt-Svc 的部署形态；
- 测试覆盖面：单元、裸 socket 回归、客户端整合、Python 双向交叉。

## 本次修复（1-5，已完成）

1. `Date` 响应头：`Response::apply_defaults()` 补 `Date`（已有则保留），h1/h2/h3 的 ResponseSink 再做一次兜底（覆盖引擎直接生成的 400/413/431/502 等响应）；三种协议的动态响应均生效。
2. `real_ip` 信任链：空列表 fail closed；只有直连 peer 在 `trusted_proxies` 内才读取 `X-Real-IP`/`X-Forwarded-For`。
3. `CookieJar`：host-only 不再泄漏到子域；同键替换；`Max-Age=0`/过期删除；`Domain` 必须匹配请求 host；`Path` 非法时回退默认目录；`Max-Age` 优先于 `Expires`。
4. `Client::open_websocket`：整个 Upgrade 握手受 `ClientConfig::response_head_timeout` 约束，超时回 `client_errc::response_head_timeout`。
5. h1 framing：`Transfer-Encoding`/`Connection` 改为逗号 token 精确匹配；只接受 `chunked`；不支持 coding 回 501；`chunked` 非最后/空 coding 回 400；TE+CL 回 400；WS 握手带 body 回 400。

验证结果：

| 套件 | 结果 |
|---|---|
| `xmake run unittest` | 305 test cases / 64427 assertions，全过 |
| `xmake run regression` | 61 test cases / 329 assertions，全过 |
| `xmake run client` | 177 checks，0 failed |
| `xmake run python-tests` | 113 checks，0 failed |
| 手工 h1/h2/h3 | `Date` 均存在；TE 各分支为 501/400/200；h2c Upgrade 与 prior-knowledge 正常 |

## 第二批修复（6、10，已完成）

### 6. `Server::stop()` 优雅 drain

- `ServerConfig::shutdown_grace`（默认 5s，0 = 旧的立即关闭语义）。
- `Server::active_requests()`：只统计已经进入 dispatch 链的请求；idle keep-alive 不计数。
- `stop()` 顺序：关闭 acceptors → QUIC `begin_drain()`（拒绝新连接、保留已建立连接）→ 等
  `active_requests` 归零或 grace 到期 → 发送 QUIC CONNECTION_CLOSE → 关闭剩余 TCP 连接
  → join worker pool。
- `quic/endpoint.h` 新增 `begin_drain()`：未知 CID 的 Initial 不再创建连接，已有 CID 仍可
  完成在途请求。
- 实测：`GET /delay?ms=5000` 发出 1s 后 `SIGTERM`，curl 现在收到完整的 `200` +
  `"waited 5000 ms"`，而不再是 `Empty reply`。

### 10. `RateLimiter` 回收与配置校验

- 每个 key 记录 `last_used`，`m_lru` 维护 LRU 顺序。
- 达到 `max_keys` 时先回收超过 `idle_ttl`（默认 10min，0 = 关闭 TTL）的 bucket，再淘汰
  LRU；新 key 不再被永久 429。
- `rate` 必须为正有限值、`burst > 0`、`max_keys > 0`、`idle_ttl >= 0`，否则构造时抛
  `std::invalid_argument`。
- `seconds_until_token()` 对非有限/溢出的秒数 clamp 到 `int64` 上限，不再有除零 UB。

第二批验证：

| 套件 | 结果 |
|---|---|
| `xmake run unittest` | 307 test cases / 64438 assertions，全过 |
| `xmake run regression` | 62 test cases / 337 assertions，全过 |
| `xmake run client` | 177 checks，0 failed |
| `xmake run python-tests` | 113 checks，0 failed |
| 手工 stop | `/delay?ms=5000` 在 SIGTERM 后完整返回 200 |

## 性能：h2（:7790）回归排查

现象：`test/stress/run.sh cpp --no-conformance h2` 的 2 KiB 档有时只有 143k req/s，
而基线（c7b902e）在 16–18 万。机器负载波动很大（单次 1M 请求的 h2load 实测可在
155k–190k 之间浮动），所以先把测量改成多轮、同 toolchain（LLVM 24）、同参数：

- 300k × 3 档：基线首档约 162–168k；修改前首档 143k/155k/169k 波动。
- 1M 请求、5 轮中位数：基线约 175k；修改后约 174k；差异落在噪声内。
- `perf record`：`gmtime_r`/`snprintf` 不是主因（约 0.1%），但每个响应多一次 Date
  字符串分配 + HPACK 编码；分配器（malloc/free）本来就占 ~30%。

修复（`http_date.h` + 各 ResponseSink）：

- 用 Howard Hinnant 的 `civil_from_days` 直接写 29 字节 IMF-fixdate，去掉
  `gmtime_r` + `snprintf`；
- `http_date_now()` 按线程、按秒缓存，Date 有 1 秒分辨率，不需要每响应格式化；
- `ResponseWriter` 预留 4 个 header 槽位，避免 Date/Content-Type/Server 逐个触发
  vector 扩容。

结果：`test/stress/run.sh cpp --no-conformance h2` 首档回到 **169k req/s**（p99
38ms），与基线同量级；剩余差异主要是多出的 Date 头本身的字节/HPACK 成本，属于
RFC 9110 要求的固定开销。


## 复现片段

```sh
# Date 缺失
curl -sS -D - -o /dev/null http://127.0.0.1:7788/world

# h2 prior-knowledge 打 h1 端口
curl -sS --http2-prior-knowledge http://127.0.0.1:7791/world

# Slow stop
curl -sS -m 10 'http://127.0.0.1:7788/delay?ms=5000' &
sleep 1; pkill -TERM -x server
```
