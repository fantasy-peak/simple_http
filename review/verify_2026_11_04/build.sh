#!/bin/sh
# Builds the verification fixture and client from the current working tree.
#
#   review/verify_2026_11_04/build.sh [tag]
#
# tag defaults to "after"; outputs bin/server_<tag> and bin/client_<tag>.
# The include/lib paths below point at this machine's xmake package cache;
# adjust them (or use -I/-L on the g++ line) on another host.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
TAG=${1:-after}
mkdir -p "$HERE/bin"

find_pkg() {
    # $1 = package/name/version, $2 = file proving the directory is the right one
    for d in /root/.xmake/packages/$1/*; do
        if [ -e "$d/$2" ]; then
            printf '%s\n' "$d"
            return 0
        fi
    done
    return 1
}

BOOST_ROOT=$(find_pkg b/boost/1.92.0 include/boost/system/error_code.hpp)
GLAZE_ROOT=$(find_pkg g/glaze/v7.9.0 include/glaze/glaze.hpp)
BROTLI_ROOT=$(find_pkg b/brotli/v1.2.0 include/brotli/decode.h)
BOOST="$BOOST_ROOT/include"
GLAZE="$GLAZE_ROOT/include"
BROTLI="$BROTLI_ROOT/include"
BROTLILIB="$BROTLI_ROOT/lib"

CXX=${CXX:-g++}
FLAGS="-std=c++23 -O1 -I$ROOT/include -I$BOOST -I$GLAZE -I$BROTLI"
LIBS="-L$BROTLILIB -lbrotlienc -lbrotlidec -lbrotlicommon -lssl -lcrypto -lz -lpthread"

$CXX $FLAGS "$HERE/server.cpp" -o "$HERE/bin/server_$TAG" $LIBS
$CXX $FLAGS "$HERE/client.cpp" -o "$HERE/bin/client_$TAG" $LIBS
echo "built $HERE/bin/server_$TAG and $HERE/bin/client_$TAG"
