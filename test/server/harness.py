"""Shared plumbing for the Python-side protocol tests.

Why these exist alongside the C++ suites: those drive simple_http with
simple_http's *own* client, so a misreading of the spec shared by both sides
cancels out and they agree on something wrong. httpx, h2 and websockets are
independent implementations, and they disagree with the library exactly where the
library is wrong.

The server under test is the fixture in test/server/fixtures/server.cpp —
plaintext on 7788, mutual TLS on 7789 — started as a subprocess and torn down at
the end. Build it first: `xmake build fixture_server`.
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Shared PASS/FAIL recording (test/support/report.py).
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from report import Reporter  # noqa: E402

_reporter = Reporter(os.environ.get("SIMPLE_HTTP_TEST_SUITE", "server-conformance"))


def find_server() -> Path:
    """Locate the fixture server binary.

    The test runner exports SIMPLE_HTTP_TEST_BIN_DIR (and optionally
    SIMPLE_HTTP_TEST_SERVER_BIN); the fallbacks keep direct runs working.
    """
    override = os.environ.get("SIMPLE_HTTP_TEST_SERVER_BIN")
    if override:
        return Path(override)
    candidates = []
    bin_dir = os.environ.get("SIMPLE_HTTP_TEST_BIN_DIR")
    if bin_dir:
        candidates.append(Path(bin_dir) / "fixture_server")
    candidates.extend((
        ROOT / "build" / "linux" / "x86_64" / "release" / "fixture_server",
        ROOT / "build" / "linux" / "x86_64" / "debug" / "fixture_server",
        ROOT / "build-dbg" / "linux" / "x86_64" / "debug" / "fixture_server",
    ))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]  # let Server report the missing path it expected

PLAIN_PORT = 7788
TLS_PORT = 7789
BASE = f"http://127.0.0.1:{PLAIN_PORT}"
BASE_TLS = f"https://127.0.0.1:{TLS_PORT}"

# The fixture brings its listeners up asynchronously. Waiting only for 7788 lets
# a suite connect to 7790/7793 before they are bound, so wait for every TCP
# listener the suites may use; the QUIC listeners are bound in the same startup
# sequence and get a short grace period afterwards.
LISTENER_PORTS = (7788, 7789, 7790, 7791, 7792, 7793, 7794)

CERT_DIR = ROOT / "test" / "tls_certificates"

def check(ok: bool, what: str) -> bool:
    """Records one assertion. Returns `ok` so callers can bail early."""
    return _reporter.check(ok, what)


def fail(what: str) -> None:
    _reporter.check(False, what)


def summary() -> int:
    return _reporter.summary()


class Server:
    """The fixture server (test/server/fixtures/server.cpp) as a subprocess."""

    def __init__(self, binary: Path | None = None) -> None:
        self._binary = binary or find_server()
        self._proc: subprocess.Popen | None = None

    def __enter__(self) -> "Server":
        if not self._binary.exists():
            sys.exit(f"{self._binary} not built — run `xmake build fixture_server` first")
        self._proc = subprocess.Popen(
            [str(self._binary)],
            cwd=ROOT,  # the TLS test certs are referenced relatively
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        for port in LISTENER_PORTS:
            self._await_port(port)
        time.sleep(0.2)  # let the UDP/QUIC sockets finish binding
        return self

    def __exit__(self, *_exc) -> None:
        if self._proc is None:
            return
        self._proc.terminate()
        try:
            self._proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._proc.kill()

    @staticmethod
    def _await_port(port: int, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with socket.socket() as probe:
                probe.settimeout(0.2)
                if probe.connect_ex(("127.0.0.1", port)) == 0:
                    return
            time.sleep(0.05)
        raise RuntimeError(f"the server never accepted a connection on {port}")
