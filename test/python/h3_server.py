#!/usr/bin/env python3
"""An independent HTTP/3 server for cross-validating simple_http's client.

Nothing here shares code with the library: aioquic implements QUIC, TLS and
HTTP/3 on its own, so a disagreement between the two means one of them misreads
the spec.

Usage (from the repository root):
    test/python/.venv/bin/python test/python/h3_server.py 27922 /path/to/ca.pem

It prints ``ready`` once the UDP socket is bound, then serves until killed.

Routes:
    GET  /hello      -> 200 "hello h3"
    GET  /big?n=N    -> 200, N bytes of 'x' (default 200000)
    *    /echo       -> 200, the request body echoed; X-Method header
    POST /expect     -> a 100-continue interim, then echoes the body
    GET  /trailers   -> 200 "trailer body" followed by an X-Checksum trailer
    GET  /stall      -> never answers (response-head deadline)
    GET  /stall-body -> a head, then silence forever (body-idle deadline)
    HEAD anywhere    -> the headers a GET would return, no body
    anything else    -> 404 "not found"
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from aioquic.asyncio import serve
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ProtocolNegotiated

REPO = Path(__file__).resolve().parents[2]
CERT = REPO / "test/tls_certificates/server_cert.pem"
KEY = REPO / "test/tls_certificates/server_key.pem"


class H3ServerProtocol(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._http: H3Connection | None = None
        self._requests: dict[int, dict] = {}

    # --- QUIC events -----------------------------------------------------

    def quic_event_received(self, event) -> None:
        if isinstance(event, ProtocolNegotiated):
            self._http = H3Connection(self._quic)
        if self._http is not None:
            for http_event in self._http.handle_event(event):
                self.http_event_received(http_event)

    # --- HTTP/3 events ---------------------------------------------------

    def http_event_received(self, event) -> None:
        if isinstance(event, HeadersReceived):
            headers = {k.decode("latin1").lower(): v.decode("latin1") for k, v in event.headers}
            self._requests[event.stream_id] = {
                "method": headers.get(":method", "GET"),
                "path": headers.get(":path", "/"),
                "headers": headers,
                "body": bytearray(),
            }
            # /expect advertises 100-continue before the body arrives, so the
            # client's interim-head path has something to parse.
            path = headers.get(":path")
            if path == "/expect" and not event.stream_ended:
                assert self._http is not None
                self._http.send_headers(event.stream_id, [(b":status", b"100")], end_stream=False)
                self.transmit()
                return
            # Deadline tests: /stall never answers at all (head timeout);
            # /stall-body sends a head and then goes silent (body-idle timeout).
            if path == "/stall":
                return
            if path == "/stall-body":
                assert self._http is not None
                self._http.send_headers(
                    event.stream_id,
                    [(b":status", b"200"), (b"content-type", b"text/plain"), (b"content-length", b"100")],
                    end_stream=False,
                )
                self.transmit()
                return
            if event.stream_ended:
                self._respond(event.stream_id)
        elif isinstance(event, DataReceived):
            state = self._requests.get(event.stream_id)
            if state is None:
                return
            state["body"].extend(event.data)
            if event.stream_ended:
                self._respond(event.stream_id)

    # --- responses -------------------------------------------------------

    def _respond(self, stream_id: int) -> None:
        state = self._requests.pop(stream_id, None)
        if state is None or self._http is None:
            return

        method: str = state["method"]
        url = urlsplit(state["path"])
        path = url.path
        body = bytes(state["body"])

        # aioquic treats a second HEADERS section as a trailer section, so after
        # the interim 100 the final response is a headers-only message carrying
        # what the server saw (aioquic will not let us send DATA after it).
        if path == "/expect":
            headers = [
                (b":status", b"200"),
                (b"content-type", b"application/octet-stream"),
                (b"x-method", method.encode()),
                (b"x-received", str(len(body)).encode()),
                (b"content-length", b"0"),
            ]
            self._http.send_headers(stream_id, headers, end_stream=True)
            self.transmit()
            return

        status = 200
        content_type = b"text/plain"
        extra: list[tuple[bytes, bytes]] = []
        trailers: list[tuple[bytes, bytes]] | None = None

        if path == "/hello":
            payload = b"hello h3"
        elif path == "/big":
            n = int(parse_qs(url.query).get("n", ["200000"])[0])
            payload = b"x" * n
            content_type = b"application/octet-stream"
        elif path == "/echo":
            payload = body
            content_type = b"application/octet-stream"
            extra.append((b"x-method", method.encode()))
        elif path == "/trailers":
            payload = b"trailer body"
            trailers = [(b"x-checksum", b"abc123")]
        else:
            status = 404
            payload = b"not found"

        headers: list[tuple[bytes, bytes]] = [
            (b":status", str(status).encode()),
            (b"content-type", content_type),
            (b"x-server", b"aioquic-h3"),
            (b"content-length", str(len(payload)).encode()),
        ]
        headers.extend(extra)

        # HEAD: the headers a GET would produce, with no body and no trailers.
        if method == "HEAD":
            self._http.send_headers(stream_id, headers, end_stream=True)
        elif trailers is not None:
            self._http.send_headers(stream_id, headers, end_stream=False)
            self._http.send_data(stream_id, payload, end_stream=False)
            self._http.send_headers(stream_id, trailers, end_stream=True)
        else:
            self._http.send_headers(stream_id, headers, end_stream=False)
            self._http.send_data(stream_id, payload, end_stream=True)
        self.transmit()


async def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 27922

    configuration = QuicConfiguration(is_client=False, alpn_protocols=H3_ALPN)
    configuration.load_cert_chain(str(CERT), str(KEY))

    await serve("127.0.0.1", port, configuration=configuration, create_protocol=H3ServerProtocol)
    print("ready", flush=True)
    await asyncio.Event().wait()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        sys.exit(0)
