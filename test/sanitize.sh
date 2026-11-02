#!/usr/bin/env bash
#
# Sanitizer orchestration. Prefer `test/run.sh sanitize ...`.
#
#   test/run.sh sanitize asan   [suite...]
#   test/run.sh sanitize tsan   [suite...]
#   test/run.sh sanitize ubsan  [suite...]
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "${HERE}/sanitize.py" "$@"
