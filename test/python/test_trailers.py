"""Response trailers (RFC 9110 §6.5), driven by hyper-h2 and a raw HTTP/1.1 socket.

The server route is /trailers: a streamed body followed by a trailer section,
plus /trailers?oneshot=1 for the one-shot send() path (which has to switch to
streamed framing to have somewhere to put the fields).

Both transports are checked, because the wire shape differs completely: HTTP/1.1
hangs the fields off the chunked terminator ("0\\r\\n<fields>\\r\\n"), HTTP/2
sends a trailing HEADERS frame after the DATA. hyper-h2 surfaces the latter as a
TrailersReceived event — independent confirmation that the fields arrived where
the spec says they go, not merely that some bytes were written.
"""

from __future__ import annotations

import asyncio
import socket
import ssl

import h2.config
import h2.connection
import h2.events

from harness import PLAIN_PORT, check, fail

H3_PORT = 7792  # the dual-transport listener's QUIC side (test/server.cpp)


def h2c_connect():
    sock = socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10)
    conn = h2.connection.H2Connection(
        config=h2.config.H2Configuration(client_side=True, header_encoding="utf-8")
    )
    conn.initiate_connection()
    sock.sendall(conn.data_to_send())
    return sock, conn


def h2_get(path: str, method: str = "GET"):
    """One request on its own connection. Returns (status, headers, body, trailers)."""
    sock, conn = h2c_connect()
    try:
        sid = conn.get_next_available_stream_id()
        conn.send_headers(
            sid,
            [
                (":method", method),
                (":path", path),
                (":scheme", "http"),
                (":authority", f"127.0.0.1:{PLAIN_PORT}"),
            ],
            end_stream=True,
        )
        sock.sendall(conn.data_to_send())

        status = 0
        headers: dict = {}
        body = b""
        trailers: dict | None = None
        while True:
            data = sock.recv(65535)
            if not data:
                raise RuntimeError("the connection closed before the stream ended")
            for event in conn.receive_data(data):
                if isinstance(event, h2.events.ResponseReceived) and event.stream_id == sid:
                    headers = dict(event.headers)
                    status = int(headers.get(":status", 0))
                elif isinstance(event, h2.events.DataReceived):
                    conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                    if event.stream_id == sid:
                        body += event.data
                elif isinstance(event, h2.events.TrailersReceived) and event.stream_id == sid:
                    trailers = dict(event.headers)
                elif isinstance(event, h2.events.StreamReset) and event.stream_id == sid:
                    raise RuntimeError(f"the server reset the stream: {event.error_code}")
                elif isinstance(event, h2.events.StreamEnded) and event.stream_id == sid:
                    return status, headers, body, trailers
            sock.sendall(conn.data_to_send())
    finally:
        sock.close()


def h2_trailers() -> None:
    try:
        status, _, body, trailers = h2_get("/trailers")
        check(status == 200 and body == b"first second",
              f"h2 /trailers streams the body -> {status} {body!r}")
        check(trailers is not None, "h2 /trailers delivers a trailer section")
        check((trailers or {}).get("x-checksum") == "abc123",
              f"h2 trailer x-checksum -> {(trailers or {}).get('x-checksum')!r}")
        check((trailers or {}).get("x-trailer-count") == "2",
              f"h2 trailer x-trailer-count -> {(trailers or {}).get('x-trailer-count')!r}")
    except (TimeoutError, socket.timeout):
        fail("h2 /trailers stalled")
    except RuntimeError as exc:
        fail(f"h2 /trailers: {exc}")


def h2_trailers_oneshot() -> None:
    try:
        status, _, body, trailers = h2_get("/trailers?oneshot=1")
        check(status == 200 and body == b"oneshot body",
              f"h2 one-shot with trailers -> {status} {body!r}")
        check((trailers or {}).get("x-checksum") == "abc123",
              f"h2 one-shot trailer x-checksum -> {(trailers or {}).get('x-checksum')!r}")
    except (TimeoutError, socket.timeout):
        fail("h2 /trailers?oneshot=1 stalled")
    except RuntimeError as exc:
        fail(f"h2 /trailers?oneshot=1: {exc}")


def h1_trailers() -> None:
    try:
        with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
            sock.sendall(b"GET /trailers HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            raw = b""
            while True:
                chunk = sock.recv(65535)
                if not chunk:
                    break
                raw += chunk
    except (TimeoutError, socket.timeout):
        fail("h1 /trailers stalled")
        return

    head, _, body = raw.partition(b"\r\n\r\n")
    check(b"transfer-encoding: chunked" in head.lower(), "h1 /trailers is chunked")
    check(b"first " in body and b"second" in body, "h1 body precedes the trailer section")
    # The chunked terminator is "0" + the field lines + the blank line.
    check(b"0\r\nx-checksum: abc123\r\nx-trailer-count: 2\r\n\r\n" in body,
          "h1 chunked terminator carries the trailer fields")


def h2_head_trailers() -> None:
    # A HEAD response has no body and no trailer section, but the response must
    # still end cleanly at its HEADERS frame (no DATA, no reset).
    try:
        status, headers, body, trailers = h2_get("/trailers?oneshot=1", method="HEAD")
        check(status == 200 and body == b"", f"h2 HEAD with trailers -> {status} {body!r}")
        check((headers or {}).get("content-length") == "12",
              f"h2 HEAD keeps its Content-Length -> {(headers or {}).get('content-length')!r}")
        check(trailers is None, "h2 HEAD carries no trailer section")
    except (TimeoutError, socket.timeout):
        fail("h2 HEAD /trailers?oneshot=1 stalled")
    except RuntimeError as exc:
        fail(f"h2 HEAD /trailers?oneshot=1: {exc}")


def h1_http10_trailers() -> None:
    # HTTP/1.0 has no chunked framing, so the body is close-delimited and the
    # fields are dropped. What must NOT happen is the chunk framing appearing in
    # the client's body (the one-shot path used to switch to it).
    def fetch(raw_request: bytes) -> bytes:
        with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
            sock.sendall(raw_request)
            raw = b""
            while True:
                chunk = sock.recv(65535)
                if not chunk:
                    break
                raw += chunk
        return raw

    try:
        raw = fetch(b"GET /trailers?oneshot=1 HTTP/1.0\r\nHost: x\r\n\r\n")
        head, _, body = raw.partition(b"\r\n\r\n")
        check(body == b"oneshot body", f"h1.0 one-shot body is not chunk-framed -> {body!r}")
        check(b"transfer-encoding" not in head.lower(), "h1.0 one-shot response announces no chunking")
        check(b"x-checksum" not in raw, "h1.0 one-shot drops the trailer fields")

        raw = fetch(b"GET /trailers HTTP/1.0\r\nHost: x\r\n\r\n")
        _, _, body = raw.partition(b"\r\n\r\n")
        check(body == b"first second", f"h1.0 streamed body is not chunk-framed -> {body!r}")
    except (TimeoutError, socket.timeout):
        fail("h1.0 /trailers stalled")


def h1_head_trailers() -> None:
    # A HEAD response carries the headers a GET would have - Content-Length
    # included - and nothing after the blank line.
    try:
        with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
            sock.sendall(b"HEAD /trailers?oneshot=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            raw = b""
            while True:
                chunk = sock.recv(65535)
                if not chunk:
                    break
                raw += chunk
    except (TimeoutError, socket.timeout):
        fail("h1 HEAD /trailers?oneshot=1 stalled")
        return

    head, _, body = raw.partition(b"\r\n\r\n")
    check(b"content-length: 12" in head.lower(), f"h1 HEAD keeps its Content-Length -> {head!r}")
    check(b"transfer-encoding" not in head.lower(), "h1 HEAD is not streamed")
    check(body == b"", f"h1 HEAD carries no body -> {body!r}")


# --- HTTP/3 (aioquic) --------------------------------------------------------
#
# The trailer section on HTTP/3 is a second HEADERS frame after the DATA, sent
# by nghttp3's `nghttp3_conn_submit_trailers`. aioquic surfaces it as a second
# HeadersReceived event on the stream — there is no separate trailers event in
# its API, which is exactly what the wire says.

try:
    import aioquic.h3.connection  # noqa: F401
    from aioquic.asyncio.client import connect as quic_connect
    from aioquic.asyncio.protocol import QuicConnectionProtocol
    from aioquic.h3.connection import H3_ALPN, H3Connection
    from aioquic.h3.events import DataReceived, HeadersReceived
    from aioquic.quic.configuration import QuicConfiguration

    _HAVE_AIOQUIC = True
except ImportError:  # pragma: no cover - aioquic is in requirements.txt
    _HAVE_AIOQUIC = False


if _HAVE_AIOQUIC:

    class _H3Client(QuicConnectionProtocol):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            self.http = H3Connection(self._quic)
            self.events = []
            self.stream_id = None
            self._done = asyncio.Event()

        def quic_event_received(self, event):
            for http_event in self.http.handle_event(event):
                self.events.append(http_event)
                if (
                    getattr(http_event, "stream_id", None) == self.stream_id
                    and getattr(http_event, "stream_ended", False)
                ):
                    self._done.set()

        def get(self, authority: str, path: str, method: str = "GET") -> int:
            self.stream_id = self._quic.get_next_available_stream_id()
            self.http.send_headers(
                self.stream_id,
                [
                    (b":method", method.encode()),
                    (b":scheme", b"https"),
                    (b":authority", authority.encode()),
                    (b":path", path.encode()),
                ],
                end_stream=True,
            )
            self.transmit()
            return self.stream_id

        async def wait_done(self) -> None:
            await self._done.wait()

    async def _h3_fetch(path: str, method: str = "GET"):
        cfg = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN)
        cfg.verify_mode = ssl.CERT_NONE  # the test listener's cert is self-signed
        async with quic_connect("127.0.0.1", H3_PORT, configuration=cfg, create_protocol=_H3Client) as client:
            sid = client.get(f"127.0.0.1:{H3_PORT}", path, method)
            await asyncio.wait_for(client.wait_done(), timeout=10)

            status = 0
            body = b""
            header_blocks = 0
            trailers = None
            for event in client.events:
                if getattr(event, "stream_id", None) != sid:
                    continue
                if isinstance(event, HeadersReceived):
                    header_blocks += 1
                    if header_blocks == 1:
                        for name, value in event.headers:
                            if name == b":status":
                                status = int(value)
                    else:
                        trailers = {(k.decode()): v.decode() for k, v in event.headers}
                elif isinstance(event, DataReceived):
                    body += event.data
            return status, body, trailers

    async def _h3_checks() -> None:
        try:
            status, body, trailers = await _h3_fetch("/trailers")
            check(status == 200 and body == b"first second",
                  f"h3 /trailers streams the body -> {status} {body!r}")
            check(trailers is not None, "h3 /trailers delivers a trailer section")
            check((trailers or {}).get("x-checksum") == "abc123",
                  f"h3 trailer x-checksum -> {(trailers or {}).get('x-checksum')!r}")
            check((trailers or {}).get("x-trailer-count") == "2",
                  f"h3 trailer x-trailer-count -> {(trailers or {}).get('x-trailer-count')!r}")
        except (TimeoutError, asyncio.TimeoutError):
            fail("h3 /trailers stalled (no trailer section arrived)")
        except Exception as exc:  # noqa: BLE001 - the check is the assertion
            fail(f"h3 /trailers: {exc!r}")

        try:
            status, body, trailers = await _h3_fetch("/trailers?oneshot=1")
            check(status == 200 and body == b"oneshot body",
                  f"h3 one-shot with trailers -> {status} {body!r}")
            check((trailers or {}).get("x-checksum") == "abc123",
                  f"h3 one-shot trailer x-checksum -> {(trailers or {}).get('x-checksum')!r}")
        except (TimeoutError, asyncio.TimeoutError):
            fail("h3 /trailers?oneshot=1 stalled")
        except Exception as exc:  # noqa: BLE001
            fail(f"h3 /trailers?oneshot=1: {exc!r}")

        try:
            status, body, trailers = await _h3_fetch("/trailers?oneshot=1", method="HEAD")
            check(status == 200 and body == b"", f"h3 HEAD with trailers -> {status} {body!r}")
            check(trailers is None, "h3 HEAD carries no trailer section")
        except (TimeoutError, asyncio.TimeoutError):
            fail("h3 HEAD /trailers?oneshot=1 stalled")
        except Exception as exc:  # noqa: BLE001
            fail(f"h3 HEAD /trailers?oneshot=1: {exc!r}")


def h3_trailers() -> None:
    if not _HAVE_AIOQUIC:
        print("SKIP  h3 (aioquic not installed)")
        return
    asyncio.run(_h3_checks())


def run() -> None:
    print("\n== response trailers (hyper-h2 + raw HTTP/1.1) ==")
    h2_trailers()
    h2_trailers_oneshot()
    h2_head_trailers()
    h1_trailers()
    h1_http10_trailers()
    h1_head_trailers()

    print("\n== response trailers over HTTP/3 (aioquic) ==")
    h3_trailers()
