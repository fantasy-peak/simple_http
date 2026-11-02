"""Server-side request decompression (Content-Encoding: gzip/br).

The middleware is opt-in: only /decode mounts it, and the reverse-proxy routes
deliberately do not, so this module checks both sides of the Go semantics:

  * a local route sees the decoded body and no Content-Encoding/Content-Length;
  * a reverse-proxy route forwards the compressed bytes and headers untouched.

httpx drives HTTP/1.1; hyper-h2 drives h2c prior-knowledge. The body pipeline is
shared by h1/h2/h3, so the h2c cases cover the pushed-frame path as well.
"""

from __future__ import annotations

import gzip

import brotli
import httpx

import test_h2
from harness import BASE, check, fail

H2C_PORT = 7790
LIMIT = 1024 * 1024


def _wire(label: str, body: bytes) -> tuple[bytes, bytes]:
    if label == "gzip":
        return gzip.compress(body), body
    if label == "br":
        return brotli.compress(body), body
    raise AssertionError(label)


def _h1_post(client: httpx.Client, path: str, wire: bytes, coding: str) -> httpx.Response:
    return client.post(f"{BASE}{path}", content=wire, headers={"content-encoding": coding}, timeout=10.0)


def _h2c_exchange(port: int, path: str, method: str, headers: list[tuple[str, str]], body: bytes | None):
    sock, conn = test_h2.h2c_connect(port)
    try:
        stream_id = conn.get_next_available_stream_id()
        conn.send_headers(
            stream_id,
            [
                (":method", method),
                (":path", path),
                (":scheme", "http"),
                (":authority", f"127.0.0.1:{port}"),
                *headers,
            ],
            end_stream=body is None,
        )
        if body is not None:
            conn.send_data(stream_id, body, end_stream=True)
        sock.sendall(conn.data_to_send())
        return test_h2.collect(sock, conn, stream_id)
    finally:
        sock.close()


def _h2c_post(path: str, wire: bytes, coding: str):
    return _h2c_exchange(H2C_PORT, path, "POST", [("content-encoding", coding)], wire)


def run() -> None:
    print("\n== request decompression (opt-in middleware) ==")
    body = ("request body that was compressed on the wire; " * 64).encode()
    wire, original = _wire("gzip", body)

    with httpx.Client(http2=False, timeout=10.0) as client:
        # 1. gzip local route: decoded body, headers consumed by the middleware.
        r = _h1_post(client, "/decode", wire, "gzip")
        check(r.status_code == 200 and r.content.decode() == f"ce=none;cl=none;{original.decode()}",
              f"h1 gzip -> decoded body, headers removed ({r.status_code})")
        check(r.status_code == 200 and "content-encoding" not in r.headers,
              "h1 gzip -> the response does not leak the request encoding")

        # 2. brotli is supported when the listener opts in.
        br_wire, br_original = _wire("br", body)
        r = _h1_post(client, "/decode", br_wire, "br")
        check(r.status_code == 200 and r.content.decode() == f"ce=none;cl=none;{br_original.decode()}",
              f"h1 br -> decoded body ({r.status_code})")

        # 3. x-gzip is the historical alias.
        r = _h1_post(client, "/decode", wire, "x-gzip")
        check(r.status_code == 200 and r.content.decode().endswith(original.decode()),
              f"h1 x-gzip -> decoded body ({r.status_code})")

        # 4. An unsupported coding is rejected before the handler runs.
        r = _h1_post(client, "/decode", b"whatever", "deflate")
        check(r.status_code == 415, f"h1 deflate -> 415 ({r.status_code})")
        check("gzip" in r.headers.get("accept-encoding", "") and "br" in r.headers.get("accept-encoding", ""),
              "415 advertises the codings the middleware accepts")

        # 5. Malformed and truncated streams are 400.
        r = _h1_post(client, "/decode", b"not gzip at all", "gzip")
        check(r.status_code == 400, f"h1 malformed gzip -> 400 ({r.status_code})")
        r = _h1_post(client, "/decode", wire[:-4], "gzip")
        check(r.status_code == 400, f"h1 truncated gzip -> 400 ({r.status_code})")

        # 6. The decoded-size budget is enforced (the route sets 1 MiB).
        bomb_body = b"z" * (2 * LIMIT)
        bomb_wire = gzip.compress(bomb_body)
        r = _h1_post(client, "/decode", bomb_wire, "gzip")
        check(r.status_code == 413, f"h1 decoded-size budget -> 413 ({r.status_code})")

        # 7. identity is left alone; the handler sees the raw body and header.
        r = _h1_post(client, "/decode", b"plain", "identity")
        check(r.status_code == 200 and r.content.decode().endswith("plain"),
              f"h1 identity -> raw body ({r.status_code})")

        # 8. No Content-Encoding: the middleware is a no-op.
        r = client.post(f"{BASE}/decode", content=b"plain")
        check(r.status_code == 200 and r.content.decode().endswith("plain"),
              f"h1 no encoding -> raw body ({r.status_code})")

        # 9. Reverse-proxy routes do not mount the middleware: the backend must
        # receive the compressed bytes and the Content-Encoding header, exactly
        # like Go's httputil.ReverseProxy.
        r = _h1_post(client, "/rproxy/echo", wire, "gzip")
        check(r.status_code == 200 and r.content == wire,
              f"reverse proxy forwards the compressed body untouched ({r.status_code}, {len(r.content)} bytes)")

    # 10. The same decode path on pushed HTTP/2 DATA frames.
    status, headers, h2_body = _h2c_post("/decode", wire, "gzip")
    check(status == 200 and h2_body.decode() == f"ce=none;cl=none;{original.decode()}",
          f"h2c gzip -> decoded body ({status})")

    status, headers, _ = _h2c_post("/decode", b"whatever", "deflate")
    check(status == 415, f"h2c deflate -> 415 ({status})")
    check("gzip" in headers.get("accept-encoding", "") and "br" in headers.get("accept-encoding", ""),
          "h2c 415 advertises the accepted codings")

    # END_STREAM on HEADERS with a non-identity coding: an empty compressed body
    # is malformed (no gzip trailer), and must be a 400, not a hang or 500.
    status, _, _ = _h2c_exchange(H2C_PORT, "/decode", "POST", [("content-encoding", "gzip")], None)
    check(status == 400, f"h2c empty gzip body -> 400 ({status})")
