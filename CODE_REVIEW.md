# simple_http 代码审查报告（client / server）

审查目标：线程安全、未定义行为（UB）、逻辑错误。
审查方法：逐文件精读 + 关键路径核验。带「✅ 已核验」的条目由我直接对照源码确认；带「⚠️ 待复核」的来自辅助扫描，结论方向可信但未逐行确认。

## 覆盖范围

- 客户端：`client/http.h`、`client/http_client.h`、`client/h1_client.h`、`client/h2_client.h`、
  `client/ws_client.h`、`client/client_stream.h`、`client/cookie_jar.h`
- 服务端：`net/server.h`、`net/connection.h`、`engine/h1/*`、`engine/h2/*`、
  `handler/router.h`、`handler/http_proxy.h`、`handler/rate_limit.h`、`handler/cors.h`、
  `handler/static_files.h`、`proto/websocket.h`、`proto/ws_frame.h`、`engine/h1/ws_proxy.h`
- 未覆盖：`engine/h3/*`、`quic/*`、`openapi/*`、`proto/multipart.h`、`core/static_table.h` 的完整细节

## 结论速览

| # | 严重 | 位置 | 类型 | 一句话 |
|---|---|---|---|---|
| 1 | 🔴 高 | `proto/ws_frame.h:227` | 逻辑/UB 风险 | 分片帧每次 `next()` 重置投递偏移，UTF-8 校验重复喂入、有效 continuation 被误杀，且 O(n²) |
| 2 | 🔴 高 | `handler/cors.h:169` | 安全/逻辑 | 通配 origin 只做后缀匹配，`*.example.com` 会放过 `evil-example.com` |
| 3 | 🔴 高 | `handler/rate_limit.h:103` | 安全/内存 | `m_buckets[key]` 先插入再判容量，`max_keys` 失效、map 无界增长 |
| 4 | 🔴 高 | `engine/h2/h2_engine.h:399,603` | 逻辑/资源泄漏 | 引擎侧 RST / 连接拆除不 fail 请求体，handler 永久挂起并泄漏整个引擎 |
| 5 | 🔴 高 | `engine/h1/h1_engine.h:521` | 安全/走私 | 重复 `Content-Length` 不拒绝（CL.CL 请求走私） |
| 6 | ✅ 已修复 | `handler/router.h` | UB/lifetime | 原 `for_executor` 返回 map 内 `Client&`，关机 `close_all()` 可致在途代理 UAF；改为每请求 client 后 map/锁/`close_all` 全删，问题结构性消失 |
| 7 | 🟠 中 | `engine/h2/h2_engine.h:203` | 逻辑 | 服务端连接级接收窗口按 `h2_initial_window` 初始化却不下发连接级 WINDOW_UPDATE，非默认值即死锁/误 GOAWAY |
| 8 | 🟠 中 | `engine/h1/h1_engine.h:512` | 逻辑/互操作 | h2c 升级丢弃已缓冲的客户端 preface 字节 |
| 9 | 🟠 中 | `engine/h1/h1_engine.h:146` | 安全/响应拆分 | 流式 `send_headers` 不擦除 handler 自带的 `content-length`，同时发 CL+TE |
| 10 | 🟠 中 | `engine/h2/h2_engine.h:1055` | 安全/DoS | 解压后 header list 无大小上限（HPACK 膨胀炸弹） |
| 11 | 🟠 中 | `client/h2_client.h:1413` | UB/迭代器 | `erase_stream` 先 `out_space->close()` 再 `erase(it)`，内联唤醒可能使 `it` 失效 |
| 12 | 🟠 中 | `client/h2_client.h:148` | 逻辑 | `RST_STREAM(REFUSED_STREAM)` 被降级成 `stream_reset`，重试策略失效 |
| 13 | 🟠 中 | `client/h2_client.h:1112` | 逻辑 | 无 `END_STREAM` 的 trailers 块被当成 body 正常结束（可截断） |
| 14 | 🟠 中 | `client/h2_client.h:124` | 线程安全/UB | `StreamOutcome::ec` 非原子，终态可被二次覆盖，跨线程读数据竞争 |
| 15 | 🟠 中 | `handler/router.h:205` | 逻辑/顺序依赖 | `insert_route(std::move(methods), …, make_route_entry(methods,…))` 实参求值顺序未定义，可能注册成 any-method |
| 16 | 🟠 中 | `handler/router.h:1222` | 逻辑 | `literal_prefix` 对顶层 `|` 只取第一支前缀，`/a\|/b` 的 `/b` 永远 404 |
| 17 | 🟠 中 | `handler/router.h:454` | 逻辑 | `group()` 前缀对 `ws_route`/`http_proxy` 等不生效，与文档矛盾 |
| 18 | 🟡 低 | `client/h2_client.h:524` | 逻辑 | `stream_read` 在仅对端半关时就 erase，双向流上传被提前中断 |
| 19 | 🟡 低 | `proto/websocket.h:146` | 安全 | 客户端掩码用 `std::rand()`，可预测且非线程安全 |
| 20 | 🟡 低 | `engine/h1/h1_engine.h:523` | 安全/走私 | `Transfer-Encoding` 用子串匹配，且只认 `chunked` 子串、不校验顺序 |
| 21 | 🟡 低 | `handler/rate_limit.h:67` | UB | `rate==0` 时 `ceil(x/0)=inf`，`static_cast<int64_t>` 是 UB |
| 22 | 🟡 低 | `core/validators.h:112` | 逻辑 | Range 数字解析 20 位溢出会回绕 |
| 23 | 🟡 低 | `engine/h1/ws_proxy.h:86` | 逻辑 | 代理建连失败返回值被调用方丢弃，连接默默关闭而非 502 |
| 24 | 🟡 低 | `engine/h1/h1_engine.h:870` | 协议混淆 | WebSocket 升级未限制为 GET，且 `upgrade` 用子串匹配 |
| 25 | 🟡 低 | `engine/h2/h2_engine.h:1154` | 逻辑 | `content-length` 转 `int64` 无上界检查，超大值变成负数从而跳过长度校验 |
| 26 | 🟡 低 | `handler/static_files.h:320` | TOCTOU | 按需读盘用 `ifstream` 跟随符号链接，扫描后替换可绕过 |
| 27 | 🟡 低 | `client/h2_client.h:287` | 逻辑 | `m_close_requested` 在请求通过准入校验前就置位，被拒请求会永久污染会话 |
| 28 | 🟡 低 | `client/h2_client.h:389` | 线程安全 | `active_streams()/last_stream_id()/draining()` 读非原子状态且可能跨线程 |

---

## 详细条目

### 🔴 #1 WebSocket 分片帧重复投递 / UTF-8 重复校验
**文件**：`proto/ws_frame.h:221-236`（配合 `proto/websocket.h:236-264, 340-357`）
**类型**：逻辑错误 + UB 风险（越界/误判）

```cpp
m_partial_active = true;
m_partial_opcode = static_cast<WsOpcode>(opcode);
m_partial_start = pos;
m_partial_len = static_cast<std::size_t>(payload_len);
m_partial_off = 0;                 // ← 每次 next() 都归零
...
out.already_delivered = m_partial_off;   // 因此恒为 0
```

`read()` 每轮循环都调用 `next(frame)`；只要帧未收全就返回 `NeedMore`，下一次 `next()` 又把
`m_partial_off` 归零，于是 `take_partial_payload()`（`ws_frame.h:252`）每次都从 offset 0 重新投递，
`already_delivered` 永远是 0。后果有二：

1. **正确性**：`websocket.h:351-352` 对 continuation 帧用 `already_delivered` 跳过已喂过的字节，
   但它恒为 0，于是增量喂过的前缀会被**再次**喂进同一个 `Utf8Validator`（continuation 故意不重置
   validator）。一个合法 text 消息的 continuation 帧若跨 TCP 段到达，会以 `1007` 被误杀。
   辅助扫描已构造最小复现（首帧 `"A"`，continuation `"€"` 分两段）确认失败。
2. **性能**：大 text 帧每读 8 KiB 就重放整段前缀，单帧 O(n²) 校验，构成 CPU 耗尽向量。

**修复**：只在“开始一个新帧”时初始化 offset（`m_partial_active` 在帧完成时已被置 false），
重解析同一在途帧时保留 `m_partial_off/m_partial_start/m_partial_len`：

```cpp
if (!m_partial_active) {
    m_partial_start = pos;
    m_partial_len = static_cast<std::size_t>(payload_len);
    m_partial_off = 0;
}
m_partial_active = true;
m_partial_opcode = static_cast<WsOpcode>(opcode);
```

### 🔴 #2 CORS 通配 origin 越界匹配
**文件**：`handler/cors.h:148-170` **✅ 已核验**
**类型**：安全逻辑错误（白名单绕过）

```cpp
const std::string_view lead = origin_rest.substr(0, origin_rest.size() - suffix.size());
return !lead.empty() && lead.front() != '.';
```

只要求 origin 以 suffix 结尾且前缀非空、首字符非 `.`，**没有校验标签边界**。于是
`https://*.example.com` 会匹配 `https://evil-example.com`（lead=`evil-`）和
`https://notexample.com`（lead=`not`）。服务端会为攻击者可控 origin 回显
`Access-Control-Allow-Origin`，若开了 credentials 即为完整绕过。

**修复**：要求紧邻 suffix 的字符是 `.`：

```cpp
return !lead.empty() && lead.back() == '.' && lead.front() != '.';
```

### 🔴 #3 RateLimiter 容量检查前先插入
**文件**：`handler/rate_limit.h:100-111` **✅ 已核验**
**类型**：安全 / 内存耗尽

```cpp
auto &bucket = m_buckets[owned];       // 已插入 nullptr 节点
if (!bucket) {
    if (m_buckets.size() >= m_max_keys) {
        return false;                  // 节点已经进去了
    }
    bucket = std::make_shared<TokenBucket>(m_rate, m_burst);
}
```

`operator[]` 先默认插入。超过 `max_keys` 后每个新 key 仍会新增一个 map 节点再返回 false，
`max_keys` 形同虚设，攻击者可用海量伪造 IP（配合 `real_ip()` 的 XFF）无界增长内存。

**修复**：先 `find`，仅在未超限时 `emplace`：

```cpp
auto it = m_buckets.find(owned);
if (it == m_buckets.end()) {
    if (m_buckets.size() >= m_max_keys) return false;
    it = m_buckets.emplace(owned, std::make_shared<TokenBucket>(m_rate, m_burst)).first;
}
return it->second->try_acquire();
```

### 🔴 #4 h2 引擎侧 RST / 连接拆除不 fail 请求体
**文件**：`engine/h2/h2_engine.h:399-405, 595-606` **✅ 已核验**
**类型**：逻辑错误 / 资源泄漏（可被单连接慢速 DoS）

`on_rst_stream`（对端发起）会 `request->body().fail(connection_reset)` 后再 erase；
但引擎自己发起 reset 的 `reset_stream()` 只 `erase_stream()`，**不 fail body**：

```cpp
void reset_stream(std::uint32_t stream_id, std::uint32_t error_code_value) {
    ...
    erase_stream(stream_id);   // 没有 request->body().fail(...)
    flush();
}
```

连接拆除时（`serve_loops` 末尾）也只 `out_space->close()`，不管请求体：

```cpp
for (auto &entry : m_streams) {
    if (entry.second.out_space) entry.second.out_space->close();
}
```

于是当一个 handler 已因 `END_HEADERS` 被 dispatch、正 `co_await req.body().read()` 时：
- 对端上传中途断连，或
- 引擎因 content-length 不匹配（`h2_engine.h:1242/1270`）等调用 `reset_stream`

该 handler 会永久挂在 `Body` 的 channel 上，且它持有 `self = shared_from_this()`
（`dispatch_stream`），导致引擎、stream 表、transport、request 全部无法析构——未认证的
慢速/内存 DoS。h3 引擎在同类路径是会 fail body 的（`h3_engine.h:540,644,981`），
说明这是遗漏而非设计。

**修复**：`reset_stream` / `erase_stream` / 拆除循环里对在途请求体调用
`request->body().fail(make_error_code(asio::error::connection_reset))`。

### 🔴 #5 重复 Content-Length 不拒绝（请求走私）
**文件**：`engine/h1/h1_engine.h:521-538`（框架在 `setup_body_framing:605-627`）**✅ 已核验**
**类型**：安全 / 请求走私

`Headers::get()` 只返回**第一个**匹配（`proto/headers.h:38-45`），而这里从不检查重复：

```cpp
auto cl = head.headers.get("content-length");
if (!chunked && cl) { ... 只校验第一个 ... }
```

`Content-Length: 5` + `Content-Length: 10` 会被按 5 帧定界，剩余 5 字节被当作下一个流水线请求，
前端/后端取值不一致即 CL.CL 走私。代码里 Host 已经用 `count()` 强制“恰好一个”，
Content-Length 也应如此。

**修复**：`if (head.headers.count("content-length") > 1) { 400; return; }`
（对 `Transfer-Encoding` 重复同理）。

### ✅ #6 反代 Client 引用在关机窗口内悬垂（已修复）
**文件**：`handler/router.h:80-104, 716-718` **✅ 已核验（引用返回 + close_all 时序）**
**类型**：UB / use-after-free

```cpp
simple_http::Client &for_executor(...) { ... return *it->second; }   // 只有 map 持有
...
auto &client = proxy->clients->for_executor(ex, proxy->client_cfg);
co_await run_http_proxy(..., client);   // client 跨多个 co_await
```

`Server::stop()` 的顺序是：`shutdown_connections()`（只 post 关闭、立即返回）→
`m_router->close_proxy_client()`（`close_all()` 立即销毁所有 `Client`）→ `m_pool->stop()`
（才排空在途协程）。在途的 `run_http_proxy` 协程可能仍持有并解引用已析构的 `Client`，
构成 UAF。锁只保护缓存，不保护返回对象生命周期。

**修复**：`for_executor` 返回 `std::shared_ptr<Client>`，`dispatch_impl` 用局部 `shared_ptr`
贯穿整个 exchange，使在途请求钉住 client。

**实际修复（2026-10-05，改为每请求 client）**：不做「返回 shared_ptr」，而是**彻底去掉
共享缓存**——路由不再持有 `ProxyClientSet`，`dispatch_impl` 为每个代理请求新建一个 pinned
`Client`（局部于协程栈），交换结束即析构。`forEach`/`close_all`/`m_mutex` 与
`Server::stop()` 里的 `close_proxy_client()` 一并删除，因此「关机窗口内解引用已析构 Client」
这条路径在结构上不存在了。

### 🟠 #7 h2 服务端连接级接收窗口初始化/回收不一致
**文件**：`engine/h2/h2_engine.h:203, 473-484, 1436-1443` **✅ 已核验**
**类型**：逻辑错误 / 死锁

```cpp
// 构造函数
m_conn_recv_window = m_limits.h2_initial_window;
// replenish_conn 阈值
if (m_conn_recv_pending >= m_limits.h2_initial_window / 2) { ... }
```

`SETTINGS_INITIAL_WINDOW_SIZE` **不改变连接级窗口**（RFC 9113 §6.9.2），连接级默认恒为 65535，
除非显式发连接级 `WINDOW_UPDATE(0, …)`。服务端 `queue_settings()` 从不发这一帧。于是：

- `h2_initial_window > 131070`：对端到 65535 就停，而阈值 `initial/2 > 65535` 永远达不到 →
  任何 >64 KiB 的请求体**永久停顿**；
- `h2_initial_window < 65535`：本地窗口先变负，对合法流量误发 `GOAWAY(FLOW_CONTROL_ERROR)`。

客户端实现是对的（`client/h2_client.h:618-628` 会补发连接级 WINDOW_UPDATE），服务端漏了。

**修复**：连接级窗口按协议默认 65535 初始化（并在需要放大时补发连接级 WINDOW_UPDATE），
`replenish_conn` 阈值用实际连接窗口而非 `h2_initial_window`。

### 🟠 #8 h2c 升级丢弃已缓冲的先导字节
**文件**：`engine/h1/h1_engine.h:512-515, 974-999` **✅ 已核验**
**类型**：逻辑错误 / 互操作

h1 解析完升级请求后把剩余字节存进 `m_buf`（`h1_engine.h:513`），但 `do_h2c_upgrade`
只把请求体 drain 出来，随后 `std::make_shared<Http2Engine>(...)` 新建引擎并调用 `run_h2c(...)`，
`run_h2c`（`h2_engine.h:223`）没有接收“已缓冲字节”的参数（`run()` 有 `prior_knowledge_bytes`，
`run_h2c` 没有）。RFC 9113 §3.2 允许客户端在升级请求后立即发送 HTTP/2 连接前言，因此这些
字节会被丢弃，h2 引擎读到第一个真实帧时前言校验失败并回 `GOAWAY(PROTOCOL_ERROR)`。

**修复**：把 `m_buf` 剩余字节传入 h2 引擎（给 `run_h2c` 增加 prior bytes 参数或设置 `m_recv_buf`）。

### 🟠 #9 流式响应同时发 Content-Length 与 Transfer-Encoding
**文件**：`engine/h1/h1_engine.h:146-159` **✅ 已核验**
**类型**：安全 / 响应拆分

```cpp
if (m_version == Version::Http1 || m_head_request) {
    m_keep_alive_out = false;
} else {
    headers.add_lower("transfer-encoding", "chunked");   // 未先 erase("content-length")
}
```

`send()` 路径有 `headers.erase("content-length")`（`h1_engine.h:96`），流式 `send_headers()`
没有。handler 若 `res->header("content-length","10").begin()`，线上就会同时出现 CL 与 TE，
偏好 CL 的中间件会与 chunked 定界不一致。

**修复**：`add_lower("transfer-encoding", ...)` 前先 `headers.erase("content-length")`。

### 🟠 #10 h2 HEADERS 解压后无大小上限
**文件**：`engine/h2/h2_engine.h:535-537, 1055-1106` **✅ 已核验（无上限检查）**
**类型**：安全 / 内存耗尽

只限制了**压缩块**大小（`header_block_too_big` ≤ `max_header_bytes`，默认 64 KiB），
`HpackDecoder` 动态表上限 4 KiB，但索引字段 1 字节即可展开成整条动态表条目。64 KiB 的
重复索引字节块可展开为数百 MB 的 `fields`，再复制进请求 `Headers`。且 `SETTINGS_MAX_HEADER_LIST_SIZE`
从未下发。

**修复**：decode 时/后按 RFC 9113 §6.5.2 的 “name+value+32/字段” 累计上限；并下发
`SETTINGS_MAX_HEADER_LIST_SIZE`。

### 🟠 #11 h2 客户端 `erase_stream` 先唤醒后 erase
**文件**：`client/h2_client.h:1413-1423` **✅ 已核验（且与 `it_finish` 的注释自相矛盾）**
**类型**：UB / 迭代器失效

```cpp
void erase_stream(std::unordered_map<...>::iterator it) {
    Stream &st = it->second;
    ...
    if (st.out_space)
        st.out_space->close();   // 可能在同 executor 内联唤醒 parked writer
    m_streams.erase(it);         // it 可能已被唤醒路径的 try_emplace 重哈希而失效
    ...
}
```

`it_finish` 明确写了“Erase before waking … 否则 erase 会拿到已被丢弃节点的迭代器”
（`h2_client.h:1388-1390`），但 `erase_stream` 自己恰好犯了这个错。被唤醒的
`await_out_space` 协程若在同 executor 内联恢复并再次 `open_stream`/`try_emplace`，
`m_streams` 重哈希即令 `it` 失效，随后 `erase(it)` 是 UB。

**修复**：先把需要的东西取出，**先 erase，再 close 通道**：

```cpp
auto space = st.out_space;
auto owed  = st.recv_owed_conn;
m_streams.erase(it);
m_active_streams.fetch_sub(1, ...);
if (owed > 0) replenish_conn(owed);
if (space) space->close();
```

### 🟠 #12 `REFUSED_STREAM` 被降级为 `stream_reset`
**文件**：`client/h2_client.h:143-157` **✅ 已核验**
**类型**：逻辑错误

`on_rst_stream`（`h2_client.h:1303-1307`）细心地把 `H2_REFUSED_STREAM` 存成 `stream_refused`，
但 `outcome_result` 对 `Reset` 一律返回 `stream_reset`：

```cpp
case StreamState::Reset:
    return std::unexpected{make_error_code(client_errc::stream_reset)};
```

默认重试规则会重试 `stream_refused`（即便非幂等），不会重试 `stream_reset`；而
`write()/finish()` 走 `to_error()` 又返回 `stream_refused`，两条路径不一致。

**修复**：`outcome_result` 的 `Reset` 分支返回 `outcome.to_error()`（或特判 `stream_refused`）。

### 🟠 #13 无 END_STREAM 的 trailers 块被当成 body 正常结束
**文件**：`client/h2_client.h:1055-1116` **✅ 已核验**
**类型**：逻辑错误（可被静默截断）

`finish_header_block` 已算出 `end_stream = st.block_end_stream`（`h2_client.h:1069`），
但 trailers 分支无条件认为结束：

```cpp
if (st.head_seen) {
    st.remote_end = true;
    wake(st);
    return true;
}
```

RFC 9113 §8.1 要求 trailers 必须由 `END_STREAM` 终止。缺失时后续 DATA 会被丢弃
（`on_data` 只回补窗口），调用方却拿到一个干净的 EOF——恶意对端可借此隐藏数据。

**修复**：`if (!end_stream) { reset_stream(id, H2_PROTOCOL_ERROR); return true; }` 再置 `remote_end`。

### 🟠 #14 `StreamOutcome::ec` 非原子 + 终态可二次覆盖
**文件**：`client/h2_client.h:124-138, 1434, 744` **✅ 已核验结构**
**类型**：线程安全 / 数据竞争（UB）

```cpp
struct StreamOutcome {
    std::atomic<int> state{...};
    error_code ec;          // 非原子
    void set(StreamState s, error_code e = {}) {
        ec = std::move(e);
        state.store(..., std::memory_order_release);
    }
};
```

`state` 用 release/acquire 供跨线程 `finished()` 读，但依赖 `ec` “只写一次”。实际终态会被二次写入：
`close()`→`fail_all_streams` 先写 `Failed/session_closed`，仍在跑 `run_loops` 随后又把它覆盖成
`Disconnect/eof`。异线程若先观察到第一个终态再调 `to_error()/outcome_result` 读 `ec`，
就与 executor 的第二次写竞争。

**修复**：让首次终态胜出（`set` 从 `Open` 做 CAS，已终态则不再覆盖），或把 `ec` 也做成原子/加锁保护。

### 🟠 #15 `route()` 实参求值顺序未定义
**文件**：`handler/router.h:202-208` **✅ 已核验**
**类型**：逻辑错误 / 顺序依赖（潜在）

```cpp
return insert_route(
    std::move(methods), std::move(scoped_registration.first),
    make_route_entry(methods, make_handler(...), std::move(scoped_registration.second)));
```

函数实参求值顺序未指定：若先对第一个实参求值并用 `std::move(methods)` 移动构造参数，
`make_route_entry(methods, ...)` 读到的就是已移动的（通常为空）vector，`method_bits` 变成
any-method（`insert_entry` 对空列表填 `tbl.any`，`router.h:850-858`），路由会匹配任意方法并破坏
405 语义。当前 GCC/Clang 恰好从右到左求值掩盖了它，属编译器相关的潜在坑。

**修复**：先物化 entry 再移动：

```cpp
auto entry = make_route_entry(methods, make_handler(...), std::move(scoped_registration.second));
return insert_route(std::move(methods), std::move(scoped_registration.first), std::move(entry));
```

### 🟠 #16 `literal_prefix` 对顶层 `|` 只保留第一支
**文件**：`handler/router.h:1215-1247`（用于 `722-741`、`find_ws_regex`、`find_ws_proxy`、`find_http_proxy`）
**✅ 已核验**
**类型**：逻辑错误

`literal_prefix` 在遇到 `|` 时直接返回已积累的前缀。对 `"/a|/b"` 得到 `"/a"`，而该正则能匹配 `/b`；
`dispatch_impl` 先做 `path.starts_with(literal_prefix)` 快筛，`/b` 被跳过 → 404。注释
“凡能匹配的路径必以此开头”对顶层交替不成立。

**修复**：模式含顶层 `|` 时令 `literal_prefix` 为空（或取各分支的最长公共字面前缀）。

### 🟠 #17 `group()` 前缀对 ws / proxy 路由不生效
**文件**：`handler/router.h:435-502` **✅ 已核验**
**类型**：逻辑错误 / 与文档矛盾

`group()` 注释（`router.h:419-434`）声明“前缀会前置到内部注册的每个路径与正则，
`route/route_regex/ws_route/http_proxy` 都可用”，但 `ws_route`、`ws_route_regex`、
`ws_proxy`、`ws_proxy_regex`、`http_proxy`、`http_proxy_regex` 都**不读 `m_scopes`**
（对比 `route`/`route_regex` 会做 `scoped_path/scope_regex`）。于是
`group("/api", [](Router& r){ r.ws_route("/chat", h); })` 实际响应 `/chat` 而非 `/api/chat`。

**修复**：对这些注册函数套用 `scoped_path` / `scope_regex`，与 `route` 对齐。

### 🟡 #18 `stream_read` 仅对端半关即 erase
**文件**：`client/h2_client.h:500-528` **✅ 已核验**
**类型**：逻辑错误（双向流）

```cpp
if (st.remote_end) {
    st.outcome->set(StreamState::Eof);
    erase_stream(it);          // 本地发送方向可能仍开着
    co_return ReadResult::end();
}
```

头文件宣称 h2 可全双工（读写并发）。服务端提前结束响应（如 4xx/413）后，客户端的上传
请求体被提前中断，后续 `finish()` 报错。而 `fill_data_frames`（`h2_client.h:1533-1538`）
的 retire 判定要求本地与远端都结束，两处不一致。

**修复**：仅在本地发送也结束（`out_finished`/`local_end`）时才 erase；否则置
`remote_end` 并返回 EOF，保留 entry 供写入侧收尾。

### 🟡 #19 客户端 WebSocket 掩码用 `std::rand()`
**文件**：`proto/websocket.h:144-146` **✅ 已核验**
**类型**：安全 / 可移植性

```cpp
for (auto &b : mask_key)
    b = static_cast<unsigned char>(std::rand() & 0xFF);
```

RFC 6455 §5.3 要求掩码 key 源自强熵；`std::rand()` 未播种、进程内可预测，标准也不保证线程安全
（多条连接在不同 executor 线程并发调用）。同文件 `ws_client_key()` 已用 `std::random_device`，
掩码路径是唯一遗漏。

**修复**：改用 `thread_local` 的 `random_device` 播种 PRNG 或 OS 熵源填 4 字节。

### 🟡 #20 `Transfer-Encoding` 子串匹配 + 不校验顺序
**文件**：`engine/h1/h1_engine.h:522-523, 609-610` **✅ 已核验**
**类型**：安全 / 走私类

```cpp
bool chunked = te && icontains(*te, "chunked");
```

`icontains` 是纯子串匹配，会命中 `notchunked`、`chunkedfoo`，且接受 `chunked, gzip`
（非末位）；`Transfer-Encoding: gzip`（无 chunked）被当作**无 body**，body 字节被读作下一个请求。
与合规前端可能产生定界分歧。

**修复**：按逗号拆 transfer-coding 列表，要求 `chunked` 为最后一个 coding，其它/乱序一律 400。

### 🟡 #21 h2c 升级错误路径不 flush GOAWAY（补充）
**文件**：`engine/h2/h2_engine.h:235-241`
`run_h2c` 在 SETTINGS 非法时 `go_away()` 后直接 `co_return`；此时写循环尚未启动
（`serve_loops` 不会到达），GOAWAY 只留在 `m_out`，客户端收到 101 后是裸关闭而非 GOAWAY。
**修复**：返回前直接写一次，或让错误路径也跑一轮 flush。

### 🟡 #22 `TokenBucket` rate==0 除零
**文件**：`handler/rate_limit.h:61-68`
`ceil((1.0 - m_tokens)/m_rate)`，`m_rate==0` 得到 `+inf`，`static_cast<int64_t>(inf)` 是 UB。
**修复**：`m_rate<=0` 时返回 0 或哨兵值。

### 🟡 #23 Range 数字解析回绕
**文件**：`core/validators.h:112-122`
`v.size() > 20` 才拒绝，但 20 位仍可能 > `UINT64_MAX`，累乘回绕（无符号、无 UB，但会选中错误区间）。
**修复**：用 `std::from_chars` 或累乘时检测溢出。

### 🟡 #24 ws_proxy 建连失败被丢弃
**文件**：`engine/h1/ws_proxy.h:86-96`，调用方 `engine/h1/h1_engine.h:481-487`
`run_ws_proxy` 返回 `bool` 标识 resolve/connect/写失败，但唯一调用方
`co_await run_ws_proxy(...)` 丢弃返回值；此时 `m_upgraded` 已置 true，引擎也不会再关连接，
于是请求以裸关闭告终而非 502。
**修复**：捕获返回值，失败时回 502 并关闭 transport。

### 🟡 #25 WebSocket 升级未限制 GET + 子串匹配
**文件**：`engine/h1/h1_engine.h:870-877`
`is_websocket_upgrade` 注解写着 “a GET carrying…”，但没有 `method == Get` 检查，
`POST` 带 `Upgrade: websocket` 也会被 101 并把 body 当帧；`icontains` 还会命中 `notwebsocket`。
**修复**：要求 GET，并对 `upgrade`/`connection` 做 token 匹配。

### 🟡 #26 h2 请求 content-length 转 int64 无上界
**文件**：`engine/h2/h2_engine.h:1150-1162`
`static_cast<std::int64_t>(value)` 对 `value > INT64_MAX` 变负，随后所有
`declared_content_length >= 0` 判断被跳过，body 长度不再校验。
**修复**：`value > INT64_MAX` 时 `malformed`。

### 🟡 #27 静态文件按需读盘的符号链接 TOCTOU
**文件**：`handler/static_files.h:320` → `core/static_table.h` 的 `read_body`
超过 `preload_max_file_bytes` 的文件每次请求从 `disk_path` 用 `std::ifstream` 重开，会跟随
符号链接；扫描后替换同名文件为同大小软链可绕过扫描期的 symlink 拒绝。
**修复**：`O_NOFOLLOW` 打开并 `fstat` 校验 dev/inode/size，或全部预载。

### 🟡 #28 h2 客户端把 `m_close_requested` 置位过于提前
**文件**：`client/h2_client.h:287-294`
在 `invalid_spec`/`too_many_streams` 等准入检查之前就因 `request->close()` 置位，
被拒请求会永久让会话 `keepable()` 为假。
**修复**：所有准入通过、流真正建立后再置位。

### 🟡 #29 h2 客户端 introspection 跨线程读非原子状态
**文件**：`client/h2_client.h:389-393`
`active_streams()` 读 `m_streams.size()`、`last_stream_id()` 读 `m_last_stream_id`、
`draining()` 读 `m_goaway_sent`，但注释明确这些字段是 executor-only（`m_active_streams` 才
特意做成原子）。若从其它线程调用即数据竞争。
**修复**：标注仅 executor 调用，或改为原子/走 `m_active_streams`。

---

## 已检查未发现问题（缩小范围）

- **模型 A 的 hop 纪律基本一致**：h1/h2 的 ResponseSink、WebSocket 写泵、客户端
  各入口都先 hop 回 pinned executor，未发现未 hop 的共享状态写。
- **h2 帧解析边界**：`strip_padding`、`on_priority`、`on_window_update`、`on_ping`、
  `on_rst_stream`、24 位长度运算均有前置长度检查，未发现 OOB/溢出。
- **HPACK 编解码动态表**：字节记账、超表条目淘汰、size update 顺序、静态/动态索引运算正确。
- **h1 chunked 解码**：chunk-size 十六进制解析、单块/总量上限、trailer 消费、块后 CRLF 检查正确。
- **Router 懒构建中间件链**：`m_chain_built` + mutex 的双检锁内存序正确，发布前已 move 完成。
- **StaticTable**：`load()` 后不可变，只读查找并发安全；路径规范化拒绝 `..` 与转义分隔符。
- **反代请求头**：合成请求行/转发头对 CR/LF/NUL 有拒绝，未发现注入。
- **IoCtxPool / Server::stop 时序**：`stop()` 先 close acceptor（post 到各自 context 并等待）再
  drain pool，`m_stopped` 保证只执行一次；post 的 close 不会在栈帧销毁后运行（pool join 在前）。

## 建议修复优先级

1. 立即：**#1 #2 #3 #4 #5**（可利用、可 DoS、安全）
2. 尽快：**#6 #7 #8 #9 #10 #11 #12 #13 #14**
3. 计划：**#15–#29**（正确性/健壮性，多为边界或配置相关）

---

## 本轮修复进展

高优先级 5 项已全部修复并验证。每项均「先复现 bug，再修，再验证」。

| # | 状态 | 改动 | 复现（修复前） | 验证（修复后） |
|---|---|---|---|---|
| 1 | ✅ 已修 | `proto/ws_frame.h`：仅新帧重置部分投递偏移 | Python raw-socket 分片 continuation → close 1007 | 同脚本回显 `A€`；Python ws 套件全绿；新增 C++ 单测 |
| 2 | ✅ 已修 | `handler/cors.h`：通配后缀要求标签边界 `.` | `evil-example.com` / `notexample.com` 被回显 ACAO | Python httpx 6 例全对；新增 C++ 单测负例 |
| 3 | ✅ 已修 | `handler/rate_limit.h`：先判容量再 emplace；新增 `bucket_count()` | 100 个敌意 key → map 增长到 100（max_keys=4） | Python 端 `/ratelimit-keys` 稳定 ≤4；新增 C++ 单测 |
| 4 | ✅ 已修 | `engine/h2/h2_engine.h`：引擎侧 RST 与拆除时 `body().fail()`；`proto/body.h` 终止符幂等（首个终止符胜出） | Python h2 短 body 触发 RST 后 handler 仍挂起（active=1） | 同脚本 active=0；Python h2/edges 套件全绿 |
| 5 | ✅ 已修 | `engine/h1/h1_engine.h`：重复 `Content-Length` 回 400 | Python raw socket 收到 200（按首值定界） | 同脚本收到 400；新增 Python 回归用例 |

相关文件：
- 修复代码：`include/simple_http/proto/ws_frame.h`、`include/simple_http/handler/cors.h`、
  `include/simple_http/handler/rate_limit.h`、`include/simple_http/engine/h2/h2_engine.h`、
  `include/simple_http/proto/body.h`、`include/simple_http/engine/h1/h1_engine.h`
- 回归测试：`test/python/test_regressions.py`（接入 `test/python/run.py`）、
  `test/unit/test_cors.cpp`、`test/unit/test_rate_limit.cpp`、`test/unit/test_ws_codec.cpp`
- 测试端点（示例服务器）：`/cors-wildcard`、`/ratelimited`、`/ratelimit-keys`、`/upload`、`/upload-active`

附带结论：
- **#6（反代 Client UAF）已在当前代码中不复存在**：`http_proxy` 改为在 dispatch 协程栈上
  **每请求构造一个 Client**（`router.h:654-664`），不再有跨请求缓存与 `close_all()` 的悬垂窗口。
  本报告最初基于旧版 `ProxyClientSet` 写法，特此更正。
- 其余中/低优先级项（#7–#29）未在本次改动范围内，仍建议按上表顺序推进。

### 验证汇总

- Python 第三方客户端套件：**53 checks, 0 failed**（含新增 4 组回归）
- C++ 单元测试：**256 用例 / 63775 断言全过**（含新增 3 个回归用例）
- 符合性套件（ASan 同轮）：h2spec **146/146**、h1spec **33/33**、h3spec **47/49**、
  Autobahn **FAILED 0 / NON-STRICT 0**
- ASan+UBSan 版 `server` 下跑 `test/stress/run.sh cpp`（四档 h1/h2/h3/ws）
