"""Shared plumbing for the Python-side protocol tests.

Why these exist alongside the C++ suites: those drive simple_http with
simple_http's *own* client, so a misreading of the spec shared by both sides
cancels out and they agree on something wrong. httpx, h2 and websockets are
independent implementations, and they disagree with the library exactly where the
library is wrong.

The server under test is the example in test/server.cpp — plaintext on 7788,
mutual TLS on 7789 — started as a subprocess and torn down at the end. Build it
first: `xmake build server`.
"""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def find_server() -> Path:
    """The example server binary, wherever this build put it.

    The path depends on the configured mode: a local `xmake build server` is
    usually debug, CI builds release.
    """
    candidates = (
        ROOT / "build" / "linux" / "x86_64" / "debug" / "server",
        ROOT / "build" / "linux" / "x86_64" / "release" / "server",
        ROOT / "build-dbg" / "linux" / "x86_64" / "debug" / "server",
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]  # let Server report the missing path it expected

PLAIN_PORT = 7788
TLS_PORT = 7789
BASE = f"http://127.0.0.1:{PLAIN_PORT}"
BASE_TLS = f"https://127.0.0.1:{TLS_PORT}"

CERT_DIR = ROOT / "test" / "tls_certificates"

_checks = 0
_failed = 0


def check(ok: bool, what: str) -> bool:
    """Records one assertion. Returns `ok` so callers can bail early."""
    global _checks, _failed
    _checks += 1
    if ok:
        print(f"PASS  {what}")
    else:
        _failed += 1
        print(f"FAIL  {what}")
    return ok


def fail(what: str) -> None:
    check(False, what)


def summary() -> int:
    print(f"\n{_checks} checks, {_failed} failed")
    return 1 if _failed else 0


class Server:
    """The example server (test/server.cpp) as a subprocess."""

    def __init__(self, binary: Path | None = None) -> None:
        self._binary = binary or find_server()
        self._proc: subprocess.Popen | None = None

    def __enter__(self) -> "Server":
        if not self._binary.exists():
            sys.exit(f"{self._binary} not built — run `xmake build server` first")
        self._proc = subprocess.Popen(
            [str(self._binary)],
            cwd=ROOT,  # the TLS test certs are referenced relatively
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self._await_port(PLAIN_PORT)
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
