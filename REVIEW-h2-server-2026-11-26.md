# HTTP/2 服务端实现 Review（2026-11-26）

审查对象（HEAD = `67438c9`，分支 `feat/standalone-asio`）：

- `include/simple_http/engine/h2/h2_engine.h`（3405 行）
- `include/simple_http/engine/h2/hpack_decode.h`、`hpack_encoder.h`、`h2_frame.h`
- `include/simple_http/engine/h2/h2_ws_transport.h`
- 交互面：`engine/dispatcher.h`、`proto/body.h`、`proto/response.h`、
  `proto/response_writer.h`、`core/limits.h`、`net/server.h`、
  `engine/h1/h1_engine.h`（h2c 入口）

方法：纯静态审查（未参考历史 review 文档），随后对 P1/P2 写最小复现验证。

---

## 总体评价

整体实现质量高，以下部分经逐条核对未发现问题：

- HPACK 整数解码溢出保护、Huffman 尾部 padding 校验、动态表字节记账、
  「超表项清空且不插入」、解码列表炸弹防护（越限后继续解码以保持表同步）。
- DATA 流量控制的所有归还路径（padding、parked frame、早响应 prepay、
  half-closed-remote DATA、erase）收支平衡，无重复/遗漏归还。
- CONTINUATION 互锁与 `m_continuation_stream` 状态机、跨 RST 的 HPACK 表同步、
  拒绝流的 discard 字节上限、closed-stream 三分法与 256 项环形记录。
- 帧级校验（HEADERS/DATA/WINDOW_UPDATE/RST/PING/SETTINGS/GOAWAY/PRIORITY）、
  自依赖 PRIORITY、SETTINGS 窗口溢出、Rapid Reset 上限。
- 响应侧：HEADERS+CONTINUATION 切分、trailers 的 END_STREAM 归属、
  HEAD/204/304、输出高/低水位与队列 compaction、`close_stream()` 的
  「先发完响应再 RST」。
- RFC 8441：`kReadEofIsHalfClose`、`ws_wake` 的 EOF 不丢失、mask 语义、
  ws 反代 101 校验与头部过滤。

---

## P1（确认）— HPACK 解压错误在「丢弃路径」被误判为可忽略

位置：`engine/h2/h2_engine.h:1371-1378`

```cpp
bool decode_discarded_header_block(std::string_view block) {
    std::vector<codec::HpackHeader> ignored;
    if (!m_decoder.decode(block, ignored) && !m_decoder.header_list_too_large()) {
        go_away(codec::H2_COMPRESSION_ERROR);
        return false;
    }
    return true;
}
```

`HpackDecoder::decode()` 的两种失败彼此独立：

- `header_list_too_large()`：解码后字段列表超过 `SETTINGS_MAX_HEADER_LIST_SIZE`。
  按 RFC 9113 §6.5.2 这是**流错误**；若块本身已完整解码，动态表仍同步。
- `compression_error()`：块本身畸形（截断整数/非法索引/非法字符串）。
  RFC 9113 §4.3.1 要求连接级 `COMPRESSION_ERROR`。

`finish_header_block()`（`h2_engine.h:1732-1753`）先判 `compression_error()`，
注释也写明「header_list_too_large() alone would misclassify that」。但丢弃路径
只看 `header_list_too_large()`：当畸形块**恰好也越过列表上限**时，
`!decode && header_list_too_large` 为假 → 不发 GOAWAY，连接继续。

后果：`decode()` 在畸形表示之前已经执行的 `dynamic_insert` 已生效，但块未解码
到结尾，无法保证与对端编码器同步；后续 HEADERS 可能按错的动态表解释。

可达性：`refuse_stream()`（并发超限、自依赖 PRIORITY）、已由本端 RST 的流上的
HEADERS、`half_closed_remote` 上的 HEADERS。均为对端可控；默认参数下几 KiB
压缩输入即可构造。

同一份逻辑也存在于 `client/h2_client.h:1162-1169`。

---

## P2（确认）— 进入 teardown 后 read 回调仍解析帧并派发新 handler

位置：`engine/h2/h2_engine.h:985-1008`

```cpp
m_transport->async_read_some(..., [self](error_code ec, std::size_t n) {
    if (ec) { self->finish_connection(); return; }
    ...
    if (!self->parse_available()) { self->finish_connection(); return; }
    self->flush();
    self->read_loop();                        // 这里有 m_finished 检查
});
```

`read_loop()` / `write_loop()` 入口都有 `if (m_finished) return;`，但 read 完成
回调本身没有。`finish_connection()`（`h2_engine.h:1073`）也不取消挂起的读：
它只 `m_watchdog->cancel()`、`m_notify.close()`，真正 `close()` 要等可选的在途
写完成（`arm_finish_deadline()` 最长 2 s）。

可达场景：`write_loop` 的写完成以 error 结束（对端 RST/半关闭、TLS 写错误）时，
read 仍在挂起；transport 关闭前若读回数据：

1. `parse_available()` 处理缓冲中的 HEADERS → `ensure_stream()` 新建
   Request/Sink/Body；
2. `dispatch_stream()` → `start_handler()` 直接运行用户 handler
   （二者均无 `m_finished/m_alive` 检查）。

`finish_connection()` 的 phase 2 已经快照并 cancel 过旧 request，**这条新流的
request 永远不会被 fail/cancel**；响应经 sink 失败（`alive()==false`），但 handler
捕获的 `shared_from_this()` 会持有整个 engine 与全部 Stream，直到它自行结束。
这违背 `finish_connection()` 中「Queued handlers must not start after teardown」
的意图。

次要后果：延迟 finish 的窗口里 `m_next_peer_stream_id` 仍可能被推进，使
`finish_connection()` 补发的第二个 GOAWAY 的 last-stream-id 大于前一个。

---

## P2 — 重复 GOAWAY，且第二次 last-stream-id 可能「递增」

位置：`h2_engine.h:2469-2481`（`go_away()` 已 append）、`1125-1134`（再次 append）

错误路径上 `go_away()` 通常在同一次调用栈内紧接着进入 `finish_connection()`，
于是 `m_out` 里有两份相同 GOAWAY，一次写出。RFC 9113 §6.8 允许多个 GOAWAY，
但要求 last stream ID **不得在后续 GOAWAY 中增大**；结合上一条，延迟 finish
窗口内解析到的新流会推高 `m_next_peer_stream_id`。

建议：`finish_connection()` 仅在 GOAWAY 尚未进入 `m_out`/已写出时补发。

---

## P2 — `EngineLimits` 的 h2 参数完全未校验/夹取

位置：`core/limits.h:54-68`、`h2_engine.h:810-838`、`1217-1220`

`Server::start()` 只校验 UNIX 地址/TLS/ALPN，不校验 limits：

- `h2_max_frame_size` 原样作为 `SETTINGS_MAX_FRAME_SIZE` 发出，RFC 9113 §6.5.2
  要求 `[2^14, 2^24-1]`；同时它还是接收上限，配置过小时会把对端合法帧判成
  `FRAME_SIZE_ERROR`。
- `h2_initial_window` 是 `int32_t`，可为负：负值会以巨大 uint32 写入 SETTINGS，
  且每条流的 `recv_window` 为负，带 body 的请求第一帧即 `FLOW_CONTROL_ERROR`。
- `h2_max_concurrent_streams == 0` 会使所有新流被 `REFUSED_STREAM`。

建议在 `start()` / `resolve_listener_limits()` 中校验并返回
`server_errc::invalid_config`。

---

## P3 — handler 提前结束后丢弃请求体的收尾不完整

位置：`h2_engine.h:2055-2076`、`2730-2747`

`handler_finished()` 在「响应已完成、请求体仍在到达」时提前归还
`recv_owed_conn` 并置 `body_discarded=true`，但：

1. 不清 `paused_frames`：此前停放的帧保留到对端 END_STREAM / 流销毁。
2. discard 分支收到 END_STREAM 时只 `maybe_complete_stream()`，**不调用
   `body().finish()/fail()`**。若存在 handler 之外的分离读者挂在
   `Body::async_read` 上，将永远等不到完成（`erase_stream()` 也不触碰 Body）。

修复：进入 `body_discarded` 时清停放帧；discard 分支 END_STREAM 时 finish/fail Body。

---

## P3 — `:authority` 缺失/为空被接受，与 h1 不一致

位置：`h2_engine.h:1904-1924`、`1975-1990`；h1 对照 `h1_engine.h:710-714`

非 CONNECT 请求只强制 `:method/:scheme/:path`，不要求 `:authority` 或 `host`；
空 `:authority` 等价于未发送。h1 对 HTTP/1.1 强制恰好一个 `Host`。结果：既无
`:authority` 也无 `host` 的请求可进入 handler / Host 路由 / 反代，而同样的
HTTP/1.1 请求会 400。`:authority` 的值也不做语法校验（h1 absolute-form 会拒绝
`user@host`）。

---

## P3 — plain CONNECT 与未知 `:protocol` 被当普通请求派发

位置：`h2_engine.h:1911`、`1927-1949`

- 无 `:protocol` 的 CONNECT 直接进入普通路由；若应用返回 200，客户端会按隧道
  发送 DATA，与服务端语义不一致（不支持应 405/501）。
- `:protocol` 非 `websocket` 时同样走普通路由，可能意外匹配 `CONNECT /path`。
- 未拒绝普通 CONNECT 携带 `:scheme/:path`（RFC 9113 §8.3.1 要求省略）。

---

## 低优先级 / 加固建议

1. **未 begin 就 write/finish 会让 h2 先发 DATA**
   `proto/response.h:247-285` 的 `async_write()/async_finish()` 不检查
   `async_begin()`；`Http2ResponseSink::send_chunk/send_last` 也不检查最终
   HEADERS 是否已提交。一次 `res->async_finish("x")`（未 begin）即可在没有
   HEADERS 的流上发出 DATA，对端以连接级 PROTOCOL_ERROR 关闭连接，
   同连接其他流一起失败。sink 已有 `m_head_started`，建议 body 类操作据此拒绝。
2. **h2c 重放 stream 1 保留 h1 hop-by-hop 字段**
   `h1_engine.h:1714-1716` 原样传入 `head.headers`，`h2_engine.h:395` 整体赋值；
   handler 会看到 `connection: Upgrade`、`upgrade: h2c`、`http2-settings`。
3. **`pump_dispatch()` 同步递归深度随并发上限增长**（`h2_engine.h:2766-2782`）。
4. **`ws_inbox.erase(0, n)` 为 O(n) FIFO**（`h2_engine.h:2962`），建议游标 + compaction。
5. 杂项：`h2_engine.h` 未直接 `#include <algorithm>`；`<random>`/`<thread>` 未使用；
   单帧 discard 路径未做 `max_header_bytes` 检查。

---

## 复现与修复结果（P1 / P2）

### 复现程序

- `review/h2-repro/repro.cpp`：自包含传输 double + 帧/HPACK 构造，直接驱动
  `Http2Engine`。编译：`review/h2-repro/build.sh`；运行：`repro p1` / `repro p2`。
- 回归用例已并入 `test/unit/test_h2_engine.cpp`：
  - `h2/engine: a malformed discarded HPACK block is a COMPRESSION_ERROR`
  - `h2/engine: a read completing after teardown does not dispatch a handler`

### P1 —— 修复前（bug 复现）

```
[P1] decoder: decode=0 compression_error=1 header_list_too_large=1 dynamic_table_bytes=38 (block=6009 bytes)
[P1] dispatched=1 goaway=0 goaway_error=0x0 rst3=1 rst3_error=0x7 written=61 bytes
[P1] BUG REPRODUCED: no GOAWAY(COMPRESSION_ERROR); ...
```

构造方式：stream 1 占满唯一并发槽（handler 永不完成），stream 3 被
`REFUSED_STREAM`；其字段块先插入一条动态表项，再解出 1500 个
`content-type: a`（12+1+32=45 字节/项，合计 67.5 KiB > 64 KiB 上限），
最后跟一个非法索引 `0x80`。解码器同时报告 `compression_error=1` 与
`header_list_too_large=1`，动态表已被部分修改（38 字节）。修复前服务端只发
`RST_STREAM(3, REFUSED_STREAM)`，连接继续存活。

### P1 —— 修复

`h2_engine.h:1387` `decode_discarded_header_block()`：改为与
`finish_header_block()` 相同的优先级——**先看 `compression_error()`**；
仅「越过列表上限但块已完整解码」时才忽略（该情况表仍同步，且没有可 reset 的
活流）。

### P1 —— 修复后

```
[P1] dispatched=1 goaway=1 goaway_error=0x9 rst3=0 rst3_error=0x0 written=82 bytes
[P1] OK: malformed discard-block is a connection-level COMPRESSION_ERROR
```

### P2 —— 修复前（bug 复现）

传输 double 让所有写失败：`serve_loops()` 先挂起读、再发起 SETTINGS 写；
写完成返回 `connection_reset` → `finish_connection()`（此时挂起的读未被取消）
→ `async_shutdown` → `close()` → `on_done()`。随后用对端字节完成那次读：

```
[P2] after teardown: done=1 dispatched=0 pending_read=1 closed=1
[P2] after late read: dispatched=1 (handler runs after on_done=1)
[P2] BUG REPRODUCED: a handler was dispatched on a connection that already finished teardown
```

### P2 —— 修复

- `h2_engine.h:995` `read_loop()` 的读完成回调：进入时先判 `m_finished`，
  是则直接丢弃本次读，不 append、不 parse、不重启读链。
- `h2_engine.h:1193` `parse_available()`：`m_finished` 后返回 false（防御
  `on_body_consumed()` 等其它入口）。

### P2 —— 修复后

```
[P2] after teardown: done=1 dispatched=0 pending_read=1 closed=1
[P2] after late read: dispatched=0 (handler runs after on_done=1)
[P2] OK: no handler dispatched after teardown
```

### 与 Go `x/net/http2` 的运行时差分（P1）

脚本：`review/h2-repro/run-go-diff.sh`（用 GOPATH 模式把只读的 module cache
软链进 GOPATH，编译 `golang.org/x/net@v0.56.0` 的真实 `http2.Server`）。它把
C++ 复现导出的同一份字节喂给 Go 服务端（`BaseConfig.MaxHeaderBytes = 64 KiB`）：

```
[go] malformed : goaway=true code=0x9 status="" streamErr="" notes=[SETTINGS SETTINGS]
[go] wellformed: goaway=false code=0x0 status="" streamErr="RST_STREAM(stream=1, code=0x1)" notes=[SETTINGS SETTINGS]
[go] OK: malformed-after-cap is a connection-level COMPRESSION_ERROR;
[go]     crossing the list cap on a well-formed block is not
```

即真实 Go 服务端的行为与修复后的 C++ 完全一致：

- 「越过 64 KiB 解码列表上限 + 畸形表示」→ **连接级 `GOAWAY(0x9)`**；
- 「仅越过上限、块本身良构」→ **不发 GOAWAY**，只做流级处理
  （本例因缺少伪头先触发 `RST_STREAM(PROTOCOL_ERROR)`；若能走到列表判定则是
  431，见 `server.go` 的 `f.Truncated` 分支）。

Go 源码对应位置（`golang.org/x/net@v0.56.0/http2`）：

- `frame.go:1729-1795`：`ReadMetaHeaders` 在解码时按 `hf.Size()` 扣减
  `remainSize`，超限只置 `MetaHeadersFrame.Truncated = true` 并关闭 emit，
  **继续解码**以保持动态表同步；
- `frame.go:1794`：`hdec.Write(frag)` 返回任何 error → 直接
  `ConnectionError(ErrCodeCompression)`——不会被前面的 `Truncated` 吞掉；
- `server.go:1972-1974`：`f.Truncated` 单独作为 HTTP 431 处理（流级）。

这正是 P1 修复所采用的分类顺序。

### 与 Go `x/net/http2` 的结构对照（P2）

P2 不是 RFC 分类问题，而是生命周期不变量：teardown 开始后不得再创建/派发流。
Go 的对应结构是显式的：

- `server.go:696-710` `readFrames()` 每次交付/等待帧时都 `select` 到
  `<-sc.doneServing` 并直接 `return`；
- `server.go:772` `serve()` 退出时 `close(sc.doneServing)`。

因此 Go 的连接一旦进入结束流程，帧读取 goroutine 不再把任何帧交给 serve
循环，也就不可能出现「teardown 后新建流并派发 handler」。本修复在
`read_loop()` 的读完成回调与 `parse_available()` 上加的同一条 `m_finished`
门控，对应同一个不变量。

### 回归验证

- `review/h2-repro/build.sh && bin/repro p1 && bin/repro p2` → 均 exit 0。
- **最终态**（含 clang-format 后的 header 重建）：
  - `xmake build -j6 test_unit` → `build ok`（exit 0）；
  - `build/linux/x86_64/release/test_unit '[h2]'` →
    **All tests passed (272 assertions in 45 test cases)**，包含新增的两个用例；
  - 全量 `build/linux/x86_64/release/test_unit` →
    **All tests passed (66191 assertions in 568 test cases)**。
- `review/h2-repro/run-go-diff.sh` → exit 0（见上）。
- 格式：`clang-format --dry-run --Werror` 对改动文件通过。

## 第二轮修复（剩余 P2 + P3）

提交 `47e5b39`（client 镜像 / 重复 GOAWAY / limits 校验）与本提交（P3 项）。

- **client P1/P2 镜像**（`client/h2_client.h`）：
  `decode_discarded_header_block()` 改为先判 `compression_error()`；read 完成回调与
  `parse_available()` 增加 `m_finished` 门控。
- **重复 GOAWAY**（`finish_connection()`）：删除补发逻辑。`go_away()` 已把 GOAWAY
  放进 `m_out`，最终写必然带上它；原补发会产生第二份，且可能把 last-stream-id
  增大（RFC 9113 §6.8 禁止）。
- **SETTINGS 参数校验**（`core/limits.h` + `net/server.h`）：
  `EngineLimits::h2_settings_valid()` 检查
  `h2_max_frame_size ∈ [2^14, 2^24-1]`、`h2_initial_window >= 0`；`Server::start()`
  在绑定前以 `invalid_config` 拒绝。
- **body_discarded 收尾**：handler 提前结束后清空停放帧；请求 END_STREAM 到达时
  `body().fail(operation_aborted)`，分离读者不会把被丢弃的尾部当成干净 EOF。
- **`:authority` / CONNECT**：非 CONNECT 请求必须带 `:authority` 或 `Host`；
  plain CONNECT 必须 authority-form（拒绝 `:scheme`/`:path`，要求非空
  `:authority`）；extended CONNECT 同样要求非空 `:authority`。
- **响应写入顺序**：`Http2ResponseSink` 的
  `send_chunk`/`send_last`/`send_trailers` 在未提交最终 HEADERS 时返回
  `invalid_argument`，避免先发 DATA 造成连接级协议错误。
- **h2c 重放过滤**：`run_h2c()` 移除 `connection`/`keep-alive`/
  `proxy-connection`/`transfer-encoding`/`upgrade`/`http2-settings`。
- **pump_dispatch 递归**：加 `m_pumping` 门控，改为外层循环排空队列。
- **discard 路径头块上限**：`decode_discarded_header_block()` 对单帧块也执行
  `max_header_bytes` 上限（`ENHANCE_YOUR_CALM`）。
- 杂项：`h2_engine.h` 增加 `<algorithm>`，删除未使用的 `<random>`/`<thread>`。

新增回归用例：

- `h2/engine: a connection error emits exactly one GOAWAY`
- `review: h2 client treats a malformed discarded HPACK block as a compression error`
- `server: invalid h2 SETTINGS limits are rejected before binding`
- `core/limits: h2 SETTINGS parameters are range-checked`
- `h2/engine: a request without :authority or Host is malformed`
- `h2/engine: a Host field satisfies the authority requirement`
- `h2/engine: a plain CONNECT must be authority-form`
- `h2/engine: a plain CONNECT without :authority is malformed`
- `h2/engine: a body write without a response head is rejected`
- `h2/engine: the h2c replayed request drops HTTP/1.1-only fields`

第二轮验证（最终态重建）：

- `test_unit '[h2]'` → **295 assertions in 52 test cases passed**；
- 全量 `test_unit` → **66229 assertions in 578 test cases passed**；
- `./format.sh --check` → 176 files properly formatted。

## 变更文件汇总

- `include/simple_http/engine/h2/h2_engine.h`：P1/P2 服务端、重复 GOAWAY、P3 各项；
- `include/simple_http/client/h2_client.h`：P1/P2 客户端镜像；
- `include/simple_http/core/limits.h`、`include/simple_http/net/server.h`：
  SETTINGS 参数校验；
- `test/unit/test_h2_engine.cpp`、`test/unit/test_review_regressions.cpp`、
  `test/unit/test_server_lifecycle.cpp`、`test/unit/test_core.cpp`：10 个回归用例；
- `review/h2-repro/`：P1/P2 最小复现、HPACK 块导出、Go `x/net/http2` 运行时差分
  （构建产物未入库，运行 `build.sh` 生成 `bin/repro`）。

## 仍未处理（记录在案）

- `:protocol` 为非 `websocket` 的 extended CONNECT 仍按普通 CONNECT 派发（应用可
  405/404；RFC 8441 建议 501，需要引擎在派发前合成响应）。
- `ws_inbox` 的 `erase(0, n)` 仍是 O(n) FIFO（纯性能项，建议游标 + compaction）。
- 两个仅在异常路径可达、未加独立回归用例的场景：body_discarded 的分离读者、
  `pump_dispatch` 的深递归（前者已按 `operation_aborted` 语义修复，后者已加门控）。
