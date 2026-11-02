#!/usr/bin/env python3
"""Unified test runner for simple_http.

    test/run.sh fast        # unit only
    test/run.sh standard    # unit + OpenAPI
    test/run.sh full        # + client/server differential + conformance + stress

    test/run.sh list
    test/run.sh doctor
    test/run.sh <suite> [...]           # e.g. unit, client-diff, server-diff
    test/run.sh unit '[h2]'             # Catch2 filter after the suite name

Exit codes: 0 pass (skips allowed), 1 failure, 2 usage, 3 environment.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

TEST = Path(__file__).resolve().parent
ROOT = TEST.parent
BIN_DIR = ROOT / "build" / "linux" / "x86_64" / "release"
VENV_PYTHON = TEST / ".venv" / "bin" / "python"


@dataclass(frozen=True)
class Suite:
    name: str
    tier: str
    targets: tuple[str, ...]
    kind: str          # binary | python | bash
    path: str
    args: tuple[str, ...] = ()
    tools: tuple[str, ...] = ()
    packages: tuple[str, ...] = ()
    timeout: int = 900


SUITES = (
    Suite("unit", "fast", ("test_unit",), "binary", "test_unit", timeout=300),
    Suite("openapi", "standard", ("diff_openapi_server",), "python",
          "diff/openapi/run.py", packages=("httpx", "jsonschema")),
    Suite("client-diff", "full",
          ("diff_client_basic", "diff_client_body", "diff_client_policy",
           "diff_client_ws", "diff_client_h3"), "python", "diff/client/run.py",
          tools=("go",)),
    Suite("server-diff", "full", ("diff_server_cpp",), "python",
          "diff/server/run.py", tools=("go",)),
    Suite("concurrency", "full", ("test_sanitizer_concurrency",), "binary",
          "test_sanitizer_concurrency", timeout=300),
    Suite("conformance", "full", ("fixture_conformance_server",), "bash",
          "external/conformance/run.sh",
          tools=("h2spec", "h3spec-linux-x86_64", "docker", "curl"), timeout=1800),
    Suite("stress", "full", ("fixture_conformance_server",), "bash",
          "external/stress/run.sh", args=("cpp", "--no-conformance"),
          tools=("h2load", "k6"), timeout=3600),
)

TIERS = {
    "fast": ("fast",),
    "standard": ("fast", "standard"),
    "full": ("fast", "standard", "full"),
}


def python_executable() -> str:
    return str(VENV_PYTHON) if VENV_PYTHON.exists() else sys.executable


def have_package(package: str) -> bool:
    try:
        result = subprocess.run([python_executable(), "-c", f"import {package}"],
                                capture_output=True, timeout=30)
        return result.returncode == 0
    except OSError:
        return False


def missing_deps(suite: Suite) -> list[str]:
    missing = [tool for tool in suite.tools if shutil.which(tool) is None]
    missing += [f"python:{package}" for package in suite.packages if not have_package(package)]
    return missing


def build_targets(targets: tuple[str, ...], jobs: int, log_path: Path) -> bool:
    if not targets:
        return True
    with open(log_path, "a", encoding="utf-8") as log:
        for target in targets:
            log.write(f"\n$ xmake build -j {jobs} {target}\n")
            log.flush()
            result = subprocess.run(["xmake", "build", "-j", str(jobs), target],
                                    cwd=str(ROOT), stdout=log, stderr=subprocess.STDOUT)
            if result.returncode != 0:
                return False
    return True


def run_suite(suite: Suite, args: list[str], report_dir: Path,
              verbose: bool) -> tuple[str, int, float]:
    log_path = report_dir / f"{suite.name}.log"
    start = time.monotonic()
    env = dict(os.environ)
    env["SIMPLE_HTTP_TEST_BIN_DIR"] = str(BIN_DIR)
    env["SIMPLE_HTTP_TEST_WORKDIR"] = str(report_dir / suite.name)
    env["SIMPLE_HTTP_TEST_SUITE"] = suite.name

    if suite.kind == "binary":
        binary = BIN_DIR / suite.path
        if not binary.exists():
            log_path.write_text(f"missing binary {binary}\n", encoding="utf-8")
            return "FAIL", 1, time.monotonic() - start
        command = [str(binary), *suite.args, *args]
    elif suite.kind == "python":
        command = [python_executable(), str(TEST / suite.path), *suite.args]
    else:
        command = ["bash", str(TEST / suite.path), *suite.args]

    if verbose:
        print(f"$ {' '.join(command)}")
    try:
        with open(log_path, "w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=str(ROOT), stdout=log,
                                    stderr=subprocess.STDOUT, env=env, timeout=suite.timeout)
    except subprocess.TimeoutExpired:
        with open(log_path, "a", encoding="utf-8") as log:
            log.write(f"\nTIMEOUT after {suite.timeout}s\n")
        return "FAIL", 124, time.monotonic() - start
    except OSError as error:
        log_path.write_text(f"{error}\n", encoding="utf-8")
        return "FAIL", 3, time.monotonic() - start
    if verbose:
        print(log_path.read_text(encoding="utf-8", errors="replace"))
    status = "PASS" if result.returncode == 0 else "FAIL"
    return status, result.returncode, time.monotonic() - start


def tail(path: Path, lines: int = 40) -> str:
    try:
        data = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(data[-lines:])


def selected_suites(args: argparse.Namespace) -> list[Suite]:
    wanted = args.suite
    if wanted and wanted in TIERS:
        tiers = TIERS[wanted]
        suites = [suite for suite in SUITES if suite.tier in tiers]
    elif wanted:
        suites = [suite for suite in SUITES if suite.name == wanted]
    else:
        suites = [suite for suite in SUITES if suite.tier in TIERS["fast"]]
    if args.filter:
        suites = [suite for suite in suites if args.filter in suite.name]
    return suites


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", nargs="?", default=None)
    parser.add_argument("catch_filter", nargs="?", default=None)
    parser.add_argument("--filter", default=None)
    parser.add_argument("--report-dir", default=None)
    parser.add_argument("--bin-dir", default=None,
                        help="directory holding the test binaries (default: build/linux/x86_64/release)")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--allow-skip", action="store_true")
    parser.add_argument("--strict-deps", action="store_true")
    parser.add_argument("--jobs", type=int, default=12)
    parser.add_argument("--parallel", type=int, default=3,
                        help="run independent suites concurrently (default 3; 1 = serial). "
                             "conformance and stress always share one serial lane.")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--doctor", action="store_true")
    args = parser.parse_args()

    global BIN_DIR
    if args.bin_dir:
        BIN_DIR = Path(args.bin_dir)

    if args.suite == "list":
        args.list = True
    if args.suite == "doctor":
        args.doctor = True

    if args.list:
        for suite in SUITES:
            print(f"{suite.name:14} tier={suite.tier:8} targets={','.join(suite.targets) or '-'}")
        return 0

    if args.doctor:
        for suite in SUITES:
            missing = missing_deps(suite)
            print(f"{suite.name:14} " + ("ok" if not missing else "missing: " + ", ".join(missing)))
        return 0

    suites = selected_suites(args)
    if not suites:
        print("no matching suite", file=sys.stderr)
        return 2

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    report_dir = Path(args.report_dir) if args.report_dir else (
        ROOT / ".cache" / "test-run" / f"{stamp}-{os.getpid()}")
    report_dir.mkdir(parents=True, exist_ok=True)
    build_log = report_dir / "build.log"

    targets: list[str] = []
    for suite in suites:
        for target in suite.targets:
            if target not in targets:
                targets.append(target)
    if not args.no_build:
        if not build_targets(tuple(targets), args.jobs, build_log):
            print(f"build failed; see {build_log}", file=sys.stderr)
            print(tail(build_log), file=sys.stderr)
            return 3

    results: list[tuple[str, str, float]] = []
    exit_code = 0

    def suite_args_for(suite: Suite) -> list[str]:
        if args.catch_filter and suite.name == "unit":
            return [args.catch_filter]
        return []

    runnable: list[Suite] = []
    for suite in suites:
        missing = missing_deps(suite)
        if missing:
            message = ", ".join(missing)
            print(f"SKIP  {suite.name}: missing {message}")
            (report_dir / f"{suite.name}.log").write_text(f"SKIP: missing {message}\n",
                                                          encoding="utf-8")
            results.append((suite.name, "SKIP", 0.0))
            if args.strict_deps:
                exit_code = 3
        else:
            runnable.append(suite)

    # conformance and stress both bind 7788-7792 and kill leftover fixtures, so
    # they share one serial lane. Every other suite uses a disjoint port block
    # (or an ephemeral port) and can run in parallel.
    serial_names = {"conformance", "stress"}
    lanes: list[list[Suite]] = []
    serial_lane = [suite for suite in runnable if suite.name in serial_names]
    if serial_lane:
        lanes.append(serial_lane)
    lanes.extend([suite] for suite in runnable if suite.name not in serial_names)

    cancelled = threading.Event()

    def run_lane(lane: list[Suite]) -> list[tuple[Suite, str, int, float]]:
        outcome: list[tuple[Suite, str, int, float]] = []
        for suite in lane:
            if cancelled.is_set():
                outcome.append((suite, "SKIP", 0, 0.0))
                continue
            status, code, elapsed = run_suite(suite, suite_args_for(suite), report_dir, args.verbose)
            outcome.append((suite, status, code, elapsed))
            if code != 0 and args.fail_fast:
                cancelled.set()
        return outcome

    if args.parallel <= 1 or len(lanes) <= 1:
        outcomes = []
        for lane in lanes:
            outcomes.extend(run_lane(lane))
    else:
        outcomes = []
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=min(args.parallel, len(lanes))) as pool:
            futures = [pool.submit(run_lane, lane) for lane in lanes]
            for future in concurrent.futures.as_completed(futures):
                lane_outcome = future.result()
                for suite, status, code, elapsed in lane_outcome:
                    print(f"{status}  {suite.name} ({elapsed:.1f}s)")
                    results.append((suite.name, status, elapsed))
                    if code != 0:
                        print(tail(report_dir / f"{suite.name}.log"), file=sys.stderr)
                outcomes.extend(lane_outcome)

    if args.parallel <= 1 or len(lanes) <= 1:
        for suite, status, code, elapsed in outcomes:
            print(f"{status}  {suite.name} ({elapsed:.1f}s)")
            results.append((suite.name, status, elapsed))
            if code != 0:
                print(tail(report_dir / f"{suite.name}.log"), file=sys.stderr)

    for _, status, code, _ in outcomes:
        if status == "SKIP":
            continue
        if code != 0:
            exit_code = 1

    order = {suite.name: index for index, suite in enumerate(suites)}
    results.sort(key=lambda item: order.get(item[0], 1 << 30))
    print("\nsummary:")
    for name, status, elapsed in results:
        print(f"  {status:5} {name:14} {elapsed:.1f}s")
    print(f"logs: {report_dir}")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
