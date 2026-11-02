#!/usr/bin/env python3
"""Runs the cross-validation of simple_http's *client* against independent
servers implemented in Python.

The library's own suites drive simple_http's server with simple_http's own
client, so a spec misreading shared by both halves cancels out. server/run.py
drives the C++ server with third-party *clients*; this one drives the C++
*client* (test/client/probes/*_cross.cpp) against independent *servers*:

  * HTTP/1.1 — stdlib http.server       (test/client/peers/http_server.py)
  * WebSocket — the websockets library  (test/client/peers/ws_echo_server.py)
  * HTTP/3 — aioquic                    (test/client/peers/h3_server.py)

A disagreement means one of the two is wrong, which is the value of each side.

Run from anywhere (the test runner sets SIMPLE_HTTP_TEST_BIN_DIR):
    test/.venv/bin/python test/client/run.py
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
PEERS = HERE / "peers"

sys.path.insert(0, str(HERE.parent / "support"))
from report import Reporter  # noqa: E402

_reporter = Reporter(os.environ.get("SIMPLE_HTTP_TEST_SUITE", "client-cross"))

HTTP_PORT = 27921
WS_PORT = 27920
H3_PORT = 27922


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


def wait_ready(proc: "subprocess.Popen[str]", timeout: float = 10.0) -> bool:
    """Waits for a server script to print ``ready`` on stdout."""
    import select

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.5)
        if not ready:
            if proc.poll() is not None:
                return False
            continue
        line = proc.stdout.readline()
        if not line:
            return False
        if line.strip() == "ready":
            return True
    return False


def _stop(servers: list[subprocess.Popen]) -> None:
    for proc in servers:
        proc.terminate()
    for proc in servers:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def main() -> int:
    servers: list[subprocess.Popen] = []
    python = sys.executable

    http = subprocess.Popen(
        [python, str(PEERS / "http_server.py"), str(HTTP_PORT)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd=str(REPO),
    )
    servers.append(http)
    if not wait_port(HTTP_PORT):
        _stop(servers)
        _reporter.check(False, f"independent HTTP/1.1 server up on :{HTTP_PORT}")
        return _reporter.summary()
    _reporter.check(True, f"independent HTTP/1.1 server up on :{HTTP_PORT}")

    ws = subprocess.Popen(
        [python, str(PEERS / "ws_echo_server.py"), str(WS_PORT)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd=str(REPO),
    )
    servers.append(ws)
    if not wait_port(WS_PORT):
        _stop(servers)
        _reporter.check(False, f"independent WebSocket server up on :{WS_PORT}")
        return _reporter.summary()
    _reporter.check(True, f"independent WebSocket server up on :{WS_PORT}")

    # HTTP/3 server (aioquic). UDP has no accept queue to probe, so the script
    # announces readiness on stdout.
    h3 = subprocess.Popen(
        [python, str(PEERS / "h3_server.py"), str(H3_PORT)],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        cwd=str(REPO),
    )
    servers.append(h3)
    if not wait_ready(h3):
        _stop(servers)
        _reporter.check(False, f"independent HTTP/3 server up on udp :{H3_PORT}")
        return _reporter.summary()
    _reporter.check(True, f"independent HTTP/3 server up on udp :{H3_PORT}")

    try:
        for name, arg in (
            ("probe_client_cross", str(HTTP_PORT)),
            ("probe_ws_cross", str(WS_PORT)),
            ("probe_h3_cross", str(H3_PORT)),
        ):
            binary = find_binary(name)
            run = subprocess.run([str(binary), arg], text=True, capture_output=True, cwd=str(REPO))
            print(run.stdout.rstrip())
            if run.stderr.strip():
                print(run.stderr.rstrip(), file=sys.stderr)
            # The probe's own PASS/FAIL lines are the real checks; keep them in
            # the machine-readable report instead of collapsing to one result.
            recorded = False
            for line in run.stdout.splitlines():
                match = re.match(r"^(PASS|FAIL)\s+(.*)$", line.strip())
                if match:
                    _reporter.record(match.group(1) == "PASS", f"{name}: {match.group(2)}")
                    recorded = True
            if not recorded:
                passed = run.returncode == 0 and not any(
                    line.startswith("FAIL") for line in run.stdout.splitlines()
                )
                _reporter.check(passed, f"{name} passed its cross-checks")
            elif run.returncode != 0:
                _reporter.record(False, f"{name} exited with {run.returncode}")
    finally:
        _stop(servers)

    return _reporter.summary()


if __name__ == "__main__":
    sys.exit(main())
