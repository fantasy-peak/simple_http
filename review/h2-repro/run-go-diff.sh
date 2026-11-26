#!/bin/sh
# Builds the C++ block dumper and runs the Go x/net/http2 differential.
set -e
cd "$(dirname "$0")/../.."

review/h2-repro/build.sh >/dev/null
WORK=/tmp/h2-go-diff
rm -rf "$WORK"
mkdir -p "$WORK"
review/h2-repro/bin/repro dump "$WORK/malformed.bin"  1500 malformed
review/h2-repro/bin/repro dump "$WORK/wellformed.bin" 1500 wellformed

# GOPATH mode: symlink the read-only module cache for x/net (+ x/text/idna).
GP="$WORK/gopath"
mkdir -p "$GP/src/golang.org/x" "$GP/src/goh2diff"
ln -s /root/go/pkg/mod/golang.org/x/net@v0.56.0  "$GP/src/golang.org/x/net"
ln -s /root/go/pkg/mod/golang.org/x/text@v0.38.0 "$GP/src/golang.org/x/text"
cp review/h2-repro/go-diff/main.go "$GP/src/goh2diff/main.go"

(cd "$GP/src/goh2diff" && GOPATH="$GP" GO111MODULE=off go build -o "$WORK/go-diff" .)
"$WORK/go-diff" "$WORK/malformed.bin" "$WORK/wellformed.bin"

# Extended CONNECT with an unimplemented :protocol.
mkdir -p "$GP/src/goh2connect"
cp review/h2-repro/go-diff/connect/main.go "$GP/src/goh2connect/main.go"
(cd "$GP/src/goh2connect" && GOPATH="$GP" GO111MODULE=off go build -o "$WORK/go-connect" .)
GODEBUG=http2xconnect=1 "$WORK/go-connect"
