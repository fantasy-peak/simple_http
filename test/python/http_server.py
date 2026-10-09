#!/usr/bin/env python3
"""A standalone HTTP/1.1 server for cross-validating simple_http's client
against an independent implementation (stdlib `http.server`, not simple_http).

The library's own suites drive simple_http's server with simple_http's own
client, so a spec misreading shared by both halves cancels out. Here the C++
client (test/client_cross.cpp) meets a server written by someone else; a
disagreement means one of the two is wrong.

Endpoints (all plaintext 127.0.0.1):
  /echo            POST: echo the body back
  /hello           GET:  "hello cross"
  /status/<n>      GET:  that status
  /redirect        302 → /hello (tests client redirect-following)
  /redirect-loop   302 → /redirect-loop (client must give up)
  /big?n=<bytes>   GET:  n bytes (default 65536) — streaming beyond one packet
  /set-cookie      GET:  one Set-Cookie, for the client CookieJar

Usage: test/python/.venv/bin/python test/python/http_server.py [port]
"""

from __future__ import annotations

import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):  # keep the console quiet
        pass

    def _send(self, status, body=b"", content_type="text/plain", extra=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        path, _, qs = self.path.partition("?")
        if path == "/hello":
            self._send(200, b"hello cross")
        elif path == "/big":
            n = 65536
            for kv in qs.split("&"):
                if kv.startswith("n="):
                    n = int(kv[2:])
            self._send(200, b"x" * n)
        elif path == "/set-cookie":
            self._send(200, b"ok", extra={"Set-Cookie": "sid=abc123; Path=/"})
        elif path.startswith("/status/"):
            try:
                self._send(int(path.rsplit("/", 1)[1]))
            except ValueError:
                self._send(400, b"bad status")
        elif path == "/redirect":
            self._send(302, b"", extra={"Location": "/hello"})
        elif path == "/redirect-loop":
            self._send(302, b"", extra={"Location": "/redirect-loop"})
        else:
            self._send(404, b"not found")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        if self.path == "/echo":
            self._send(200, body)
        else:
            self._send(404, b"not found")

    # WebSocket upgrade is out of scope here (protocol is not HTTP); the
    # WebSocket cross-check lives in ws_cross / ws_echo_server.py.


def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 27921
    with ThreadingHTTPServer(("127.0.0.1", port), Handler) as httpd:
        print(f"http echo server listening on 127.0.0.1:{port}", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()