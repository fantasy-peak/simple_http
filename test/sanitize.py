#!/usr/bin/env python3
"""Sanitizer orchestration for the current test suite.

It does not define new tests. It builds the existing suite with ASan/TSan/UBSan
in an isolated worktree (or a caller-provided build directory), runs the same
`test/run.py` suites, and scans every log for sanitizer findings.

    test/run.sh sanitize asan   [suite...]
    test/run.sh sanitize tsan   [suite...]
    test/run.sh sanitize ubsan  [suite...]

Defaults are deliberately small for ASan: the unit suite is memory-hungry when
compiled with instrumentation, so the default `--jobs` is 7. A full `-j14`
build OOMs on a typical WSL2 machine.

Suite names are exactly the ones in test/run.py plus `conformance` and `stress`:

    unit openapi client-diff server-diff concurrency conformance stress
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

TEST = Path(__file__).resolve().parent
ROOT = TEST.parent
VENV = TEST / ".venv"
CACHE = ROOT / ".cache" / "sanitize"

ALL_SUITES = {
    "unit", "openapi", "client-diff", "server-diff", "concurrency",
    "conformance", "stress",
}

DEFAULT_SUITES = {
    # PR-sized matrix. conformance/stress are nightly/manual because they are
    # far heavier under instrumentation.
    "asan": ["unit", "openapi", "client-diff", "server-diff", "concurrency"],
    "tsan": ["unit", "concurrency", "server-diff"],
    "ubsan": ["unit", "client-diff", "server-diff"],
}

# sanitizer name -> (compiler -fsanitize list, extra safety flags)
FLAGS = {
    "asan": ("address,undefined", ["-fno-sanitize-recover=undefined"]),
    "tsan": ("thread", []),
    "ubsan": ("undefined", ["-fno-sanitize-recover=all"]),
}

PATTERNS = {
    "asan": [
        "ERROR: AddressSanitizer",
        "ERROR: LeakSanitizer",
        "SUMMARY: AddressSanitizer",
        "SUMMARY: UndefinedBehaviorSanitizer",
        "runtime error:",
    ],
    "ubsan": [
        "runtime error:",
        "SUMMARY: UndefinedBehaviorSanitizer",
    ],
    "tsan": [
        "WARNING: ThreadSanitizer",
        "ThreadSanitizer: data race",
        "ThreadSanitizer: lock-order-inversion",
        "ThreadSanitizer: heap-use-after-free",
    ],
}

STRESS_LIMITS = {
    "LOAD_REQUESTS": "20000",
    "LOAD_CLIENTS": "50",
    "LOAD_STREAMS": "10",
    "WS_MSG_VUS": "10",
    "WS_MSG_ITERS": "100",
    "WS_MSG_WANT": "100",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sanitizer", choices=sorted(FLAGS))
    parser.add_argument("suites", nargs="*", help="suite names (default: per sanitizer)")
    parser.add_argument("--jobs", type=int, default=7,
                        help="compile parallelism (default 7; ASan unit OOMs at 14)")
    parser.add_argument("--build-dir", default=None,
                        help="build in the current repo with this xmake builddir instead of a worktree")
    parser.add_argument("--keep-worktree", action="store_true")
    parser.add_argument("--report-dir", default=None)
    parser.add_argument("--parallel", type=int, default=1,
                        help="run independent sanitizer suites concurrently (default 1; "
                             "instrumented processes use much more memory)")
    parser.add_argument("--with-autobahn", action="store_true",
                        help="include Autobahn in the conformance run")
    parser.add_argument("--allow-skip", action="store_true",
                        help="missing third-party conformance tools are a SKIP, not a failure")
    return parser.parse_args()


def run(cmd: list[str], *, cwd: Path, env: dict[str, str] | None = None,
        log_path: Path | None = None, timeout: int | None = None) -> int:
    try:
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            with open(log_path, "w", encoding="utf-8") as log:
                result = subprocess.run(cmd, cwd=str(cwd), env=env, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=timeout)
        else:
            result = subprocess.run(cmd, cwd=str(cwd), env=env, text=True,
                                    capture_output=True, timeout=timeout)
            if result.returncode != 0:
                print(result.stdout[-2000:], file=sys.stderr)
                print(result.stderr[-2000:], file=sys.stderr)
    except subprocess.TimeoutExpired:
        if log_path is not None:
            with open(log_path, "a", encoding="utf-8") as log:
                log.write(f"\nTIMEOUT after {timeout}s\n")
        return 124
    return result.returncode


def sanitizer_env(name: str, root: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["SIMPLE_HTTP_TEST_STOP_GRACE_MS"] = "15000"
    supp = root / "test" / "sanitizer"
    if name == "asan":
        env["ASAN_OPTIONS"] = (
            "detect_leaks=1:abort_on_error=1:strict_string_checks=1:"
            "detect_stack_use_after_return=1"
        )
        env["LSAN_OPTIONS"] = f"suppressions={supp / 'lsan.supp'}"
        env["UBSAN_OPTIONS"] = (
            f"halt_on_error=1:print_stacktrace=1:suppressions={supp / 'ubsan.supp'}"
        )
    elif name == "tsan":
        env["TSAN_OPTIONS"] = (
            "halt_on_error=1:history_size=7:second_deadlock_stack=1:"
            f"suppressions={supp / 'tsan.supp'}"
        )
    else:  # ubsan
        env["UBSAN_OPTIONS"] = (
            f"halt_on_error=1:print_stacktrace=1:suppressions={supp / 'ubsan.supp'}"
        )
    return env


def configure(root: Path, name: str, build_dir: Path) -> None:
    sanitize, extra = FLAGS[name]
    cxxflags = " ".join([f"-fsanitize={sanitize}", *extra,
                         "-fno-omit-frame-pointer", "-g", "-O1"])
    ldflags = f"-fsanitize={sanitize}"
    cmd = ["xmake", "f", "-c", "-o", str(build_dir), "-m", "debug",
           f"--cxxflags={cxxflags}", f"--ldflags={ldflags}"]
    if run(cmd, cwd=root) != 0:
        raise RuntimeError(f"xmake configure failed in {root}")


def create_worktree(name: str, cache: Path) -> Path:
    path = cache / f"worktree-{name}"
    subprocess.run(["git", "worktree", "remove", "--force", str(path)],
                   cwd=str(ROOT), capture_output=True)
    subprocess.run(["git", "worktree", "prune"], cwd=str(ROOT), capture_output=True)
    if path.exists():
        shutil.rmtree(path, ignore_errors=True)
    result = subprocess.run(["git", "worktree", "add", "--detach", str(path), "HEAD"],
                            cwd=str(ROOT), text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(f"git worktree add failed:\n{result.stderr}")
    if VENV.exists() and not (path / "test" / ".venv").exists():
        (path / "test" / ".venv").symlink_to(VENV)
    return path


def remove_worktree(path: Path) -> None:
    subprocess.run(["git", "worktree", "remove", "--force", str(path)],
                   cwd=str(ROOT), capture_output=True)
    subprocess.run(["git", "worktree", "prune"], cwd=str(ROOT), capture_output=True)


def run_current_suites(root: Path, bin_dir: Path, suites: list[str], jobs: int,
                       report_dir: Path, env: dict[str, str],
                       parallel: int = 1) -> int:
    cmd = [sys.executable, str(root / "test" / "run.py"),
           "--bin-dir", str(bin_dir), "--jobs", str(jobs),
           "--parallel", str(parallel),
           "--report-dir", str(report_dir),
           "--suites", ",".join(suites)]
    return run(cmd, cwd=root, env=env, log_path=report_dir / "runner.log")


def run_conformance(root: Path, bin_dir: Path, jobs: int, report_dir: Path,
                    env: dict[str, str], with_autobahn: bool,
                    allow_skip: bool) -> int:
    missing = [tool for tool in ("h2spec", "h3spec-linux-x86_64", "docker", "curl")
               if shutil.which(tool) is None]
    if missing:
        print(f"SKIP  conformance: missing {', '.join(missing)}")
        return 0 if allow_skip else 1
    if run(["xmake", "build", "-j", str(jobs), "fixture_conformance_server"],
           cwd=root, env=env, log_path=report_dir / "conformance-build.log") != 0:
        return 1
    conformance_env = dict(env)
    conformance_env["SERVER_BIN"] = str(bin_dir / "fixture_conformance_server")
    suites = ["h2spec", "h1spec", "h3spec"]
    if with_autobahn:
        suites.append("autobahn")
    code = run(["bash", str(root / "test" / "external" / "conformance" / "run.sh"), *suites],
               cwd=root, env=conformance_env,
               log_path=report_dir / "conformance.log", timeout=3600)
    # The script redirects the fixture's stdout/stderr to its own cache dir;
    # copy that log into the sanitizer report so the scanner sees ASan/LSan.
    server_log = root / ".cache" / "conformance" / "server.log"
    if server_log.exists():
        shutil.copy(server_log, report_dir / "conformance-server.log")
    return code


def run_stress(root: Path, bin_dir: Path, jobs: int,
               report_dir: Path, env: dict[str, str], parallel: int = 1) -> int:
    stress_env = dict(env)
    stress_env.update(STRESS_LIMITS)
    stress_env["SERVER_BIN"] = str(bin_dir / "fixture_conformance_server")
    code = run_current_suites(root, bin_dir, ["stress"], jobs, report_dir, stress_env,
                              parallel)
    server_log = root / ".cache" / "stress" / "server.log"
    if server_log.exists():
        shutil.copy(server_log, report_dir / "stress-server.log")
    return code


def scan(report_dir: Path, name: str) -> list[dict[str, str]]:
    findings: list[dict[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for path in sorted(report_dir.rglob("*")):
        if not path.is_file() or path.suffix not in {".log", ".stderr", ".txt"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        relative = str(path.relative_to(report_dir))
        for pattern in PATTERNS[name]:
            if pattern in text and (relative, pattern) not in seen:
                seen.add((relative, pattern))
                findings.append({"file": relative, "pattern": pattern, "severity": "error"})
        if "STOP_TIMEOUT: forced SIGKILL" in text and (relative, "STOP_TIMEOUT") not in seen:
            seen.add((relative, "STOP_TIMEOUT"))
            findings.append({"file": relative, "pattern": "STOP_TIMEOUT",
                             "severity": "warning"})
    return findings


def write_report(report_dir: Path, name: str, jobs: int, root: Path,
                 suites: list[str], statuses: dict[str, int],
                 findings: list[dict[str, str]]) -> Path:
    data = {
        "sanitizer": name,
        "jobs": jobs,
        "root": str(root),
        "suites": [{"name": suite, "exit_code": statuses.get(suite, 0)} for suite in suites],
        "findings": findings,
        "passed": all(code == 0 for code in statuses.values())
                  and not any(f["severity"] == "error" for f in findings),
    }
    (report_dir / "sanitize-report.json").write_text(
        json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    lines = [f"# sanitizer report: {name}", "",
             f"- jobs: {jobs}", f"- root: `{root}`", ""]
    lines += ["| suite | exit |", "|---|---|"]
    for suite in suites:
        code = statuses.get(suite, statuses.get("runner", 0))
        lines.append(f"| {suite} | {code} |")
    lines += ["", "## findings", ""]
    if not findings:
        lines.append("No sanitizer findings.")
    else:
        lines += ["| severity | file | pattern |", "|---|---|---|"]
        for finding in findings:
            lines.append(f"| {finding['severity']} | `{finding['file']}` | "
                         f"`{finding['pattern']}` |")
    path = report_dir / "sanitize-report.md"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def main() -> int:
    args = parse_args()
    name = args.sanitizer
    suites = args.suites or DEFAULT_SUITES[name]
    unknown = set(suites) - ALL_SUITES
    if unknown:
        print(f"unknown suite(s): {', '.join(sorted(unknown))}", file=sys.stderr)
        return 2
    if args.jobs <= 0:
        print("--jobs must be positive", file=sys.stderr)
        return 2

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    report_dir = (Path(args.report_dir) if args.report_dir else (
        CACHE / name / stamp)).resolve()
    report_dir.mkdir(parents=True, exist_ok=True)

    worktree: Path | None = None
    root = ROOT
    build_dir = Path(args.build_dir).resolve() if args.build_dir else None
    if build_dir is None:
        worktree = create_worktree(name, CACHE)
        root = worktree
        build_dir = root / "build"

    statuses: dict[str, int] = {}
    try:
        configure(root, name, build_dir)
        env = sanitizer_env(name, root)
        bin_dir = build_dir / "linux" / "x86_64" / "debug"
        print(f"{name}: root={root} bin={bin_dir} jobs={args.jobs}")

        current = [suite for suite in suites
                   if suite not in {"conformance", "stress"}]
        if current:
            statuses["runner"] = run_current_suites(
                root, bin_dir, current, args.jobs, report_dir, env, args.parallel)
        if "conformance" in suites:
            statuses["conformance"] = run_conformance(
                root, bin_dir, args.jobs, report_dir, env, args.with_autobahn,
                args.allow_skip)
        if "stress" in suites:
            statuses["stress"] = run_stress(
                root, bin_dir, args.jobs, report_dir, env, args.parallel)

        findings = scan(report_dir, name)
        report = write_report(report_dir, name, args.jobs, root, suites, statuses, findings)
        errors = [finding for finding in findings if finding["severity"] == "error"]
        failed = any(code != 0 for code in statuses.values()) or bool(errors)
        print(f"\n{report}")
        for finding in findings:
            print(f"{finding['severity'].upper()}  {finding['file']}: {finding['pattern']}")
        print(f"{name}: " + ("FAIL" if failed else "PASS")
              + f" (report: {report_dir})")
        return 1 if failed else 0
    except RuntimeError as error:
        print(f"FAIL  {error}", file=sys.stderr)
        return 3
    finally:
        if worktree is not None:
            if args.keep_worktree:
                print(f"worktree kept: {worktree}")
            else:
                remove_worktree(worktree)
        elif build_dir is not None and args.build_dir:
            # Restore the normal build configuration. This intentionally does
            # not try to preserve a custom user config; the default is what the
            # repository's normal test runs use.
            subprocess.run(["xmake", "f", "-c", "-o", "build", "-m", "release"],
                           cwd=str(ROOT), capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
