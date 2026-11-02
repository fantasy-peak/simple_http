"""Shared plumbing for the differential suites (standard library only).

Every differential run has the same shape:

  * start one or more reference servers,
  * run a C++ probe and a Go probe against the same inputs,
  * compare the JSON observations field by field,
  * classify each difference: PASS, KNOWN (registered in known_diffs.json),
    or FAIL.

Keeping normalization here — not in each probe — is what makes the output
comparable. A probe only reports what it saw; this module decides what counts
as the same behavior.
"""

from __future__ import annotations

import fnmatch
import json
import os
import re
import socket
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
KNOWN_FILE = HERE.parent / "known_diffs.json"

# Headers that are transport/implementation detail by default. A case that
# wants to pin one of them names it in `expect.compare_headers`.
IGNORED_HEADERS = {
    "date",
    "server",
    "alt-svc",
    "connection",
    "keep-alive",
    "transfer-encoding",
    "content-length",
    "trailer",
    "te",
    "upgrade",
    "proxy-connection",
    "etag",
    "last-modified",
    "set-cookie",
}

# Headers whose value is a comma-separated set, so order and spacing are not
# semantic.
SET_HEADERS = {"allow", "vary", "accept-encoding", "cache-control"}


# --------------------------------------------------------------------------- #
# binaries, processes, ports
# --------------------------------------------------------------------------- #

def find_binary(name: str) -> Path:
    bin_dir = os.environ.get("SIMPLE_HTTP_TEST_BIN_DIR")
    if bin_dir:
        candidate = Path(bin_dir) / name
        if candidate.exists():
            return candidate
    for mode in ("release", "debug"):
        candidate = REPO / "build" / "linux" / "x86_64" / mode / name
        if candidate.exists():
            return candidate
    return REPO / "build" / "linux" / "x86_64" / "release" / name


def wait_port(host: str, port: int, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.25):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def start(cmd: list[str], log_path: Path, cwd: Path = REPO,
          env: dict[str, str] | None = None) -> subprocess.Popen:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(log_path, "w", encoding="utf-8")
    process = subprocess.Popen(cmd, stdout=handle, stderr=subprocess.STDOUT,
                               cwd=str(cwd), env=env)
    process._test_log = handle  # type: ignore[attr-defined]
    return process


def stop(*processes: subprocess.Popen | None) -> None:
    # Sanitizer builds (especially ASan/LSan) may take longer to run their
    # atexit checks; a SIGKILL would silently lose the leak report.
    grace = float(os.environ.get("SIMPLE_HTTP_TEST_STOP_GRACE_MS", "5000")) / 1000.0
    for process in processes:
        if process is None:
            continue
        process.terminate()
    for process in processes:
        if process is None:
            continue
        try:
            process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            # The sanitizer orchestrator treats a forced kill as "leak check
            # skipped" rather than as a clean pass.
            handle = getattr(process, "_test_log", None)
            if handle is not None:
                handle.write("\nSTOP_TIMEOUT: forced SIGKILL (sanitizer checks skipped)\n")
                handle.flush()
            process.kill()
            process.wait(timeout=5)
        handle = getattr(process, "_test_log", None)
        if handle is not None:
            handle.close()


def tail(path: Path, lines: int = 30) -> str:
    try:
        data = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(data[-lines:])


# --------------------------------------------------------------------------- #
# observation normalization and comparison
# --------------------------------------------------------------------------- #

def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or not line.startswith("{"):
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows


def by_case(rows: Iterable[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {row["case"]: row for row in rows if "case" in row}


def normalize_headers(headers: dict[str, Any] | None) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for name, value in (headers or {}).items():
        key = str(name).lower()
        values = value if isinstance(value, list) else [value]
        cleaned = [str(item).strip() for item in values]
        if key in SET_HEADERS:
            parts = []
            for item in cleaned:
                parts.extend(part.strip() for part in item.split(",") if part.strip())
            cleaned = sorted(parts, key=str.lower)
        out[key] = cleaned
    return out


def canonical_body(body: Any, mode: str, parse_json_strings: Iterable[str] = ()) -> Any:
    if mode == "none":
        return None
    if mode == "json":
        value = body
        if not isinstance(body, (dict, list)):
            try:
                value = json.loads(body or "null")
            except (TypeError, json.JSONDecodeError):
                return body
        # Some APIs model a structured extension member as a JSON string
        # (JsonError.details). Comparing the embedded structure rather than the
        # spelling keeps the differential about behavior.
        if isinstance(value, dict):
            value = dict(value)
            for name in parse_json_strings:
                inner = value.get(name)
                if isinstance(inner, str):
                    try:
                        value[name] = json.loads(inner)
                    except json.JSONDecodeError:
                        pass
        return value
    return body if isinstance(body, str) else json.dumps(body, sort_keys=True)


def _header_value(headers: dict[str, list[str]], name: str) -> Any:
    values = headers.get(name.lower())
    if values is None:
        return None
    return values[0] if len(values) == 1 else values


def apply_scrub(value: Any, scrub: list[list[str]]) -> Any:
    if not scrub:
        return value
    if isinstance(value, list):
        return [apply_scrub(item, scrub) for item in value]
    if not isinstance(value, str):
        return value
    for item in scrub:
        if len(item) >= 2:
            value = re.sub(item[0], item[1], value)
    return value


def compare_headers(case: dict[str, Any], cpp: dict[str, list[str]],
                    go: dict[str, list[str]]) -> list[tuple[str, Any, Any]]:
    expect = case.get("expect") or {}
    scrub = expect.get("scrub", [])
    if expect.get("compare_headers"):
        names = [name.lower() for name in expect["compare_headers"]]
    else:
        ignored = set(IGNORED_HEADERS) | {h.lower() for h in expect.get("ignore_headers", [])}
        names = sorted(set(cpp) | set(go))
        names = [name for name in names if name not in ignored]
    diffs: list[tuple[str, Any, Any]] = []
    for name in names:
        left = apply_scrub(_header_value(cpp, name), scrub)
        right = apply_scrub(_header_value(go, name), scrub)
        if left != right:
            diffs.append((f"headers.{name}", left, right))
    return diffs


def compare_case(case: dict[str, Any], cpp: dict[str, Any],
                 go: dict[str, Any]) -> list[tuple[str, Any, Any]]:
    """Return (field, cpp_value, go_value) for every differing field."""
    expect = case.get("expect") or {}
    diffs: list[tuple[str, Any, Any]] = []

    # An error observation carries no status/body; comparing a missing field
    # against an empty one would be noise. The error class is the behavior.
    if cpp.get("error") or go.get("error"):
        if cpp.get("error") != go.get("error"):
            diffs.append(("error", cpp.get("error"), go.get("error")))
        return diffs

    status_in = expect.get("status_in")
    if status_in:
        if cpp.get("status") not in status_in:
            diffs.append(("status", cpp.get("status"), status_in))
        if go.get("status") not in status_in:
            diffs.append(("status", status_in, go.get("status")))
    elif cpp.get("status") != go.get("status"):
        diffs.append(("status", cpp.get("status"), go.get("status")))

    body_mode = expect.get("body_mode", "exact")
    scrub = expect.get("scrub", [])
    parse_strings = expect.get("parse_json_strings", [])
    left_body = canonical_body(apply_scrub(cpp.get("body"), scrub), body_mode, parse_strings)
    right_body = canonical_body(apply_scrub(go.get("body"), scrub), body_mode, parse_strings)
    if body_mode != "none" and left_body != right_body:
        diffs.append(("body", left_body, right_body))

    diffs.extend(compare_headers(case, normalize_headers(cpp.get("headers")),
                                 normalize_headers(go.get("headers"))))

    if expect.get("compare_version") and cpp.get("version") != go.get("version"):
        diffs.append(("version", cpp.get("version"), go.get("version")))

    if expect.get("compare_framing") and cpp.get("framing") != go.get("framing"):
        diffs.append(("framing", cpp.get("framing"), go.get("framing")))

    cpp_notes = cpp.get("notes") or {}
    go_notes = go.get("notes") or {}
    for key in sorted(set(cpp_notes) | set(go_notes)):
        if apply_scrub(cpp_notes.get(key), scrub) != apply_scrub(go_notes.get(key), scrub):
            diffs.append((f"notes.{key}", cpp_notes.get(key), go_notes.get(key)))

    # `expect` is a direct assertion, used by assert / *-only modes. It checks
    # one side against a literal rather than comparing the two.
    for key, wanted in (expect.get("fields") or {}).items():
        actual = cpp if case.get("mode", "compare") != "go-only" else go
        value = actual.get(key)
        if value != wanted:
            diffs.append((key, value, wanted))
    return diffs


# --------------------------------------------------------------------------- #
# known differences
# --------------------------------------------------------------------------- #

def load_known(section: str) -> dict[str, dict[str, Any]]:
    try:
        data = json.loads(KNOWN_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data.get(section, {})


def known_for(registry: dict[str, dict[str, Any]], case: str,
              field_name: str) -> dict[str, Any] | None:
    for pattern, entry in registry.items():
        if not fnmatch.fnmatchcase(case, pattern):
            continue
        for candidate in entry.get("fields", []):
            if field_name == candidate or field_name.startswith(candidate + "."):
                return entry
    return None


# --------------------------------------------------------------------------- #
# reporting
# --------------------------------------------------------------------------- #

@dataclass
class Outcome:
    passed: int = 0
    known: int = 0
    failed: int = 0
    skipped: int = 0
    rows: list[dict[str, Any]] = field(default_factory=list)

    @property
    def returncode(self) -> int:
        return 1 if self.failed else 0


class DiffReport:
    def __init__(self, suite: str, registry: dict[str, dict[str, Any]]) -> None:
        self.suite = suite
        self.registry = registry
        self.outcome = Outcome()

    def record(self, case: str, diffs: list[tuple[str, Any, Any]]) -> None:
        if not diffs:
            self.outcome.passed += 1
            print(f"PASS  {case}")
            self.outcome.rows.append({"case": case, "field": "", "cpp": "",
                                      "go": "", "verdict": "pass", "reason": ""})
            return
        unexpected: list[tuple[str, Any, Any]] = []
        for field_name, left, right in diffs:
            entry = known_for(self.registry, case, field_name)
            verdict = "known" if entry else "fail"
            reason = entry.get("reason", "") if entry else ""
            self.outcome.rows.append({"case": case, "field": field_name,
                                      "cpp": _short(left), "go": _short(right),
                                      "verdict": verdict, "reason": reason})
            if entry:
                self.outcome.known += 1
            else:
                unexpected.append((field_name, left, right))
            print(f"{'KNOWN' if entry else 'FAIL'} {case}: {field_name} "
                  f"cpp={_short(left)} go={_short(right)}"
                  + (f"  ({reason})" if reason else ""))
        if unexpected:
            self.outcome.failed += 1
        else:
            self.outcome.passed += 1

    def skip(self, case: str, reason: str) -> None:
        self.outcome.skipped += 1
        print(f"SKIP  {case}: {reason}")
        self.outcome.rows.append({"case": case, "field": "", "cpp": "", "go": "",
                                  "verdict": "skip", "reason": reason})

    def fail(self, name: str, detail: str = "") -> None:
        self.outcome.failed += 1
        print(f"FAIL  {name}" + (f": {detail}" if detail else ""))
        self.outcome.rows.append({"case": name, "field": "", "cpp": "", "go": "",
                                  "verdict": "fail", "reason": detail})

    def summary(self, report_dir: Path | None = None) -> int:
        out = self.outcome
        print(f"\n{self.suite}: {out.passed} passed, {out.known} known, "
              f"{out.failed} failed, {out.skipped} skipped")
        if report_dir:
            report_dir.mkdir(parents=True, exist_ok=True)
            (report_dir / "diff-report.md").write_text(render_markdown(self.suite, out.rows),
                                                       encoding="utf-8")
        return out.returncode


def _short(value: Any, limit: int = 160) -> str:
    try:
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False,
                                                                sort_keys=True)
    except (TypeError, ValueError):
        text = repr(value)
    text = text.replace("\n", "\\n")
    return text if len(text) <= limit else text[: limit - 3] + "..."


def render_markdown(suite: str, rows: list[dict[str, Any]]) -> str:
    lines = [f"# {suite} differential report", "",
             "| case | field | cpp | go | verdict | reason |",
             "|---|---|---|---|---|---|"]
    for row in rows:
        if row["verdict"] == "pass" and not row["field"]:
            continue
        cells = [str(row[k]).replace("|", "\\|") for k in
                 ("case", "field", "cpp", "go", "verdict", "reason")]
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    return "\n".join(lines)
