"""Regression cases for bugs found in review, driven by third-party clients.

Each case was verified to fail against the pre-fix server and to pass after:
  * duplicate Content-Length is a request-smuggling split -> 400;
  * a CORS subdomain wildcard must not accept look-alike hosts;
  * a hostile per-key rate-limit key space must not grow past max_keys;
  * an HTTP/2 request body that stops short of content-length resets the stream,
    and that reset must release the handler instead of parking it forever.
"""

from __future__ import annotations

import socket
import time

import h2.events
import httpx

from harness import BASE, PLAIN_PORT, check
from test_h2 import h2c_connect


def _pump(sock, conn):
    """Receive one batch, ack DATA, and flush. False on EOF."""
    data = sock.recv(65535)
    if not data:
        return False
    events = conn.receive_data(data)
    for event in events:
        if isinstance(event, h2.events.DataReceived):
            conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
    sock.sendall(conn.data_to_send())
    return events


def duplicate_content_length() -> None:
    req = (
        b"POST /echo HTTP/1.1\r\n"
        b"Host: x\r\n"
        b"Content-Length: 5\r\n"
        b"Content-Length: 6\r\n"
        b"Connection: close\r\n"
        b"\r\n"
        b"HELLO!"
    )
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=5) as s:
        s.sendall(req)
        resp = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            resp += chunk
    status = resp.split(b"\r\n", 1)[0] if resp else b"<none>"
    check(b" 400 " in status, f"duplicate Content-Length is refused -> {status.decode(errors='replace')}")


def cors_wildcard_boundary() -> None:
    url = f"{BASE}/cors-wildcard"
    cases = {
        "https://app.example.com": True,
        "https://a.b.example.com": True,
        "https://example.com": False,
        "https://evil-example.com": False,
        "https://notexample.com": False,
    }
    with httpx.Client(timeout=5) as c:
        for origin, allowed in cases.items():
            r = c.get(url, headers={"Origin": origin})
            got = "access-control-allow-origin" in r.headers
            check(got == allowed, f"CORS wildcard origin {origin!r} allowed={got} (expected {allowed})")


def rate_limit_key_cap() -> None:
    with httpx.Client(timeout=5) as c:
        for i in range(100):
            c.get(f"{BASE}/ratelimited", headers={"x-rate-key": f"hostile-{i}"})
        count = int(c.get(f"{BASE}/ratelimit-keys").text)
    check(count <= 4, f"a hostile rate-limit key space stays capped -> {count} buckets (max 4)")


def _query_upload_active(sock, conn) -> str:
    sid = conn.get_next_available_stream_id()
    conn.send_headers(sid, [(":method", "GET"), (":path", "/upload-active"),
                            (":scheme", "http"), (":authority", f"127.0.0.1:{PLAIN_PORT}")],
                      end_stream=True)
    sock.sendall(conn.data_to_send())
    body = b""
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        events = _pump(sock, conn)
        if not events:
            break
        for event in events:
            if isinstance(event, h2.events.DataReceived) and event.stream_id == sid:
                body += event.data
            elif isinstance(event, h2.events.StreamEnded) and event.stream_id == sid:
                return body.decode()
    return body.decode() or "<none>"


def h2_short_body_releases_handler() -> None:
    sock, conn = h2c_connect()
    try:
        # content-length 100, but only 5 bytes and END_STREAM: the server must
        # reset the stream and fail the handler's body channel.
        sid = conn.get_next_available_stream_id()
        conn.send_headers(sid, [(":method", "POST"), (":path", "/upload"),
                                (":scheme", "http"), (":authority", f"127.0.0.1:{PLAIN_PORT}"),
                                ("content-length", "100")],
                          end_stream=False)
        conn.send_data(sid, b"hello", end_stream=True)
        sock.sendall(conn.data_to_send())

        reset = False
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and not reset:
            events = _pump(sock, conn)
            if not events:
                break
            for event in events:
                if isinstance(event, h2.events.StreamReset) and event.stream_id == sid:
                    reset = True
        check(reset, "a short HTTP/2 body resets the stream")

        time.sleep(0.3)
        active = _query_upload_active(sock, conn)
        check(active == "0", f"the reset released the parked upload handler -> active={active}")
    finally:
        sock.close()


def h2_backed_up_body_does_not_stall_other_streams() -> None:
    """A stream that backs up its request body must not stall its siblings.

    Stream 1 POSTs to /sync (whose handler ignores the body) as more than the
    inbound Body channel capacity (1024) of one-byte DATA frames, then stream 2
    makes a normal request on the same connection. Pre-fix the engine set one
    connection-wide parse pause when the channel filled, so stream 2 never got a
    response; the pause is now per-stream.
    """
    sock, conn = h2c_connect()
    try:
        n = 1500  # > Body channel capacity (1024 frames)
        sid1 = conn.get_next_available_stream_id()
        conn.send_headers(sid1, [(":method", "POST"), (":path", "/sync"),
                                 (":scheme", "http"), (":authority", f"127.0.0.1:{PLAIN_PORT}"),
                                 ("content-length", str(n))], end_stream=False)
        for i in range(n):
            conn.send_data(sid1, b"x", end_stream=(i == n - 1))
        sock.sendall(conn.data_to_send())

        sid2 = conn.get_next_available_stream_id()
        conn.send_headers(sid2, [(":method", "GET"), (":path", "/world"),
                                 (":scheme", "http"), (":authority", f"127.0.0.1:{PLAIN_PORT}")],
                          end_stream=True)
        sock.sendall(conn.data_to_send())

        answered = False
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and not answered:
            events = _pump(sock, conn)
            if not events:
                break
            for event in events:
                if isinstance(event, h2.events.ResponseReceived) and event.stream_id == sid2:
                    answered = True
        check(answered, "a backed-up body on one stream does not stall another stream")
    finally:
        sock.close()


def run() -> None:
    print("\n== regressions (third-party clients) ==")
    duplicate_content_length()
    cors_wildcard_boundary()
    rate_limit_key_cap()
    h2_short_body_releases_handler()
    h2_backed_up_body_does_not_stall_other_streams()
