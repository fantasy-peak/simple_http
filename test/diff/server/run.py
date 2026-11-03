#!/usr/bin/env python3
"""Server differential: Go client -> C++ server vs the same request -> Go server.

The C++ and Go reference servers register the same route table. One Go probe
per feature group sends every request to both and prints two JSON observations
(target=cpp / target=go). The runner compares them and classifies differences
through test/diff/known_diffs.json.

Usage:
    test/diff/server/run.py [--groups routing,body,semantics,middleware,framework,static,ws]
    test/run.sh server-diff
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DIFF = HERE.parent
REPO = DIFF.parent.parent
sys.path.insert(0, str(DIFF / "support"))

import harness  # noqa: E402

ALL_GROUPS = ("routing", "body", "semantics", "middleware", "framework", "static", "ws", "raw", "h2raw")

# The two reference servers bind different ports, and /echo reports the Host
# header verbatim. Scrub the port so the same request is byte-comparable.
DEFAULT_EXPECT = {
    "body_mode": "exact",
    "scrub": [[r"127\.0\.0\.1:78[0-9][0-9]", "host:port"]],
}

# Per-case comparison policy. JSON error bodies compare semantically; bodies
# the two frameworks intentionally spell differently compare as `none`.
CASE_OPTIONS: dict[str, dict] = {
    "routing.template_invalid": {"body_mode": "json"},
    "routing.head": {"body_mode": "none"},
    "routing.options": {"body_mode": "none"},
    "routing.method_not_allowed": {"body_mode": "none"},
    "routing.fallback": {"body_mode": "exact"},
    "body.multipart_form": {"body_mode": "json"},
    "body.json_invalid": {"body_mode": "json"},
    "body.json_type_mismatch": {"body_mode": "json"},
    "body.too_large": {"body_mode": "none", "ignore_headers": ["content-type"]},
    "body.deflate_request": {"body_mode": "none", "ignore_headers": ["accept-encoding"]},
    "body.bad_gzip": {"body_mode": "none"},
    "body.gzip_over_limit": {"body_mode": "none"},
    "semantics.status_204": {"body_mode": "none"},
    "middleware.cors_preflight": {"body_mode": "none"},
    "framework.valid_fail": {"body_mode": "json", "parse_json_strings": ["details"]},
    "framework.header_missing": {"body_mode": "json"},
    "framework.jwt_missing": {"body_mode": "json"},
    "static.index": {"body_mode": "exact"},
    "static.file": {"body_mode": "exact"},
    "static.spa": {"body_mode": "exact"},
    "static.missing_asset": {"body_mode": "none"},
    "static.range": {"body_mode": "exact"},
    "static.precompressed": {"body_mode": "exact"},
    "static.etag_304": {"body_mode": "none", "ignore_headers": ["content-type"]},
    "semantics.gzip": {"body_mode": "exact"},
    "middleware.gzip": {"body_mode": "exact"},
    "middleware.secure_plain": {"body_mode": "exact"},
    "middleware.secure_tls": {"body_mode": "exact"},
    "semantics.h3_echo": {"body_mode": "json"},
}


def cache_dir(explicit: str | None) -> Path:
    path = Path(explicit) if explicit else Path(
        os.environ.get("SIMPLE_HTTP_TEST_WORKDIR", REPO / ".cache" / "server-diff"))
    path = path.resolve()
    path.mkdir(parents=True, exist_ok=True)
    return path


def build_go(cache: Path, groups: tuple[str, ...]) -> dict[str, Path]:
    go = shutil.which("go")
    if not go:
        raise RuntimeError("go is not installed")
    binary_dir = cache / "go-bin"
    binary_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("GOFLAGS", "-mod=mod")

    outputs: dict[str, Path] = {}
    server = binary_dir / "server_refserver"
    result = subprocess.run([go, "build", "-o", str(server), "./server/go/server"],
                            cwd=str(DIFF), text=True, capture_output=True, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"go build server failed:\n{result.stderr}")
    outputs["server"] = server
    for group in groups:
        binary = binary_dir / f"server_probe_{group}"
        if group == "h2raw":
            command = [go, "build", "-o", str(binary), "./server/go/h2raw"]
        else:
            command = [go, "build", "-ldflags", f"-X main.defaultGroup={group}",
                       "-o", str(binary), "./server/go/probes"]
        result = subprocess.run(command, cwd=str(DIFF), text=True,
                                capture_output=True, env=env)
        if result.returncode != 0:
            raise RuntimeError(f"go build probe {group} failed:\n{result.stderr}")
        outputs[group] = binary
    return outputs


def prepare_static(cache: Path) -> Path:
    source = HERE / "fixtures" / "static"
    root = cache / "static"
    shutil.copytree(source, root, dirs_exist_ok=True)
    fixed = 1_700_000_000
    for item in root.rglob("*"):
        if item.is_file():
            os.utime(item, (fixed, fixed))
    data = (root / "data.txt").read_bytes()
    (root / "data.txt.gz").write_bytes(gzip.compress(data, mtime=0))
    try:
        import brotli  # type: ignore
        (root / "data.txt.br").write_bytes(brotli.compress(data))
    except ImportError:
        pass
    return root


def wait_ready(log_path: Path, timeout: float = 25.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            text = log_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        if "READY" in text:
            return True
        time.sleep(0.05)
    return False


def run_probe(command: list[str], output: Path, timeout: int = 180) -> subprocess.CompletedProcess:
    with open(output, "w", encoding="utf-8") as handle:
        return subprocess.run(command, cwd=str(REPO), stdout=handle,
                              stderr=subprocess.PIPE, text=True, timeout=timeout)


def run_h2raw(probe: Path, report_dir: Path, registry: dict) -> int:
    """Raw h2 assertions are C++-only: parse PASS/FAIL rather than diff targets."""
    output = report_dir / "h2raw.log"
    stderr_path = report_dir / "h2raw.stderr"
    result = run_probe([str(probe), "--h2c", "http://127.0.0.1:7821"], output)
    stderr_path.write_text(result.stderr or "", encoding="utf-8")
    report = harness.DiffReport("server-h2raw", registry)
    found = False
    for line in output.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^(PASS|FAIL)\s+(.*)$", line.strip())
        if not match:
            continue
        found = True
        case_id = match.group(2).strip()
        if match.group(1) == "PASS":
            report.record(case_id, [])
        else:
            report.fail(case_id, "assertion failed")
    if not found:
        report.fail("h2raw", "no PASS/FAIL output; see h2raw.log")
    if result.returncode != 0 and found:
        # The individual FAIL lines already recorded the assertion; keep the
        # process exit as a separate diagnostic only when nothing else failed.
        if report.outcome.failed == 0:
            report.fail("h2raw", f"probe exited {result.returncode}")
    return report.summary(report_dir / "h2raw")


def compare_group(group: str, probe: Path, report_dir: Path, registry: dict,
                  h3_ok: bool) -> int:
    if group == "h2raw":
        return run_h2raw(probe, report_dir, registry)

    output = report_dir / f"{group}.jsonl"
    stderr_path = report_dir / f"{group}.stderr"
    result = run_probe([str(probe), "--group", group], output)
    stderr_path.write_text(result.stderr or "", encoding="utf-8")
    report = harness.DiffReport(f"server-{group}", registry)
    if result.returncode != 0:
        report.fail(f"probe-{group}", result.stderr.strip()[-500:])

    rows = harness.load_jsonl(output)
    cpp = {row["case"]: row for row in rows if row.get("target") == "cpp"}
    go = {row["case"]: row for row in rows if row.get("target") == "go"}

    for case_id in sorted(set(cpp) | set(go)):
        if case_id == "semantics.h3_echo" and not h3_ok:
            report.skip(case_id, "HTTP/3 listener unavailable")
            continue
        cpp_obs, go_obs = cpp.get(case_id), go.get(case_id)
        if cpp_obs is None or go_obs is None:
            report.fail(case_id, f"missing observation from {'cpp' if cpp_obs is None else 'go'}")
            continue
        option = CASE_OPTIONS.get(case_id, {})
        if case_id.startswith("raw."):
            option = {"body_mode": "none"}
        definition = {"id": case_id, "expect": {**DEFAULT_EXPECT, **option}}
        report.record(case_id, harness.compare_case(definition, cpp_obs, go_obs))

    return report.summary(report_dir / group)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--groups", default=",".join(ALL_GROUPS))
    parser.add_argument("--report-dir", default=None)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--allow-skip", action="store_true")
    args = parser.parse_args()

    groups = tuple(group.strip() for group in args.groups.split(",") if group.strip())
    if set(groups) - set(ALL_GROUPS):
        print("unknown group", file=sys.stderr)
        return 2

    cache = cache_dir(args.report_dir)
    registry = harness.load_known("server")
    static_root = prepare_static(cache)

    if not shutil.which("go"):
        print("SKIP  server-diff: go is not installed", file=sys.stderr)
        return 0 if args.allow_skip else 3

    try:
        if args.no_build:
            binary_dir = cache / "go-bin"
            outputs = {group: binary_dir / f"server_probe_{group}" for group in groups}
            outputs["server"] = binary_dir / "server_refserver"
        else:
            outputs = build_go(cache, groups)
    except RuntimeError as error:
        print(f"FAIL  server-diff: {error}", file=sys.stderr)
        return 1

    cpp_binary = harness.find_binary("diff_server_cpp")
    if not cpp_binary.exists():
        print(f"FAIL  server-diff: {cpp_binary} not built", file=sys.stderr)
        return 1

    cpp_log = cache / "cpp-server.log"
    go_log = cache / "go-server.log"
    cpp_server = harness.start([str(cpp_binary), "--static-root", str(static_root)], cpp_log)
    go_server = harness.start([str(outputs["server"]), "--static-root", str(static_root)], go_log)
    try:
        if not wait_ready(cpp_log) or not wait_ready(go_log):
            print("FAIL  server-diff: a reference server did not become ready", file=sys.stderr)
            print("== cpp ==\n" + harness.tail(cpp_log), file=sys.stderr)
            print("== go ==\n" + harness.tail(go_log), file=sys.stderr)
            return 1

        cpp_text = cpp_log.read_text(encoding="utf-8", errors="replace")
        h3_ok = "tcp+udp h3 (ok=true)" in cpp_text

        exit_code = 0
        for group in groups:
            probe = outputs.get(group)
            if probe is None or not probe.exists():
                print(f"SKIP  server-{group}: probe not built", file=sys.stderr)
                continue
            exit_code |= compare_group(group, probe, cache, registry, h3_ok)
        return exit_code
    finally:
        harness.stop(cpp_server, go_server)


if __name__ == "__main__":
    sys.exit(main())
