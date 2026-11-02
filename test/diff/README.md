# Differential suites

Two independent comparisons, both against Go's standard library:

| 目录 | 方向 | 结果 |
|---|---|---|
| `client/` | C++ client ↔ Go `net/http` client → **同一个** Go 参考服务端 | 客户端行为差异 |
| `server/` | Go client → C++ 参考服务端 ↔ Go 参考服务端 | 服务端响应差异 |
| `openapi/` | C++ petshop 文档 ↔ 真实 HTTP | 文档结构、契约、UI |

## 布局

```
diff/
├── known_diffs.json            # 合理的已知差异登记（未登记 = 失败）
├── support/
│   └── harness.py              # 进程/端口/JSONL/归一化/差分/报告
├── client/
│   ├── cases/{basic,body,policy,ws,h3}.json
│   ├── cpp/driver.cpp          # 一个源文件，按 DIFF_GROUP_* 编成多个二进制
│   ├── go/refserver/main.go    # 同一个 Go 参考服务端（h1/tls/h2c/h3/ws）
│   ├── go/driver/main.go       # Go 客户端探针
│   └── run.py
├── server/
│   ├── cpp/server.cpp          # C++ 参考服务端（统一路由表）
│   ├── go/server/main.go       # Go 参考服务端（同一路由表）
│   ├── go/probes/main.go       # 一个源文件，按 group 编成多个 Go 探针
│   ├── fixtures/static/        # 静态文件夹具
│   └── run.py
└── openapi/
    ├── demo_server.cpp
    ├── schema/openapi-3.1.json
    ├── verify.py
    └── run.py
```

## 可执行程序

| 二进制 | 构建者 | 说明 |
|---|---|---|
| `diff_client_{basic,body,policy,ws,h3}` | xmake | C++ 客户端，按 `DIFF_GROUP_*` 区分 |
| `diff_server_cpp` | xmake | C++ 参考服务端 |
| `diff_openapi_server` | xmake | OpenAPI petshop |
| `client_refserver` / `go_probe_client_*` | `client/run.py` `go build` | Go 参考服务端与客户端探针 |
| `server_refserver` / `server_probe_*` | `server/run.py` `go build` | Go 参考服务端与 Go 客户端探针 |

每个功能组一个可执行程序；失败时只影响该组，且不需要构建其他组。

## 客户端差分

参考服务端 `client_refserver` 监听：

- `7810` 明文 h1 + `ws://…/ws`
- `7811` TLS h2/h1（ALPN）
- `7812` h2c
- `7813` UDP h3

端点都是确定性的：`/echo` 回显 method/path/query/headers/body/trailers；`/redirect/*`、
`/redirect-chain/*`、`/cookies/*`、`/compress/gzip`、`/retry/*`、`/trailers`、`/sse`、
`/multipart`、`/expect-*`、`/request-trailer`、`/jwks`、`/large`、`/version`。

C++ 与 Go 驱动读同一份 `cases/*.json`：

```json
{
  "id": "redirect.307.post",
  "client": {"version": "h1", "max_redirects": 5},
  "request": {"method": "POST", "path": "/redirect/307",
              "body": {"kind": "text", "value": "payload"}},
  "expect": {"status": 200, "body_mode": "json"}
}
```

无法声明式的用例（流式 multipart、文件上传、100-continue、trailers、SSE、WS）
写在各自驱动的 `main` 里，输出同样的 JSONL。

## 服务端差分

`diff_server_cpp` 监听 `7820`（嗅探）、`7821`（h2c）、`7822`（TLS）、`7823`（tcp+udp h3）；
Go 参考服务端监听 `7830`–`7833`。两者注册同一套可观测行为：

- 路由：精确 / `{param}` / `{*rest}` / 正则 / host / fallback / 405+Allow / 自动 OPTIONS / HEAD
- 请求：JSON / form / multipart / chunked / trailers / 100-continue / 1 MiB body cap
- 语义：状态、header、framing、keep-alive、重定向、gzip、响应 trailers、h1/h2/h3
- 中间件：CORS、security headers、Basic Auth、request-id、限流
- 框架层：State、extractor、validation、Session、CSRF、JWT、Host
- 静态文件：ETag/304、Range/206、SPA fallback、预压缩 `.gz`
- WebSocket

Go 探针按 group 构建：`routing`、`body`、`semantics`、`middleware`、`framework`、
`static`、`ws`。

## 观测与归一化

一行 JSON（JSONL）：

```json
{"case":"...","target":"cpp","status":200,"version":"HTTP/1.1",
 "headers":{"content-type":["text/plain"]},"body":"...","body_encoding":"utf8",
 "framing":"content-length","error":null,"notes":{}}
```

`harness.py` 的规则：

- header 名小写；多值保留数组；`allow`/`vary`/`cache-control` 拆成集合后排序。
- 默认忽略 `date`、`server`、`content-length`、`transfer-encoding`、`connection`、
  `etag`、`last-modified`、`set-cookie` 等传输/随机字段。
- body：`exact` / `json`（解析后比较，支持 `parse_json_strings`）/ `none`。
- `scrub` 正则用于把服务端端口等夹具差异替换成占位符。
- 错误只比较语义类别（`timeout` / `connect` / `body_too_large` / ...），不比较平台错误串。
- 不比较耗时、进程号、Date。

## 已知差异

`known_diffs.json`：

```json
{
  "client": {
    "case.or.pattern": {
      "fields": ["headers.content-encoding"],
      "verdict": "acceptable",
      "reason": "RFC ×××；Go 无对应能力，simple_http 是扩展"
    }
  },
  "server": {}
}
```

- 未登记差异 → 失败。
- 已登记 → `KNOWN`，打印原因，不失败。
- 每次运行写 `diff-report.md`，即行为差异分析报告。

## 新增一个功能组

1. C++ 客户端/服务端实现功能；Go 参考实现写出等价行为（或标 `cpp-only`）。
2. 客户端组：在 `client/cases/<group>.json` 加用例，并在 `xmake.lua` 的
   `{"basic","body","policy","ws","h3"}` 列表加组名（重编一个二进制）。
3. 服务端组：在 `server/go/probes/main.go` 的 switch 与 `ALL_GROUPS`（`run.py`）加组；
   在 `server/cpp/server.cpp` 与 `server/go/server/main.go` 加同名路由。
4. 在 `client/run.py` / `server/run.py` 的组列表与 `CASE_OPTIONS` 中登记比较策略。
5. 跑 suite，分析每一条未登记差异；确属双方合法实现选择才写入 `known_diffs.json`。
