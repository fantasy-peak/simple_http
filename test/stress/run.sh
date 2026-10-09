#!/usr/bin/env bash
#
# 负载与连接生命周期套件 —— **每次改动传输层或流的生老病死之后跑一次**。
#
# 和 test/conformance/ 的分工：那边问「协议对不对」，而且每个用例一条干净的连接；
# 这边问「连接复用起来还对不对」。一致性套件天然看不见「同一条连接上第 N 个请求」
# 这一类问题——每个用例都从一条新连接开始，正好把生命周期整个跳过去。
#
# 截至写下这些行，它抓到过（一致的套件全都看不见）：
#   * HTTP/3 在「上一条流 retire 之后再开新流」时把新流**静默吞掉**：靠 id 高低猜
#     「这条流早 retire 了」，而帧的到达顺序并不是流被打开的顺序，于是承载 QPACK
#     动态表插入的 encoder 流（客户端 uni 流 6）被当成重传丢掉，引用动态表的字段段
#     永远 Blocked，请求无限期挂起——`-n 10 -c 1 -m 1` 只成 1 个、卡满 30 秒，
#     而 h1/h2 在同一台机器上一切正常。修在 quic/connection.h 的 peer_stream_retired。
#
# 用法（统一入口，平时只执行这一个脚本）：
#   test/stress/run.sh [cpp|rust|both] [h1|h2|h3|ws]... [--no-conformance]
#     cpp  = 压 simple_http（C++），端口 7788-7792
#     rust = 压 rust_http_server，       端口 7888-7892
#     both = 两个都压（默认）
#     不带档位参数 = 四档全跑。
#     --no-conformance = 跳过符合性套件，直接压（等价 RUN_CONFORMANCE=0）
#   例：test/stress/run.sh both h3        # 两实现都只跑 h3 档
#       test/stress/run.sh cpp --no-conformance   # 只压 C++，不跑符合性
#
# 脚本开头会**无条件清理所有遗留测试服务**（C++ release/debug server、rust_http_server、
# server_dbg），并验证 TCP+UDP 端口释放后才启动被测服务器——否则 SO_REUSEPORT 会把
# 新服务器与残留进程哈希分流，QUIC 连接表被劈开（症状：20 万请求 ~800 failed）。
#
# 内建「改动后必跑顺序」：跑到 cpp 目标时先执行 test/conformance/run.sh（符合性），
# 全绿才继续 cpp 压力测试；rust 目标（含 both 里的 rust 那一轮）不跑合规套件。
# `RUN_CONFORMANCE=0` 可跳过。平时改完代码跑本脚本一条龙即可。
#
# 工具矩阵（能用 k6 的都用 k6）：h1/ws 负载走 k6，复用门线与 h2c/h3 负载走 h2load
# （k6 不支持 h2c/h3，且自动重连表达不了单连接门线）。python 兜底已移除。
#
# 端口与 conformance 一致，别改：
#   h1 → 7791  纯 h1       h2 → 7790  纯 h2c       h3 → 7792  udp（双传输端点的 QUIC 侧）
# 7789 是 mTLS 端口，h2load 不带客户端证书，不要拿它做负载。
#
# 退出码：0 = 全部达标；1 = 有未达标，或环境不全（缺什么会打印出来）。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# 允许调用方用环境变量覆盖被测服务器二进制（兼容原 SERVER_BIN=... 用法）。
SERVER_BIN_ENV="${SERVER_BIN:-}"

# --- 目标实现配置（cpp / rust，由参数选定）--------------------------------
RUST_ROOT="${RUST_ROOT:-/root/github/rust_http_server}"
TARGET=""
CACHE=""
SERVER_BIN=""
PORT_WS=""; PORT_H2C=""; PORT_H1=""; PORT_H3=""
BUILD_SERVER=""

# ===== 测试参数配置（可随时改，改完直接跑）=====================================
# 请求/响应体规格（需求方指定）：请求 POST 1 KiB body，响应三档 2k/10k/20k。
# 复用门线用最小的那一档代表负载形态。
RESP_TIERS="2048 10240 20480"      # 响应体三档（字节）
# 负载档（h2load + k6）：
LOAD_THREADS=4                     # h2load -t：发压进程数
LOAD_REQUESTS=300000               # 每个负载档的总请求数
LOAD_CLIENTS=200                   # 客户端数/连接数（h2load -c；k6 的 VUS）
LOAD_STREAMS=20                    # 每连接并发流数（h2load -m；k6 h1 档不适用）
# WebSocket 档（打 /echo1k：请求 WS_MSG_SIZE 字节，服务端固定回 WS_RESP_SIZE 字节）：
WS_MSG_VUS=100                     # ws 客户端数（k6 VUS）
WS_MSG_ITERS=600                   # ws 连接数（k6 iterations，总消息数 = ITERS × WANT）
WS_MSG_WANT=2000                   # 每连接发送帧数
WS_MSG_SIZE=512                    # ws 请求包字节
WS_RESP_SIZE=1024                  # ws 响应包字节（两端 /echo1k 固定回这么多）
# 改动后必跑顺序（内建）：先符合性（simple_http 的 test/conformance/run.sh，仅 cpp
# 目标那一轮跑），全绿才继续压力测试。设 RUN_CONFORMANCE=0 可跳过符合性。
RUN_CONFORMANCE="${RUN_CONFORMANCE:-1}"
# ====================================================================

select_target() {
    case "$1" in
        cpp)
            TARGET=cpp
            CACHE="${ROOT}/.cache/stress"
            SERVER_BIN="${SERVER_BIN_ENV:-${ROOT}/build/linux/x86_64/release/server}"
            PORT_WS=7788; PORT_H2C=7790; PORT_H1=7791; PORT_H3=7792
            BUILD_SERVER="xmake build server"
            REQ_BODY="${CACHE}/req1024.bin"
            ;;
        rust)
            TARGET=rust
            CACHE="${RUST_ROOT}/.cache/stress"
            SERVER_BIN="${SERVER_BIN_ENV:-${RUST_ROOT}/target/release/rust_http_server}"
            PORT_WS=7888; PORT_H2C=7890; PORT_H1=7891; PORT_H3=7892
            BUILD_SERVER="cargo build --release --manifest-path ${RUST_ROOT}/Cargo.toml"
            REQ_BODY="${CACHE}/req1024.bin"
            ;;
        *)
            say "内部错误：未知目标 $1"
            return 1
            ;;
    esac
}

# ws 驱动：k6 是**唯一**驱动（python websockets 兜底已按需求移除）。k6 同时是
# h1 负载档的驱动（见 run_load_k6），所以 k6 是必装工具而不是可选项。
WS_DRIVER_ACTUAL="k6"

# 复用门线：一条连接上连发 200 个请求，必须 200/200。数字写死是有意的——这个用例
# 就是为「同一条连接的第 2 个请求」设计的，它失败必是连接生命周期出了问题，而不是
# 机器慢（机器慢只会让它更慢，不会让它失败）。
REUSE_N=200

SERVER_PID=""
FAILURES=0
SUITES=()

# --- 输出 ----------------------------------------------------------------

if [[ -t 1 ]]; then
    B=$'\033[1m'; G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; N=$'\033[0m'
else
    B=""; G=""; R=""; Y=""; N=""
fi

say()   { printf '%s\n' "$*"; }
head_() { printf '\n%s==> %s%s\n' "$B" "$*" "$N"; }
ok()    { printf '  %s✓%s %s\n' "$G" "$N" "$*"; }
bad()   { printf '  %s✗%s %s\n' "$R" "$N" "$*"; FAILURES=$((FAILURES + 1)); }
warn()  { printf '  %s!%s %s\n' "$Y" "$N" "$*"; }

# --- 环境自检 ------------------------------------------------------------
#
# 系统包里的 h2load 通常**没有编 ngtcp2/nghttp3**，压 h1/h2 没问题，压 h3 会退化
# 成 TCP TLS 然后 ALPN 谈崩（报 "No supported protocol was negotiated"）。所以
# h3 这一档不看版本号——那东西编不编都打印一样——而是在服务器起来之后**实测一发**。

require_h2load() {
    command -v h2load >/dev/null 2>&1 && return 0
    warn "缺 h2load（nghttp2 的负载工具）"
    say  "      apt install nghttp2-client   # 只够压 h1/h2；h3 需要自编，见下"
    return 1
}

require_ws_driver() {
    if command -v k6 >/dev/null 2>&1; then
        return 0
    fi
    warn "缺 k6（WebSocket 与 h1 负载的唯一驱动）"
    say  "      k6: 见 https://github.com/grafana/k6/releases（单二进制）"
    return 1
}

# h2load 支持 HTTP/3 才算环境齐备。自编的配方（ngtcp2 生态，OpenSSL 3.5 起带原生 QUIC API）：
#
#   nghttp3 v1.18.0 / ngtcp2 v1.25.0 → /opt/h3，再编 nghttp2 v1.70.0：
#   cmake -B build -DENABLE_HTTP3=ON -DENABLE_APP=ON \
#         -DCMAKE_PREFIX_PATH=/opt/h3 -DCMAKE_INSTALL_PREFIX=/opt/h3
#   （nghttp2 要求 ngtcp2 >= 1.23.0、nghttp3 >= 1.17.0；两个库都是 cmake，且
#     **必须先把 submodule 拉下来**，否则 lib 的目标会以「No SOURCES given」告败。）
#
# 这里只检查、不自动下载执行远程内容：那应该是使用者的显式决定。
probe_h3_support() {
    local out
    out="$("$H2LOAD" --alpn-list=h3 -n 1 -c 1 "https://127.0.0.1:${PORT_H3}/world" 2>&1)"
    if printf '%s' "$out" | grep -qE '^requests: 1 total, 1 started, 1 done, 1 succeeded'; then
        return 0
    fi
    warn "这份 h2load 压不了 HTTP/3（实测一发没谈成 h3）"
    say  "      自带的 h2load 一般没编 ngtcp2/nghttp3，需自行编译，见本脚本注释"
    say  "      实测输出：$(printf '%s' "$out" | grep -iE 'negotiat|error' | head -n 1)"
    return 1
}

# --- 服务器 --------------------------------------------------------------

start_server() {
    if [[ ! -x "$SERVER_BIN" ]]; then
        head_ "构建 server"
        ( cd "$ROOT" && eval "$BUILD_SERVER" ) || { bad "构建 server 失败：$BUILD_SERVER"; return 1; }
    fi
    head_ "启动被测服务器（目标 $TARGET，端口 ws=$PORT_WS h2c=$PORT_H2C h1=$PORT_H1 h3=$PORT_H3）"
    # 必须在仓库根目录起：证书路径是相对路径。
    ( cd "$ROOT" && exec "$SERVER_BIN" >"${CACHE}/server.log" 2>&1 ) &
    SERVER_PID=$!

    local port ready i
    for port in "$PORT_H1" "$PORT_H2C" "$PORT_H3" "$PORT_WS"; do
        ready=0
        for i in $(seq 1 50); do
            if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then ready=1; break; fi
            if ! kill -0 "$SERVER_PID" 2>/dev/null; then
                bad "服务器启动即退出，见 ${CACHE}/server.log"
                tail -n 5 "${CACHE}/server.log" | sed 's/^/      /'
                return 1
            fi
            sleep 0.1
        done
        [[ "$ready" == "1" ]] || { bad "端口 ${port} 一直没就绪"; return 1; }
    done
    ok "监听就绪"
}

stop_server() {
    [[ -z "$SERVER_PID" ]] && return 0
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
}

trap stop_server EXIT INT TERM

# --- 用例 ----------------------------------------------------------------

# h2load 的结果行解析。返回 "succeeded failed errored"。
h2load_counts() {
    local out="$1"
    printf '%s\n' "$out" | sed -n 's/^requests: [0-9]* total, [0-9]* started, [0-9]* done, \([0-9]*\) succeeded, \([0-9]*\) failed, \([0-9]*\) errored.*/\1 \2 \3/p' | tail -n 1
}

h2load_summary() { printf '%s\n' "$1" | grep -E '^finished' | tail -n 1; }
h2load_p99()     { printf '%s\n' "$1" | grep -E '^(request     :|time for request:)' | head -n 1 | awk '{print $7}'; }

# 复用门线：一条连接、串行发 REUSE_N 个请求。这是「连接生命周期」的最小复现，
# 也是 h3 那个静默吞流的 bug 唯一稳定复现的形态。
run_reuse() {
    local name="$1" port="$2"; shift 2
    local out counts succ failed errored
    out="$("$H2LOAD" "$@" -d "$REQ_BODY" -n "$REUSE_N" -c 1 -m 1 "http://127.0.0.1:${port}/world?n=${RESP_TIERS%% *}" 2>&1)" || true
    counts="$(h2load_counts "$out")"
    read -r succ failed errored <<<"$counts"
    if [[ -z "${succ:-}" ]]; then
        bad "${name} 复用：没解析到结果"
        printf '%s\n' "$out" | tail -n 5 | sed 's/^/      /'
        return 1
    fi
    if [[ "$succ" != "$REUSE_N" || "$failed" != "0" || "$errored" != "0" ]]; then
        bad "${name} 复用：${succ}/${REUSE_N} 成功，failed ${failed}，errored ${errored}"
        say  "      一条连接上连发 ${REUSE_N} 个请求必须全部成功——少一个就是连接生命周期出了问题"
        return 1
    fi
    ok "${name} 复用：${succ}/${REUSE_N}，0 失败"
}

# 同一形状的 h3 版本：alpn 走 h3，scheme 走 https。
run_reuse_h3() {
    local name="$1" port="$2"
    local out counts succ failed errored
    out="$("$H2LOAD" --alpn-list=h3 -d "$REQ_BODY" -n "$REUSE_N" -c 1 -m 1 "https://127.0.0.1:${port}/world?n=${RESP_TIERS%% *}" 2>&1)" || true
    counts="$(h2load_counts "$out")"
    read -r succ failed errored <<<"$counts"
    if [[ -z "${succ:-}" ]]; then
        bad "${name} 复用：没解析到结果"
        printf '%s\n' "$out" | tail -n 5 | sed 's/^/      /'
        return 1
    fi
    if [[ "$succ" != "$REUSE_N" || "$failed" != "0" || "$errored" != "0" ]]; then
        bad "${name} 复用：${succ}/${REUSE_N} 成功，failed ${failed}，errored ${errored}"
        return 1
    fi
    ok "${name} 复用：${succ}/${REUSE_N}，0 失败"
}

# 吞吐：**只报告，不做门线**。req/s 依赖机器（核数、内存带宽、回环实现），
# 写死一个数会在别的机器上误报；真正确定性的是「失败数为 0」，那是门线。
run_load() {
    local label="$1" port="$2"; shift 2
    local n out
    for n in $RESP_TIERS; do
        out="$("$H2LOAD" "$@" -d "$REQ_BODY" -t "$LOAD_THREADS" -n "$LOAD_REQUESTS" -c "$LOAD_CLIENTS" -m "$LOAD_STREAMS" \
            "http://127.0.0.1:${port}/world?n=${n}" 2>&1)" || true
        report_load "${label}@${n}B" "$out"
    done
}

run_load_h3() {
    local label="$1" port="$2"
    local n out
    for n in $RESP_TIERS; do
        out="$("$H2LOAD" --alpn-list=h3 -d "$REQ_BODY" -t "$LOAD_THREADS" -n "$LOAD_REQUESTS" -c "$LOAD_CLIENTS" -m "$LOAD_STREAMS" \
            "https://127.0.0.1:${port}/world?n=${n}" 2>&1)" || true
        report_load "${label}@${n}B" "$out"
    done
}

report_load() {
    local label="$1" out="$2" counts succ failed errored
    counts="$(h2load_counts "$out")"
    read -r succ failed errored <<<"$counts"
    if [[ -z "${succ:-}" ]]; then
        bad "${label} 负载：没解析到结果"
        printf '%s\n' "$out" | tail -n 5 | sed 's/^/      /'
        return 1
    fi
    say "  ${label}：$(h2load_summary "$out" | sed 's/^finished in //')，p99 $(h2load_p99 "$out")"
    if [[ "$failed" != "0" || "$errored" != "0" ]]; then
        bad "${label} 负载：failed ${failed}，errored ${errored}"
        return 1
    fi
    ok "${label} 负载：${succ} 个请求全成"
}

# HTTP/1.1 档用 k6 驱动（需求方要求能用 k6 的都用 k6）。k6 不支持 h2c/h3，
# 那两档仍走 h2load；复用门线也保留 h2load——k6 在连接断开时会自动重连，
# 表达不了「一条连接必须 200/200」的语义。
run_load_k6() {
    local label="$1" port="$2"
    local n out rps errors
    for n in $RESP_TIERS; do
        out="$(K6_URL="http://127.0.0.1:${port}/world?n=${n}" K6_VUS="$LOAD_CLIENTS" K6_ITERATIONS="$LOAD_REQUESTS" \
            k6 run --quiet "$ROOT/test/stress/k6_h1.js" 2>&1)" || true
        rps="$(printf '%s\n' "$out" | grep -a 'http_reqs' | sed -n 's/.* \([0-9.]*\)\/s.*/\1/p' | tail -n 1)"
        errors="$(printf '%s\n' "$out" | grep -a '^errors' | sed -n 's/.*: \([0-9]*\).*/\1/p' | tail -n 1)"
        if [[ -z "${rps:-}" ]]; then
            bad "${label}@${n}B：没解析到 k6 结果"
            printf '%s\n' "$out" | tail -n 6 | sed 's/^/      /'
            continue
        fi
        if [[ "${errors:-0}" != "0" ]]; then
            bad "${label}@${n}B：k6 errors ${errors}"
            continue
        fi
        ok "${label}@${n}B：${rps} req/s，0 错误"
    done
}

run_h1() {
    head_ "HTTP/1.1（:${PORT_H1}，纯 h1）"
    run_reuse h1 "$PORT_H1" --h1
    run_load_k6 h1 "$PORT_H1"
}

run_h2() {
    head_ "HTTP/2（:${PORT_H2C}，纯 h2c）"
    run_reuse h2c "$PORT_H2C"
    run_load  h2c "$PORT_H2C"
}

run_h3() {
    head_ "HTTP/3（udp :${PORT_H3}，双传输端点的 QUIC 侧）"
    probe_h3_support || return 1
    run_reuse_h3 h3 "$PORT_H3"
    run_load_h3  h3 "$PORT_H3"
}

# WebSocket 档：门线是「回显数==发送数 且 0 失败」。驱动固定 k6（python 兜底已移除）。
run_ws() {
    head_ "WebSocket echo1k（ws://127.0.0.1:${PORT_WS}/echo1k，请求 ${WS_MSG_SIZE}B → 响应 ${WS_RESP_SIZE}B，驱动 k6）"
    local out sent recv failed msgs
    out="$(WS_URL="ws://127.0.0.1:${PORT_WS}/echo1k" VUS="$WS_MSG_VUS" ITERS="$WS_MSG_ITERS" WANT="$WS_MSG_WANT" \
        SIZE="$WS_MSG_SIZE" k6 run --quiet "$ROOT/test/stress/k6_ws.js" 2>&1)"
    # "ws_messages...........: <count> <rate>/s"
    recv="$(printf '%s\n' "$out" | grep -a 'ws_messages' | tail -n 1 | sed -n 's/^.*ws_messages[. ]*: \([0-9]*\).*/\1/p')"
    msgs="$(printf '%s\n' "$out" | grep -a 'ws_messages' | tail -n 1 | sed -n 's/^.*: [0-9]* \([0-9.]*\)\/s.*/\1/p')"
    sent=$(( WS_MSG_ITERS * WS_MSG_WANT ))
    failed=$(( recv == sent ? 0 : 1 ))
    if [[ -z "${recv:-}" ]]; then
        bad "ws 回显：没解析到结果"
        printf '%s\n' "$out" | tail -n 5 | sed 's/^/      /'
        return 1
    fi
    say "  ws echo k6：${msgs} msgs/s"
    if [[ "$recv" != "$sent" || "$failed" != "0" ]]; then
        bad "ws 回显：recv ${recv}/${sent}"
        return 1
    fi
    ok "ws 回显：${recv}/${sent} 条全成，0 失败"
}

# --- 主流程 --------------------------------------------------------------

usage() {
    say "用法：$(basename "$0") [cpp|rust|both] [h1|h2|h3|ws]... [--no-conformance]"
    say "      cpp  = simple_http（C++，端口 7788-7792）"
    say "      rust = rust_http_server（端口 7888-7892）"
    say "      both = 两个都跑（默认）"
    say "      --no-conformance = 跳过符合性套件，只跑压力测试（等价 RUN_CONFORMANCE=0；"
    say "                         只对 cpp 轮有影响，rust 轮本就不跑符合性）"
    say "      不带档位参数 = 四档全跑。例：run.sh both h3 / run.sh cpp --no-conformance"
}

want() {
    [[ ${#SUITES[@]} -eq 0 ]] && return 0
    local s
    for s in "${SUITES[@]}"; do [[ "$s" == "$1" ]] && return 0; done
    return 1
}

# 强制清理（REQUIREMENTS.md 第 0 条铁律落到脚本里）：无论本次跑哪个目标，先把两个
# 实现 + conformance（debug）的遗留测试服务全部杀干净，并验证 TCP/UDP 端口释放。
# 不清理的后果：SO_REUSEPORT 让残留进程与新服务器同挂一个 UDP 端口、QUIC 连接被
# 哈希劈给两个进程（实测 20 万请求 ~800 failed）；7789 无 reuse_port 则直接 EADDRINUSE。
kill_all_test_servers() {
    head_ "清理遗留测试服务（强制，改动后必跑）"
    local pat='[r]elease/server|[d]ebug/server|[r]ust_http_server|[s]erver_dbg'
    local pids
    pids="$(ps ax -o pid,cmd | grep -E "$pat" | grep -v grep | awk '{print $1}')"
    if [[ -z "$pids" ]]; then
        ok "无遗留测试服务"
    else
        say "  发现遗留进程，先 SIGTERM（优雅退出是异步的，等 3 秒）再 SIGKILL 兜底：$(echo "$pids" | tr '\n' ' ')"
        kill $pids 2>/dev/null
        sleep 3
        for again in $(ps ax -o pid,cmd | grep -E "$pat" | grep -v grep | awk '{print $1}'); do
            kill -9 "$again" 2>/dev/null
        done
        sleep 1
    fi
    local busy
    busy="$(ss -uln -tln 2>/dev/null | grep -E ':(778|788)[0-9]' || true)"
    if [[ -n "$busy" ]]; then
        bad "端口仍被占用（TCP/UDP 7788-7792 或 7888-7892）："
        printf '%s\n' "$busy" | sed 's/^/      /'
        return 1
    fi
    ok "进程与端口均已释放"
}

run_target_suites() {
    local target="$1"
    select_target "$target" || return 1
    mkdir -p "$CACHE"
    # 需求方指定的请求体：POST 1 KiB。h2load -d 与 k6_h1.js 共用。
    head -c 1024 /dev/zero | tr '\0' 'x' > "$REQ_BODY"

    head_ "环境自检（目标 $target）"
    ENV_OK=1
    require_h2load || ENV_OK=0
    require_ws_driver || ENV_OK=0
    [[ "$ENV_OK" == "1" ]] && ok "齐备"
    if [[ "$ENV_OK" != "1" ]]; then
        say ""
        say "环境不全，未开始测试。按上面的提示准备后重跑。"
        return 1
    fi
    # 自编的 h2load 装到别处时，用 H2LOAD=... 指过来（配合 LD_LIBRARY_PATH）。
    H2LOAD="${H2LOAD:-/opt/h3/bin/h2load}"

    # 改动后必跑顺序：先符合性，再压力。符合性只测 C++ 实现，所以只在 cpp 这一轮
    # 跑——跑 both 时 rust 那轮完全不碰合规套件。$CACHE 此时已由 select_target 设好。
    if [[ "$RUN_CONFORMANCE" == "1" && "$target" == "cpp" ]]; then
        head_ "符合性测试（改动后必过，源：simple_http/test/conformance/run.sh）"
        local conf_rc
        ( cd "$ROOT" && test/conformance/run.sh ) 2>&1 | tee "${CACHE}/conformance.last.log"
        conf_rc=${PIPESTATUS[0]}
        if [[ "$conf_rc" != "0" ]]; then
            bad "符合性未达到基线（退出码 $conf_rc，详情见 ${CACHE}/conformance.last.log）；跳过可设 RUN_CONFORMANCE=0"
            exit 1
        fi
        ok "符合性通过（退出码 0）"
    fi

    start_server || return 1

    want h1 && run_h1
    want h2 && run_h2
    want h3 && run_h3
    want ws && run_ws

    stop_server
}

# 解析参数：目标（cpp/rust/both）+ 档位（h1/h2/h3/ws）
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        cpp|rust) TARGETS+=("$arg") ;;
        both|all) TARGETS=(cpp rust) ;;
        h1|h2|h3|ws) SUITES+=("$arg") ;;
        --no-conformance|--skip-conformance|--no-conformance-suite) RUN_CONFORMANCE=0 ;;
        *) say "未知参数：$arg"; usage; exit 2 ;;
    esac
done
[[ ${#TARGETS[@]} -eq 0 ]] && TARGETS=(cpp rust)   # 默认 both

kill_all_test_servers || exit 1

for t in "${TARGETS[@]}"; do
    head_ ">>>>>> 目标实现：$t <<<<<<"
    run_target_suites "$t"
done

head_ "汇总"
if [[ "$FAILURES" == "0" ]]; then
    say "  全部达标。"
    exit 0
fi
say "  ${FAILURES} 项未达标。"
exit 1
