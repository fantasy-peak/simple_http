#!/usr/bin/env bash
#
# Parser fuzzing.
#
# Default (GCC): deterministic mutation driver + ASan/UBSan. No libFuzzer
# runtime needed; every target runs FUZZ_ITERATIONS mutations.
#
#   test/fuzz/run.sh
#   FUZZ_ITERATIONS=100000 test/fuzz/run.sh
#
# libFuzzer (clang): builds the same LLVMFuzzerTestOneInput targets with
# -fsanitize=fuzzer and runs a coverage-guided campaign.
#
#   test/fuzz/run.sh --libfuzzer --time 60
#
# clang is looked up at /usr/lib/llvm-24 (the SDK used by v2ray-cpp/build.sh)
# and falls back to clang++ on PATH.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

TARGETS=(fuzz_h1 fuzz_h2 fuzz_ws fuzz_multipart)
NAMES=(h1 h2 ws multipart)
ITERATIONS="${FUZZ_ITERATIONS:-20000}"
TIME="${FUZZ_TIME:-30}"
JOBS="${FUZZ_JOBS:-7}"
BUILD_DIR="${FUZZ_BUILD_DIR:-}"
LIBFUZZER=0
KEEP_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --libfuzzer) LIBFUZZER=1 ;;
        --iterations) ITERATIONS="$2"; shift ;;
        --time) TIME="$2"; shift ;;
        --jobs) JOBS="$2"; shift ;;
        --build-dir) BUILD_DIR="$2"; shift ;;
        --keep-build) KEEP_BUILD=1 ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [[ -z "$BUILD_DIR" ]]; then
    if [[ "$LIBFUZZER" == "1" ]]; then
        BUILD_DIR="build-fuzz"
    else
        BUILD_DIR="build"
    fi
fi

find_binary() {
    local name="$1"
    for mode in release debug; do
        local path="${BUILD_DIR}/linux/x86_64/${mode}/${name}"
        if [[ -x "$path" ]]; then
            echo "$path"
            return 0
        fi
    done
    return 1
}

run_standalone() {
    local index=0
    for name in "${NAMES[@]}"; do
        local target="${TARGETS[$index]}"
        echo "==> build ${target}"
        xmake build -j "$JOBS" "$target"
        local binary
        if ! binary="$(find_binary "$target")"; then
            echo "missing binary for ${target}" >&2
            return 1
        fi
        echo "==> ${target}: ${ITERATIONS} iterations"
        ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:strict_string_checks=1" \
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
            "$binary" --corpus "test/fuzz/corpus/${name}" --iterations "$ITERATIONS"
        index=$((index + 1))
    done
}

run_libfuzzer() {
    local clang="/usr/lib/llvm-24/bin/clang++"
    local sdk="/usr/lib/llvm-24"
    if [[ ! -x "$clang" ]]; then
        clang="$(command -v clang++ || true)"
        sdk=""
    fi
    if [[ -z "$clang" ]]; then
        echo "libFuzzer mode needs clang++; install it or run without --libfuzzer" >&2
        return 1
    fi

    echo "==> configure ${BUILD_DIR} with LLVM (${clang})"
    local sdk_flag=()
    if [[ -n "$sdk" ]]; then
        sdk_flag=(--sdk="$sdk")
    fi
    xmake f -c -o "$BUILD_DIR" -m release --toolchain=llvm "${sdk_flag[@]}" \
        --cxxflags="-stdlib=libstdc++" --ldflags="-stdlib=libstdc++" \
        --fuzz_libfuzzer=y

    local index=0
    for name in "${NAMES[@]}"; do
        local target="${TARGETS[$index]}"
        echo "==> build ${target}"
        xmake build -j "$JOBS" "$target"
        local binary
        if ! binary="$(find_binary "$target")"; then
            echo "missing binary for ${target}" >&2
            return 1
        fi
        local corpus=".cache/fuzz/corpus-${name}"
        rm -rf "$corpus"
        mkdir -p "$corpus"
        if [[ -d "test/fuzz/corpus/${name}" ]]; then
            cp -a "test/fuzz/corpus/${name}/." "$corpus/"
        fi
        echo "==> ${target}: libFuzzer for ${TIME}s"
        "$binary" -max_total_time="$TIME" -print_final_stats=1 -detect_leaks=1 "$corpus"
        index=$((index + 1))
    done
}

restore_config() {
    if [[ "$LIBFUZZER" == "1" ]]; then
        xmake f -c -o build -m release >/dev/null 2>&1 || true
    fi
    if [[ "$KEEP_BUILD" != "1" && -d "$BUILD_DIR" && "$LIBFUZZER" == "1" ]]; then
        rm -rf "$BUILD_DIR"
    fi
}

trap restore_config EXIT INT TERM

if [[ "$LIBFUZZER" == "1" ]]; then
    run_libfuzzer
else
    run_standalone
fi

echo "fuzz: all targets completed"
