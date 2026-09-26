#!/usr/bin/env bash
#
# 外部一致性套件 —— **每次改动协议层后都应该跑一次**。
#
# 三套都是独立于 simple_http 自己的客户端的第三方实现。和 test/python/ 是同一
# 个理由（客户端与服务端共享的规范误读会互相抵消），但更权威：它们是各自协议的
# 参考级一致性套件，只在服务端真的错的地方分歧。截至写下这些行，它们抓到过：
# RSV 位不校验、text 消息完全不校验 UTF-8、close code 不校验、协议错误不发 Close
# 帧、Host 缺失/重复不检查、字段名与字段值的字符集不检查，以及"靠请求行形状猜
# 协议"这个做法本身与 HTTP/1.1 语义的冲突 —— 全部是自研套件看不见的。
#
# 用法：
#   test/conformance/run.sh              # 三套全跑（默认）
#   test/conformance/run.sh h2spec       # 只跑指定的（h2spec / h1spec / autobahn）
#
# 退出码：0 = 三套都达到基线；1 = 有套件未达标，或环境不全（缺什么会打印出来）。
#
# 端口不是随便选的，别改：
#   h2spec   → 7790  纯 h2c 端点
#   h1spec   → 7791  纯 h1 端点
#   Autobahn → 7788  嗅探端点（WebSocket 挂在它上面）
# 单协议端口是必需的：在嗅探端口上"畸形的 HTTP/2 前导"和"畸形的 HTTP/1.x 请求
# 行"是同一批字节，而 h2spec 要 GOAWAY、h1spec 要 400 —— 只有端点声明了自己服务
# 什么，两个要求才可能同时成立（见 net/connection.h 的 PlaintextProtocols）。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CACHE="${ROOT}/.cache/conformance"
SERVER_BIN="${ROOT}/build/linux/x86_64/debug/server"

H2SPEC_URL="https://github.com/summerwind/h2spec/releases/download/v2.6.0/h2spec_linux_amd64.tar.gz"
H1SPEC_URL="https://raw.githubusercontent.com/uNetworking/h1spec/main/http_test.ts"
AUTOBAHN_IMAGE="crossbario/autobahn-testsuite"
DENO_IMAGE="denoland/deno"

PORT_H2C=7790
PORT_H1=7791
PORT_SNIFF=7788

# 基线。低于它就退出非零 —— 数字写死是有意的：套件自己会随上游版本变化，
# 而"通过数变少了"必须是个信号，不能悄悄跟着漂。
EXPECT_H2SPEC_PASSED=146
EXPECT_H1SPEC_TOTAL=33

SERVER_PID=""
FAILURES=0
SUITES=()

# --- 输出 ----------------------------------------------------------------

if [[ -t 1 ]]; then
    B=$'\033[1m'; G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; N=$'\033[0m'
else
    B=""; G=""; R=""; Y=""; N=""
fi

say()  { printf '%s\n' "$*"; }
head_() { printf '\n%s==> %s%s\n' "$B" "$*" "$N"; }
ok()   { printf '  %s✓%s %s\n' "$G" "$N" "$*"; }
bad()  { printf '  %s✗%s %s\n' "$R" "$N" "$*"; FAILURES=$((FAILURES + 1)); }
warn() { printf '  %s!%s %s\n' "$Y" "$N" "$*"; }

# --- 环境自检 ------------------------------------------------------------
#
# 三套工具都不在仓库里（许可证/体积原因），这里只检查、并打印准备命令。
# 不自动下载执行远程内容：那应该是使用者的显式决定。

require_h2spec() {
    command -v h2spec >/dev/null 2>&1 && return 0
    warn "缺 h2spec（HTTP/2 一致性套件）"
    say  "      curl -fsSL -o /tmp/h2spec.tgz ${H2SPEC_URL}"
    say  "      tar xzf /tmp/h2spec.tgz -C /usr/local/bin h2spec"
    return 1
}

require_h1spec() {
    local script="${CACHE}/h1spec/http_test.ts"
    if [[ -f "$script" ]]; then
        printf '%s' "$script"
        return 0
    fi
    # 该仓库没有 LICENSE（默认版权保留），所以脚本不进仓库，只缓存到 .cache/。
    # 失败信息必须走 stderr：成功路径的 stdout 就是这个函数的返回值。
    if ! mkdir -p "$(dirname "$script")" || ! curl -fsSL -o "$script" "$H1SPEC_URL"; then
        printf '  %s!%s 无法取到 h1spec 脚本：%s\n' "$Y" "$N" "$H1SPEC_URL" >&2
        return 1
    fi
    printf '%s' "$script"
}

require_docker_image() {
    local image="$1"
    if ! command -v docker >/dev/null 2>&1; then
        warn "缺 docker"
        return 1
    fi
    if ! docker image inspect "$image" >/dev/null 2>&1; then
        warn "缺镜像 ${image}"
        say  "      docker pull ${image}"
        return 1
    fi
    return 0
}

# --- 服务器 --------------------------------------------------------------

start_server() {
    if [[ ! -x "$SERVER_BIN" ]]; then
        head_ "构建 server"
        ( cd "$ROOT" && xmake build server ) || { bad "xmake build server 失败"; return 1; }
    fi
    head_ "启动示例服务器（:7788 嗅探 / :7789 TLS / :7790 h2c / :7791 h1）"
    # 必须在仓库根目录起：证书路径是相对路径。
    # exec 让子 shell 被 server 自己替换掉，$! 才是 server 的 PID —— 否则 kill
    # 落在中间那层 shell 上，server 会变成孤儿继续占着端口。
    ( cd "$ROOT" && exec "$SERVER_BIN" >"${CACHE}/server.log" 2>&1 ) &
    SERVER_PID=$!

    local port ready i
    for port in "$PORT_SNIFF" "$PORT_H2C" "$PORT_H1"; do
        ready=0
        for i in $(seq 1 50); do
            # 只探 TCP 可连：h2c 端点会拒掉普通 GET（那本来就是无效前导），
            # 用 HTTP 探活对三个端口并不通用。
            if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then ready=1; break; fi
            if [[ -n "$SERVER_PID" ]] && ! kill -0 "$SERVER_PID" 2>/dev/null; then
                bad "服务器启动即退出，见 ${CACHE}/server.log"
                tail -n 5 "${CACHE}/server.log" | sed 's/^/      /'
                return 1
            fi
            sleep 0.1
        done
        if [[ "$ready" != "1" ]]; then
            bad "端口 ${port} 一直没就绪"
            return 1
        fi
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

# --- 套件 ----------------------------------------------------------------

run_h2spec() {
    head_ "h2spec —— HTTP/2 帧层与流状态机（:${PORT_H2C}，纯 h2c）"
    local out
    out="$(h2spec -h 127.0.0.1 -p "$PORT_H2C" -P /world 2>&1)"
    local summary passed failed
    summary="$(printf '%s\n' "$out" | grep -E '^[0-9]+ tests,' | tail -n 1)"
    passed="$(printf '%s' "$summary" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+')"
    failed="$(printf '%s' "$summary" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+')"

    if [[ -z "${passed:-}" ]]; then
        bad "没解析到结果，输出尾部："
        printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
        return 1
    fi
    say "  ${summary:-（无摘要）}"
    if [[ "${failed:-1}" != "0" || "$passed" -lt "$EXPECT_H2SPEC_PASSED" ]]; then
        bad "通过 ${passed}（基线 ${EXPECT_H2SPEC_PASSED}），失败 ${failed:-?}"
        printf '%s\n' "$out" | grep -B1 -A4 '×' | head -n 40 | sed 's/^/      /'
        return 1
    fi
    ok "通过 ${passed}/${passed}（基线 ${EXPECT_H2SPEC_PASSED}）"
}

run_h1spec() {
    head_ "h1spec —— HTTP/1.1 请求解析与边界（:${PORT_H1}，纯 h1）"
    require_h1spec >/dev/null || return 1

    local out
    out="$(docker run --rm --network host -v "${CACHE}/h1spec:/app" "$DENO_IMAGE" \
             run --allow-net /app/http_test.ts 127.0.0.1 "$PORT_H1" 2>&1)"
    local passed total
    passed="$(printf '%s' "$out" | grep -oE '^[0-9]+ out of' | grep -oE '^[0-9]+')"
    total="$(printf '%s' "$out" | grep -oE 'out of [0-9]+ tests' | grep -oE '[0-9]+')"

    if [[ -z "${passed:-}" ]]; then
        bad "没解析到结果，输出尾部："
        printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
        return 1
    fi
    say "  ${passed}/${total:-?} 通过"
    if [[ "$total" != "$EXPECT_H1SPEC_TOTAL" ]]; then
        bad "用例总数是 ${total}，基线是 ${EXPECT_H1SPEC_TOTAL} —— 上游套件变了，请复核基线"
        return 1
    fi
    if [[ "$passed" != "$total" ]]; then
        bad "有未通过用例："
        printf '%s\n' "$out" | grep '❌' | sed 's/^/      /'
        return 1
    fi
    ok "通过 ${passed}/${total}"
}

run_autobahn() {
    head_ "Autobahn —— WebSocket / RFC 6455（:${PORT_SNIFF}，嗅探端点）"
    require_docker_image "$AUTOBAHN_IMAGE" || return 1
    require_docker_image "$DENO_IMAGE" >/dev/null || true  # 只 h1spec 需要

    local dir="${CACHE}/autobahn"
    mkdir -p "${dir}/reports"
    rm -rf "${dir}/reports"/*

    cat >"${dir}/fuzzingclient.json" <<EOF
{
   "outdir": "/reports",
   "servers": [
      {"agent": "simple_http", "url": "ws://127.0.0.1:${PORT_SNIFF}/echo"}
   ],
   "cases": ["*"],
   "exclude-cases": [],
   "exclude-agent-cases": {}
}
EOF

    if ! docker run --rm --network host -v "${dir}:/config" -v "${dir}/reports:/reports" \
            "$AUTOBAHN_IMAGE" wstest -m fuzzingclient -s /config/fuzzingclient.json \
            >"${dir}/run.log" 2>&1; then
        bad "Autobahn 运行失败，见 ${dir}/run.log"
        return 1
    fi

    local summary
    summary="$(python3 - "${dir}/reports/index.json" <<'PY'
import json, collections, sys
try:
    d = json.load(open(sys.argv[1]))["simple_http"]
except Exception as e:
    print("PARSE_ERROR", e)
    sys.exit(0)
rows = [v for v in d.values() if isinstance(v, dict)]
c = collections.Counter(v.get("behavior") for v in rows)
bad = [k for k, v in d.items() if isinstance(v, dict) and v.get("behavior") not in ("OK", "UNIMPLEMENTED", "INFORMATIONAL")]
print(len(rows), c.get("OK", 0), c.get("FAILED", 0), c.get("NON-STRICT", 0), c.get("UNIMPLEMENTED", 0), c.get("INFORMATIONAL", 0), ",".join(sorted(bad)))
PY
)"
    local total ok_n failed nonstrict unimpl info offenders
    read -r total ok_n failed nonstrict unimpl info offenders <<<"$summary"

    if [[ "${total:-}" == "PARSE_ERROR" || -z "${total:-}" ]]; then
        bad "解析报告失败：${summary}"
        return 1
    fi
    say "  ${total} 个用例：OK ${ok_n}，UNIMPLEMENTED ${unimpl}（permessage-deflate），INFORMATIONAL ${info}"
    # FAILED 和 NON-STRICT 都必须为 0：前者是明确的错，后者是"通过了但偏离 SHOULD"，
    # 这个仓库的立场是两者都不要（6.4.3/6.4.4 的 fail-fast 就是这么修掉的）。
    if [[ "$failed" != "0" || "$nonstrict" != "0" ]]; then
        bad "FAILED ${failed}，NON-STRICT ${nonstrict}"
        [[ -n "$offenders" ]] && say "      用例：${offenders}"
        return 1
    fi
    ok "FAILED 0，NON-STRICT 0"
}

# --- 主流程 --------------------------------------------------------------

usage() {
    say "用法：$(basename "$0") [h2spec|h1spec|autobahn]..."
    say "      不带参数 = 三套全跑。"
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
        h2spec|h1spec|autobahn) SUITES+=("$arg") ;;
        *) say "未知参数：$arg"; usage; exit 2 ;;
    esac
done

mkdir -p "$CACHE"

head_ "环境自检"
ENV_OK=1
want h2spec   && { require_h2spec   || ENV_OK=0; }
want h1spec   && { require_h1spec >/dev/null || ENV_OK=0; }
want autobahn && {
    require_docker_image "$AUTOBAHN_IMAGE" || ENV_OK=0
}
[[ "$ENV_OK" == "1" ]] && ok "齐备"

if [[ "$ENV_OK" != "1" ]]; then
    say ""
    say "环境不全，未开始测试。按上面的提示准备后重跑。"
    exit 1
fi

start_server || exit 1

want h2spec   && run_h2spec
want h1spec   && run_h1spec
want autobahn && run_autobahn

stop_server

head_ "汇总"
if [[ "$FAILURES" == "0" ]]; then
    say "  全部达到基线。"
    exit 0
fi
say "  ${FAILURES} 个套件未达标。"
exit 1
