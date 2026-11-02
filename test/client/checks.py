#!/usr/bin/env python3
"""Standalone client checks, each driven against a purpose-built Python peer.

These are narrower than server/run.py or client/run.py: one behaviour per peer,
so a failure points at one client contract rather than a protocol matrix.

  probe_h1_duplex          <- h1_stall_server.py       (half-duplex guard)
  probe_body_cap           <- big_body_server.py       (buffered body cap)
  probe_read_all_timeout   <- slow_body_server.py      (body-idle deadline)
  probe_retry              <- retry_echo_server.py     (default retry policy)
  probe_h2_limit           <- h2_max_concurrent_server.py (peer stream limit)
  test_h1_lifetime         <- in-process (no peer)

Run from anywhere (the test runner sets SIMPLE_HTTP_TEST_BIN_DIR):
    test/.venv/bin/python test/client/checks.py
"""

from __future__ import annotations

import os
import select
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
PEERS = HERE / "peers"

sys.path.insert(0, str(HERE.parent / "support"))
from report import Reporter  # noqa: E402

_reporter = Reporter(os.environ.get("SIMPLE_HTTP_TEST_SUITE", "client-checks"))


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


def start_peer(script: str, *args: str) -> subprocess.Popen:
    return subprocess.Popen(
        [sys.executable, str(PEERS / script), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        cwd=str(REPO),
    )


def wait_line(proc: subprocess.Popen, needle: str, timeout: float = 10.0) -> bool:
    """Waits for ``needle`` in the peer's stdout.

    The client checks use raw one-connection peers that must not be probed with
    a TCP connect (that would consume the connection), so the startup line is
    the readiness signal.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        ready, _, _ = select.select([proc.stdout], [], [], 0.2)
        if not ready:
            continue
        line = proc.stdout.readline()
        if not line:
            return False
        if needle in line:
            return True
    return False


def stop(*procs: subprocess.Popen) -> None:
    for proc in procs:
        proc.terminate()
    for proc in procs:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_probe(name: str, *args: str) -> bool:
    binary = find_binary(name)
    run = subprocess.run([str(binary), *args], text=True, capture_output=True, cwd=str(REPO))
    output = (run.stdout or "").strip()
    if output:
        print(output)
    if run.stderr and run.stderr.strip():
        print(run.stderr.rstrip(), file=sys.stderr)
    return run.returncode == 0 and not any(
        line.startswith("FAIL") for line in (run.stdout or "").splitlines()
    )


def main() -> int:
    peers: list[subprocess.Popen] = []
    try:
        peer = start_peer("h1_stall_server.py", "7822")
        peers.append(peer)
        if wait_line(peer, "listening"):
            _reporter.check(run_probe("probe_h1_duplex", "http://127.0.0.1:7822/", "1500"),
                            "HTTP/1.1 half-duplex guard")
        else:
            _reporter.check(False, "h1_stall_server ready")

        peer = start_peer("big_body_server.py", "7823", "8388608")
        peers.append(peer)
        if wait_line(peer, "listening"):
            _reporter.check(run_probe("probe_body_cap", "http://127.0.0.1:7823/", "1048576"),
                            "streaming response body cap")
        else:
            _reporter.check(False, "big_body_server ready")

        peer = start_peer("slow_body_server.py", "7824", "8388608", "2")
        peers.append(peer)
        if wait_line(peer, "listening"):
            _reporter.check(run_probe("probe_read_all_timeout", "http://127.0.0.1:7824/", "500"),
                            "read_all() is exempt from the body-idle budget; read() is not")
        else:
            _reporter.check(False, "slow_body_server ready")

        peer = start_peer("retry_echo_server.py", "7825", "8388608")
        peers.append(peer)
        if wait_line(peer, "listening"):
            _reporter.check(run_probe("probe_retry", "http://127.0.0.1:7825/"),
                            "default retry condition")
        else:
            _reporter.check(False, "retry_echo_server ready")

        peer = start_peer("h2_max_concurrent_server.py", "--port", "7813", "--limit", "0")
        peers.append(peer)
        if wait_line(peer, "listening"):
            _reporter.check(run_probe("probe_h2_limit", "http://127.0.0.1:7813/", "200"),
                            "peer SETTINGS_MAX_CONCURRENT_STREAMS")
        else:
            _reporter.check(False, "h2_max_concurrent_server ready")

        _reporter.check(run_probe("test_h1_lifetime", "7826"),
                        "h1 deferred body lifetime")
    finally:
        stop(*peers)

    return _reporter.summary()


if __name__ == "__main__":
    sys.exit(main())
