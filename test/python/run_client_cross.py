#!/usr/bin/env python3
"""Runs the cross-validation of simple_http's *client* against independent
servers implemented in Python.

The library's own suites drive simple_http's server with simple_http's own
client, so a spec misreading shared by both halves cancels out. run.py (the
existing suite) drives the C++ server with third-party *clients*; this one
drives the C++ *client* (test/client_cross.cpp, test/ws_client_cross.cpp)
against independent *servers*:

  * HTTP/1.1 — stdlib http.server       (test/python/http_server.py)
  * WebSocket — the websockets library  (test/python/ws_echo_server.py)

A disagreement means one of the two is wrong, which is the value of each side.

Usage (from the repo root, after `xmake build client_cross ws_cross`):
    test/python/.venv/bin/python test/python/run_client_cross.py
Also wired up as `xmake run client-cross-python`.
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
VENV = HERE / ".venv/bin/python"

HTTP_PORT = 27921
WS_PORT = 27920

CHECKED = 0
FAILED = 0


def check(ok: bool, what: str) -> None:
    global CHECKED, FAILED
    CHECKED += 1
    if not ok:
        FAILED += 1
    print("PASS  " + what if ok else "FAIL  " + what)


def wait_port(port: int, timeout: float = 10.0) -> bool:
    import socket

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def main() -> int:
    servers = []

    # HTTP server
    http = subprocess.Popen(
        [str(VENV), str(HERE / "http_server.py"), str(HTTP_PORT)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    servers.append(http)
    check(wait_port(HTTP_PORT), f"independent HTTP/1.1 server up on :{HTTP_PORT}")
    if not wait_port(HTTP_PORT):
        http.kill()
        return 1

    # WebSocket server
    ws = subprocess.Popen(
        [str(VENV), str(HERE / "ws_echo_server.py"), str(WS_PORT)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    servers.append(ws)
    if not wait_port(WS_PORT):
        check(False, f"independent WebSocket server up on :{WS_PORT}")
        ws.kill()
        http.kill()
        return 1
    check(True, f"independent WebSocket server up on :{WS_PORT}")

    try:
        for binary, arg in (
            (REPO / "build/linux/x86_64/release/client_cross", str(HTTP_PORT)),
            (REPO / "build/linux/x86_64/release/ws_cross", str(WS_PORT)),
        ):
            r = subprocess.run([str(binary), arg], text=True, capture_output=True)
            print(r.stdout.rstrip())
            passed = r.returncode == 0
            for line in r.stdout.splitlines():
                if line.startswith("FAIL"):
                    passed = False
            check(passed, f"{binary.name} passed its cross-checks")
    finally:
        for p in servers:
            p.terminate()
        for p in servers:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()

    print(f"\n{CHECKED} checks, {FAILED} failed")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())