#!/usr/bin/env python3
"""Unified test runner for simple_http.

One entry point, three tiers, machine-readable results:

    test/run.sh fast        # pure logic + in-process C++ (no venv needed)
    test/run.sh standard    # + independent Python peers (needs test/.venv)
    test/run.sh full        # + Go differentials + external conformance/stress

    test/run.sh list
    test/run.sh doctor
    test/run.sh <suite> [...]          # e.g. unit, server-conformance, client-cross
    test/run.sh unit --catch-filter '[h2]'

Common options:
    --filter GLOB          only run matching suite names
    --exclude GLOB         skip matching suite names
    --report-dir DIR       report/log directory (default .cache/test-run/<timestamp>)
    --no-build             do not invoke xmake
    --bin-dir DIR          where the test binaries live
    --python PATH          interpreter for Python suites (default test/.venv)
    --jobs N               xmake -j value
    --timeout SEC          override every suite timeout
    --fail-fast            stop after the first failure
    --strict-deps          missing dependencies fail instead of SKIP
    --allow-skip           an explicitly selected suite may SKIP
    --catch-filter ARG     pass an extra argument to Catch2 suites
    --verbose              echo each suite log while it runs

Exit codes: 0 pass (skips allowed), 1 test failure, 2 usage error,
3 environment/build error (or SKIP under --strict-deps).
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from copy import deepcopy
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

TEST = Path(__file__).resolve().parent
ROOT = TEST.parent
CACHE = ROOT / ".cache" / "test-run"


# --------------------------------------------------------------------------- #
# Suite registry
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class Suite:
    name: str
    tier: str
    targets: tuple[str, ...]
    kind: str                      # binary | python | bash
    path: str                      # target name or script path under test/
    args: tuple[str, ...] = ()
    ports: tuple[int, ...] = ()
    packages: tuple[str, ...] = ()
    tools: tuple[str, ...] = ()
    timeout: int = 300
    catch2: bool = False


SUITES: tuple[Suite, ...] = (
    # ---- fast -------------------------------------------------------------
    Suite("unit", "fast", ("test_unit",), "binary", "test_unit",
          ports=(), timeout=180, catch2=True),
    Suite("server-regression", "fast", ("test_server_regression",), "binary",
          "test_server_regression", timeout=300, catch2=True),
    Suite("client", "fast", ("test_client",), "binary", "test_client",
          ports=(27910, 27911, 27912), timeout=300),
    Suite("h1-lifetime", "fast", ("test_h1_lifetime",), "binary", "test_h1_lifetime",
          args=("7826",), ports=(7826,), timeout=120),
    Suite("readme", "fast", ("example_readme",), "binary", "example_readme", timeout=60),

    # ---- standard ---------------------------------------------------------
    Suite("threading", "standard", ("test_threading",), "binary", "test_threading",
          args=("--quick",), ports=(27920,), timeout=300),
    Suite("server-conformance", "standard", ("fixture_server",), "python",
          "server/run.py", ports=tuple(range(7788, 7795)),
          packages=("httpx", "h2", "websockets"), timeout=600),
    Suite("h2-websocket", "standard", ("fixture_server",), "python",
          "server/h2_websocket.py", ports=tuple(range(7788, 7795)),
          packages=("h2", "hpack", "hyperframe"), timeout=600),
    Suite("h2-window-leak", "standard", ("fixture_server",), "python",
          "server/h2_window_leak.py", ports=tuple(range(7788, 7795)),
          packages=("hpack", "hyperframe"), timeout=300),
    Suite("client-cross", "standard",
          ("probe_client_cross", "probe_ws_cross", "probe_h3_cross"), "python",
          "client/run.py", ports=(27920, 27921, 27922),
          packages=("websockets", "aioquic"), timeout=600),
    Suite("client-checks", "standard",
          ("probe_h1_duplex", "probe_body_cap", "probe_read_all_timeout",
           "probe_retry", "probe_h2_limit", "test_h1_lifetime"), "python",
          "client/checks.py", ports=(7813, 7822, 7823, 7824, 7825, 7826),
          packages=("h2",), timeout=300),
    Suite("openapi", "standard", ("example_openapi",), "python",
          "examples/openapi_verify.py", ports=(7795,),
          packages=("httpx",), timeout=300),

    # ---- full -------------------------------------------------------------
    Suite("framework-diff", "full", ("fixture_framework_server",), "python",
          "differential/run.py", args=("framework",), ports=(7798, 7799),
          packages=("httpx", "cryptography"), tools=("go", "openssl"), timeout=600),
    Suite("client-diff", "full", ("probe_client_diff", "fixture_framework_server"), "python",
          "differential/run.py", args=("client",), ports=(7802, 7803, 7804),
          tools=("go",), timeout=600),
    Suite("external-conformance", "full", ("fixture_server",), "bash",
          "external/conformance/run.sh", ports=(7788, 7790, 7791, 7792),
          tools=("h2spec", "h3spec-linux-x86_64", "docker", "curl"), timeout=1800),
    Suite("stress", "full", ("fixture_server",), "bash",
          "external/stress/run.sh", args=("cpp", "--no-conformance"),
          ports=tuple(range(7788, 7793)), tools=("h2load", "k6"), timeout=3600),

    # ---- manual -----------------------------------------------------------
    Suite("bench", "manual", ("fixture_server",), "bash", "external/bench/tls_bench.sh",
          ports=(7790, 7791, 7797), tools=("h2load",), timeout=3600),
)

TIERS = {
    "fast": ("fast",),
    "standard": ("fast", "standard"),
    "full": ("fast", "standard", "full"),
}

SUITE_BY_NAME = {suite.name: suite for suite in SUITES}


# --------------------------------------------------------------------------- #
# Results
# --------------------------------------------------------------------------- #

@dataclass
class Check:
    name: str
    ok: bool
    detail: str = ""
    skipped: bool = False


@dataclass
class Result:
    suite: Suite
    status: str = "pass"           # pass | fail | skip | error
    returncode: int | None = None
    duration: float = 0.0
    checks: list[Check] = field(default_factory=list)
    log: Path | None = None
    report: Path | None = None
    catch2_xml: Path | None = None
    skip_reason: str = ""
    diagnostics: dict = field(default_factory=dict)

    @property
    def failed(self) -> int:
        return sum(1 for check in self.checks if not check.ok)

    @property
    def passed(self) -> int:
        return sum(1 for check in self.checks if check.ok and not check.skipped)

    @property
    def skipped_checks(self) -> int:
        return sum(1 for check in self.checks if check.skipped)


# --------------------------------------------------------------------------- #
# Environment
# --------------------------------------------------------------------------- #

def find_bin_dir(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).expanduser().resolve()
    env = os.environ.get("SIMPLE_HTTP_TEST_BIN_DIR")
    if env:
        return Path(env).expanduser().resolve()
    candidates = (
        ROOT / "build" / "linux" / "x86_64" / "release",
        ROOT / "build" / "linux" / "x86_64" / "debug",
        ROOT / "build-dbg" / "linux" / "x86_64" / "debug",
    )
    for candidate in candidates:
        if (candidate / "fixture_server").exists() or (candidate / "test_unit").exists():
            return candidate
    return candidates[0]


def find_python(explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit).expanduser()
        return path if path.exists() else None
    env = os.environ.get("SIMPLE_HTTP_TEST_PYTHON")
    if env:
        path = Path(env).expanduser()
        return path if path.exists() else None
    for candidate in (TEST / ".venv" / "bin" / "python",
                      TEST / "python" / ".venv" / "bin" / "python"):
        if candidate.exists():
            return candidate
    return None


def tool_missing(tools: tuple[str, ...]) -> list[str]:
    return [tool for tool in tools if shutil.which(tool) is None]


_package_cache: dict[tuple[str, ...], str] = {}


def packages_missing(python: Path | None, packages: tuple[str, ...]) -> str | None:
    """Returns None when importable, otherwise a human-readable reason."""
    if not packages:
        return None
    if python is None:
        return "no virtualenv (python3 -m venv test/.venv && test/.venv/bin/pip install -r test/requirements.txt)"
    key = packages
    if key in _package_cache:
        return _package_cache[key] or None
    module_names = [package.split("[", 1)[0].replace("-", "_") for package in packages]
    code = "import " + ", ".join(module_names)
    run = subprocess.run([str(python), "-c", code], capture_output=True, text=True)
    reason = None
    if run.returncode != 0:
        reason = (f"missing Python packages: {', '.join(packages)} "
                  f"(pip install -r test/requirements.txt)")
    _package_cache[key] = reason or ""
    return reason


def port_busy(ports: tuple[int, ...]) -> list[str]:
    if not ports or shutil.which("ss") is None:
        return []
    run = subprocess.run(["ss", "-ltnup"], capture_output=True, text=True)
    if run.returncode != 0:
        return []
    hits = []
    for line in run.stdout.splitlines():
        for port in ports:
            if re.search(rf":{port}\b", line):
                hits.append(f":{port}  {line.strip()}")
                break
    return hits


def port_diagnostics(ports: tuple[int, ...]) -> str:
    if not ports or shutil.which("ss") is None:
        return ""
    run = subprocess.run(["ss", "-ltnup"], capture_output=True, text=True)
    if run.returncode != 0:
        return ""
    lines = [line for line in run.stdout.splitlines()
             if any(re.search(rf":{port}\b", line) for port in ports)]
    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Execution
# --------------------------------------------------------------------------- #

def suite_command(suite: Suite, options: argparse.Namespace, python: Path | None,
                  catch_args: list[str]) -> list[str]:
    if suite.kind == "binary":
        binary = options.bin_dir / suite.path
        return [str(binary), *catch_args, *suite.args, *options.suite_args]
    if suite.kind == "python":
        assert python is not None
        return [str(python), str(TEST / suite.path), *suite.args, *options.suite_args]
    return ["bash", str(TEST / suite.path), *suite.args, *options.suite_args]


def build_targets(targets: list[str], options: argparse.Namespace, run_dir: Path,
                  build_log: Path) -> tuple[set[str], list[str]]:
    """Builds each target atomically. Returns (built, failures)."""
    built: set[str] = set()
    failures: list[str] = []
    if options.no_build:
        return set(targets), failures
    if shutil.which("xmake") is None:
        return set(), ["xmake not found"]
    with open(build_log, "a", encoding="utf-8") as log:
        for target in targets:
            print(f"[build] {target}", flush=True)
            run = subprocess.run(["xmake", "build", f"-j{options.jobs}", target],
                                 cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            if run.returncode == 0:
                built.add(target)
            else:
                failures.append(target)
                print(f"[build] FAILED {target} (see {build_log})", file=sys.stderr)
    return built, failures


def run_suite(suite: Suite, options: argparse.Namespace, python: Path | None,
              run_dir: Path, catch_args: list[str]) -> Result:
    result = Result(suite=suite)
    result.log = run_dir / f"{suite.name}.log"
    if suite.catch2:
        result.catch2_xml = run_dir / f"{suite.name}.xml"
        catch_args = [*catch_args, "-r", "console", "-r", f"junit::out={result.catch2_xml}"]

    command = suite_command(suite, options, python, catch_args)
    result.report = run_dir / f"{suite.name}.json"

    env = os.environ.copy()
    env["SIMPLE_HTTP_TEST_BIN_DIR"] = str(options.bin_dir)
    env["SIMPLE_HTTP_TEST_SUITE"] = suite.name
    env["SIMPLE_HTTP_TEST_REPORT"] = str(result.report)
    env["SIMPLE_HTTP_TEST_WORKDIR"] = str(run_dir / suite.name)
    (run_dir / suite.name).mkdir(parents=True, exist_ok=True)

    started = time.monotonic()
    timeout = options.timeout or suite.timeout
    print(f"RUN   {suite.name}", flush=True)
    with open(result.log, "w", encoding="utf-8") as log:
        proc = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
        try:
            result.returncode = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                result.returncode = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            result.returncode = -signal.SIGKILL
            result.diagnostics["timeout"] = timeout
    result.duration = time.monotonic() - started

    if options.verbose:
        print(result.log.read_text(encoding="utf-8", errors="replace"), flush=True)

    result.checks = collect_checks(result)
    if result.returncode != 0 and not any(not check.ok for check in result.checks):
        # A non-zero exit is authoritative even when the suite's own PASS/FAIL
        # lines all look green (e.g. it crashed after printing them).
        result.checks.append(Check(f"{suite.name} process", False,
                                   f"exited with {result.returncode}"))
    if result.returncode != 0 or result.failed:
        result.status = "fail"
    else:
        result.status = "pass"

    if result.status == "pass":
        print(f"PASS  {suite.name}  {len(result.checks)} checks, {result.duration:.1f}s", flush=True)
    else:
        print(f"FAIL  {suite.name}  rc={result.returncode}  {result.duration:.1f}s",
              file=sys.stderr, flush=True)
    return result


def collect_checks(result: Result) -> list[Check]:
    # 1. Suite-written JSON (Python suites).
    if result.report and result.report.exists():
        try:
            payload = json.loads(result.report.read_text(encoding="utf-8"))
            checks = [Check(record["name"], bool(record.get("ok")),
                            record.get("detail", ""), bool(record.get("skipped")))
                      for record in payload.get("checks", [])]
            if checks:
                return checks
        except (OSError, ValueError, KeyError):
            pass

    # 2. Catch2 JUnit (unit / regression): one case per testcase element.
    if result.catch2_xml and result.catch2_xml.exists():
        try:
            tree = ET.parse(result.catch2_xml)
            checks = []
            for case in tree.iter("testcase"):
                failed = case.find("failure") is not None or case.find("error") is not None
                detail = ""
                node = case.find("failure")
                if node is not None:
                    detail = node.get("message", "") or (node.text or "")
                checks.append(Check(case.get("name", "case"), not failed, detail))
            if checks:
                return checks
        except ET.ParseError:
            pass

    # 3. PASS/FAIL lines printed by the C++ probes and the Python suites.
    if result.log and result.log.exists():
        checks = []
        pattern = re.compile(r"^(PASS|FAIL)\s+(.*)$")
        for line in result.log.read_text(encoding="utf-8", errors="replace").splitlines():
            match = pattern.match(line.strip())
            if match:
                checks.append(Check(match.group(2), match.group(1) == "PASS"))
        if checks:
            return checks

    # 4. No structured output: the suite is one opaque check.
    return [Check(result.suite.name, result.returncode == 0,
                  "" if result.returncode == 0 else "suite exited non-zero")]


# --------------------------------------------------------------------------- #
# Reports
# --------------------------------------------------------------------------- #

def write_json_report(path: Path, results: list[Result], options: argparse.Namespace,
                      started_at: datetime, duration: float, build: dict) -> None:
    suites = []
    for result in results:
        suites.append({
            "id": result.suite.name,
            "status": result.status,
            "duration_s": round(result.duration, 3),
            "returncode": result.returncode,
            "checks": {
                "total": len(result.checks),
                "passed": result.passed,
                "failed": result.failed,
                "skipped": result.skipped_checks,
            },
            "log": str(result.log.relative_to(path.parent)) if result.log else None,
            "report": str(result.report.relative_to(path.parent)) if result.report and result.report.exists() else None,
            "catch2": str(result.catch2_xml.relative_to(path.parent)) if result.catch2_xml and result.catch2_xml.exists() else None,
            "ports": list(result.suite.ports),
            "skip_reason": result.skip_reason or None,
            "diagnostics": result.diagnostics or None,
        })
    payload = {
        "schema_version": 1,
        "tier": options.tier or "custom",
        "started_at": started_at.isoformat(),
        "duration_s": round(duration, 3),
        "build": build,
        "suites": suites,
        "totals": {
            "suites": len(results),
            "passed": sum(1 for result in results if result.status == "pass"),
            "failed": sum(1 for result in results if result.status == "fail"),
            "skipped": sum(1 for result in results if result.status == "skip"),
            "errors": sum(1 for result in results if result.status == "error"),
            "checks_total": sum(len(result.checks) for result in results),
            "checks_failed": sum(result.failed for result in results),
        },
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def write_junit_report(path: Path, results: list[Result]) -> None:
    root = ET.Element("testsuites", name="simple_http")
    for result in results:
        suite_node = ET.SubElement(
            root, "testsuite", name=result.suite.name,
            tests=str(max(1, len(result.checks))),
            failures=str(result.failed),
            errors="1" if result.status == "error" else "0",
            skipped="1" if result.status == "skip" else "0",
            time=f"{result.duration:.3f}",
        )
        properties = ET.SubElement(suite_node, "properties")
        ET.SubElement(properties, "property", name="status", value=result.status)
        if result.log:
            ET.SubElement(properties, "property", name="log", value=str(result.log))
        if result.skip_reason:
            ET.SubElement(properties, "property", name="skip_reason", value=result.skip_reason)

        if result.status == "skip":
            ET.SubElement(suite_node, "testcase", name=result.suite.name).append(
                ET.Element("skipped", message=result.skip_reason))
            continue
        if result.status == "error":
            case = ET.SubElement(suite_node, "testcase", name=result.suite.name)
            ET.SubElement(case, "error", message=result.skip_reason or "environment error")
            continue

        if result.catch2_xml and result.catch2_xml.exists():
            try:
                tree = ET.parse(result.catch2_xml)
                for case in tree.iter("testcase"):
                    suite_node.append(deepcopy(case))
                continue
            except ET.ParseError:
                pass

        if not result.checks:
            ET.SubElement(suite_node, "testcase", name=result.suite.name)
        for check in result.checks:
            case = ET.SubElement(suite_node, "testcase", name=check.name,
                                 classname=result.suite.name)
            if check.skipped:
                ET.SubElement(case, "skipped", message=check.detail)
            elif not check.ok:
                ET.SubElement(case, "failure", message=check.detail or "failed")

    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def print_failure(result: Result) -> None:
    print(f"\n--- {result.suite.name} failed (rc={result.returncode}) ---", file=sys.stderr)
    if result.log and result.log.exists():
        print(f"log: {result.log}", file=sys.stderr)
        lines = result.log.read_text(encoding="utf-8", errors="replace").splitlines()
        for line in lines[-40:]:
            print(f"  {line}", file=sys.stderr)
    listeners = port_diagnostics(result.suite.ports)
    if listeners:
        print("listeners:", file=sys.stderr)
        for line in listeners.splitlines():
            print(f"  {line}", file=sys.stderr)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("command", nargs="*")
    parser.add_argument("--filter")
    parser.add_argument("--exclude")
    parser.add_argument("--report-dir")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--bin-dir")
    parser.add_argument("--python")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--timeout", type=int)
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--strict-deps", action="store_true")
    parser.add_argument("--allow-skip", action="store_true")
    parser.add_argument("--catch-filter", action="append", default=[])
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("-h", "--help", action="store_true")
    # argparse's optional nargs="*" positional stops accepting values once an
    # option is seen ("unit --no-build '[h2]'"), so collect the leftovers and
    # append them to the suite arguments. This is what makes both
    # `unit '[h2]' --no-build` and `unit --no-build '[h2]'` work.
    options, unknown = parser.parse_known_args(argv)
    options.command.extend(unknown)
    return options


def selected_suites(options: argparse.Namespace) -> tuple[list[Suite], bool, list[str]]:
    """Returns (suites, explicit, extra args for a single explicit suite)."""
    command = list(options.command)
    if not command:
        return [], False, []
    if command[0] in TIERS:
        extra = command[1:]
        if extra:
            raise SystemExit("usage: no extra arguments after a tier")
        options.tier = command[0]
        tier_names = TIERS[command[0]]
        return [suite for suite in SUITES if suite.tier in tier_names and suite.tier != "manual"], False, []

    names = []
    extra = []
    for item in command:
        if item in SUITE_BY_NAME:
            names.append(item)
        elif not names:
            raise SystemExit(f"unknown suite: {item}")
        else:
            extra.append(item)
    options.tier = None
    if len(names) == 1:
        return [SUITE_BY_NAME[name] for name in names], True, extra
    if extra:
        raise SystemExit("extra arguments are only accepted with a single suite")
    return [SUITE_BY_NAME[name] for name in names], True, []


def apply_filters(suites: list[Suite], options: argparse.Namespace) -> list[Suite]:
    if options.filter:
        patterns = [part.strip() for part in options.filter.split(",") if part.strip()]
        suites = [suite for suite in suites
                  if any(fnmatch.fnmatchcase(suite.name, pattern) for pattern in patterns)]
    if options.exclude:
        patterns = [part.strip() for part in options.exclude.split(",") if part.strip()]
        suites = [suite for suite in suites
                  if not any(fnmatch.fnmatchcase(suite.name, pattern) for pattern in patterns)]
    return suites


def print_list() -> None:
    print(f"{'suite':<24} {'tier':<9} {'targets'}")
    for suite in SUITES:
        print(f"{suite.name:<24} {suite.tier:<9} {', '.join(suite.targets) or '-'}")


def print_doctor(options: argparse.Namespace, python: Path | None) -> int:
    xmake = shutil.which("xmake")
    print(f"xmake:    {xmake or 'MISSING'}")
    print(f"python:   {python or 'no test virtualenv'}")
    print(f"bin dir:  {options.bin_dir}")
    if xmake is None:
        return 3
    print()
    print(f"{'suite':<24} {'deps'}")
    for suite in SUITES:
        problems = []
        missing_tools = tool_missing(suite.tools)
        if missing_tools:
            problems.append("tools: " + ", ".join(missing_tools))
        reason = packages_missing(python, suite.packages)
        if reason:
            problems.append(reason)
        print(f"{suite.name:<24} {'OK' if not problems else '; '.join(problems)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    options = parse_args(sys.argv[1:] if argv is None else argv)

    if options.help:
        print(__doc__)
        return 0
    if not options.command or options.command[0] in ("-h", "--help"):
        print(__doc__)
        return 0

    options.bin_dir = find_bin_dir(options.bin_dir)
    python = find_python(options.python)
    started_at = datetime.now(timezone.utc).replace(microsecond=0)
    start = time.monotonic()

    if options.command[0] == "list":
        print_list()
        return 0
    if options.command[0] == "doctor":
        return print_doctor(options, python)

    suites, explicit, extra = selected_suites(options)
    options.suite_args = extra
    suites = apply_filters(suites, options)
    if not suites:
        print("no suites selected", file=sys.stderr)
        return 2

    if not options.no_build and shutil.which("xmake") is None:
        print("error: xmake not found; use --no-build with --bin-dir", file=sys.stderr)
        return 3

    run_root = Path(options.report_dir).expanduser().resolve() if options.report_dir else CACHE
    stamp = datetime.now().strftime("%Y%m%dT%H%M%SZ")
    run_dir = run_root / f"{stamp}-{os.getpid()}"
    run_dir.mkdir(parents=True, exist_ok=True)
    build_log = run_dir / "build.log"

    print(f"==> {options.tier or 'custom'}  ({len(suites)} suites)  -> {run_dir}")

    # Build once for every selected suite that needs binaries.
    build_targets_list = sorted({target for suite in suites for target in suite.targets})
    built, build_failures = build_targets(build_targets_list, options, run_dir, build_log)

    results: list[Result] = []
    for suite in suites:
        broken = [target for target in suite.targets if target in build_failures]
        if broken:
            result = Result(suite=suite, status="error",
                            skip_reason=f"build failed: {', '.join(broken)}")
            result.diagnostics = {"build": str(build_log)}
            print(f"ERROR {suite.name}: build failed for {', '.join(broken)}", file=sys.stderr)
            results.append(result)
            continue

        if suite.kind == "binary" and not (options.bin_dir / suite.path).exists():
            result = Result(suite=suite, status="error",
                            skip_reason=f"binary not built: {options.bin_dir / suite.path}")
            print(f"ERROR {suite.name}: {result.skip_reason}", file=sys.stderr)
            results.append(result)
            if options.fail_fast:
                break
            continue
        if suite.kind in ("python", "bash") and not (TEST / suite.path).exists():
            result = Result(suite=suite, status="error",
                            skip_reason=f"suite script not found: {TEST / suite.path}")
            print(f"ERROR {suite.name}: {result.skip_reason}", file=sys.stderr)
            results.append(result)
            if options.fail_fast:
                break
            continue

        missing_tools = tool_missing(suite.tools)
        missing_packages = packages_missing(python, suite.packages)
        busy = port_busy(suite.ports)
        if missing_tools or missing_packages or busy:
            reasons = []
            if missing_tools:
                reasons.append("missing tools: " + ", ".join(missing_tools))
            if missing_packages:
                reasons.append(missing_packages)
            if busy:
                reasons.append("ports in use: " + "; ".join(busy))
            reason = " | ".join(reasons)
            fatal = explicit and not options.allow_skip
            result = Result(suite=suite, status="error" if fatal else "skip",
                            skip_reason=reason)
            result.diagnostics = {"missing_tools": missing_tools,
                                  "packages": missing_packages, "ports": busy}
            print(f"{'ERROR' if fatal else 'SKIP '}  {suite.name}: {reason}",
                  file=sys.stderr if fatal else sys.stdout)
            results.append(result)
            if fatal and options.fail_fast:
                break
            continue

        result = run_suite(suite, options, python, run_dir, list(options.catch_filter))
        results.append(result)
        if result.status == "fail":
            print_failure(result)
            if options.fail_fast:
                break

    duration = time.monotonic() - start
    report_json = run_dir / "report.json"
    junit = run_dir / "junit.xml"
    write_json_report(report_json, results, options,
                      started_at, duration,
                      {"bin_dir": str(options.bin_dir), "targets": build_targets_list,
                       "failures": build_failures, "log": str(build_log)})
    write_junit_report(junit, results)

    print(f"\nreport: {report_json}")
    print(f"junit:  {junit}")
    totals = {
        "pass": sum(1 for result in results if result.status == "pass"),
        "fail": sum(1 for result in results if result.status == "fail"),
        "skip": sum(1 for result in results if result.status == "skip"),
        "error": sum(1 for result in results if result.status == "error"),
    }
    print(f"suites: {totals['pass']} passed, {totals['fail']} failed, "
          f"{totals['skip']} skipped, {totals['error']} errors")

    if totals["error"]:
        return 3
    if totals["fail"]:
        return 1
    if options.strict_deps and totals["skip"]:
        return 3
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
