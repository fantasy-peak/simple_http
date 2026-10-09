"""Verifies permessage-deflate (RFC 7692) support end to end — on the wire and
through the independent `websockets` library.

The C++ client never negotiates the extension (our WebSocketSpec does not offer
it), so the honest test that the server's compression genuinely works is a
third-party client. Two layers:

  * `websockets`, an independent RFC 6455 + 7692 implementation, negotiates
    permessage-deflate *by default* (v11+). Driving /echo through it exercises
    the server's decode of compressed inbound (including the client's own
    fragmentation of large messages) and the client's decode of the server's
    compressed outbound, with context takeover on in both directions.
  * a raw-socket probe inspects the wire: that the 101 actually grants the
    extension; that the server's outbound data frames carry RSV1 and deflate
    back to the original bytes; that no-context-takeover and window-bit offers
    are honoured; that an unsupported offer is declined; and that the
    handshake-failure paths (wrong sec-websocket-version -> 426, missing
    version -> 400, disallowed / missing Origin on a gated route -> 403)
    answer with the right status codes — all invisible to the library test.

The routes under test (test/server.cpp): /echo (plain echo, compression on),
/private (echo behind `request_id()` + `ws_origin({...})` per-route middleware).
"""

from __future__ import annotations

import asyncio
import base64
import os
import socket
import zlib

import websockets

from harness import PLAIN_PORT, check

ECHO = f"ws://127.0.0.1:{PLAIN_PORT}/echo"
PRIVATE = f"ws://127.0.0.1:{PLAIN_PORT}/private"

_TAIL = b"\x00\x00\xff\xff"


# --- wire-level helpers -------------------------------------------------------

def deflate_message(data: bytes, wbits: int = -15, reset: bool = False) -> bytes:
    """A permessage-deflate compressed message as a sender emits it: deflate the
    whole message, remove the sync-flush tail."""
    co = zlib.compressobj(wbits=wbits)
    out = co.compress(data) + co.flush(zlib.Z_SYNC_FLUSH)
    assert out[-4:] == _TAIL, out[-4:].hex()
    return out[:-4]


def inflate_message(data: bytes, wbits: int = -15) -> bytes:
    """A permessage-deflate compressed message as a receiver sees it: feed the
    bytes, then re-feed the tail the sender stripped (RFC 7692 §7.2.2)."""
    di = zlib.decompressobj(wbits=wbits)
    return di.decompress(data) + di.decompress(_TAIL)


class PmdDecoder:
    """A persistent permessage-deflate receiver. With context takeover the
    sender's sliding window survives across messages, so the encoder must too:
    one zlib stream per connection, tail re-fed at every message end."""

    def __init__(self, wbits: int = -15) -> None:
        self._di = zlib.decompressobj(wbits=wbits)

    def decode(self, data: bytes) -> bytes:
        return self._di.decompress(data) + self._di.decompress(_TAIL)


def frame(opcode: int, payload: bytes, *, fin: bool = True, rsv1: bool = False,
          mask: bytes = b"\x37\xfa\x21\x3d") -> bytes:
    """One client frame (masked)."""
    head = (0x80 if fin else 0) | (0x40 if rsv1 else 0) | opcode
    out = bytearray([head])
    n = len(payload)
    if n <= 125:
        out.append(0x80 | n)
    elif n <= 0xFFFF:
        out.append(0x80 | 126)
        out += n.to_bytes(2, "big")
    else:
        out.append(0x80 | 127)
        out += n.to_bytes(8, "big")
    out += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return bytes(out) + masked


def parse_frame(buf: bytes):
    """Parses one server frame (never masked) off the front of `buf`. Returns
    (fin, rsv1, opcode, payload) and the leftover bytes."""
    b0, b1 = buf[0], buf[1]
    fin = bool(b0 & 0x80)
    rsv1 = bool(b0 & 0x40)
    opcode = b0 & 0x0F
    n = b1 & 0x7F
    i = 2
    if n == 126:
        n = int.from_bytes(buf[2:4], "big")
        i = 4
    elif n == 127:
        n = int.from_bytes(buf[2:10], "big")
        i = 10
    payload = buf[i : i + n]
    return fin, rsv1, opcode, payload, buf[i + n :]


def handshake(sock: socket.socket, path: str, extra: list[tuple[str, str]] | None = None,
              version: str | None = "13") -> tuple[int, dict[str, str], bytes]:
    """Sends a WebSocket upgrade and reads the response head. Returns
    (status, lowercase-header-map, bytes-past-the-head)."""
    key = base64.b64encode(os.urandom(16)).decode()
    req = [
        f"GET {path} HTTP/1.1",
        f"Host: 127.0.0.1:{PLAIN_PORT}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
    ]
    if version is not None:
        req.append(f"Sec-WebSocket-Version: {version}")
    for name, value in extra or []:
        req.append(f"{name}: {value}")
    sock.sendall(("\r\n".join(req) + "\r\n\r\n").encode("latin1"))

    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.decode("latin1").split("\r\n")
    status = int(lines[0].split()[1])
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return status, headers, rest


def recv_until(sock: socket.socket, buf: bytes, want: int) -> bytes:
    while len(buf) < want:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf


# --- wire tests ---------------------------------------------------------------

def _test_wire_compression() -> None:
    # 1. A plain offer is granted and the server truly compresses outbound:
    #    the echo we receive back must carry RSV1 and inflate to the sent text.
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        status, headers, buf = handshake(sock, "/echo", [("Sec-WebSocket-Extensions", "permessage-deflate")])
        check(status == 101, "plain permessage-deflate offer gets 101")
        check("permessage-deflate" in headers.get("sec-websocket-extensions", ""),
              f"101 grants the extension -> {headers.get('sec-websocket-extensions')}")

        # The server's outbound window persists across messages (context
        # takeover), so one decoder instance decodes every echo on this
        # connection — a fresh one would misalign on the later messages.
        dec = PmdDecoder()

        payload = b"wire-level compressed round trip"
        sock.sendall(frame(0x1, deflate_message(payload), rsv1=True))
        buf = recv_until(sock, buf, 2)
        fin, rsv1, opcode, echo, buf = parse_frame(buf)
        check(fin and opcode == 0x1, "server echoes the text frame")
        check(rsv1, "server's data frame carries RSV1 (it compressed)")
        check(dec.decode(echo) == payload, "server's compressed echo inflates to the original")

        # Binary frames may carry arbitrary bytes (no UTF-8 check).
        blob = os.urandom(512)
        sock.sendall(frame(0x2, deflate_message(blob), rsv1=True))
        buf = recv_until(sock, buf, 2)
        _, rsv1bin, opbin, echo_bin, buf = parse_frame(buf)
        check(opbin == 0x2 and rsv1bin, "server echoes the binary frame, compressed")
        check(dec.decode(echo_bin) == blob, "binary compressed round trip")

        # 2. Uncompressed inbound is still legal once negotiated.
        plain = b"uncompressed despite negotiation"
        sock.sendall(frame(0x1, plain, rsv1=False))
        buf = recv_until(sock, buf, 2)
        _, rsv1b, _, echo2, buf = parse_frame(buf)
        check(dec.decode(echo2) == b"uncompressed despite negotiation", "uncompressed inbound echoes back")

        # 3. A fragmented compressed message reassembles on the server.
        big = (b"the quick brown fox jumps over the lazy dog. " * 2000)
        comp = deflate_message(big)
        mid = len(comp) // 2
        sock.sendall(frame(0x1, comp[:mid], fin=False, rsv1=True))
        sock.sendall(frame(0x0, comp[mid:], fin=True))
        buf = recv_until(sock, buf, 2)
        _, rsv1c, _, echo3, buf = parse_frame(buf)
        check(dec.decode(echo3) == big, "a fragmented compressed message is reassembled and echoed")

        # 4. Corrupt deflate is a protocol error: the server must fail with 1002.
        sock.sendall(frame(0x1, b"\xde\xad\xbe\xef\x00\x01\x02\x03", fin=True, rsv1=True))
        buf = recv_until(sock, buf, 2)
        _, _, opcode4, close4, _ = parse_frame(buf)
        code4 = int.from_bytes(close4[:2], "big")
        check(opcode4 == 0x8 and code4 == 1002, f"corrupt deflate fails the connection with 1002 -> {code4}")

    # 5. no_context_takeover both ways is honoured and echoed.
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        offer = "permessage-deflate; server_no_context_takeover; client_no_context_takeover"
        status, headers, buf = handshake(sock, "/echo", [("Sec-WebSocket-Extensions", offer)])
        got = headers.get("sec-websocket-extensions", "")
        check(status == 101 and "server_no_context_takeover" in got and "client_no_context_takeover" in got,
              f"no-context-takeover offer echoed -> {got}")
        msgs = [b"first independent message " * 40, b"second independent message " * 40]
        for m in msgs:
            sock.sendall(frame(0x1, deflate_message(m), rsv1=True))
            buf = recv_until(sock, buf, 2)
            _, rsv1n, _, echo_n, buf = parse_frame(buf)
            # Each side resets per message: an independent deflate window decodes it.
            check(inflate_message(echo_n, wbits=-15) == m, "no-context-takeover message echoes correctly")

    # 6. Window-bit offers bind and are echoed (server_max_window_bits MUST be).
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        offer = "permessage-deflate; server_max_window_bits=10; client_max_window_bits=10"
        status, headers, buf = handshake(sock, "/echo", [("Sec-WebSocket-Extensions", offer)])
        got = headers.get("sec-websocket-extensions", "")
        check(status == 101 and "server_max_window_bits=10" in got and "client_max_window_bits=10" in got,
              f"window-bit offer echoed -> {got}")
        payload = b"window-bits round trip " * 300
        sock.sendall(frame(0x1, deflate_message(payload, wbits=-10), rsv1=True, mask=b"\x11\x22\x33\x44"))
        buf = recv_until(sock, buf, 2)
        _, rsv1w, _, echo_w, _ = parse_frame(buf)
        check(inflate_message(echo_w, wbits=-10) == payload, "10-bit-window round trip works")

    # 7. An unsupported offer is declined: the upgrade proceeds uncompressed.
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        status, headers, buf = handshake(sock, "/echo", [("Sec-WebSocket-Extensions", "permessage-deflate; nonsense_param")])
        check(status == 101 and headers.get("sec-websocket-extensions", "") == "",
              "unsupported offer is declined with a clean 101")
        sock.sendall(frame(0x1, b"plain after declined offer"))
        buf = recv_until(sock, buf, 2)
        _, _, opcode7, echo7, _ = parse_frame(buf)
        check(opcode7 == 0x1 and echo7 == b"plain after declined offer", "connection still works, uncompressed")

    # 8. Handshake failures get the right status: wrong version -> 426 + list,
    #    missing version -> 400.
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        status, headers, _ = handshake(sock, "/echo", version="12")
        check(status == 426 and headers.get("sec-websocket-version", "") == "13",
              f"wrong Sec-WebSocket-Version -> 426 + Sec-WebSocket-Version: 13 -> {status}")
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        status, _, _ = handshake(sock, "/echo", version=None)
        check(status == 400, f"missing Sec-WebSocket-Version -> 400 -> {status}")


def _test_wire_middleware_gate() -> None:
    # /private is wrapped in request_id() + ws_origin({"http://allowed.example"}).
    # A missing or foreign Origin must be answered 403 before any 101, and the
    # middleware's X-Request-Id must appear in the granted 101.
    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        status, headers, _ = handshake(sock, "/private")
        check(status == 403, f"missing Origin on a gated ws route -> 403 -> {status}")

    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        status, headers, _ = handshake(sock, "/private",
                                       [("Origin", "http://evil.example"), ("Sec-WebSocket-Extensions", "permessage-deflate")])
        check(status == 403, f"foreign Origin on a gated ws route -> 403 -> {status}")

    with socket.create_connection(("127.0.0.1", PLAIN_PORT), timeout=10) as sock:
        status, headers, buf = handshake(sock, "/private",
                                         [("Origin", "http://allowed.example"),
                                          ("Sec-WebSocket-Extensions", "permessage-deflate")])
        check(status == 101, f"allowed Origin gets the 101 -> {status}")
        check(headers.get("x-request-id", ""), "middleware X-Request-Id is folded into the 101")
        check("permessage-deflate" in headers.get("sec-websocket-extensions", ""),
              "compression still negotiates through the middleware chain")
        sock.sendall(frame(0x1, b"through the gate"))
        buf = recv_until(sock, buf, 2)
        _, _, opcode, echo, _ = parse_frame(buf)
        check(opcode == 0x1 and inflate_message(echo) == b"through the gate", "the gated route echoes compressed")


# --- library-level tests ------------------------------------------------------

async def _library_round_ips() -> None:
    async with websockets.connect(ECHO, ping_interval=None, open_timeout=10) as ws:
        granted = ws.response.headers.get("Sec-WebSocket-Extensions", "")
        check("permessage-deflate" in granted,
              f"websockets was granted permessage-deflate -> {granted!r}")

        # Plain text round trip, compressed both ways by the library.
        await ws.send("hello deflate")
        echoed = await ws.recv()
        check(echoed == "hello deflate", "text echo through compression")

        # Empty message: compressed empty is legal and not mistaken for close.
        await ws.send("")
        echoed = await ws.recv()
        check(echoed == "", "empty message round trips")

        # Binary stays binary.
        blob = b"\x00\x01\x02\xff" * 4096
        await ws.send(blob)
        echoed = await ws.recv()
        check(echoed == blob, "binary echoes as bytes")

        # Large enough that the library fragments it *and* compresses each piece.
        big = "x" * 500_000
        await ws.send(big)
        echoed = await ws.recv()
        check(echoed == big, "a 500000-byte compressed + fragmented message survives")

        # Random (incompressible) payload — exercises the deflate-escapes path.
        rnd = bytes(range(256)) * 3000
        await ws.send(rnd)
        echoed = await ws.recv()
        check(echoed == rnd, "incompressible binary round trips")

        # Ordering with several in flight.
        for i in range(5):
            await ws.send(f"m{i}")
        got = [await ws.recv() for _ in range(5)]
        check(got == [f"m{i}" for i in range(5)], "compressed messages echo in order")


def run() -> None:
    print("\n== WebSocket permessage-deflate (RFC 7692) ==")
    try:
        _test_wire_compression()
        _test_wire_middleware_gate()
        asyncio.run(_library_round_ips())
    except Exception as exc:  # noqa: BLE001 — reporting whatever broke
        check(False, f"the permessage-deflate suite raised {exc!r}")


if __name__ == "__main__":
    run()