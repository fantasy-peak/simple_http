#!/usr/bin/env python3
"""Runs the Python-side protocol tests against the example server.

    test/python/.venv/bin/python test/python/run.py

These drive simple_http with third-party clients (httpx, h2, websockets) rather
than with the library's own, because a spec misreading shared by both halves of
the library cancels out: the C++ suites would agree with a wrong server. Where an
independent implementation disagrees, one of the two is wrong — which is the
whole value here.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import harness  # noqa: E402  (needs the sys.path line above)

import test_edges  # noqa: E402
import test_h2  # noqa: E402
import test_http1  # noqa: E402
import test_ws  # noqa: E402


def main() -> int:
    with harness.Server():
        test_http1.run()
        test_h2.run()
        test_ws.run()
        test_edges.run()
    return harness.summary()


if __name__ == "__main__":
    sys.exit(main())
