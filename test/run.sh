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
exec python3 "${HERE}/run.py" "$@"
