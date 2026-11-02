#!/usr/bin/env bash
#
# Unified test entry point. All orchestration lives in test/run.py; this wrapper
# only makes the script convenient to run from anywhere.
#
#   test/run.sh fast | standard | full
#   test/run.sh list | doctor
#   test/run.sh <suite> [...]
#
# See test/README.md and `test/run.sh --help`.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Sanitizer builds need their own isolated worktree/build configuration, so
# they live in a separate orchestrator while this stays the single entry point.
if [[ "${1:-}" == "sanitize" ]]; then
    shift
    exec python3 "${HERE}/sanitize.py" "$@"
fi

exec python3 "${HERE}/run.py" "$@"
