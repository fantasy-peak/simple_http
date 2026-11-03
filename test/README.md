# simple_http 测试体系

唯一入口是 `test/run.sh`（内部 `test/run.py`）。xmake 只构建 C++ 测试二进制，
Go 探针由各差分 runner 自行 `go build`。

```sh
test/run.sh fast        # unit
test/run.sh standard    # unit + openapi
test/run.sh full        # + client-diff + server-diff + conformance + stress
test/run.sh list
test/run.sh doctor
test/run.sh <suite> [...]          # 单套件，例如 unit / client-diff
test/run.sh unit '[h2]'            # Catch2 过滤参数跟在套件名后
```

## 目录

| 目录 | 职责 |
|---|---|
| `unit/` | Catch2 纯逻辑测试（保留原有 401 个用例） |
| `fixtures/conformance_server.cpp` | 压力/合规脚本专用的极简 server |
| `external/conformance/` | h2spec / h1spec / Autobahn / h3spec |
| `external/stress/` | h2load / k6 负载与连接复用门线 |
| `diff/client/` | C++ 客户端 ↔ Go 客户端，同一个 Go 参考服务端 |
| `diff/server/` | Go 客户端 → C++ 服务端 ↔ Go 服务端 |
| `diff/openapi/` | OAS 3.1 元模式 + 文档/实际路由契约 + Swagger UI |
| `support/` | Catch2 `main`、报告模块、静态文件夹具 |
| `tls_certificates/` | 测试证书；`./test/tls_certificates/...` 是相对路径契约 |

## 套件矩阵

| suite | tier | 被测对象 | 依赖 |
|---|---|---|---|
| `unit` | fast | Catch2 纯逻辑 | xmake |
| `openapi` | standard | petshop 文档 + 真实 HTTP 契约 | httpx、jsonschema |
| `client-diff` | full | C++ / Go 客户端 → 同一个 Go server | go |
| `server-diff` | full | Go client → C++ server / Go server | go |
| `conformance` | full | h2spec / h1spec / Autobahn / h3spec | h2spec、h3spec、docker、curl |
| `stress` | full | h2load / k6 | h2load、k6 |

`fast` = `unit`；`standard` = `fast` + `openapi`；`full` = 全部。缺失依赖默认
打印 `SKIP`，`--strict-deps` 会把 SKIP 当作失败。

并行执行：`--parallel N`（默认 3）。`conformance` 与 `stress` 共用 7788–7792，
因此固定放在同一条串行 lane；`unit` / `openapi` / `client-diff` / `server-diff` /
`concurrency` 使用互不重叠的端口（concurrency 用临时端口），可以并行：

```sh
test/run.sh full --parallel 4
test/run.sh full --parallel 1   # 完全串行，便于定位
```

编译并行度由 `--jobs` 控制；sanitizer 默认 7。

## 差分协议

每个探针输出 JSONL，一行一个观测：

```json
{"case":"basic.get_echo","target":"cpp","status":200,"version":"HTTP/1.1",
 "headers":{"content-type":["application/json"]},"body":"...","body_encoding":"utf8",
 "framing":"content-length","error":null}
```

- **client-diff**：C++ 与 Go 客户端读同一份 `diff/client/cases/*.json`，请求同一个
  Go 参考服务端；服务端回显 method/header/body，因此差异只可能来自客户端。
- **server-diff**：同一个 Go 探针把相同请求分别发给 C++ 与 Go 参考服务端，
  每个案例输出 `target=cpp` 与 `target=go` 两行；runner 按 case 配对。`h2raw`
  组是 C++ 单侧原始 HTTP/2 断言（extended CONNECT、Rapid Reset），由
  `diff/server/go/h2raw` 输出 PASS/FAIL，不参与双端差分。
- **归一化**（`diff/support/harness.py`）：header 名小写、`allow`/`vary`/`cache-control`
  按集合比较；默认忽略 `date`/`server`/`content-length`/`transfer-encoding`/
  `connection`/`etag`/`last-modified`/`set-cookie` 等传输细节；body 支持
  `exact` / `json` / `none`；可配置 `scrub` 正则（例如把服务端端口替换成占位符）。
- **已知差异**：`diff/known_diffs.json`。未登记的差异直接失败；登记的差异打印
  `KNOWN` 并附带理由。每次运行写 `diff-report.md`，逐字段列出 cpp/go 值与判定。
- **用例模式**：`compare`（默认）、`cpp-only` / `go-only`（仅一侧有能力，
  另一侧不做差分）、`assert`（单侧断言）。

## 新增功能时

1. 在 C++ 实现功能；若 Go 有对应能力，在参考实现里写出等价行为。
2. 在对应功能组的 `cases/*.json` 或用例表中加用例；客户端与服务端尽量读同一份数据。
3. Go 没有对应能力时标 `cpp-only` 并写期望值；不要为了凑差分而伪造 Go 行为。
4. 双方都合法但实现不同的差异登记到 `known_diffs.json`，写清 RFC 依据。
5. 跑 `test/run.sh client-diff` / `server-diff`，检查 `diff-report.md`。

详细布局与扩展点见 `test/diff/README.md`。

## 端口

| 端口 | 用途 |
|---|---|
| 7788 / 7790 / 7791 / 7792 | conformance/stress fixture（嗅探+ws / 纯 h2c / 纯 h1 / tcp+udp h3） |
| 7810–7813 | client-diff 的 Go 参考服务端（h1 / TLS h2 / h2c / h3） |
| 7814 | client-diff 的 mTLS 服务端（需要客户端证书） |
| 7815 | client-diff 的第二 origin（跨 origin 重定向） |
| 7816 / 7817 | client-diff 的 HTTP CONNECT / SOCKS5 代理 |
| 7820–7824 | server-diff 的 C++ 服务端（嗅探 / h2c / TLS / tcp+udp h3 / 纯 h1） |
| 7830–7833 | server-diff 的 Go 服务端 |
| 7795 | OpenAPI petshop |

端口是脚本契约，不要改。

## 环境

```sh
python3 -m venv test/.venv
test/.venv/bin/pip install -r test/requirements.txt

# Go 探针依赖（版本固定）
cd test/diff && go mod download
```

`test/run.sh doctor` 会列出每个套件缺失的工具/Python 包。

## Coverage

```sh
test/coverage.sh
COVERAGE_SUITES="unit server-diff" test/coverage.sh
COVERAGE_FAIL_UNDER=70 test/coverage.sh
```

用 `--coverage` 构建独立的 `build-cov`，跑选定的测试套件后由 `gcovr` 聚合
`include/simple_http` 的行/分支覆盖率；报告写到 `.cache/coverage/coverage.txt`
和 `coverage.html`。需要 `test/.venv` 中的 `gcovr`。脚本结束会恢复默认 xmake 配置。

## Fuzzing

```sh
test/fuzz/run.sh                         # GCC + ASan/UBSan，确定性变异
FUZZ_ITERATIONS=100000 test/fuzz/run.sh
test/fuzz/run.sh --libfuzzer --time 60   # clang-24 + libFuzzer
```

目标：`fuzz_h1`（请求头）、`fuzz_h2`（帧头/HPACK）、`fuzz_ws`（帧解析）、
`fuzz_multipart`（增量 multipart）。默认模式不需要 clang；`--libfuzzer` 使用
`/usr/lib/llvm-24`（回退 `clang++`），以 `-fsanitize=fuzzer,address,undefined`
构建。种子语料在 `test/fuzz/corpus/`，libFuzzer 的变异语料写到 `.cache/fuzz/`。

## Sanitizer

```sh
test/run.sh sanitize asan [suite...]     # ASan + LSan + UBSan
test/run.sh sanitize tsan [suite...]     # TSan
test/run.sh sanitize ubsan [suite...]    # 仅 UBSan
```

默认矩阵：

| sanitizer | suites |
|---|---|
| `asan` | `unit openapi client-diff server-diff concurrency` |
| `tsan` | `unit concurrency server-diff` |
| `ubsan` | `unit client-diff server-diff` |

- 默认 **`--jobs 7`**：ASan 编译 unit 时 `-j14` 会 OOM；内存足够才上调。
- 默认在 `.cache/sanitize/worktree-<san>` 建独立 worktree，不污染日常 `build/`；
  也可 `--build-dir build-asan` 复用已有目录（结束时恢复默认 xmake 配置）。
- `conformance` / `stress` 需显式指定；stress 自动缩减到 20000 请求/50 连接。
- 新增 `concurrency` 用例：多客户端、多线程、跨 executor 写、日志 sink 替换，
  专门给 TSan/ASan 抓共享状态与拆卸问题。
- 报告：`.cache/sanitize/<san>/<timestamp>/sanitize-report.{md,json}`。
- Go 参考服务端/探针不插桩；只检查 C++ 侧。

详见 `test/sanitizer/README.md`。

## 外部套件

conformance 与 stress 的端口、基线数字保持不变，只把被测 server 换成了
`fixture_conformance_server`（只含 `/`、`/world`、`ws /echo`、`ws /echo1k`）：

```sh
test/external/conformance/run.sh
test/external/conformance/run.sh h2spec
test/external/stress/run.sh cpp h3 --no-conformance
```

工具不在仓库里（许可证/体积），脚本会自检并打印准备命令。
