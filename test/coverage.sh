#!/usr/bin/env bash
#
# Line/branch coverage of include/simple_http, driven by the real test suites.
#
#   test/coverage.sh
#   COVERAGE_SUITES="unit server-diff" test/coverage.sh
#   COVERAGE_FAIL_UNDER=70 test/coverage.sh
#
# Builds an isolated build-cov with --coverage, runs the selected suites, then
# aggregates with gcovr. The normal build configuration is restored on exit.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${COVERAGE_BUILD_DIR:-build-cov}"
REPORT_DIR="${COVERAGE_REPORT_DIR:-.cache/coverage}"
JOBS="${COVERAGE_JOBS:-7}"
SUITES="${COVERAGE_SUITES:-unit openapi server-diff client-diff concurrency}"
PYTHON="${COVERAGE_PYTHON:-test/.venv/bin/python}"
GCOVR="${COVERAGE_GCOVR:-test/.venv/bin/gcovr}"
GCOV="${COVERAGE_GCOV:-$(command -v gcov-16 || command -v gcov)}"

if [[ ! -x "$GCOVR" ]]; then
    echo "error: gcovr not found at $GCOVR; install it with:" >&2
    echo "  test/.venv/bin/pip install -r test/requirements.txt" >&2
    exit 3
fi
if [[ ! -x "$PYTHON" ]]; then
    echo "error: $PYTHON not found; run 'test/run.sh doctor'" >&2
    exit 3
fi

mkdir -p "$REPORT_DIR"
restore_config() {
    xmake f -c -o build -m release >/dev/null 2>&1 || true
}
trap restore_config EXIT INT TERM

echo "==> configure $BUILD_DIR with --coverage"
xmake f -c -o "$BUILD_DIR" -m release \
    --cxxflags="--coverage -O0 -g" --ldflags="--coverage"

echo "==> run suites: $SUITES"
SUITE_LIST="$(printf '%s' "$SUITES" | tr ' ' ',')"
"$PYTHON" test/run.py \
    --bin-dir "$BUILD_DIR/linux/x86_64/release" \
    --jobs "$JOBS" --parallel 1 \
    --report-dir "$REPORT_DIR/test-run" \
    --suites "$SUITE_LIST"

echo "==> aggregate coverage"
FILTER="$ROOT/include/simple_http/"
# GCC's gcov occasionally emits negative hit counts for inlined/templated
# lines (gcc bug 68080); warn instead of aborting the whole report.
COMMON=(-r "$ROOT" --filter "$FILTER" --gcov-executable "$GCOV" \
    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file "$BUILD_DIR")
"$GCOVR" "${COMMON[@]}" --txt --txt-summary --output "$REPORT_DIR/coverage.txt"
"$GCOVR" "${COMMON[@]}" --html-details "$REPORT_DIR/coverage.html" >/dev/null

if [[ -n "${COVERAGE_FAIL_UNDER:-}" ]]; then
    "$GCOVR" "${COMMON[@]}" --fail-under-line "$COVERAGE_FAIL_UNDER" >/dev/null
fi

echo
tail -n 8 "$REPORT_DIR/coverage.txt"
echo
echo "coverage report: $REPORT_DIR/coverage.txt"
echo "coverage html:   $REPORT_DIR/coverage.html"
