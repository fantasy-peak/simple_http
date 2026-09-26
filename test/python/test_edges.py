"""Server-side boundary cases, driven by third-party clients.

Where the other Python modules check that the protocols work, this one checks the
edges: the sizes, bursts and malformed input that the C++ suites either cannot
reach (they use the library's own client) or do not ask about.

The headline case is HTTP/2 receive backpressure — see h2_receive_backpressure.
"""

from __future__ import annotations

import socket
import struct

import h2.events
import httpx

from harness import BASE, PLAIN_PORT, check, fail
from test_h2 import collect, exchange, h2c_connect


def h2_receive_backpressure() -> None:
    """A body sent as one burst of many small DATA frames must arrive whole.

    This is the case the server's receive-side backpressure exists for. Its frame
    parser drains every complete frame in the receive buffer in a single
    synchronous pass — there is no suspension in that loop, so the consumer
    coroutine never gets to run while it works — and a few thousand one-byte
    frames therefore outrun a handler that has not read yet. Before the
    backpressure, the frames past the body channel's capacity were dropped and
    the handler read a truncated body as if it were complete.

    /drain reads the whole body and reports its length, so a dropped frame shows
    up as a short count rather than as an error.
    """
    frames = 2000  # well past the body channel's capacity, well inside the window
    sock, conn = h2c_connect()
    try:
        stream_id = conn.get_next_available_stream_id()
        conn.send_headers(
            stream_id,
            [(":method", "POST"), (":path", "/drain"), (":scheme", "http"),
             (":authority", f"127.0.0.1:{PLAIN_PORT}")],
            end_stream=False,
        )
        for _ in range(frames):
            conn.send_data(stream_id, b"x")
        conn.send_data(stream_id, b"", end_stream=True)
        # One write: the server receives them together, which is what makes this
        # a backpressure test rather than a test of its read loop.
        sock.sendall(conn.data_to_send())

        status, _, body = collect(sock, conn, stream_id)
        check(status == 200 and body == f"received {frames} bytes".encode(),
              f"{frames} one-byte DATA frames arrive whole -> {body!r}")
    except (TimeoutError, socket.timeout):
        fail("the burst stalled: the server stopped reading and never resumed")
    finally:
        sock.close()


def h2_large_body() -> None:
    """1 MiB of DATA frames, which has to cross the flow-control window."""
    payload = b"y" * (1024 * 1024)
    sock, conn = h2c_connect()
    try:
        # Sent in 16 KiB pieces: one send_data of 1 MiB would exceed the window
        # and hyper-h2 would refuse it, so this is also the client half of flow
        # control being exercised.
        stream_id = conn.get_next_available_stream_id()
        conn.send_headers(
            stream_id,
            [(":method", "POST"), (":path", "/drain"), (":scheme", "http"),
             (":authority", f"127.0.0.1:{PLAIN_PORT}")],
            end_stream=False,
        )
        view = memoryview(payload)
        for at in range(0, len(payload), 16384):
            chunk = bytes(view[at:at + 16384])
            conn.send_data(stream_id, chunk, end_stream=False)
            sock.sendall(conn.data_to_send())
            # Let the server's WINDOW_UPDATEs come back before sending more.
            sock.settimeout(0.01)
            try:
                while True:
                    conn.receive_data(sock.recv(65535))
            except (TimeoutError, socket.timeout):
                pass
            sock.settimeout(10)
        conn.send_data(stream_id, b"", end_stream=True)
        sock.sendall(conn.data_to_send())

        status, _, body = collect(sock, conn, stream_id)
        check(status == 200 and body == f"received {len(payload)} bytes".encode(),
              f"a 1 MiB request body over DATA frames -> {body!r}")
    except (TimeoutError, socket.timeout):
        fail("a 1 MiB upload stalled: the upload window was never replenished")
    finally:
        sock.close()


def h2_strips_connection_specific_headers() -> None:
    """/badhdr sets Connection and Transfer-Encoding on the response.

    Both are illegal in HTTP/2 (RFC 9113 §8.2.2) and would make hyper-h2 treat the
    response as malformed, so the engine has to drop them — while keeping the
    header that is fine.
    """
    sock, conn = h2c_connect()
    try:
        status, headers, _ = exchange(sock, conn, "/badhdr")
        names = {name for name, _ in headers.items()}
        check(status == 200, f"/badhdr answers -> {status}")
        check("connection" not in names, f"Connection is stripped -> {sorted(names)}")
        check("transfer-encoding" not in names, "Transfer-Encoding is stripped")
        check("x-ok" in names, "an ordinary header survives")
    except Exception as exc:  # noqa: BLE001 — hyper-h2 raises on a malformed block
        fail(f"the response block was rejected by hyper-h2: {exc!r}")
    finally:
        sock.close()


def h2_reset_then_reuse() -> None:
    """A reset stream must not take the connection with it."""
    sock, conn = h2c_connect()
    try:
        stream_id = conn.get_next_available_stream_id()
        conn.send_headers(
            stream_id,
            [(":method", "GET"), (":path", "/big?n=200000"), (":scheme", "http"),
             (":authority", f"127.0.0.1:{PLAIN_PORT}")],
            end_stream=True,
        )
        sock.sendall(conn.data_to_send())
        # Take one frame, then cancel: the server has to stop sending on this
        # stream without tearing down the connection.
        data = sock.recv(65535)
        for event in conn.receive_data(data):
            if isinstance(event, h2.events.DataReceived):
                # Acknowledge first. The connection-level window is shared by every
                # stream on the connection, and a reset does not return the credit
                # for data already received (RFC 9113 §5.1) — so a client that
                # resets without acking stalls every stream, including its own next
                # request. That is a rule of the protocol, not a server defect.
                conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                conn.reset_stream(event.stream_id)
                break
        sock.sendall(conn.data_to_send())

        status, _, body = exchange(sock, conn, "/world")
        check(status == 200 and body == b"hello from HTTP/2",
              f"the connection still works after a reset -> {status} {body!r}")
    finally:
        sock.close()


def h1_large_upload() -> None:
    """1 MiB up over HTTP/1.1, and the server must account for every byte."""
    payload = b"z" * (1024 * 1024)
    with httpx.Client(http2=False, timeout=30.0) as client:
        r = client.post(f"{BASE}/drain", content=payload)
        check(r.status_code == 200 and r.text == f"received {len(payload)} bytes",
              f"a 1 MiB request body -> {r.text!r}")


def h1_oversized_head() -> None:
    """A request head past the limit is refused with 431, not dropped.

    httpx will not send it (it caps header size itself), so this goes over a raw
    socket — a header line larger than the server's max_header_bytes.
    """
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        # Comfortably past any sane limit: the example server does not configure
        # max_header_bytes, so this has to beat its default rather than a value
        # the test picked.
        big = "x" * (256 * 1024)
        sock.sendall(f"GET /world HTTP/1.1\r\nHost: x\r\nX-Big: {big}\r\n\r\n".encode())
        seen = b""
        sock.settimeout(5)
        try:
            while b"\r\n\r\n" not in seen:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                seen += chunk
        except (TimeoutError, socket.timeout):
            pass
        check(b"431" in seen, f"an oversized head is refused with 431 -> {seen[:40]!r}")


def ws_unmasked_frame_is_refused() -> None:
    """RFC 6455 §5.1: a client frame must be masked.

    An unmasked frame from a client is a protocol error, and accepting it is how
    a cache-poisoning proxy gets exploited — the mask is what stops a client's
    bytes being replayed verbatim through an intermediary. `websockets` always
    masks, so this hand-rolls the frame on a raw socket.
    """
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        sock.settimeout(5)
        handshake = (
            "GET /chat HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        sock.sendall(handshake.encode())
        reply = b""
        try:
            while b"\r\n\r\n" not in reply:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                reply += chunk
        except (TimeoutError, socket.timeout):
            pass
        if b"101" not in reply:
            fail(f"the WebSocket handshake did not complete -> {reply[:60]!r}")
            return

        # A text frame with FIN set, no mask bit, five bytes of payload.
        sock.sendall(struct.pack("!BB", 0x81, 0x05) + b"hello")
        seen = b""
        try:
            seen = sock.recv(4096)
        except (TimeoutError, socket.timeout):
            seen = b""
        # Either a Close frame (opcode 0x8) or a bare close — anything but an
        # echo of the unmasked payload.
        refused = not seen or (seen[0] & 0x0F) == 0x8
        check(refused, f"an unmasked client frame is refused -> {seen[:16]!r}")


def run() -> None:
    print("\n== server boundaries (third-party clients) ==")
    h2_receive_backpressure()
    h2_large_body()
    h2_strips_connection_specific_headers()
    h2_reset_then_reuse()
    h1_large_upload()
    h1_oversized_head()
    ws_unmasked_frame_is_refused()
