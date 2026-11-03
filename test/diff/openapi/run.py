#!/usr/bin/env python3
"""Starts the petshop fixture and runs the OpenAPI verification suite."""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DIFF = HERE.parent
REPO = DIFF.parent.parent
sys.path.insert(0, str(DIFF / "support"))

import harness  # noqa: E402

PORT = 7795


def main() -> int:
    binary = harness.find_binary("diff_openapi_server")
    if not binary.exists():
        print(f"FAIL  openapi: {binary} not built", file=sys.stderr)
        return 1

    cache = Path(os.environ.get("SIMPLE_HTTP_TEST_WORKDIR", REPO / ".cache" / "openapi"))
    cache = cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    log = cache / "openapi-server.log"
    server = harness.start([str(binary)], log)
    try:
        if not harness.wait_port("127.0.0.1", PORT, timeout=15):
            print("FAIL  openapi: fixture did not start", file=sys.stderr)
            print(harness.tail(log), file=sys.stderr)
            return 1
        result = subprocess.run(
            [sys.executable, str(HERE / "verify.py"),
             "--base-url", f"http://127.0.0.1:{PORT}"],
            cwd=str(REPO), text=True, capture_output=True)
        print(result.stdout.rstrip())
        if result.stderr.strip():
            print(result.stderr.rstrip(), file=sys.stderr)
        return result.returncode
    finally:
        harness.stop(server)


if __name__ == "__main__":
    sys.exit(main())
