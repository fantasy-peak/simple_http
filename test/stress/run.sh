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
# 用法：
#   test/stress/run.sh              # 全部跑
#   test/stress/run.sh h3           # 只跑指定的（h1 / h2 / h3）
#
# 端口与 conformance 一致，别改：
#   h1 → 7791  纯 h1       h2 → 7790  纯 h2c       h3 → 7792  udp（双传输端点的 QUIC 侧）
# 7789 是 mTLS 端口，h2load 不带客户端证书，不要拿它做负载。
#
# 退出码：0 = 全部达标；1 = 有未达标，或环境不全（缺什么会打印出来）。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CACHE="${ROOT}/.cache/stress"
# 默认用 debug 构建：这个套件看的是「连接还活不活得下去」，断言开着的价值高于跑得快。
SERVER_BIN="${SERVER_BIN:-${ROOT}/build/linux/x86_64/debug/server}"

PORT_H1=7791
PORT_H2C=7790
PORT_H3=7792

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
        ( cd "$ROOT" && xmake build server ) || { bad "xmake build server 失败"; return 1; }
    fi
    head_ "启动示例服务器（:7788 嗅探 / :7789 mTLS / :7790 h2c / :7791 h1 / :7792 双传输）"
    # 必须在仓库根目录起：证书路径是相对路径。
    ( cd "$ROOT" && exec "$SERVER_BIN" >"${CACHE}/server.log" 2>&1 ) &
    SERVER_PID=$!

    local port ready i
    for port in "$PORT_H1" "$PORT_H2C" "$PORT_H3"; do
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
    out="$("$H2LOAD" "$@" -n "$REUSE_N" -c 1 -m 1 "http://127.0.0.1:${port}/world" 2>&1)" || true
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
    out="$("$H2LOAD" --alpn-list=h3 -n "$REUSE_N" -c 1 -m 1 "https://127.0.0.1:${port}/world" 2>&1)" || true
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
    local out
    out="$("$H2LOAD" "$@" -t 4 -n 200000 -c 200 -m 20 "http://127.0.0.1:${port}/world" 2>&1)" || true
    report_load "$label" "$out"
}

run_load_h3() {
    local label="$1" port="$2"
    local out
    out="$("$H2LOAD" --alpn-list=h3 -t 4 -n 200000 -c 200 -m 20 "https://127.0.0.1:${port}/world" 2>&1)" || true
    report_load "$label" "$out"
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

run_h1() {
    head_ "HTTP/1.1（:${PORT_H1}，纯 h1）"
    run_reuse h1 "$PORT_H1" --h1
    run_load  h1 "$PORT_H1" --h1
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

# --- 主流程 --------------------------------------------------------------

usage() {
    say "用法：$(basename "$0") [h1|h2|h3]..."
    say "      不带参数 = 三档全跑。"
}

want() {
    [[ ${#SUITES[@]} -eq 0 ]] && return 0
    local s
    for s in "${SUITES[@]}"; do [[ "$s" == "$1" ]] && return 0; done
    return 1
}

for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        h1|h2|h3) SUITES+=("$arg") ;;
        *) say "未知参数：$arg"; usage; exit 2 ;;
    esac
done

mkdir -p "$CACHE"

head_ "环境自检"
ENV_OK=1
require_h2load || ENV_OK=0
[[ "$ENV_OK" == "1" ]] && ok "齐备"
if [[ "$ENV_OK" != "1" ]]; then
    say ""
    say "环境不全，未开始测试。按上面的提示准备后重跑。"
    exit 1
fi

# 自编的 h2load 装到别处时，用 H2LOAD=... 指过来（配合 LD_LIBRARY_PATH 指向它的
# 依赖，若它不是静态链接的）。
H2LOAD="${H2LOAD:-$(command -v h2load)}"

start_server || exit 1

want h1 && run_h1
want h2 && run_h2
want h3 && run_h3

stop_server

head_ "汇总"
if [[ "$FAILURES" == "0" ]]; then
    say "  全部达标。"
    exit 0
fi
say "  ${FAILURES} 项未达标。"
exit 1
