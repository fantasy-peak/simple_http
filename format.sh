#!/usr/bin/env bash
#
# 用 clang-format 格式化（或只检查）include/ 与 test/ 下的 C++ 源码。
#
# 用法：
#   ./format.sh              # 就地格式化
#   ./format.sh --check      # 只检查，有文件不符合格式则退出码 1（CI 用）
#   CLANG_FORMAT=/usr/bin/clang-format-18 ./format.sh
#
# 版本：默认优先 clang-format-23（本仓库的 .clang-format 按它校验），
# 找不到再退回 clang-format；可用环境变量 CLANG_FORMAT 显式指定。
#
# 只看 include/ 与 test/：其它目录（build/、.cache/、v2ray-cpp/ 等）不属于
# 本库源码。格式化读取仓库根目录的 .clang-format。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if [[ -n "${CLANG_FORMAT:-}" ]]; then
    fmt="$CLANG_FORMAT"
else
    fmt="$(command -v clang-format-23 || command -v clang-format || true)"
fi
if [[ -z "$fmt" || ! -x "$fmt" ]]; then
    echo "error: clang-format not found; set CLANG_FORMAT=/path/to/clang-format" >&2
    exit 1
fi

check=0
case "${1:-}" in
    --check|-n) check=1 ;;
    "") ;;
    *)
        echo "usage: $(basename "$0") [--check]" >&2
        exit 2
        ;;
esac

# include/ 与 test/ 下的 C/C++ 源文件。
mapfile -t files < <(find "$ROOT/include" "$ROOT/test" -type f \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.hh' -o -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
    -not -path '*/build/*' | sort)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no C++ source files found under include/ and test/"
    exit 0
fi

if [[ "$check" == 1 ]]; then
    changed=0
    for f in "${files[@]}"; do
        if ! "$fmt" --style=file "$f" | diff -q "$f" - >/dev/null; then
            echo "needs formatting: ${f#"$ROOT"/}"
            changed=$((changed + 1))
        fi
    done
    if [[ "$changed" -ne 0 ]]; then
        echo "$changed file(s) need formatting; run $0 to fix." >&2
        exit 1
    fi
    echo "all ${#files[@]} file(s) are properly formatted ($fmt)"
else
    "$fmt" --style=file -i "${files[@]}"
    echo "formatted ${#files[@]} file(s) with $fmt"
fi
