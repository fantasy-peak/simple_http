# simple_http 全库代码审查报告

- 日期：2026-10-27
- 审查基线：`test` 分支 `91b8bad`（行号以该版本为准）
- 对象：`include/simple_http/` 全部 86 个头文件（约 36,210 行），含 core / proto / transport / engine(h1,h2,h3) / quic / client / handler / net / openapi
- 方法：4 路只读子代理分区通读 + 主审对关键结论逐行复核；**纯静态分析，未编译、未跑测试**
- 图例：✅ 主审逐行核对源码确认；○ 子代理给出且推理自洽，主审未逐行复核；⚠ 疑似刻意行为或需产品决策

## 摘要

共 15 条高危、约 55 条中危、若干低危。问题分布不在帧编解码正确性本身，而在：

1. **请求边界/响应边界的校验缺口**（h1 头部混合行尾、WS 反代 preamble、trailer CR/LF、二次响应）→ 走私/响应拆分；
2. **资源上限没有贯穿到 h2/h3**（`max_body_bytes` 只在 h1 生效；h3 无 Rapid Reset 上限）；
3. **连接/流状态机的收尾**（h2 `finish_pending` 提前 erase、h2/h3 流控信用泄漏、QUIC 关闭时序）；
4. **组合语义**（`group()` 对代理路由失效、CORS 谓词短路、CookieJar 首请求、客户端免费重拨）；
5. **互操作细节**（HPACK 超限后的动态表同步、permessage-deflate 8 位窗口、Accept-Encoding q）。

外部一致性基线（h2spec 146/146、h1spec 33/33、Autobahn 0 失败、h3spec ≥47/49）仍然有效；下列问题多数落在套件不覆盖的组合与时序上。

---

## 一、高危

### H1. h1 头部边界对混合行尾处理不一致 → 请求走私面 ✅
`engine/h1/h1_parser.h:187-205`、`:243-247`

`parse_head()` 先找 `\r\n\r\n`，找不到才回退 `\n\n`；同一缓冲里“先 `\n\n`、后 `\r\n\r\n`”时头部块跨过更早的空行，而 `parse_block()` 对块内空行 `continue`。`Host: a\r\n\nX: y\r\n\r\n` 在本库得到 `{host:a, x:y}`；按 `\n\n` 定界的前端会把 `X: y` 当 body。前后端边界差分。

### H2. h1 WS 反代在校验后端 101 之前转发客户端余留字节 ✅
`engine/h1/ws_proxy.h:157-176`

`preamble.append(initial)` 后立即写给后端，`ws_proxy_read_reply()` 在其后才校验 101。客户端“握手 + 第二个请求”同包发送时，第二个请求被原样转给后端；后端拒绝时还会把其响应当作拒绝体回传，形成可读走私通道。h2 extended CONNECT 路径是 101/200 之后才发 initial，应与之对齐。

### H3. `finish_trailers(Headers)` 绕过 CR/LF/NUL 校验 → 响应拆分 ✅
`proto/response.h:238-244`，配合 `engine/h1/h1_engine.h` 的 `send_trailers`

`trailer(name,value)` 有 `contains_ctl` 检查，`finish_trailers(Headers)` 直接把调用方 `Headers` 交给 writer；h1 仅过滤 forbidden trailer 名后原样拼进 `0\r\n...`，h2 同理。值中含 `\n` 可伪造尾部字段/终止块。

### H4. h2/h3 完全不执行 `max_body_bytes` → 无界内存 ✅
`engine/h2/h2_engine.h` 的 `on_data`（约 1532-1556）、`engine/h3/h3_engine.h` 的 `on_data`（约 873-887）、`proto/body.h:112` 的 `read_all()`

全仓 grep：该上限只在 `h1_engine.h` 生效。h2/h3 的 `read_all()/read_json_body()` 等会无限 `append`，且流控信用在消费后归还，可被持续大 body 打爆内存；同一请求在 h1 上 64 MiB 就 413。

### H5. `group()` 内注册的 `http_proxy` / `ws_proxy` 忽略前缀与组中间件 ✅
`handler/router.h:387-411`（直接 `emplace`），对照文档承诺 `:332-336`

`ws_proxy`/`ws_proxy_regex`/`http_proxy`/`http_proxy_regex` 既不 `scoped()` 也不拼 `m_scopes` 前缀。`group("/api", {auth_mw}, ...)` 内注册的代理实际挂到全局路径且 `auth_mw` 不执行——配置即鉴权漏洞。

### H6. 客户端“免费重拨”会重放已执行的非幂等请求 ✅
`client/http.h:669-678`

条件只有 `opened->reused && transport_failure(ec) && !stale_redialed`，不检查方法、不检查响应头是否已收到。keep-alive 上的 POST 已返回 200、读 body 时被 RST，会透明重发一次，服务端执行两次。注释“provably never arrived”不成立。

### H7. h2 `finish_pending` 的流被提前 erase → handler 永久阻塞 ✅
`engine/h2/h2_engine.h:1577-1583`、`:1804-1807`、`:1820-1827`、`:1700-1721`

channel 满时记 `finish_pending=true`；但 `maybe_complete_stream()` 只看 `half_closed_remote && end_stream_sent` 就 `erase_stream()`，既不检查 `finish_pending`，也不调用 `body().finish()/fail()`；`paused_frames` 随流销毁，END_STREAM 信号丢失。handler 先写响应、后读 body 时，`body().read()` 永远等不到 EOF。

### H8. handler 已开始响应后抛异常 → 写出第二个响应 ✅（h1；h2/h3 未逐行核）
`handler/handler.h:165-170`、`proto/response.h:140-172`、`engine/h1/h1_engine.h:112-135`

`handler` 的兜底无条件 `send(500)`，注释称“writer 会拒绝第二次 send”，但 `Response::send()` 只查 `m_field_rejected`，`m_started` 从不参与；h1 sink 只查连接级 `m_open`（仍为 true）。已 `begin()/send()` 后再抛异常，h1 会拼出第二个 `HTTP/1.1 500`，破坏 keep-alive/前置代理。

### H9. h3 没有 Rapid Reset 并发上限 ✅
`engine/h3/h3_engine.h:1166-1198`

`spawn_handler` 无 `m_active_handlers`、无 `pending_dispatch`、无并发上限（全文件 grep 无相关字段）；`connection.h:1051-1069` 的 `cb_stream_close` 立即归还流额度。反复“开流→HEADERS→RESET_STREAM”可无限累积 handler，正是 CVE-2023-44487 的形状；h2 已修而 h3 未修。

### H10. HPACK header-list 超限被当作流错误并中止解码 → 动态表失步 ○
`engine/h2/hpack_decode.h:265-274`、`:314-316`；`engine/h2/h2_engine.h:1250-1259`

`emit()` 失败直接 return，跳过该字段的 `dynamic_insert` 及其后续 incremental-indexed 字段；连接继续。对端编码器已插入条目，后续流按动态索引解析会静默命中错误条目（可影响 proxy 的 Host/鉴权判断）。应继续完整解码或关连接（COMPRESSION_ERROR）。

### H11. QUIC Retry token 校验用错 CID ○
`quic/endpoint.h:377-390`

`ngtcp2_crypto_verify_retry_token2` 的 dcid 参数应为客户端 Initial 的 DCID（Retry 包的 SCID），代码传的是 Initial 的 SCID。与 ngtcp2 `crypto/shared.c` 的 AAD 比对确认不一致。仅 `retry=true` 时触发，该路径整体不可用。

### H12. QUIC 通告全 0 的 stateless reset token ✅
`quic/ngtcp2_config.h:114-117`

`stateless_reset_token_present = 1`，但全仓无对 `stateless_reset_token` 的赋值（grep 仅见 present 与发送侧生成），transport params 里是 16 字节 0。RFC 9000 §10.3 要求不可预测；可伪造 stateless reset 杀连接。

### H13. h1 ExchangeGate 完成时内联唤醒下一个请求 → 跨请求响应混淆 ✅（代码路径；依赖 Asio 同 executor 内联 resume）
`client/h1_client.h:673`、`client/http_client.h:552`、`:270-283`

`finish_exchange()` → `gate->release(token)` → `wake.done()` 内联恢复排队者；后者 `reset_exchange()` 清掉 `m_head_parsed/m_buf/m_body_mode` 并发出自己的请求。前一个协程仍在 `exchange_read()` 的 `for(;;){ ensure_head(); if (m_head_parsed) break; }` 里，返回后看到 `m_head_parsed=false`，继续读并解析后一个请求的响应。共享 Client 的同源并发请求 + 204/304/HEAD/CL:0 即可触发。

### H14. h2 客户端在连接关闭时丢弃已收完但未读的响应体 ✅
`client/h2_client.h:763-777`

`read_loop` 因 EOF/错误返回后，`run_loops()` 对所有流无条件 `set(Disconnect, eof)` 并 `m_streams.clear()`；流只在 `remote_end && 队列空` 时才置 Eof。END_STREAM 已到、数据仍在 `in_queue` 时，对端正常关闭连接会把完整响应标记为失败并丢弃。

### H15. CookieJar 对每个 `send()` 的首个请求不生效 ✅
`client/http.h:704-726`、`:773-779`

`cookie_header()` 只在重定向后的下一跳调用；初始请求从不查 jar，只有响应后 `store()`。`post(url).send()` 收到 Set-Cookie 后再 `get(同源).send()` 不带 Cookie，与 `ClientConfig` 的 Go `CookieJar` 语义不符。

---

## 二、中危

### A. Server / 传输生命周期
- **h3 绕过 `m_active_requests`**：`net/server.h:1384-1393` 的 `make_h3_serve` 直连 `router->dispatch`，无计数/Guard；`shutdown_grace` 对 h3 不生效，`active_requests()` 偏低。✅
- **SO_REUSEPORT 无回退**：`net/server.h:1048-1056`、`1071-1084`、`1266-1281`；默认 `reuse_port=true` 时 probe 自身要求 SO_REUSEPORT 成功，`FanOut::Unavailable` 的“单 acceptor 继续”分支不可达。✅
- **TLS 端口接受 h2c Upgrade**：`net/connection.h:190-199` 未调 `allow_h2c_upgrade(false)`，`h1_engine.h:1267/1175-1183` 也不看 TLS；`TlsHttp::Http1Only` 端口发 `Upgrade: h2c` 会 101。○
- **TLS 从不发 `close_notify`**：`transport/transport.h:96-102` 直接 shutdown+close。○
- **IPv6 指定地址 bind 失败回退 `0.0.0.0`**：`net/server.h:1071-1084` 只判断 `v6==false && is_ipv6_host`。○
- **QUIC 停止跨线程**：`net/server.h:667-670` 从控制线程直接 `shutdown_connections()`，`quic/endpoint.h:235-242` 同步操作 ngtcp2/无锁 channel；TCP 路径专门 post 回连接 executor。○
- **`endpoint->close()` 在 `m_pool->stop()` 之后**：`net/server.h:676-681`，每次停服固定等 2 秒 `wait_for`。✅（代码顺序）
- **UNIX listener 无条件删除配置路径已有文件，stop() 不清理自己创建的 socket**：`net/server.h:1290-1291`、`:599-685`。✅
- **`idle_timeout=0` 语义矛盾**：`net/connection.h:111/160` 说“禁用”，`h1_engine.h:433` 直接 `now + 0` 立即超时。○
- **构造后 builder 追加的 listener 拿不到 main context**：`net/server.h:354-368`、`:546-580`、`:1073`。○

### B. HTTP/2 服务端
- **半关闭流上的 DATA 不归还对端连接信用、不发 WINDOW_UPDATE**：`engine/h2/h2_engine.h:1484`；只恢复本地镜像。✅
- **流控/长度错误路径泄漏连接信用**：`h2_engine.h:1488-1492`、`:1536-1543`；已扣的 `frame_len` 既不进 `recv_owed_conn` 也不归还。○
- **`REFUSED_STREAM` 未推进 `m_next_peer_stream_id`**：`h2_engine.h:1123-1133` 早于 `1153-1155`；在途 DATA/RST 会命中 idle 判定 → 整连接 GOAWAY。✅
- **`content-length` uint64→int64 回绕**：`h2_engine.h:1420-1430`；≥2^63 变负数后被当作“未声明”。○
- **trailing HEADERS 并入 `mutable_headers()` 且不过滤 forbidden 字段**：`h2_engine.h:1359-1376`；trailer 里的 `content-length`/`host` 会被 handler/proxy 看到。○
- **拆包的 trailing HEADERS 漏 END_STREAM 校验**：`h2_engine.h:1165-1170` 只覆盖首帧带 END_HEADERS 的情况，`:1213-1224` 的 CONTINUATION 路径没有同样检查。○
- **SETTINGS_INITIAL_WINDOW_SIZE 变更未 clamp 结果**：`h2_engine.h:703-713`。○
- **h2c 非法 SETTINGS 的 GOAWAY 发不出去**：`h2_engine.h:315-331`，未进入 serve_loops 就返回。○
- **idle/closed 流用单一 `m_next_peer_stream_id` 归类**：`h2_engine.h:1095-1104` 等；对 idle 流回 RST 违反 RFC 9113 §6.4。⚠ 注释声称刻意。

### C. HTTP/3 / QUIC
- **STOP_SENDING 关错方向**：`engine/h3/h3_engine.h:688-698` 调 `shutdown_stream_read`，应为 write。○
- **`Body::feed` 失败丢 DATA 不归还信用**：`h3_engine.h:873-887`；违反 nghttp3 “recv_data 后自行 extend”契约。○
- **出站背压用低水位**：`h3_engine.h:442-457` 用 256 KiB，1 MiB 高水位从未使用。○
- **非法 header 字符仍 dispatch handler**：`h3_engine.h:819-822`、`:844-871`；reset 发生在 handler 已跑之后。○
- **请求 trailer 回调未注册**：`engine/h3/h3_callbacks.h` 缺 `begin_trailers/recv_trailer/end_trailers`，请求 trailer 被静默丢弃。○
- **stateless reset token 派生不一致**：`quic/endpoint.h:607-627`、`connection.h:1132-1147`；注册回调丢弃 ngtcp2 给的 token，发送侧另用 secret 派生。○
- **广告支持迁移但未实现**：`connection.h:572-587`、`endpoint.h:318-344`；reuse_port 下新四元组落到别的 endpoint 会被 reset。⚠ 疑似已知取舍。
- **`ngtcp2_crypto` 析构空指针**：`quic/ngtcp2_crypto.h:77-95`；`get_ssl(nullptr)` 直接解引用。○

### D. HTTP/1 引擎
- **chunked trailer 读失败被当正常 EOF，残留字节用于复用**：`h1_engine.h:794-807`；`0\r\n` 后跟 ≥64KB 无换行 trailer 时 `drain_body()` 仍 true。✅
- **延迟读 body 会用到下一请求的引擎状态**：`h1_engine.h:646-653`、`:698-718`；注释声称支持保存 Request 后再读。○
- **HEAD 流式响应强制 `Connection: close`**：`h1_engine.h:165-172`；HEAD 本可复用。✅
- **204/304 的 `send_bodyless` 不清理 CL/TE**：`h1_engine.h:123-137`、`response.h:152`。✅
- **拉取 body 时不发 100 Continue**：`h1_engine.h:722-735`；只有反代路径调用 `send_continue()`。✅
- **handler 未写任何响应仍复用连接**：`h1_engine.h:679-687` 不检查 `started()`；可能把下一个请求的响应当作第一个。○
- **chunk-size 行接受裸 LF**：`h1_engine.h:976-983`，与 data 后严格 CRLF 校验不一致。⚠ 注释称刻意。
- **响应 `Connection` 头与 `keep_alive_out()` 脱节**：`h1_engine.h:338-359`、`:685`。○
- **`parse_uint` 上界检查下溢**：`h1_engine.h:867`（`max<d` 时 `max-d` 回绕）。○
- **WS 升级不校验方法/版本**：`h1_engine.h:1039-1047`；POST+HTTP/1.0 也回 101。○
- **`Expect: 100-continue` + 提前响应仍按 CL drain**：`h1_engine.h:677`、`:905-972`；客户端不发 body 时最长等 120s，复用时被吞。○

### E. proto / WebSocket / multipart / 压缩
- **WS 帧放行 RSV1|RSV2/RSV3**：`proto/ws_frame.h:182-186`；内层条件未排除 0x20/0x10。✅
- **客户端帧掩码用未播种 `std::rand()`**：`proto/websocket.h:231-233`；同仓库 `ws_client.h:66-73` 已改 random_device，帧掩码漏改。✅
- **permessage-deflate 回显 8 位窗口但 zlib 实际用 9**：`proto/ws_deflate.h:204-207`、`:258-261`；对端按 8 位解压会 `Z_DATA_ERROR`。○
- **Close 回复只在 `read()` 里发出**：`proto/websocket.h:349-373`、`:499-528`；纯写 handler 永不回应。○
- **multipart 双重剥 CRLF**：`proto/multipart.h:150` 的 `break_delim="\r\n"+delim` 已排除 framing CRLF，`:175-184` 又去掉内容自身尾部 CRLF。✅
- **multipart 不识别 quoted-string**：`proto/multipart.h:62-80`；`filename="a; name=evil"` 会截断并覆盖 name。○
- **gzip 多 member 流被截断**：`core/content_encoding.h:277-280` 遇 `Z_STREAM_END` 忽略 `avail_in`。○
- **编码器初始化失败静默发空 body**：`content_encoding.h:171-200`、`:363-372`；`ContentEncoder` 无 `failed()`，`make_encoder()` 仍返回非空。✅
- **zlib `size()` 截断为 `uInt`**：`content_encoding.h:115`、`:269`；单次 >4 GiB 静默丢尾部。○
- **`compress_all()` 注释的“失败回退输入副本”不成立**：`content_encoding.h:388-395`。○

### F. 客户端
- **h2 重复/说谎的 Content-Length**：`client/h2_client.h:733-750`；调用方自带 CL 未过滤，随后又追加一条。○
- **WS 握手 Origin 未校验**：`client/ws_client.h:98-100`；其余 header 有 `contains_ctl`，origin 直接拼接。○
- **显式 host 覆盖绕过 CR/LF 校验**：`client/h1_client.h:474-476`、`h2_client.h:721-723`。○
- **`request_timeout` 不是总预算**：`client/http.h:791-810`、`:634-640`；每块 `read()` 重新起满额定时器。○
- **相对 Location 解析不符 RFC 3986**：`client/http.h:849-857`；`?page=2`/`#frag`/大写 scheme 出错。○
- **跨源重定向剥离凭据只比较 host**：`client/http.h:767-771`；端口/协议变化仍带 Authorization。⚠ 与 Go 行为一致。
- **`send()` 把 head timeout 当整体 deadline，idle 丢弃**：`client/http.h:905-908`、`:366-371`。○
- **解压上限事后检查，`Stream::read_all(0)` 无上限**：`client/decompressing_stream.h:88-94`、`http.h:139`。○
- **h2 客户端 `idle_timeout=0` 立即关闭**：`h2_client.h:675`、`:825-835`；与 h1 的“0=不设”相反。✅
- **`Stream::abort()` 不释放 h1 gate**：`client/http.h:153-156`、`h1_client.h:1026-1054`；继续持有时同源请求永久排队。○

### G. handler / 静态文件 / 中间件
- **路由段 trie 不回溯**：`handler/router.h:1001-1048`；`/a/b/d`+`/a/{x}/c` 请求 `/a/b/c` 404。✅
- **静态表请求期按路径重开文件，跟随符号链接**：`core/static_table.h:185-195`、`:489-493`；与“扫描期消除 symlink/TOCTOU”的头注释矛盾。○
- **SPA fallback + Range 的 Content-Length 不一致**：`handler/static_files.h:333-351`；写全文件长度但只发 range 字节。○
- **静态大文件同步读盘阻塞 io_context**：`static_files.h:320` → `static_table.h:489-493`。○
- **send 后立刻 close，可能 RST 掉 400/405**：`static_files.h:183-185`、`:214-218`。○
- **index 用字符串后缀而非路径分量**：`static_table.h:404-415`；`myindex.html` 生成 `/my`。○
- **预压缩 sibling 挂到别名 key**：`static_table.h:426-451`；`/blog` 返回 `blog.gz` 字节。○
- **basic_auth 非常量时间比较**：`handler/builtin_middleware.h:198-200`。✅
- **`clean_path` 把解码出的 `%3F` 变 query 分隔符**：`builtin_middleware.h:377-379`、`core/url_path.h:142`；授权/限流看到缩短的 path；static 还会二次解码。○
- **CORS 预检缺 `Vary`**：`handler/cors.h:303-311`。○
- **recovery 不区分取消**：`builtin_middleware.h:178-181`；timeout 的 504 被 500 顶替。○
- **access_log 异常路径漏日志**：`builtin_middleware.h:87-131`。○
- **只配 `allow_origin_fn` 时放行任意 Origin**：`cors.h:179-193`；`allowed.empty()` 先返回 true。⚠ 头注释确说“空=任意”，属危险默认值而非实现错误。✅
- **`real_ip` 的 IPv6/CIDR/v4-mapped 细节**：`builtin_middleware.h`（子代理标待复核）。○
- **`compress` 的 `excluded_paths` 无段边界**：`/api/` 也排除 `/apix`。○

### H. 校验器 / 日期 / 传输
- **Accept-Encoding q 大小写敏感且子串匹配**：`core/accept_encoding.h:66-90`、`content_encoding.h:448-453`；`br;Q=0` 仍选 brotli。✅
- **`parse_range` 20 位数字无溢出检查**：`core/validators.h:108-118`；`bytes=18446744073709551616-` 回绕成 0。✅
- **`parse_http_date` 不校验字段范围/星期/GMT/尾部**：`core/http_date.h:191-249`；`timegm` 会把 2 月 31 日、99 时规范化成合法时间。✅
- **`etag_matches` 朴素逗号切分，If-Range 用弱比较**：`validators.h:61-72`。○
- **`TlsContext` 可移动但 ALPN 回调参数指向 `m_alpn`**：`transport/tls_context.h:133-153`；移动后悬垂（当前唯一构造点不移动，属潜伏 UAF）。○
- **`stream_write_seq` >8 buffer 在 release 下静默截断**：`transport/transport.h:76-88`。○
- **`set_cookie` 丢弃 `Max-Age=0`**：`proto/response.h:122-125`；无法删除 cookie。○
- **HEAD 响应完全跳过压缩**：`proto/compressing_writer.h:333-335`。○
- **forbidden trailer 清单不完整**：`core/http_field.h:157-163`。○

### I. OpenAPI
- **递归 DTO 无环检测 → 注册时栈溢出**：`openapi/openapi.h:110-135`；`struct Node{ vector<Node> kids; }` + `.request_body<Node>()`。✅
- **enum/map/array/variant/shared_ptr 未映射，含 enum 字段直接编译失败**：`openapi.h:117-135`。○
- **`spec_url` 未转义拼进 JS 字符串**：`openapi_doc.h:550-560`。○
- **`JsonOut::quote` 放行非 UTF-8 字节**：`openapi_doc.h:160-176`；生成非法 JSON。○
- **`.path_param<T>("name")` 拼写不校验**：`openapi.h:380-383`、`:448-450`。○
- **重复注册同一 (path,method) 输出重复 JSON key**：`openapi_doc.h:205-211`、`:317-323`。○
- **object schema 不输出 `required`；path 参数 required 默认 false**：子代理实测。○

---

## 三、低危 / 合规（摘录）

- WS 超限帧回 1002 而非 1009；延迟 Close 可能永远发不出；并发 `close()` 会取消正在发送的 Close。
- `write_view` 在 hop 前读取非原子压缩开关；`Request` 移动后 `m_params` 悬垂；`clone_for_replay` 丢 CL/Version/state。
- HTTP/2 `GOAWAY` payload < 8 未判 FRAME_SIZE_ERROR；对 idle 流发 RST（见 B9）。
- range-unit 大小写敏感；`File` 后缀匹配；`params.h` 的 `optional<T>` 缺失即失败。
- `io_pool.at()` 越界只有 assert；`main_context()` 在未添加 main 时静默返回 worker。
- 日志超长消息格式化两遍；`logging_json` 的转义细节。
- OpenAPI 未知字段运行时报 400 但文档说允许；重复状态码渲染成重复 key。

---

## 四、建议修复顺序

1. **安全边界（先修）**：H1 → H2 → H3 → H8 → H5（组鉴权失效）→ H15（会话丢失）。
2. **资源上限与状态机**：H4（h2/h3 body cap）→ H9（h3 Rapid Reset）→ H7（h2 finish_pending）→ H10（HPACK 同步）。
3. **QUIC（若启用）**：H11、H12 → C6/C7/A6/A7。
4. **客户端正确性**：H13（gate 重入）→ H14（h2 断连丢 body）→ H6（免费重拨）。
5. **互操作/中危**：deflate 8 位窗口、Accept-Encoding q、multipart CRLF、HPACK、静态文件 symlink、OpenAPI 递归。
6. 低危及“疑似刻意”项按产品语义决定，不建议盲改。

## 五、覆盖与局限

- 已覆盖：core、proto（含 WS）、transport、h1、h2/HPACK、h3/QUIC、handler 中间件、router/反代、net/server、client 核心与 h1/h2、OpenAPI。
- hpack_huffman.h 仅用脚本核对表尺寸（encode 257、decode [256][16]=4096）与索引边界，未逐行审算法。
- QUIC 的 recvmmsg/GSO 原生路径只做静态阅读，未实机验证；h3 默认未编入。
- 纯静态分析，未编译、未跑一致性/压力测试；标记 ○ 的条目建议在修复前先写最小复现。
- 与本轮并行停止的另一个审查会话的报告有重叠；其独有的高危（H2/H3/H7/H8/H9/H11 等）已在此逐条复核后并入。
