#!/bin/sh
# Builds the P1/P2 repro against the same xmake packages the test suite uses.
set -e
cd "$(dirname "$0")/../.."

PKGS=/root/.xmake/packages
INC_BOOST=$PKGS/b/boost/1.92.0/5b02e7380b884b34b7999345bfa00cb9/include
INC_OPENSSL=$PKGS/o/openssl4/4.0.3/f4eee25958f04c3b93406aa2ca169000/include
LIB_OPENSSL=$PKGS/o/openssl4/4.0.3/f4eee25958f04c3b93406aa2ca169000/lib
INC_BROTLI=$PKGS/b/brotli/v1.2.0/9e99a0e56961453bb42404f66ba18f0d/include
LIB_BROTLI=$PKGS/b/brotli/v1.2.0/9e99a0e56961453bb42404f66ba18f0d/lib

mkdir -p review/h2-repro/bin
g++ -std=c++23 -O0 -g -Iinclude -isystem "$INC_BOOST" -isystem "$INC_OPENSSL" -isystem "$INC_BROTLI" \
    -o review/h2-repro/bin/repro review/h2-repro/repro.cpp \
    -L"$LIB_OPENSSL" -L"$LIB_BROTLI" \
    -lssl -lcrypto -lz -lbrotlienc -lbrotlidec -lbrotlicommon -lrt -lpthread -ldl
echo "built review/h2-repro/bin/repro"
