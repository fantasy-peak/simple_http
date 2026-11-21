#!/usr/bin/env python3
"""Raw HTTP/3 field-validity differential.

Go's net/http/3 client cannot put malformed pseudo-headers or field names on
the wire, so this probe uses aioquic, whose QPACK encoder sends the bytes
verbatim. It drives the same cases against the C++ (:7823) and Go (:7833)
reference servers and reports the terminal behaviour:

  response          -- the server produced a response HEADERS block
  stream_reset      -- the server reset the request stream
  connection_closed -- the server closed the QUIC connection
  timeout           -- neither happened within the budget

A difference is not automatically a bug: nghttp3 (C++) and quic-go (Go) make
different choices for malformed headers, and the known_diffs registry records
each divergence with its RFC rationale.
"""
import argparse
import asyncio
import json
import ssl
import sys

try:
    from aioquic.asyncio import connect
    from aioquic.asyncio.protocol import QuicConnectionProtocol
    from aioquic.h3.connection import H3_ALPN, H3Connection
    from aioquic.h3.events import DataReceived, HeadersReceived
    from aioquic.quic.configuration import QuicConfiguration
    from aioquic.quic.events import ConnectionTerminated, StreamReset
except ImportError:  # pragma: no cover - reported by run.py as a skip
    print("SKIP  server-h3raw: aioquic is not installed", file=sys.stderr)
    sys.exit(3)


class Client(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.http = H3Connection(self._quic)
        self.response = False
        self.reset = False
        self.closed = False

    def send(self, headers):
        stream_id = self._quic.get_next_available_stream_id()
        self.http.send_headers(stream_id, headers, end_stream=True)
        self.transmit()

    def quic_event_received(self, event):
        if isinstance(event, StreamReset):
            self.reset = True
        elif isinstance(event, ConnectionTerminated):
            self.closed = True
        for ev in self.http.handle_event(event):
            if isinstance(ev, (HeadersReceived, DataReceived)):
                self.response = True


async def probe(host, port, headers, timeout=2.0):
    configuration = QuicConfiguration(
        is_client=True, alpn_protocols=H3_ALPN, verify_mode=ssl.CERT_NONE, server_name="localhost"
    )
    try:
        async with connect(host, port, configuration=configuration, create_protocol=Client,
                           wait_connected=True) as client:
            client.send(headers)
            deadline = asyncio.get_running_loop().time() + timeout
            while asyncio.get_running_loop().time() < deadline:
                await asyncio.sleep(0.05)
                if client.response or client.reset or client.closed:
                    # Give a late reset/close a moment to arrive after an error
                    # response; the first signal alone is not the whole story.
                    await asyncio.sleep(0.1)
                    break
            if client.response:
                return "response"
            if client.reset:
                return "stream_reset"
            if client.closed:
                return "connection_closed"
            return "timeout"
    except Exception:  # noqa: BLE001 - a refused/broken connection is an outcome
        return "connection_closed"


def request(method=b"GET", path=b"/echo", authority=b"127.0.0.1", extra=()):
    return [(b":method", method), (b":scheme", b"https"), (b":path", path),
            (b":authority", authority), *extra]


CASES = {
    "h3raw.valid": request(),
    "h3raw.method_not_token": request(method=b"GE T"),
    "h3raw.method_empty": request(method=b""),
    "h3raw.path_space": request(path=b"/a b"),
    "h3raw.path_backslash": request(path=b"/a\\b"),
    "h3raw.authority_space": request(authority=b"exa mple.com"),
    "h3raw.header_uppercase": request(extra=[(b"X-Foo", b"v")]),
    "h3raw.header_separator": request(extra=[(b"x(y", b"v")]),
    "h3raw.value_leading_space": request(extra=[(b"x-foo", b" v")]),
    "h3raw.options_asterisk": request(method=b"OPTIONS", path=b"*"),
}


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp", default="127.0.0.1:7823")
    parser.add_argument("--go", default="127.0.0.1:7833")
    args = parser.parse_args()
    cpp_host, cpp_port = args.cpp.rsplit(":", 1)
    go_host, go_port = args.go.rsplit(":", 1)

    for case_id, headers in CASES.items():
        cpp = await probe(cpp_host, int(cpp_port), headers)
        go = await probe(go_host, int(go_port), headers)
        print(json.dumps({"case": case_id, "cpp": cpp, "go": go}), flush=True)


if __name__ == "__main__":
    asyncio.run(main())
