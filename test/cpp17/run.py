#!/usr/bin/env python3
"""Python-client interop test for the C++17 port.

Starts fixture_cpp17_server on ephemeral plaintext and TLS ports, then drives it
with httpx (HTTP/1.1 and HTTP/2) and websockets (RFC 6455) -- independent
implementations, so the checks do not share a bug with the C++ test client.

The inverse direction (a Python/Go server driving the C++ client) arrives with
the client port.
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

failures = 0


def check(condition: bool, what: str, detail: str = "") -> None:
    global failures
    if condition:
        print(f"PASS  {what}")
        return
    failures += 1
    print(f"FAIL  {what}{': ' + detail if detail else ''}")


def read_ports(process: subprocess.Popen[str], timeout: float = 10.0) -> tuple[int | None, int | None]:
    deadline = time.monotonic() + timeout
    plain: int | None = None
    tls: int | None = None
    assert process.stdout is not None
    while time.monotonic() < deadline and (plain is None or tls is None):
        line = process.stdout.readline()
        if not line:
            break
        if line.startswith("PORT "):
            plain = int(line.split()[1])
        elif line.startswith("TLSPORT "):
            tls = int(line.split()[1])
    return plain, tls


def main() -> int:
    if not SERVER.exists():
        print(f"missing binary {SERVER}")
        return 2

    # A second fixture process is the WebSocket backend for /ws-proxy, so the
    # reverse-proxy tunnel is exercised against an independent connection.
    backend = subprocess.Popen(
        [str(SERVER)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    process = None
    try:
        backend_port, _ = read_ports(backend)
        if backend_port is None:
            print("backend server did not report a port")
            return 1

        process = subprocess.Popen(
            [str(SERVER), "0", str(backend_port)],
            cwd=str(ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        port, tls_port = read_ports(process)
        if port is None:
            print("server did not report a port")
            return 1

        import httpx

        with httpx.Client(base_url=f"http://127.0.0.1:{port}", timeout=5.0) as client:
            response = client.get("/")
            check(response.status_code == 200 and response.text == "hello", "httpx GET /",
                  f"status={response.status_code} body={response.text!r}")

            payload = b"hello from python"
            response = client.post("/echo", content=payload)
            check(response.status_code == 200 and response.content == payload, "httpx POST /echo",
                  f"status={response.status_code} body={response.content!r}")

            response = client.get("/missing")
            check(response.status_code == 404, "httpx GET /missing", f"status={response.status_code}")

            response = client.head("/")
            check(response.status_code == 200 and response.content == b"", "httpx HEAD /",
                  f"status={response.status_code}")

            response = client.get("/file")
            check(response.status_code == 200 and response.text == "streamed file body",
                  "httpx GET /file (send_file)", f"status={response.status_code} body={response.text!r}")

            upload_payload = b"uploaded through the stream"
            response = client.post("/upload", content=upload_payload)
            check(response.status_code == 200 and response.text == str(len(upload_payload)),
                  "httpx POST /upload (read_body_to_file)",
                  f"status={response.status_code} body={response.text!r}")

        if tls_port is not None:
            with httpx.Client(base_url=f"https://127.0.0.1:{tls_port}", http2=True, verify=False,
                              timeout=5.0) as client:
                response = client.get("/")
                check(response.status_code == 200 and response.text == "hello", "httpx HTTP/2 GET /",
                      f"version={response.http_version} status={response.status_code} body={response.text!r}")
                check(response.http_version == "HTTP/2", "TLS ALPN negotiated h2",
                      f"version={response.http_version}")

                payload = b"h2 body"
                response = client.post("/echo", content=payload)
                check(response.status_code == 200 and response.content == payload, "httpx HTTP/2 POST /echo",
                      f"version={response.http_version} body={response.content!r}")

        from websockets.sync.client import connect

        with connect(f"ws://127.0.0.1:{port}/ws", open_timeout=5, close_timeout=2) as websocket:
            websocket.send("websocket echo")
            echoed = websocket.recv(timeout=5)
            check(echoed == "websocket echo", "websockets /ws echo", f"received={echoed!r}")

            websocket.send("second")
            check(websocket.recv(timeout=5) == "second", "websockets second message")

        with connect(f"ws://127.0.0.1:{port}/ws-proxy", open_timeout=5, close_timeout=2) as websocket:
            websocket.send("through the proxy")
            check(websocket.recv(timeout=5) == "through the proxy", "websockets /ws-proxy tunnel")
    except Exception as error:  # noqa: BLE001 - a failed interop check is a test failure
        check(False, "interop run", repr(error))
    finally:
        for proc in (process, backend):
            if proc is None:
                continue
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
