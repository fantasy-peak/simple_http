"""HTTP/2 behaviour, driven by hyper-h2 and httpx.

hyper-h2 is the reference implementation of the HTTP/2 state machine, so it is
strict exactly where it should be: a frame the server mis-serializes, a
flow-control window it miscounts, or a pseudo-header rule it ignores surfaces as
an exception or a reset rather than as a plausible-looking response.

Two paths are covered:
  * h2c prior knowledge (plaintext, a manual hyper-h2 connection) — the path the
    C++ suite exercises with simple_http's own client;
  * HTTP/2 over TLS with ALPN and mutual TLS, via httpx.
"""

from __future__ import annotations

import socket

import h2.config
import h2.connection
import h2.events
import httpx

from harness import BASE_TLS, CERT_DIR, PLAIN_PORT, check, fail


def h2c_connect(port: int = PLAIN_PORT):
    """A hyper-h2 connection in prior-knowledge mode (no Upgrade dance)."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    conn = h2.connection.H2Connection(
        config=h2.config.H2Configuration(client_side=True, header_encoding="utf-8")
    )
    conn.initiate_connection()
    sock.sendall(conn.data_to_send())
    return sock, conn


def exchange(sock, conn, path: str, method: str = "GET", body: bytes | None = None):
    """One request on its own stream. Returns (status, headers, body_bytes)."""
    stream_id = conn.get_next_available_stream_id()
    conn.send_headers(
        stream_id,
        [
            (":method", method),
            (":path", path),
            (":scheme", "http"),
            (":authority", f"127.0.0.1:{PLAIN_PORT}"),
        ],
        end_stream=body is None,
    )
    if body is not None:
        conn.send_data(stream_id, body, end_stream=True)
    sock.sendall(conn.data_to_send())
    return collect(sock, conn, stream_id)


def collect(sock, conn, stream_id: int):
    headers = None
    body = b""
    while True:
        data = sock.recv(65535)
        if not data:
            raise RuntimeError("the connection closed before the stream ended")
        for event in conn.receive_data(data):
            if isinstance(event, h2.events.ResponseReceived) and event.stream_id == stream_id:
                headers = event.headers
            elif isinstance(event, h2.events.DataReceived):
                # Acknowledge *every* stream's data, not only the one being waited
                # on. The connection-level window is shared by all streams on the
                # connection, so withholding credit for a stream this call is not
                # reading exhausts it — and the server then cannot send on the
                # stream this call *is* waiting for either. (That is what a naive
                # stream_id == stream_id filter here does, and it looks exactly
                # like a server that stopped answering.)
                conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                if event.stream_id == stream_id:
                    body += event.data
            elif isinstance(event, h2.events.StreamReset) and event.stream_id == stream_id:
                raise RuntimeError(f"the server reset the stream: {event.error_code}")
            elif isinstance(event, h2.events.StreamEnded) and event.stream_id == stream_id:
                return status_of(headers), dict(headers or []), body
        sock.sendall(conn.data_to_send())


def status_of(headers) -> int:
    if not headers:
        return 0
    for name, value in headers:
        if name == ":status":
            return int(value)
    return 0


def run() -> None:
    print("\n== HTTP/2 (hyper-h2, h2c prior knowledge) ==")
    h2_basics()
    h2_flow_control()
    h2_multiplexing()

    print("\n== HTTP/2 over TLS (httpx, ALPN + mutual TLS) ==")
    _tls()


def h2_basics() -> None:
    sock, conn = h2c_connect()
    try:
        status, headers, body = exchange(sock, conn, "/world")
        # The server answers the same handlers over h2, and reports the version
        # it actually served: "hello from HTTP/2" is proof it did not fall back.
        check(status == 200 and body == b"hello from HTTP/2",
              f"GET /world over h2c -> {status} {body!r}")

        status, _, _ = exchange(sock, conn, "/not-a-route")
        check(status == 404, f"an unknown path over h2c -> {status}")

        status, _, _ = exchange(sock, conn, "/throw")
        check(status == 500, f"a throwing handler over h2c -> {status}")

        # A request body over DATA frames, echoed back.
        payload = b"h2 request body"
        status, _, body = exchange(sock, conn, "/echo", method="POST", body=payload)
        check(status == 200 and body == payload,
              f"a DATA-framed request body is echoed -> {status} {len(body)} bytes")
    finally:
        sock.close()


def h2_flow_control() -> None:
    sock, conn = h2c_connect()
    try:
        # 1 MiB over a connection whose initial window is 65535: this only
        # completes if the server emits WINDOW_UPDATEs as we consume, so a
        # miscounted window shows up as a hang (the socket times out) rather
        # than as a wrong answer.
        status, _, body = exchange(sock, conn, "/big?n=1048576")
        check(status == 200 and len(body) == 1048576,
              f"a 1 MiB response crosses the flow-control window -> {len(body)} bytes")
    except (TimeoutError, socket.timeout):
        fail("a 1 MiB response stalled: the server never replenished the window")
    finally:
        sock.close()


def h2_multiplexing() -> None:
    sock, conn = h2c_connect()
    try:
        # Two streams open at once, answered on one connection. Interleaving is
        # what HTTP/2 is for, and a server that serialized them (or routed the
        # second response to the first stream) would show up as a mismatch here.
        a = conn.get_next_available_stream_id()
        conn.send_headers(a, [(":method", "GET"), (":path", "/big?n=200000"),
                              (":scheme", "http"), (":authority", f"127.0.0.1:{PLAIN_PORT}")],
                          end_stream=True)
        b = conn.get_next_available_stream_id()
        conn.send_headers(b, [(":method", "GET"), (":path", "/world"),
                              (":scheme", "http"), (":authority", f"127.0.0.1:{PLAIN_PORT}")],
                          end_stream=True)
        sock.sendall(conn.data_to_send())

        got = {}
        ended = set()
        # Wait for both streams to *end*, not merely to have produced something:
        # stopping at the first data would truncate the 200000-byte stream and
        # look like a server bug when it is the harness quitting early.
        while len(ended) < 2:
            data = sock.recv(65535)
            if not data:
                break
            for event in conn.receive_data(data):
                if isinstance(event, h2.events.DataReceived):
                    got[event.stream_id] = got.get(event.stream_id, b"") + event.data
                    conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                elif isinstance(event, h2.events.StreamEnded):
                    ended.add(event.stream_id)
            sock.sendall(conn.data_to_send())

        check(got.get(a, b"") == b"x" * 200000, f"the 200000-byte stream is intact -> {len(got.get(a, b''))}")
        check(got.get(b, b"") == b"hello from HTTP/2", f"the small stream is unaffected -> {got.get(b, b'')!r}")
    finally:
        sock.close()


def _tls() -> None:
    # ALPN picks h2; httpx is told to require it, so a server that negotiated
    # http/1.1 instead would fail rather than quietly downgrade.
    #
    # verify=False because the test CA lacks a key-usage extension and Python's
    # ssl rejects it — a property of the fixtures, not of the server. The C++ TLS
    # tests verify the same way. What is under test here is the h2/TLS path and
    # the client certificate reaching the handler, not certificate validation.
    with httpx.Client(http2=True, verify=False,
                      cert=(str(CERT_DIR / "client_cert.pem"), str(CERT_DIR / "client_key.pem")),
                      timeout=10.0) as client:
        r = client.get(f"{BASE_TLS}/world")
        check(r.status_code == 200 and r.http_version == "HTTP/2",
              f"mTLS + ALPN serves HTTP/2 -> {r.http_version} {r.text!r}")

        r = client.get(f"{BASE_TLS}/whoami")
        check(r.status_code == 200 and "SimpleHttpClient" in r.text,
              f"the client certificate reaches the handler -> {r.text!r}")

        r = client.post(f"{BASE_TLS}/echo", content=b"tls body")
        check(r.status_code == 200 and r.content == b"tls body",
              f"a request body over HTTP/2/TLS -> {len(r.content)} bytes")
