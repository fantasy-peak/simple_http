#!/usr/bin/env python3
"""Client differential: C++ client vs Go net/http client, same Go server.

For every group the runner:

  1. builds the Go probes (and the Go reference server) with `go build`,
  2. starts one reference server (h1 / TLS h2 / h2c / h3 / WebSocket),
  3. runs the C++ probe and the Go probe over the same JSON cases,
  4. compares the JSONL observations field by field,
  5. classifies differences through test/diff/known_diffs.json.

Usage:
    test/diff/client/run.py [--groups basic,body,policy,ws,h3]
    test/run.sh client-diff
"""

from __future__ import annotations

import argparse
import json
import os
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

ALL_GROUPS = ("basic", "body", "policy", "ws", "h3")


def cache_dir(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
    else:
        path = Path(os.environ.get("SIMPLE_HTTP_TEST_WORKDIR", REPO / ".cache" / "client-diff"))
    path.mkdir(parents=True, exist_ok=True)
    return path


def go_available() -> bool:
    return shutil.which("go") is not None


def build_go(cache: Path, groups: tuple[str, ...]) -> dict[str, Path]:
    """Build the reference server and one probe binary per group."""
    go = shutil.which("go")
    if not go:
        raise RuntimeError("go is not installed")
    binary_dir = cache / "go-bin"
    binary_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("GOFLAGS", "-mod=mod")

    outputs: dict[str, Path] = {}
    reference = binary_dir / "client_refserver"
    result = subprocess.run([go, "build", "-o", str(reference), "./client/go/refserver"],
                            cwd=str(DIFF), text=True, capture_output=True, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"go build refserver failed:\n{result.stderr}")
    outputs["refserver"] = reference

    for group in groups:
        binary = binary_dir / f"go_probe_client_{group}"
        result = subprocess.run(
            [go, "build", "-ldflags", f"-X main.defaultGroup={group}",
             "-o", str(binary), "./client/go/driver"],
            cwd=str(DIFF), text=True, capture_output=True, env=env)
        if result.returncode != 0:
            raise RuntimeError(f"go build probe {group} failed:\n{result.stderr}")
        outputs[group] = binary
    return outputs


def wait_ready(log_path: Path, timeout: float = 20.0) -> bool:
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


def cases_path(group: str) -> Path | None:
    if group in {"body", "ws"}:
        return None
    path = HERE / "cases" / f"{group}.json"
    return path if path.exists() else None


def load_case_definitions(path: Path | None) -> dict[str, dict]:
    if path is None:
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return {case["id"]: case for case in data if "id" in case}


def run_probe(command: list[str], output: Path, timeout: int = 180) -> subprocess.CompletedProcess:
    with open(output, "w", encoding="utf-8") as handle:
        return subprocess.run(command, cwd=str(REPO), stdout=handle,
                              stderr=subprocess.PIPE, text=True, timeout=timeout)


def run_group(group: str, cpp_binary: Path, go_binary: Path, report_dir: Path,
              registry: dict) -> int:
    definition = cases_path(group)
    definitions = load_case_definitions(definition)
    cpp_out = report_dir / f"{group}.cpp.jsonl"
    go_out = report_dir / f"{group}.go.jsonl"
    cpp_err = report_dir / f"{group}.cpp.stderr"
    go_err = report_dir / f"{group}.go.stderr"

    cpp_cmd = [str(cpp_binary)]
    go_cmd = [str(go_binary), "--group", group]
    if definition is not None:
        cpp_cmd += ["--cases", str(definition)]
        go_cmd += ["--cases", str(definition)]

    cpp_result = run_probe(cpp_cmd, cpp_out)
    cpp_err.write_text(cpp_result.stderr or "", encoding="utf-8")
    go_result = run_probe(go_cmd, go_out)
    go_err.write_text(go_result.stderr or "", encoding="utf-8")

    report = harness.DiffReport(f"client-{group}", registry)
    if cpp_result.returncode != 0:
        report.fail(f"cpp-{group}", cpp_result.stderr.strip()[-500:])
    if go_result.returncode != 0:
        report.fail(f"go-{group}", go_result.stderr.strip()[-500:])

    cpp_rows = harness.by_case(harness.load_jsonl(cpp_out))
    go_rows = harness.by_case(harness.load_jsonl(go_out))

    for case_id in sorted(set(definitions) | set(cpp_rows) | set(go_rows)):
        definition = definitions.get(case_id, {"id": case_id, "expect": {"body_mode": "exact"}})
        cpp_obs = cpp_rows.get(case_id)
        go_obs = go_rows.get(case_id)
        if cpp_obs is not None and go_obs is not None:
            report.record(case_id, harness.compare_case(definition, cpp_obs, go_obs))
            continue
        present = cpp_obs if cpp_obs is not None else go_obs
        mode = (present or {}).get("mode") or definition.get("mode") or "compare"
        if present is None:
            report.fail(case_id, "no observation from either side")
        elif mode in {"cpp-only", "go-only"}:
            if present.get("error"):
                report.record(case_id, [("error", present.get("error"), None)])
            else:
                report.record(case_id, [])
        else:
            report.fail(case_id, f"missing observation from {'go' if cpp_obs else 'cpp'}")

    target = report_dir / group
    return report.summary(target)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--groups", default=",".join(ALL_GROUPS),
                        help="comma-separated group list (default: all)")
    parser.add_argument("--report-dir", default=None)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--allow-skip", action="store_true")
    args = parser.parse_args()

    groups = tuple(group.strip() for group in args.groups.split(",") if group.strip())
    unknown = set(groups) - set(ALL_GROUPS)
    if unknown:
        print(f"unknown groups: {', '.join(sorted(unknown))}", file=sys.stderr)
        return 2

    cache = cache_dir(args.report_dir)
    report_dir = cache
    registry = harness.load_known("client")

    if not go_available():
        print("SKIP  client-diff: go is not installed", file=sys.stderr)
        return 0 if args.allow_skip else 3

    try:
        if args.no_build:
            binary_dir = cache / "go-bin"
            outputs = {group: binary_dir / f"go_probe_client_{group}" for group in groups}
            outputs["refserver"] = binary_dir / "client_refserver"
            for path in outputs.values():
                if not path.exists():
                    raise RuntimeError(f"missing Go binary {path} (run without --no-build)")
        else:
            outputs = build_go(cache, groups)
    except RuntimeError as error:
        print(f"FAIL  client-diff: {error}", file=sys.stderr)
        return 1

    log_path = report_dir / "refserver.log"
    reference = harness.start([str(outputs["refserver"])], log_path)
    try:
        if not wait_ready(log_path):
            print("FAIL  client-diff: reference server did not become ready", file=sys.stderr)
            print(harness.tail(log_path), file=sys.stderr)
            return 1

        exit_code = 0
        for group in groups:
            cpp_binary = harness.find_binary(f"diff_client_{group}")
            if not cpp_binary.exists():
                if group == "h3" or args.allow_skip:
                    print(f"SKIP  client-{group}: {cpp_binary} not built", file=sys.stderr)
                    continue
                print(f"FAIL  client-{group}: {cpp_binary} not built", file=sys.stderr)
                exit_code = 1
                continue
            go_binary = outputs.get(group)
            if go_binary is None or not go_binary.exists():
                print(f"SKIP  client-{group}: Go probe not built", file=sys.stderr)
                continue
            exit_code |= run_group(group, cpp_binary, go_binary, report_dir, registry)
        return exit_code
    finally:
        harness.stop(reference)


if __name__ == "__main__":
    sys.exit(main())
