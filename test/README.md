# simple_http 测试体系

唯一入口是 `test/run.sh`（内部是 `test/run.py`）。xmake 只负责构建测试二进制；
不再用 xmake phony target 编排套件。

```sh
test/run.sh fast        # 纯逻辑 + 进程内 C++，不需要 venv
test/run.sh standard    # + 独立 Python 实现（需要 test/.venv）
test/run.sh full        # + Go 差分 + 外部一致性/压力（工具齐全时）
test/run.sh list
test/run.sh doctor
test/run.sh <suite> [...]           # 单套件，例如 unit / server-conformance
```

Catch2 过滤参数直接跟在套件名后：

```sh
test/run.sh unit '[h2]'
# 或
test/run.sh unit --catch-filter '[h2]'
```

## 目录

| 目录 | 职责 |
|---|---|
| `unit/` | Catch2 纯逻辑测试，无 socket |
| `integration/` | 进程内 C++ 集成：client 自检、原始 socket 回归、线程模型、h1 生命周期 |
| `server/` | **被测服务端** ← 独立客户端（httpx / hyper-h2 / websockets）。`fixtures/` 是被驱动的 C++ 服务端 |
| `client/` | **被测客户端** → 独立服务端。`probes/` 是 C++ 客户端程序，`peers/` 是 Python 独立服务端 |
| `differential/` | C++ 与 Go `net/http` 的双向差分 |
| `external/` | 依赖外部工具的长套件：h2spec / h1spec / Autobahn / h3spec、h2load / k6、TLS 基准 |
| `examples/` | 必须持续编译的文档示例与 OpenAPI demo |
| `tools/` | 手动工具：loadgen、公网烟测、code-server 反代夹具 |
| `support/` | Catch2 `main`、静态文件夹具、Python 报告模块 |
| `tls_certificates/` | 测试证书；路径 `./test/tls_certificates/...` 是外部契约 |

## 依赖安装

```sh
python3 -m venv test/.venv
test/.venv/bin/pip install -r test/requirements.txt
```

`test/run.sh doctor` 会逐个套件列出缺失的工具/包。缺失的套件在分层运行时显示
`SKIP` 并继续；显式指定套件时默认报错（`--allow-skip` 可放宽），`--strict-deps`
会把任何 SKIP 视为失败。runner 自身只用 Python 标准库，venv 缺失时 `fast` 仍可运行。

## 分层矩阵

| suite | tier | 被测对象 / 驱动 | 依赖 |
|---|---|---|---|
| `unit` | fast | Catch2 纯逻辑 | xmake |
| `server-regression` | fast | 原始 socket → 进程内服务端 | xmake |
| `client` | fast | 进程内 server + 原始 responder → 客户端 | xmake |
| `h1-lifetime` | fast | 引擎销毁后的 deferred body read | xmake |
| `readme` | fast | README 示例编译/运行 | xmake |
| `threading` | standard | 客户端多线程/拆卸 | xmake |
| `server-conformance` | standard | 独立客户端 → fixture server | httpx、h2、websockets |
| `h2-websocket` | standard | hyper-h2 RFC 8441 → fixture server | h2、hpack、hyperframe |
| `h2-window-leak` | standard | 原始 h2 帧 → fixture server | hpack、hyperframe |
| `client-cross` | standard | C++ client → 独立 Python server | websockets、aioquic |
| `client-checks` | standard | 专项 probe + 一对一 Python peer | h2 |
| `openapi` | standard | OpenAPI demo 端到端 | httpx |
| `framework-diff` | full | C++ framework server ↔ Go net/http | go、openssl、httpx、cryptography |
| `client-diff` | full | C++/Go client ↔ Go/C++ server | go |
| `external-conformance` | full | h2spec / h1spec / Autobahn / h3spec | h2spec、h3spec、docker、curl |
| `stress` | full | h2load / k6（C++，默认不进 conformance） | h2load、k6 |
| `bench` | manual | TLS 缓冲基准 | h2load |

`bench` 与 `probe_client_go` 不进入自动分层：前者耗时且非门禁，后者需要外部 Go
参考服务。

## 端口约定（不要改）

| 端口 | 用途 |
|---|---|
| 7788 | fixture server：明文嗅探 + WebSocket |
| 7789 | fixture server：TLS / mTLS |
| 7790 / 7791 / 7793 | 纯 h2c / 纯 h1 / 大窗口 h2c |
| 7792 | TCP + UDP 双传输（QUIC） |
| 7794 | builder 配置的纯 h1 监听 |
| 7797 | 非 mTLS 的 TLS h1 监听（TLS 基准） |
| 7795 | OpenAPI demo |
| 7798 / 7799 | framework 差分：C++ / Go |
| 7802–7804 | client 差分：Go server / Go baseline / C++ server |
| 7813、7822–7826 | 专项 check 的 Python peers |
| 27910–27912 | `test_client` 进程内监听 |
| 27920–27922 | client 交叉验证：WS / HTTP / HTTP3 |
| 7888–7892 | stress 的 Rust 目标（只在显式 `both` 时使用） |

## 报告与失败诊断

每次运行写到 `.cache/test-run/<timestamp>-<pid>/`（可用 `--report-dir` 覆盖）：

- `report.json`：每个 suite 的状态、耗时、check 计数、日志路径、SKIP 原因、诊断信息。
- `junit.xml`：聚合 JUnit；Catch2 套件保留真实的 testcase。
- `build.log`：xmake 构建输出。
- `<suite>.log`：该套件 stdout/stderr。
- `<suite>.json`：Python 套件写出的逐 check 记录。
- `<suite>.xml`：Catch2 JUnit 原始输出。

失败时 runner 自动打印日志尾部 40 行、日志路径以及相关端口的 `ss -ltnup` 占用行；
退出码：`0` 通过（允许 SKIP）、`1` 测试失败、`2` 用法错误、`3` 环境/构建错误。

## 常用命令

```sh
# 只跑快速层
test/run.sh fast

# 单个套件 + Catch2 过滤
test/run.sh unit '[h2]'
test/run.sh server-regression

# 标准层，指定报告目录
test/run.sh standard --report-dir /tmp/simple-http-report

# 外部套件（也可直接调用脚本；参数会透传给底层脚本）
test/run.sh external-conformance
test/run.sh external-conformance h2spec
test/external/conformance/run.sh h2spec
test/external/conformance/run.sh h1spec
test/external/stress/run.sh cpp h1 --no-conformance
test/external/bench/tls_bench.sh

# 差分
test/.venv/bin/python test/differential/run.py framework
test/.venv/bin/python test/differential/run.py client
```

自动化测试统一用 `cwd = 仓库根` 启动（证书是相对路径）；二进制路径由 runner 通过
`SIMPLE_HTTP_TEST_BIN_DIR` 注入，直接运行单个脚本时会自动探测
`build/linux/x86_64/{release,debug}`。
