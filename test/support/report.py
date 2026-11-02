"""Shared PASS/FAIL recording for the Python-side test suites.

The C++ probe programs print the same ``PASS  <name>`` / ``FAIL  <name>`` lines
and are parsed by ``test/run.py``; Python suites go through this module so their
per-check records can be written to a machine-readable file as well.

The orchestrator sets ``SIMPLE_HTTP_TEST_REPORT=<path>`` and
``SIMPLE_HTTP_TEST_SUITE=<id>`` before starting a suite. A direct run still
prints the human-readable lines and simply skips the JSON file.

This module must stay on the Python standard library: ``test/run.py`` runs it
with the system interpreter even when the third-party test venv is absent.
"""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path


class Reporter:
    """Collects one suite's checks and emits an optional JSON result."""

    def __init__(self, suite: str) -> None:
        self.suite = suite
        self.records: list[dict] = []

    def record(self, ok: bool, name: str, detail: str = "", skipped: bool = False) -> bool:
        """Adds a check without printing (for parsed child-process output)."""
        record = {"name": str(name), "ok": bool(ok), "detail": str(detail)}
        if skipped:
            record["skipped"] = True
        self.records.append(record)
        return bool(ok)

    def check(self, ok: bool, name: str, detail: str = "") -> bool:
        self.record(ok, name, detail)
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        return bool(ok)

    def fail(self, name: str, detail: str = "") -> bool:
        return self.check(False, name, detail)

    def skip(self, name: str, reason: str) -> None:
        self.records.append({"name": str(name), "ok": True, "skipped": True, "detail": str(reason)})
        print(f"SKIP  {name}: {reason}")

    @property
    def passed(self) -> int:
        return sum(1 for record in self.records if record["ok"] and not record.get("skipped"))

    @property
    def failed(self) -> int:
        return sum(1 for record in self.records if not record["ok"])

    @property
    def skipped(self) -> int:
        return sum(1 for record in self.records if record.get("skipped"))

    def summary(self) -> int:
        total = len(self.records)
        print(f"\n{total} checks, {self.failed} failed, {self.skipped} skipped")
        self._write_report()
        return 1 if self.failed else 0

    def _write_report(self) -> None:
        path = os.environ.get("SIMPLE_HTTP_TEST_REPORT")
        if not path:
            return
        payload = {
            "suite": self.suite,
            "checks": self.records,
            "total": len(self.records),
            "passed": self.passed,
            "failed": self.failed,
            "skipped": self.skipped,
        }
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        # Write-then-rename so a killed suite never leaves a half-written file.
        fd, temporary = tempfile.mkstemp(dir=target.parent, prefix=target.name + ".")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, indent=2)
                handle.write("\n")
            os.replace(temporary, target)
        except BaseException:
            try:
                os.unlink(temporary)
            except OSError:
                pass
            raise
