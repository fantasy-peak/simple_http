#!/usr/bin/env python3
"""C++ client against a Python reference server.

The inverse of run.py: instead of a Python client driving the C++ server, this
starts an http.server with the endpoints the client driver checks (GET, POST
echo, a redirect, Set-Cookie and a Cookie echo), then runs test_cpp17_client and
relays its PASS/FAIL lines.
"""

from __future__ import annotations

import gzip
import http.server
import os
import socket
import socketserver
import subprocess
import sys
import threading
import time
from pathlib import Path

TEST = Path(__file__).resolve().parent
ROOT = TEST.parents[1]
BIN_DIR = Path(os.environ.get("SIMPLE_HTTP_TEST_BIN_DIR", ROOT / "build" / "linux" / "x86_64" / "release"))
DRIVER = BIN_DIR / "test_cpp17_client"
FIXTURE = BIN_DIR / "fixture_cpp17_server"


class ConnectProxy(socketserver.ThreadingTCPServer):
    """Minimal HTTP CONNECT proxy: opens a tunnel and splices bytes verbatim."""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), ConnectHandler)
        self.used = False


class ConnectHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        self.server.used = True  # type: ignore[attr-defined]
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            head += chunk
        line = head.split(b"\r\n", 1)[0].decode()
        parts = line.split()
        if len(parts) != 3 or parts[0] != "CONNECT":
            self.request.sendall(b"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n")
            return
        host, _, port = parts[1].rpartition(":")
        try:
            upstream = socket.create_connection((host, int(port)), timeout=5)
        except OSError:
            self.request.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
            return
        self.request.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")

        def pump(src: socket.socket, dst: socket.socket) -> None:
            try:
                while True:
                    data = src.recv(16384)
                    if not data:
                        break
                    dst.sendall(data)
            except OSError:
                pass
            finally:
                try:
                    dst.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        thread = threading.Thread(target=pump, args=(upstream, self.request), daemon=True)
        thread.start()
        pump(self.request, upstream)
        thread.join(timeout=5)
        upstream.close()


class Socks5Proxy(socketserver.ThreadingTCPServer):
    """Minimal SOCKS5 CONNECT proxy (RFC 1928), no authentication."""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), Socks5Handler)
        self.used = False


class Socks5Handler(socketserver.BaseRequestHandler):
    def _recv_exact(self, n: int) -> bytes:
        data = b""
        while len(data) < n:
            chunk = self.request.recv(n - len(data))
            if not chunk:
                raise OSError("eof")
            data += chunk
        return data

    def handle(self) -> None:
        self.server.used = True  # type: ignore[attr-defined]
        try:
            version, count = self._recv_exact(2)
            self._recv_exact(count)
            self.request.sendall(b"\x05\x00")  # no authentication
            version, command, _reserved, atyp = self._recv_exact(4)
            assert version == 5 and command == 1
            if atyp == 1:
                host = socket.inet_ntoa(self._recv_exact(4))
            elif atyp == 4:
                host = socket.inet_ntop(socket.AF_INET6, self._recv_exact(16))
            elif atyp == 3:
                length = self._recv_exact(1)[0]
                host = self._recv_exact(length).decode()
            else:
                raise OSError("bad atyp")
            port = int.from_bytes(self._recv_exact(2), "big")
            upstream = socket.create_connection((host, port), timeout=5)
        except OSError:
            self.request.sendall(b"\x05\x01\x00\x01\x00\x00\x00\x00\x00\x00")
            return
        self.request.sendall(b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00")

        def pump(src: socket.socket, dst: socket.socket) -> None:
            try:
                while True:
                    data = src.recv(16384)
                    if not data:
                        break
                    dst.sendall(data)
            except OSError:
                pass
            finally:
                try:
                    dst.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        thread = threading.Thread(target=pump, args=(upstream, self.request), daemon=True)
        thread.start()
        pump(self.request, upstream)
        thread.join(timeout=5)
        upstream.close()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, status: int, body: bytes = b"", headers: dict[str, str] | None = None) -> None:
        self.send_response(status)
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 - http.server API
        if self.path == "/hello":
            self._send(200, b"hello")
        elif self.path == "/redirect":
            self._send(302, b"", {"Location": "/hello"})
        elif self.path == "/set-cookie":
            self._send(200, b"set", {"Set-Cookie": "session=abc; Path=/"})
        elif self.path == "/cookie":
            self._send(200, self.headers.get("Cookie", "").encode())
        elif self.path == "/gzip":
            self._send(200, gzip.compress(b"hello gzip"), {"Content-Encoding": "gzip"})
        elif self.path == "/sse":
            self._send(200, b"data: one\n\ndata: two\n\n",
                       {"Content-Type": "text/event-stream"})
        else:
            self._send(404, b"missing")

    def do_POST(self) -> None:  # noqa: N802 - http.server API
        length = int(self.headers.get("Content-Length", "0"))
        self._send(200, self.rfile.read(length))

    def log_message(self, *args) -> None:  # keep the suite log quiet
        pass


def main() -> int:
    if not DRIVER.exists():
        print(f"missing binary {DRIVER}")
        return 2

    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]
    proxy = ConnectProxy()
    proxy_thread = threading.Thread(target=proxy.serve_forever, daemon=True)
    proxy_thread.start()
    proxy_port = proxy.server_address[1]

    socks = Socks5Proxy()
    threading.Thread(target=socks.serve_forever, daemon=True).start()
    socks_port = socks.server_address[1]
    fixture = None
    tls_port = None
    seen: dict[str, int] = {}
    try:
        if FIXTURE.exists():
            fixture = subprocess.Popen([str(FIXTURE)], cwd=str(ROOT), stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            deadline = time.monotonic() + 10
            seen: dict[str, int] = {}
            assert fixture.stdout is not None
            while time.monotonic() < deadline and "TLSPORT" not in seen:
                line = fixture.stdout.readline()
                if not line:
                    break
                if line.startswith("PORT "):
                    seen["PORT"] = int(line.split()[1])
                elif line.startswith("TLSPORT "):
                    seen["TLSPORT"] = int(line.split()[1])
            tls_port = seen.get("TLSPORT")

        command = [str(DRIVER), str(port)]
        if tls_port is not None:
            command.append(str(tls_port))
        if "PORT" in seen:
            command.append(str(seen["PORT"]))
        command.append(str(proxy_port))
        command.append(str(socks_port))
        # The driver tests proxies explicitly through ClientConfig; inherited
        # HTTP(S)_PROXY/ALL_PROXY/NO_PROXY would make its direct requests take a
        # different path (and this CI host's uppercase NO_PROXY does not match
        # the loopback literals).
        env = {k: v for k, v in os.environ.items()
               if k.lower() not in {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}}
        result = subprocess.run(command, cwd=str(ROOT), env=env, timeout=60)
        if not proxy.used:
            print("FAIL  client CONNECT proxy was not used")
            return 1
        print("PASS  client CONNECT proxy tunnel")
        if not socks.used:
            print("FAIL  client SOCKS5 proxy was not used")
            return 1
        print("PASS  client SOCKS5 proxy tunnel")
        return result.returncode
    finally:
        if fixture is not None:
            fixture.terminate()
            try:
                fixture.wait(timeout=10)
            except subprocess.TimeoutExpired:
                fixture.kill()
                fixture.wait()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    sys.exit(main())
