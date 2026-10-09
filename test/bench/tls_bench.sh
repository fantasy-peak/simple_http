#!/usr/bin/env bash
# tls_bench.sh — 完整服务端性能基准：h2c/h1 明文 vs. TLS，及 TLS 下
# asio::ssl::stream 内部缓冲大小的影响。只加载服务端（test/server.cpp）。
#
# 端点（全部来自同一个 server 进程、同一套路由）：
#   :7790  纯 h2c（plaintext_protocols = Http2）
#   :7791  纯 h1 （plaintext_protocols = Http1）
#   :7794  非 mTLS TLS，ALPN {h2, http/1.1} —— 与上面两个共用同一引擎，
#          因此每个协议对里唯一的变量就是 TLS 记录层。
#
# 每个协议对先跑明文基线，再按 SIMPLE_HTTP_TLS_BUF_IN / _OUT（服务端启动时
# 读取，故每个缓冲配置都要重启一次 server）跑 TLS。缓冲 0 = asio 默认
# （output buffer 0 还打开 SSL_MODE_ENABLE_PARTIAL_WRITE，一次 SSL_write 只编
# 一条记录立即 flush）；更大的 output buffer 关掉 partial-write、让一次
# SSL_write 编多条记录再整批发，代价是每连接内存。
#
# 用法：
#   test/bench/tls_bench.sh                             # h1+h2 × 1k/16k/1M × 默认/调大缓冲
#   N=20000 C=16 M=16 DURATION=5 test/bench/tls_bench.sh
#   SIZES=1024 test/bench/tls_bench.sh                  # 只压 1 KiB 响应
#   PROTO=h2 test/bench/tls_bench.sh                    # 只压 h2
#   CONFIGS=default:0:0 test/bench/tls_bench.sh         # 只跑默认缓冲
#   H2LOAD=/usr/bin/h2load test/bench/tls_bench.sh      # 用系统 h2load
#
# 退出码 0 仅当每一趟都是 0 failed / 0 errored。

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2

H2LOAD="${H2LOAD:-/opt/h3/bin/h2load}"
SERVER_BIN="${SERVER_BIN:-build/linux/x86_64/release/server}"

PORT_H2C=7790
PORT_H1=7791
PORT_TLS=7794

# 负载形状（h2load；-n 在 -D 时被忽略）。
N="${N:-100000}"
C="${C:-20}"
M="${M:-40}"
T="${T:-4}"
DURATION="${DURATION:-8}"
WARMUP="${WARMUP:-2}"

# 响应体大小（/world?n=）：1 KiB = 小包高频，16 KiB ≈ 一条大记录，
# 1 MiB = 纯吞吐。
SIZES="${SIZES:-1024 16384 1048576}"

# 协议维度，空格分隔。h2 = h2c vs h2-over-TLS；h1 = h1 vs h1-over-TLS。
PROTO="${PROTO:-h2 h1}"

# TLS 缓冲配置 name:input:output（字节，0 = asio 默认）。
CONFIGS="${CONFIGS:-default:0:0 out64k:0:65536 both64k:65536:65536}"

bad()   { printf '\033[31m[FAIL]\033[0m %s\n' "$*" >&2; }
ok()    { printf '\033[32m[ OK ]\033[0m %s\n' "$*"; }
head_() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

require_h2load() {
    if [[ ! -x "$H2LOAD" ]]; then
        bad "h2load 不在 $H2LOAD（可用 H2LOAD=... 指定）"
        return 1
    fi
    return 0
}

ensure_server() {
    if [[ ! -x "$SERVER_BIN" ]]; then
        head_ "构建 server"
        ( cd "$ROOT" && xmake build server ) || return 1
    fi
}

SERVER_PID=""
start_server() {
    local in="$1" out="$2"
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=""
    fi
    ( cd "$ROOT" && exec env SIMPLE_HTTP_TLS_BUF_IN="$in" SIMPLE_HTTP_TLS_BUF_OUT="$out" \
        "$SERVER_BIN" >/tmp/opencode/tls_bench_server.log 2>&1 ) &
    SERVER_PID=$!
    local port ready i
    for port in "$PORT_H2C" "$PORT_H1" "$PORT_TLS"; do
        ready=0
        for i in $(seq 1 50); do
            if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then ready=1; break; fi
            sleep 0.1
        done
        [[ "$ready" == "1" ]] || { bad "端口 ${port} 未就绪"; return 1; }
    done
    return 0
}

stop_server() {
    [[ -z "$SERVER_PID" ]] && return 0
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
}
trap stop_server EXIT INT TERM

# 服务端 CPU 秒数（/proc/<pid>/stat utime+stime；先调用一次以归零基线）。
prev_cpu="#unset"
server_cpu_seconds() {
    local line ticks
    if [[ ! -r /proc/$SERVER_PID/stat ]]; then echo 0; return; fi
    read -r line < /proc/$SERVER_PID/stat || { echo 0; return; }
    line="${line#*\) }"          # 去掉 "pid (comm) "，$1=state(字段3)、...
    set -- $line
    ticks=$(( ${12} + ${13} ))   # utime(14)/stime(15) 总体下标
    if [[ "$prev_cpu" == "#unset" ]]; then
        prev_cpu="$ticks"
        echo 0
        return
    fi
    local delta=$(( ticks - prev_cpu ))
    prev_cpu="$ticks"
    echo "$delta"
}

# 跑一趟 h2load，输出一行，返回 0/1；req/s 数值放进 $LAST_RATE。
# $1 label  $2 url  $3 额外参数  $4 可选：明文基线 req/s（显示占比）
LAST_RATE=""
run_pass() {
    local label="$1" url="$2" extra="$3" base="${4:-}"
    server_cpu_seconds > /dev/null
    local started ended out
    started="$(date +%s%N)"
    local args=( -n "$N" -c "$C" -m "$M" -t "$T" -D "$DURATION" --warm-up-time "$WARMUP" )
    if [[ -n "$extra" ]]; then
        # shellcheck disable=SC2206
        args+=( $extra )
    fi
    args+=( "$url" )
    out="$( "$H2LOAD" "${args[@]}" 2>&1 )"
    local rc=$?
    ended="$(date +%s%N)"
    local wall_s cpu_sec
    wall_s=$(awk "BEGIN{printf \"%.2f\", ($ended-$started)/1e9}")
    cpu_sec=$(awk "BEGIN{printf \"%.2f\", $(server_cpu_seconds)/100}")

    local summary reqs succ failed errored
    summary="$(printf '%s\n' "$out" | grep -E '^finished in' | tail -n 1)"
    reqs="$(printf '%s\n' "$out" | grep -E '^requests:' | tail -n 1)"
    succ="$(printf '%s' "$reqs" | grep -oE '[0-9]+ succeeded' | grep -oE '[0-9]+')"
    failed="$(printf '%s' "$reqs" | grep -oE '[0-9]+ failed' | head -n1 | grep -oE '[0-9]+')"
    errored="$(printf '%s' "$reqs" | grep -oE '[0-9]+ errored' | head -n1 | grep -oE '[0-9]+')"
    local rate
    rate="$(printf '%s' "$summary" | sed -E 's/^finished in [0-9.]+s, ([0-9.]+) req\/s.*/\1/')"
    LAST_RATE="$rate"

    local pct=""
    if [[ -n "$rate" && -n "$base" && "$base" != "0" ]]; then
        pct="  ($(awk "BEGIN{printf \"%.0f%%\", $rate/$base*100}") of plain)"
    fi

    if [[ -z "$rate" ]]; then
        printf '%-17s %-11s  %-10s %-10s  n/a (exit %d)\n' "$label" "$url" "-" "-" "$rc"
        printf '%s\n' "$out" | tail -n 5 | sed 's/^/    /'
        return 1
    fi
    printf '%-17s %-11s  %-10s %-10s  %s%s\n' \
        "$label" "$url" "$rate req/s" "cpu ${cpu_sec}s" "$summary" "$pct"

    local r=0
    [[ "$rc" -eq 0 ]] || r=1
    [[ "${succ:-0}" -gt 0 ]] || r=1
    [[ "${failed:-0}" == "0" && "${errored:-0}" == "0" ]] || r=1
    if [[ "$r" -ne 0 ]]; then
        bad "$label 失败（succeeded=$succ failed=$failed errored=$errored）"
        printf '%s\n' "$out" | tail -n 5 | sed 's/^/    /'
    fi
    return $r
}

require_h2load || exit 1
ensure_server   || exit 1

head_ "负载形状（h2load）"
printf '  N=%s  C=%s  M=%s  T=%s  duration=%ss + warmup=%ss  H2LOAD=%s\n' \
    "$N" "$C" "$M" "$T" "$DURATION" "$WARMUP" "$H2LOAD"

FAILURES=0
declare -A PLAIN_RATE   # "size:proto" → 明文 req/s（TLS 行显示占比）

for size in $SIZES; do
    head_ "响应体 ${size} 字节（/world?n=${size}）"
    for proto in $PROTO; do
        if [[ "$proto" == "h2" ]]; then
            plain_url="http://127.0.0.1:${PORT_H2C}/world?n=${size}"
            plain_extra=""
            tls_extra="--alpn-list=h2"
        elif [[ "$proto" == "h1" ]]; then
            plain_url="http://127.0.0.1:${PORT_H1}/world?n=${size}"
            plain_extra="--no-tls-proto=http/1.1"
            tls_extra="--h1"
        else
            bad "未知协议：$proto（可用 h2/h1）"
            exit 2
        fi
        tls_url="https://127.0.0.1:${PORT_TLS}/world?n=${size}"

        started_plain=0
        for cfg in $CONFIGS; do
            name="${cfg%%:*}"
            rest="${cfg#*:}"
            in="${rest%%:*}"
            out="${rest#*:}"
            start_server "$in" "$out" || exit 1
            if [[ "$started_plain" == "0" ]]; then
                run_pass "${proto}-plain" "$plain_url" "$plain_extra" || FAILURES=$((FAILURES + 1))
                PLAIN_RATE["${size}:${proto}"]="$LAST_RATE"
                started_plain=1
            fi
            run_pass "tls:${name}" "$tls_url" "$tls_extra" "${PLAIN_RATE[${size}:${proto}]}" || FAILURES=$((FAILURES + 1))
        done
    done
done

stop_server

head_ "汇总"
if [[ "$FAILURES" == "0" ]]; then
    ok "全部通过（0 failed / 0 errored）"
    exit 0
fi
bad "${FAILURES} 项未达标"
exit 1