#!/usr/bin/env python3
"""C++17 HTTP/3 client interop test.

Starts fixture_cpp17_server (TCP interop fixture plus an ephemeral QUIC
listener), then runs test_cpp17_h3_client against its UDP port.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

TEST = Path(__file__).resolve().parent
ROOT = TEST.parents[1]
BIN_DIR = Path(os.environ.get("SIMPLE_HTTP_TEST_BIN_DIR", ROOT / "build" / "linux" / "x86_64" / "release"))
SERVER = BIN_DIR / "fixture_cpp17_server"
DRIVER = BIN_DIR / "test_cpp17_h3_client"


def main() -> int:
    if not SERVER.exists() or not DRIVER.exists():
        print(f"missing binary: {SERVER} or {DRIVER}")
        return 2

    process = subprocess.Popen(
        [str(SERVER)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        deadline = time.monotonic() + 10
        quic_port = None
        assert process.stdout is not None
        while time.monotonic() < deadline:
            line = process.stdout.readline()
            if not line:
                break
            if line.startswith("QUICPORT "):
                quic_port = int(line.split()[1])
                break
        if quic_port is None:
            print("server did not report a QUIC port")
            return 1
        return subprocess.run([str(DRIVER), str(quic_port)], cwd=str(ROOT), timeout=60).returncode
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    sys.exit(main())
