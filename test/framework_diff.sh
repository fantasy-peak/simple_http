#!/usr/bin/env bash
#
# Differential test: framework-layer routes in simple_http vs Go net/http.
#
# Builds test/framework_server.cpp and test/go/framework_server.go, starts both
# on loopback, and runs test/python/framework_diff.py. The Python driver has the
# assertions; this script only manages lifetimes and ports.
#
#   test/framework_diff.sh
#   CPP_PORT=7898 GO_PORT=7899 test/framework_diff.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CPP_PORT="${CPP_PORT:-7798}"
GO_PORT="${GO_PORT:-7799}"
CACHE="${ROOT}/.cache/framework_diff"
KEYS="${CACHE}/keys"
CPP_BIN="${ROOT}/build/linux/x86_64/release/framework_server"
GO_BIN="${CACHE}/framework_server_go"
PY="${ROOT}/test/python/.venv/bin/python"

if [[ ! -x "$PY" ]]; then
    echo "error: no Python venv at test/python/.venv" >&2
    echo "  python3 -m venv test/python/.venv" >&2
    echo "  test/python/.venv/bin/pip install -r test/python/requirements.txt" >&2
    exit 1
fi

mkdir -p "$CACHE" "$KEYS"

echo "==> generating RSA / EC keys"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$KEYS/rsa_priv.pem" 2>/dev/null
openssl pkey -in "$KEYS/rsa_priv.pem" -pubout -out "$KEYS/rsa_pub.pem" 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out "$KEYS/ec_priv.pem" 2>/dev/null
openssl pkey -in "$KEYS/ec_priv.pem" -pubout -out "$KEYS/ec_pub.pem" 2>/dev/null

echo "==> building C++ framework server"
xmake build framework_server >/dev/null

echo "==> building Go framework server"
go build -o "$GO_BIN" "$ROOT/test/go/framework_server.go"

CPP_LOG="$CACHE/cpp.log"
GO_LOG="$CACHE/go.log"
: >"$CPP_LOG"
: >"$GO_LOG"

"$CPP_BIN" "$CPP_PORT" "$KEYS/rsa_pub.pem" "$KEYS/ec_pub.pem" >"$CPP_LOG" 2>&1 &
CPP_PID=$!
"$GO_BIN" "$GO_PORT" "$KEYS/rsa_pub.pem" "$KEYS/ec_pub.pem" >"$GO_LOG" 2>&1 &
GO_PID=$!

cleanup() {
    kill "$CPP_PID" "$GO_PID" 2>/dev/null || true
    wait "$CPP_PID" "$GO_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "==> simple_http on :$CPP_PORT, Go on :$GO_PORT"
"$PY" "$ROOT/test/python/framework_diff.py" "$CPP_PORT" "$GO_PORT" "$KEYS/rsa_priv.pem" "$KEYS/ec_priv.pem"
