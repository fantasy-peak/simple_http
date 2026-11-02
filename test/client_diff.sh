#!/usr/bin/env bash
#
# Client differential suite for the new features (streaming multipart,
# Expect: 100-continue, request trailers, 429 Retry-After, JWKS fetch).
#
# Three legs:
#   * C++ client probe  -> Go reference server      (C++ client correctness)
#   * Go client probe   -> Go reference server      (baseline)
#   * Go client probe   -> C++ framework server     (C++ server correctness)
#
# The PASS/FAIL lines of the three runs are diffed. Each server gets a fresh
# process (and therefore a fresh retry counter).
#
#   test/client_diff.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CACHE="${ROOT}/.cache/client_diff"
mkdir -p "$CACHE"

GO_SERVER_PORT="${GO_SERVER_PORT:-7802}"
GO_BASELINE_PORT="${GO_BASELINE_PORT:-7803}"
CPP_SERVER_PORT="${CPP_SERVER_PORT:-7804}"

GO_SERVER_BIN="${CACHE}/client_diff_server"
GO_PROBE_BIN="${CACHE}/client_diff_probe"
CPP_PROBE_BIN="${ROOT}/build/linux/x86_64/release/client_diff_probe"
CPP_SERVER_BIN="${ROOT}/build/linux/x86_64/release/framework_server"

echo "==> building probes and servers"
xmake build framework_server >/dev/null
xmake build client_diff_probe >/dev/null
go build -o "$GO_SERVER_BIN" "$ROOT/test/go/client_diff_server.go"
go build -o "$GO_PROBE_BIN" "$ROOT/test/go/client_diff_probe.go"

wait_url() {
    local url="$1"
    for _ in $(seq 1 100); do
        if curl -sf "$url" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.05
    done
    echo "error: no server at $url" >&2
    return 1
}

cleanup() {
    kill "${CPP_PID:-}" "${GO_A_PID:-}" "${GO_B_PID:-}" 2>/dev/null || true
    wait "${CPP_PID:-}" "${GO_A_PID:-}" "${GO_B_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT

"$GO_SERVER_BIN" "$GO_SERVER_PORT" >"$CACHE/go_server_a.log" 2>&1 &
GO_A_PID=$!
"$GO_SERVER_BIN" "$GO_BASELINE_PORT" >"$CACHE/go_server_b.log" 2>&1 &
GO_B_PID=$!
"$CPP_SERVER_BIN" "$CPP_SERVER_PORT" >"$CACHE/cpp_server.log" 2>&1 &
CPP_PID=$!

wait_url "http://127.0.0.1:$GO_SERVER_PORT/world"
wait_url "http://127.0.0.1:$GO_BASELINE_PORT/world"
wait_url "http://127.0.0.1:$CPP_SERVER_PORT/world"

probe_status=0
echo "==> C++ client  -> Go server"
"$CPP_PROBE_BIN" "http://127.0.0.1:$GO_SERVER_PORT" >"$CACHE/cpp.out" 2>&1 || probe_status=1
cat "$CACHE/cpp.out"

echo "==> Go client   -> Go server"
"$GO_PROBE_BIN" "http://127.0.0.1:$GO_BASELINE_PORT" >"$CACHE/go.out" 2>&1 || probe_status=1
cat "$CACHE/go.out"

echo "==> Go client   -> C++ server"
"$GO_PROBE_BIN" "http://127.0.0.1:$CPP_SERVER_PORT" >"$CACHE/go_cpp.out" 2>&1 || probe_status=1
cat "$CACHE/go_cpp.out"

grep -E '^(PASS|FAIL)' "$CACHE/cpp.out" >"$CACHE/cpp.lines"
grep -E '^(PASS|FAIL)' "$CACHE/go.out" >"$CACHE/go.lines"
grep -E '^(PASS|FAIL)' "$CACHE/go_cpp.out" >"$CACHE/go_cpp.lines"

status=$probe_status
if ! diff -u "$CACHE/cpp.lines" "$CACHE/go.lines"; then
    echo "error: C++ client and Go client disagree" >&2
    status=1
fi
if ! diff -u "$CACHE/go.lines" "$CACHE/go_cpp.lines"; then
    echo "error: C++ server and Go server disagree" >&2
    status=1
fi

if grep -q '^FAIL' "$CACHE"/*.lines; then
    echo "error: at least one probe reported FAIL" >&2
    status=1
fi

if [[ "$status" == 0 ]]; then
    echo "client differential OK"
fi
exit "$status"
