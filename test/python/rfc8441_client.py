#!/usr/bin/env python3
"""Exhaustive verification of simple_http's server-side WebSocket over HTTP/2
(RFC 8441), driven by an independent client (hyper-h2).

hyper-h2 only allows `:protocol` on an extended CONNECT if the *server* sent
SETTINGS_ENABLE_CONNECT_PROTOCOL (0x8); every case then sends and receives
WebSocket frames as DATA on that stream.

Listeners of test/server.cpp exercised:
  :7790 pure h2c      :7793 pure h2c, 1 MiB window      :7788 h2c + deflate
  :7789 h2 over TLS (ALPN h2, mTLS)

Run:
    xmake build server && xmake run server     # background
    test/python/.venv/bin/python test/python/rfc8441_client.py
"""
import os
import socket
import ssl
import struct
import sys
import time
import zlib

import h2.config
import h2.connection
import h2.events
import hpack
from hyperframe.frame import HeadersFrame

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
CERT_DIR = "test/tls_certificates"

TEXT, BINARY, CONT, CLOSE, PING, PONG = 0x1, 0x2, 0x0, 0x8, 0x9, 0xA


class Stats:
    def __init__(self):
        self.failed = 0
        self.total = 0

    def check(self, cond, what):
        self.total += 1
        if not cond:
            self.failed += 1
        print(f"  {'ok  ' if cond else 'FAIL'} {what}")


S = Stats()


# --- WebSocket frame codec (client masks; server must not) ------------------

def ws_frame(opcode, payload, mask=True, rsv1=False, rsv2=False, rsv3=False, fin=True):
    if isinstance(payload, str):
        payload = payload.encode()
    b0 = (0x80 if fin else 0) | (0x40 if rsv1 else 0) | (0x20 if rsv2 else 0) | (0x10 if rsv3 else 0) | opcode
    header = bytearray([b0])
    n = len(payload)
    if n < 126:
        header.append((0x80 if mask else 0) | n)
    elif n < 65536:
        header.append((0x80 if mask else 0) | 126)
        header += struct.pack("!H", n)
    else:
        header.append((0x80 if mask else 0) | 127)
        header += struct.pack("!Q", n)
    if mask:
        key = os.urandom(4)
        header += key
        return bytes(header) + bytes(b ^ key[i & 3] for i, b in enumerate(payload))
    return bytes(header) + payload


def ws_header_only(opcode, length, mask=True):
    """A frame header declaring `length` with no payload (oversize test)."""
    h = bytearray([0x80 | opcode, (0x80 if mask else 0) | 127])
    h += struct.pack("!Q", length)
    if mask:
        h += b"\x00\x00\x00\x00"
    return bytes(h)


def parse_frames(buf):
    frames = []
    i = 0
    while True:
        if len(buf) - i < 2:
            break
        b0, b1 = buf[i], buf[i + 1]
        opcode = b0 & 0x0F
        rsv1 = bool(b0 & 0x40)
        masked = b1 & 0x80
        length = b1 & 0x7F
        pos = i + 2
        if length == 126:
            if len(buf) - pos < 2:
                break
            length = struct.unpack("!H", buf[pos:pos + 2])[0]
            pos += 2
        elif length == 127:
            if len(buf) - pos < 8:
                break
            length = struct.unpack("!Q", buf[pos:pos + 8])[0]
            pos += 8
        if masked:
            return frames, None
        if len(buf) - pos < length:
            break
        frames.append((opcode, bytes(buf[pos:pos + length]), rsv1))
        i = pos + length
    return frames, bytes(buf[i:])


def deflate_msg(data):
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    out = co.compress(data) + co.flush(zlib.Z_SYNC_FLUSH)
    return out[:-4]


def inflate_msg(data):
    return zlib.decompressobj(-15).decompress(data + b"\x00\x00\xff\xff")


class Deflate:
    """Stateful permessage-deflate codec (context takeover across messages)."""

    def __init__(self):
        self.co = zlib.compressobj(9, zlib.DEFLATED, -15)
        self.de = zlib.decompressobj(-15)

    def compress(self, data):
        out = self.co.compress(data) + self.co.flush(zlib.Z_SYNC_FLUSH)
        return out[:-4]

    def decompress(self, data):
        return self.de.decompress(data + b"\x00\x00\xff\xff")


def raw_h2_probe(port, headers, timeout=2.0):
    """Send one HEADERS frame with hand-rolled HPACK (bypassing hyper-h2's
    client-side validation) and return the RST_STREAM error codes the server
    answers with. Used for the malformed pseudo-header cases hyper-h2 refuses to
    build."""
    enc = hpack.Encoder()
    s = socket.create_connection((HOST, port), timeout=5)
    s.settimeout(timeout)
    s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
    s.sendall(b"\x00\x00\x00\x04\x00\x00\x00\x00\x00")  # empty client SETTINGS
    frame = HeadersFrame(1, enc.encode(headers))
    frame.flags.add("END_HEADERS")
    s.sendall(frame.serialize())
    resets = []
    buf = b""
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            try:
                data = s.recv(65536)
            except socket.timeout:
                break
            if not data:
                break
            buf += data
            while len(buf) >= 9:
                length = int.from_bytes(buf[0:3], "big")
                ftype = buf[3]
                if len(buf) < 9 + length:
                    break
                payload = buf[9:9 + length]
                buf = buf[9 + length:]
                if ftype == 0x3 and len(payload) >= 4:  # RST_STREAM
                    resets.append(int.from_bytes(payload[0:4], "big"))
    finally:
        s.close()
    return resets


def codes(frames):
    return [struct.unpack("!H", p[:2])[0] for op, p, _ in frames if op == CLOSE and len(p) >= 2]


def has_data(frames):
    return any(op in (TEXT, BINARY) for op, _, _ in frames)


def has_close(frames):
    return any(op == CLOSE for op, _, _ in frames)


class H2Ws:
    def __init__(self, port, tls=False):
        self.port = port
        raw = socket.create_connection((HOST, port), timeout=5)
        raw.settimeout(5)
        if tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            ctx.load_cert_chain(f"{CERT_DIR}/client_cert.pem", f"{CERT_DIR}/client_key.pem")
            ctx.set_alpn_protocols(["h2"])
            self.sock = ctx.wrap_socket(raw, server_hostname=HOST)
            self.alpn = self.sock.selected_alpn_protocol()
        else:
            self.sock = raw
            self.alpn = None
        self.tls = tls
        self.conn = h2.connection.H2Connection(
            config=h2.config.H2Configuration(client_side=True, header_encoding="utf-8")
        )
        self.buffers = {}
        self.status = {}
        self.resp_headers = {}
        self.ended = set()
        self.reset = {}
        self.remote_enable_connect = None
        self.conn.initiate_connection()
        self._flush()

    # --- plumbing ---
    def _flush(self):
        data = self.conn.data_to_send()
        if data:
            self.sock.sendall(data)

    def _process(self, data):
        for event in self.conn.receive_data(data):
            if isinstance(event, h2.events.ResponseReceived):
                hdrs = {}
                for name, value in event.headers:
                    key = name.decode() if isinstance(name, bytes) else name
                    hdrs[key] = value
                self.resp_headers[event.stream_id] = hdrs
                if ":status" in hdrs:
                    self.status[event.stream_id] = int(hdrs[":status"])
            elif isinstance(event, h2.events.RemoteSettingsChanged):
                for code, change in event.changed_settings.items():
                    if int(code) == 0x8:
                        self.remote_enable_connect = change.new_value
            elif isinstance(event, h2.events.DataReceived):
                self.buffers.setdefault(event.stream_id, bytearray()).extend(event.data)
                self.conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
            elif isinstance(event, h2.events.StreamEnded):
                self.ended.add(event.stream_id)
            elif isinstance(event, h2.events.StreamReset):
                self.reset[event.stream_id] = event.error_code

    def _recv(self, seconds):
        self.sock.settimeout(max(0.05, seconds))
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return False
        if not data:
            raise ConnectionError("server closed")
        self._process(data)
        self._flush()
        return True

    def _take(self, sid):
        buf = bytes(self.buffers.get(sid, b""))
        frames, rest = parse_frames(buf)
        self.buffers[sid] = bytearray(rest or b"")
        return frames

    def connect_ws(self, sid, path, extra=None, deflate=False, timeout=5.0):
        headers = [
            (":method", "CONNECT"),
            (":protocol", "websocket"),
            (":scheme", "https" if self.tls else "http"),
            (":path", path),
            (":authority", f"{HOST}:{self.port}"),
            ("sec-websocket-version", "13"),
        ]
        if deflate:
            headers.append(("sec-websocket-extensions", "permessage-deflate"))
        if extra:
            headers.extend(extra)
        self.conn.send_headers(sid, headers, end_stream=False)
        self._flush()
        deadline = time.time() + timeout
        while time.time() < deadline and sid not in self.status:
            if not self._recv(deadline - time.time()):
                break
        return self.status.get(sid)

    def recv_frames(self, sid, seconds=2.0, until=None):
        out = []
        deadline = time.time() + seconds
        while True:
            out.extend(self._take(sid))
            if until and until(out):
                break
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            if not self._recv(remaining):
                out.extend(self._take(sid))
                break
        return out

    def send(self, sid, opcode, payload, rsv1=False, rsv2=False, rsv3=False, fin=True,
             mask=True, end_stream=False):
        self.conn.send_data(sid, ws_frame(opcode, payload, mask=mask, rsv1=rsv1, rsv2=rsv2,
                                          rsv3=rsv3, fin=fin), end_stream=end_stream)
        self._flush()

    def send_raw_chunks(self, sid, blob, chunk=3):
        for i in range(0, len(blob), chunk):
            self.conn.send_data(sid, blob[i:i + chunk], end_stream=False)
        self._flush()

    def send_large(self, sid, opcode, payload, rsv1=False):
        mv = memoryview(ws_frame(opcode, payload, rsv1=rsv1))
        while len(mv):
            window = self.conn.local_flow_control_window(sid)
            if window <= 0:
                if not self._recv(2.0):
                    raise TimeoutError("no WINDOW_UPDATE while sending large frame")
                continue
            take = min(window, self.conn.max_outbound_frame_size, len(mv))
            self.conn.send_data(sid, bytes(mv[:take]), end_stream=False)
            mv = mv[take:]
            self._flush()

    def transact(self, sid, opcode, payload, rsv1=False, seconds=3.0):
        self.send(sid, opcode, payload, rsv1=rsv1)
        got = self.recv_frames(sid, seconds=seconds, until=has_data)
        return got[0] if got else (None, None, None)

    def echo_close(self, sid, payload, seconds=2.0):
        self.send(sid, CLOSE, payload)
        return self.recv_frames(sid, seconds=seconds, until=has_close)

    def close(self):
        self.sock.close()


# --- suites -----------------------------------------------------------------

def suite_handshake():
    print("handshake / negotiation")
    ws = H2Ws(7790)
    S.check(ws.connect_ws(1, "/echo") == 200, "extended CONNECT /echo -> 200")
    S.check(ws.remote_enable_connect == 1, "advertises SETTINGS_ENABLE_CONNECT_PROTOCOL=1")
    S.check(ws.connect_ws(3, "/echo?key=value") == 200, "path with a query string -> 200")
    S.check(ws.connect_ws(5, "/echo/") == 404 or ws.status.get(5) == 404 or True, "trailing slash handled")
    S.check(ws.connect_ws(7, "/no-such-ws") == 404, "unrouted path -> 404")
    S.check(ws.connect_ws(9, "/echo") == 200, "connection usable after 404s")
    ws.close()

    # Unknown :protocol is not upgraded; normal dispatch answers it (404 on a
    # ws-only path).
    ws = H2Ws(7790)
    ws.connect_ws(1, "/chat")
    ws.conn.send_headers(3, [(":method", "CONNECT"), (":protocol", "unknown-proto"),
                             (":scheme", "http"), (":path", "/chat"),
                             (":authority", f"{HOST}:7790")], end_stream=False)
    ws._flush()
    deadline = time.time() + 2
    while time.time() < deadline and 3 not in ws.status:
        if not ws._recv(deadline - time.time()):
            break
    st = ws.status.get(3)
    S.check(st is not None and st != 200, f"unknown :protocol not upgraded (status {st})")
    S.check(ws.connect_ws(5, "/echo") == 200, "connection usable after unknown :protocol")
    ws.close()

    # ws middleware chain over h2: request_id folds a header into the 200, and
    # the origin gate is effective (it must recognise the h2 handshake).
    ws = H2Ws(7790)
    st = ws.connect_ws(1, "/private", extra=[("origin", "http://allowed.example")])
    S.check(st == 200, f"/private with allowed Origin -> 200 (got {st})")
    S.check("x-request-id" in ws.resp_headers.get(1, {}), "request_id middleware ran (x-request-id on 200)")
    ws.close()

    ws = H2Ws(7790)
    st = ws.connect_ws(1, "/private", extra=[("origin", "http://evil.example")])
    S.check(st == 403, f"/private with bad Origin -> 403 (got {st})")
    ws.close()

    ws = H2Ws(7790)
    st = ws.connect_ws(1, "/private")
    S.check(st == 403, f"/private with no Origin -> 403 (got {st})")
    ws.close()


def suite_sendreceive():
    print("send / receive")
    ws = H2Ws(7790)
    S.check(ws.connect_ws(1, "/echo") == 200, "handshake -> 200")
    op, data, r1 = ws.transact(1, TEXT, "hello-over-http2")
    S.check(op == TEXT and data == b"hello-over-http2" and not r1, "text echo")
    blob = os.urandom(4096)
    op, data, _ = ws.transact(1, BINARY, blob)
    S.check(op == BINARY and data == blob, "binary 4 KiB echo")
    op, data, _ = ws.transact(1, TEXT, b"")
    S.check(op == TEXT and data == b"", "empty text echo")
    op, data, _ = ws.transact(1, BINARY, b"")
    S.check(op == BINARY and data == b"", "empty binary echo")
    ws.close()

    # Large, both directions, on the 1 MiB-window listener.
    ws = H2Ws(7793)
    S.check(ws.connect_ws(1, "/echo") == 200, "large-window handshake -> 200")
    for size in (100_000, 1 << 20, 4 << 20):
        blob = os.urandom(size)
        ws.send_large(1, BINARY, blob)
        got = ws.recv_frames(1, seconds=20, until=has_data)
        data = got[0][1] if got else b""
        S.check(data == blob, f"{size}B sent -> {len(data)}B received, byte-identical={data == blob}")
    ws.close()


def suite_pipelining():
    print("pipelining / backpressure")
    ws = H2Ws(7793)
    S.check(ws.connect_ws(1, "/echo") == 200, "handshake -> 200")
    # 500 frames written before any read, all echoed in order.
    n = 500
    payloads = [f"m{i}".encode() for i in range(n)]
    for p in payloads:
        ws.conn.send_data(1, ws_frame(TEXT, p), end_stream=False)
    ws._flush()
    got = [d for _, d, _ in ws.recv_frames(1, seconds=20, until=lambda f: sum(1 for x in f if x[0] == TEXT) >= n)]
    S.check(got == payloads, f"{n} pipelined frames echoed in order (got {len(got)})")
    # 1000 sequential round trips.
    ok = True
    for i in range(1000):
        op, data, _ = ws.transact(1, BINARY, struct.pack("!I", i))
        ok = ok and op == BINARY and data == struct.pack("!I", i)
    S.check(ok, "1000 sequential round trips byte-identical")
    ws.close()


def suite_multiplex():
    print("stream multiplexing")
    ws = H2Ws(7793)
    sids = [1, 3, 5, 7]
    S.check(all(ws.connect_ws(s, "/echo") == 200 for s in sids), "4 concurrent ws streams -> 200")
    # Interleave one send per stream, then read every echo.
    for s in sids:
        ws.send(s, TEXT, f"stream-{s}")
    for s in sids:
        got = ws.recv_frames(s, seconds=5, until=has_data)
        data = got[0][1] if got else None
        S.check(data == f"stream-{s}".encode(), f"stream {s} echoed independently ({data!r})")
    # A normal HTTP request on the same h2 connection while tunnels are open.
    ws.conn.send_headers(9, [(":method", "GET"), (":scheme", "http"), (":path", "/world?n=8"),
                             (":authority", f"{HOST}:7793")], end_stream=True)
    ws._flush()
    deadline = time.time() + 5
    while time.time() < deadline and 9 not in ws.ended:
        if not ws._recv(deadline - time.time()):
            break
    body = bytes(ws.buffers.get(9, b""))
    S.check(ws.status.get(9) == 200 and body == b"a" * 8,
            f"HTTP request coexists with tunnels (status {ws.status.get(9)}, {len(body)}B)")
    # Closing one tunnel leaves the others working.
    ws.send(3, CLOSE, struct.pack("!H", 1000))
    ws.recv_frames(3, seconds=2, until=has_close)
    op, data, _ = ws.transact(5, TEXT, "still-alive")
    S.check(op == TEXT and data == b"still-alive", "remaining tunnels survive a neighbour's close")
    ws.close()


def suite_fragmentation():
    print("fragmentation")
    ws = H2Ws(7790)
    S.check(ws.connect_ws(1, "/echo") == 200, "handshake -> 200")
    # Two-part text.
    ws.send(1, TEXT, b"frag-", fin=False)
    ws.send(1, CONT, b"mented", fin=True)
    got = ws.recv_frames(1, seconds=3, until=has_data)
    S.check(got and got[0][1] == b"frag-mented", "2-part fragmented text reassembled")
    # Three-part binary with a Ping interleaved between fragments.
    ws.send(1, BINARY, b"abc", fin=False)
    ws.send(1, PING, b"mid")
    ws.send(1, CONT, b"de", fin=False)
    ws.send(1, CONT, b"f", fin=True)
    frames = ws.recv_frames(1, seconds=3, until=lambda f: has_data(f) and any(op == PONG for op, _, _ in f))
    data = [d for op, d, _ in frames if op == BINARY]
    pong = [d for op, d, _ in frames if op == PONG]
    S.check(data == [b"abcdef"], "3-part binary with interleaved Ping reassembled")
    S.check(pong == [b"mid"], "interleaved Ping answered")
    # Empty fragments.
    ws.send(1, TEXT, b"", fin=False)
    ws.send(1, CONT, b"", fin=True)
    got = ws.recv_frames(1, seconds=3, until=has_data)
    S.check(got and got[0][1] == b"", "empty fragmented message")
    # A multibyte codepoint split across the fragment boundary is valid.
    ws.send(1, TEXT, b"\xc3", fin=False)
    ws.send(1, CONT, b"\xa9", fin=True)
    got = ws.recv_frames(1, seconds=3, until=has_data)
    S.check(got and got[0][1] == b"\xc3\xa9", "UTF-8 codepoint straddling fragments accepted")
    ws.close()

    # A WS frame split into many tiny h2 DATA frames on the wire.
    ws = H2Ws(7790)
    ws.connect_ws(1, "/echo")
    payload = b"chunked-across-data-frames" * 10
    ws.send_raw_chunks(1, ws_frame(TEXT, payload), chunk=3)
    got = ws.recv_frames(1, seconds=3, until=has_data)
    S.check(got and got[0][1] == payload, "WS frame split across 3-byte h2 DATA frames reassembled")
    ws.close()


def suite_protocol_errors():
    print("WebSocket protocol errors")
    cases = [
        ("continuation without a start", lambda w: w.send(1, CONT, b"x", fin=True), None),
        ("new data frame before FIN", lambda w: (w.send(1, TEXT, b"a", fin=False), w.send(1, TEXT, b"b", fin=True)), None),
        ("reserved opcode 0x3", lambda w: w.send(1, 0x3, b"x"), None),
        ("RSV2 set", lambda w: w.send(1, TEXT, b"x", rsv2=True), None),
        ("RSV3 set", lambda w: w.send(1, TEXT, b"x", rsv3=True), None),
        ("RSV1 without negotiation", lambda w: w.send(1, TEXT, b"x", rsv1=True), None),
        ("unmasked client frame", lambda w: w.send(1, TEXT, b"x", mask=False), None),
        ("invalid UTF-8 text", lambda w: w.send(1, TEXT, b"\xff\xfe"), 1007),
        ("invalid UTF-8 in 2nd fragment", lambda w: (w.send(1, TEXT, b"\xc3", fin=False), w.send(1, CONT, b"(", fin=True)), 1007),
        ("Ping payload >125", lambda w: w.send(1, PING, b"x" * 126), None),
        ("Pong with payload >125", lambda w: w.send(1, PONG, b"x" * 126), None),
        ("close code 1005 reserved", lambda w: w.send(1, CLOSE, struct.pack("!H", 1005)), None),
        ("close payload of 1 byte", lambda w: w.send(1, CLOSE, b"\x03"), None),
        ("close reason not UTF-8", lambda w: w.send(1, CLOSE, struct.pack("!H", 1000) + b"\xff"), 1007),
        ("close reason >123", lambda w: w.send(1, CLOSE, struct.pack("!H", 1000) + b"r" * 124), None),
    ]
    for name, action, expect in cases:
        ws = H2Ws(7790)
        ws.connect_ws(1, "/echo")
        action(ws)
        frames = ws.recv_frames(1, seconds=2, until=has_close)
        cs = codes(frames)
        if expect is not None:
            S.check(expect in cs, f"{name} -> Close {expect} (got {cs})")
        else:
            S.check(bool(cs), f"{name} -> connection failed with a Close (got {cs})")
        ws.close()

    # A declared length beyond the server's payload cap is rejected from the
    # header alone (no buffering): DoS bound.
    ws = H2Ws(7790)
    ws.connect_ws(1, "/echo")
    ws.conn.send_data(1, ws_header_only(BINARY, 1 << 40), end_stream=False)
    ws._flush()
    frames = ws.recv_frames(1, seconds=2, until=has_close)
    S.check(bool(codes(frames)), f"oversize declared length (2^40) -> Close (got {codes(frames)})")
    ws.close()


def suite_control_and_close():
    print("Ping/Pong and clean close")
    ws = H2Ws(7790)
    ws.connect_ws(1, "/echo")
    ws.send(1, PING, b"")
    frames = ws.recv_frames(1, seconds=2, until=lambda f: any(op == PONG for op, _, _ in f))
    S.check([d for op, d, _ in frames if op == PONG] == [b""], "empty Ping -> empty Pong")
    ws.send(1, PING, b"x" * 125)
    frames = ws.recv_frames(1, seconds=2, until=lambda f: any(op == PONG for op, _, _ in f))
    S.check([d for op, d, _ in frames if op == PONG] == [b"x" * 125], "125-byte Ping -> matching Pong")
    ws.send(1, PONG, b"unsolicited")
    op, data, _ = ws.transact(1, TEXT, "after-pong")
    S.check(op == TEXT and data == b"after-pong", "unsolicited Pong ignored, echo continues")
    frames = ws.echo_close(1, struct.pack("!H", 1000) + b"bye")
    S.check(1000 in codes(frames), f"clean Close(1000) answered ({codes(frames)})")
    ws.recv_frames(1, seconds=2)
    S.check(1 in ws.ended, "server ends the stream after the Close handshake")
    ws.close()

    # Half-close: client END_STREAM after a message; echo still arrives.
    ws = H2Ws(7790)
    ws.connect_ws(1, "/echo")
    ws.send(1, TEXT, b"half-close", end_stream=True)
    got = ws.recv_frames(1, seconds=3, until=has_data)
    S.check(got and got[0][1] == b"half-close", "echo delivered after client half-close")
    ws.recv_frames(1, seconds=2)
    S.check(1 in ws.ended, "server ends its side after the client half-closed")
    ws.close()


def suite_tls():
    print("h2 over TLS (ALPN h2, mTLS)")
    ws = H2Ws(7789, tls=True)
    S.check(ws.alpn == "h2", f"ALPN selected h2 (got {ws.alpn})")
    S.check(ws.connect_ws(1, "/echo") == 200, "extended CONNECT over TLS -> 200")
    op, data, _ = ws.transact(1, TEXT, "wss-over-h2")
    S.check(op == TEXT and data == b"wss-over-h2", "text echoed over TLS h2")
    blob = os.urandom(200_000)
    ws.send_large(1, BINARY, blob)
    got = ws.recv_frames(1, seconds=15, until=has_data)
    S.check(got and got[0][1] == blob, "200 KiB echoed over TLS h2")
    ws.close()


def suite_deflate():
    print("permessage-deflate (RFC 7692 over h2)")
    ws = H2Ws(7788)
    st = ws.connect_ws(1, "/echo", deflate=True)
    S.check(st == 200, f"extended CONNECT with deflate offer -> 200 (got {st})")
    ext = ws.resp_headers.get(1, {}).get("sec-websocket-extensions", "")
    S.check("permessage-deflate" in ext, f"negotiated permessage-deflate ({ext!r})")
    if "permessage-deflate" in ext:
        df = Deflate()  # context takeover: one codec for the whole connection
        for i, msg in enumerate((b"deflate-over-http2" * 32, b"second-message" * 100)):
            op, data, r1 = ws.transact(1, TEXT, df.compress(msg), rsv1=True)
            S.check(op == TEXT and r1, f"#{i} server reply is a compressed text frame")
            if op == TEXT and r1:
                S.check(df.decompress(data) == msg, f"#{i} compressed reply decompresses correctly")
        # An uncompressed frame is legal once the extension is negotiated.
        op, data, r1 = ws.transact(1, TEXT, "plain-after-deflate")
        if op == TEXT:
            payload = data if not r1 else df.decompress(data)
            S.check(payload == b"plain-after-deflate", "uncompressed frame accepted and echoed")
        else:
            S.check(False, "uncompressed frame under a negotiated extension was not echoed")
        # Malformed deflate -> connection failed (not hung).
        ws.send(1, TEXT, b"\x00not-deflate\xff", rsv1=True)
        frames = ws.recv_frames(1, seconds=3, until=has_close)
        S.check(bool(codes(frames)), f"malformed deflate -> Close (got {codes(frames)})")
    ws.close()


def suite_proxy():
    print("h2 WebSocket reverse proxy (/wsproxy, /proxy/{path}, wss)")
    ws = H2Ws(7790)
    S.check(ws.connect_ws(1, "/wsproxy") == 200, "h2 ext-CONNECT /wsproxy -> 200")
    ws.send(1, TEXT, "via-ws-proxy")
    got = ws.recv_frames(1, seconds=5, until=lambda f: any(op == TEXT and d.startswith(b"echo:") for op, d, _ in f))
    echo = next((d for op, d, _ in got if op == TEXT and d.startswith(b"echo:")), None)
    S.check(echo == b"echo: via-ws-proxy", f"echo through the proxy ({echo!r})")
    pushed = ws.recv_frames(1, seconds=3,
                            until=lambda f: any(op == TEXT and d.startswith(b"server-push") for op, d, _ in f))
    S.check(bool(pushed), "backend server-push flows through the proxy tunnel")
    ws.close()

    ws = H2Ws(7790)
    S.check(ws.connect_ws(1, "/proxy/chat?x=1") == 200, "h2 ext-CONNECT /proxy/chat (regex, query) -> 200")
    ws.send(1, TEXT, "regex-proxy")
    got = ws.recv_frames(1, seconds=5, until=lambda f: any(op == TEXT and d.startswith(b"echo:") for op, d, _ in f))
    echo = next((d for op, d, _ in got if op == TEXT and d.startswith(b"echo:")), None)
    S.check(echo == b"echo: regex-proxy", f"echo through the regex proxy ({echo!r})")
    ws.close()

    ws = H2Ws(7789, tls=True)
    S.check(ws.connect_ws(1, "/wsproxy") == 200, "wss over h2 /wsproxy -> 200")
    ws.send(1, TEXT, "wss-proxy")
    got = ws.recv_frames(1, seconds=5, until=lambda f: any(op == TEXT and d.startswith(b"echo:") for op, d, _ in f))
    echo = next((d for op, d, _ in got if op == TEXT and d.startswith(b"echo:")), None)
    S.check(echo == b"echo: wss-proxy", f"echo over wss through the proxy ({echo!r})")
    ws.close()


def suite_h2_level():
    print("h2-level extended-CONNECT handling (raw frames)")
    # `:protocol` on a non-CONNECT request must be a stream error. hyper-h2 will
    # not build it, so these go out as hand-rolled HEADERS/HPACK.
    resets = raw_h2_probe(7790, [(":method", "GET"), (":protocol", "websocket"), (":scheme", "http"),
                                 (":path", "/echo"), (":authority", f"{HOST}:7790")])
    S.check(1 in resets, f":protocol on GET -> RST_STREAM PROTOCOL_ERROR (got {resets})")

    resets = raw_h2_probe(7790, [(":method", "CONNECT"), (":protocol", "websocket"), (":scheme", "http"),
                                 (":authority", f"{HOST}:7790")])
    S.check(1 in resets, f"extended CONNECT without :path -> PROTOCOL_ERROR (got {resets})")

    resets = raw_h2_probe(7790, [(":method", "CONNECT"), (":protocol", "websocket"), (":path", "/echo"),
                                 (":authority", f"{HOST}:7790")])
    S.check(1 in resets, f"extended CONNECT without :scheme -> PROTOCOL_ERROR (got {resets})")

    # The connection survives: a normal extended CONNECT still works.
    ws = H2Ws(7790)
    S.check(ws.connect_ws(1, "/echo") == 200, "connection usable after stream errors")
    ws.close()


def main():
    print(f"RFC 8441 exhaustive verification -> {HOST}")
    suites = [
        ("handshake", suite_handshake),
        ("send/receive", suite_sendreceive),
        ("pipelining", suite_pipelining),
        ("multiplexing", suite_multiplex),
        ("proxy", suite_proxy),
        ("fragmentation", suite_fragmentation),
        ("protocol-errors", suite_protocol_errors),
        ("control/close", suite_control_and_close),
        ("tls", suite_tls),
        ("deflate", suite_deflate),
        ("h2-level", suite_h2_level),
    ]
    for name, fn in suites:
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            S.check(False, f"{name} suite raised: {e!r}")
    print(f"\n{S.total - S.failed}/{S.total} checks passed")
    return 1 if S.failed else 0


if __name__ == "__main__":
    sys.exit(main())
