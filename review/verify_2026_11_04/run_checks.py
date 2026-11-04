#!/usr/bin/env python3
"""Verification programs for REVIEW-2026-11-04.

Runs a C++ fixture server (and C++ client driver) built from the working tree
against raw HTTP/1.1, WebSocket and HTTP/2 peers written here, plus h11/hpack
as reference parsers. Every check prints PASS/FAIL for "the library behaves
correctly"; run it on the unmodified tree first (bug checks FAIL), then after
the fixes (all PASS).

    python3 -I run_checks.py --bin-dir bin/before --client bin/before/client
"""

import argparse
import base64
import hashlib
import hmac
import json
import os
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from dataclasses import dataclass, field

import hpack
import hyperframe.frame

TIMEOUT = 4.0


# ---------------------------------------------------------------- utilities

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def recv_all(sock, timeout=TIMEOUT):
    sock.settimeout(timeout)
    out = b""
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        out += chunk
    return out


def parse_http(data):
    """Returns (status, headers:dict[str,list[str]], body:bytes)."""
    split = data.find(b"\r\n\r\n")
    if split < 0:
        return 0, {}, data
    head = data[:split].decode("latin-1")
    rest = data[split + 4:]
    lines = head.split("\r\n")
    status_line = lines[0]
    try:
        status = int(status_line.split(" ")[1])
    except Exception:
        status = 0
    headers = {}
    for line in lines[1:]:
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        headers.setdefault(k.strip().lower(), []).append(v.strip())
    if "transfer-encoding" in headers and any("chunked" in v.lower() for v in headers["transfer-encoding"]):
        body = b""
        p = 0
        while True:
            nl = rest.find(b"\r\n", p)
            if nl < 0:
                break
            size = int(rest[p:nl].split(b";")[0], 16)
            p = nl + 2
            if size == 0:
                break
            body += rest[p:p + size]
            p += size + 2
        rest = body
    else:
        cls = headers.get("content-length")
        if cls:
            try:
                rest = rest[:int(cls[0])]
            except ValueError:
                pass
    return status, headers, rest


def header_values(headers, name):
    return ",".join(headers.get(name.lower(), []))


def raw_http(port, request, timeout=TIMEOUT):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(request)
    data = recv_all(s, timeout)
    s.close()
    return parse_http(data)


# ---------------------------------------------------------------- fixture server

class Fixture:
    def __init__(self, binary, env=None):
        self.binary = binary
        self.env = dict(os.environ)
        if env:
            self.env.update(env)
        self.h1 = free_port()
        self.h2 = free_port()
        self.proc = subprocess.Popen(
            [binary, str(self.h1), str(self.h2)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=self.env,
        )
        line = self.proc.stdout.readline()
        if "verify_server ready" not in line:
            err = self.proc.stderr.read()
            raise RuntimeError(f"fixture failed to start: {line!r} {err!r}")

    def stop(self):
        try:
            self.proc.stdin.write("stop\n")
            self.proc.stdin.flush()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# ---------------------------------------------------------------- HTTP tests

def check_abs_authority(port):
    status, headers, body = raw_http(
        port,
        b"GET http://authority.example/echo-host HTTP/1.1\r\n"
        b"Host: host.example\r\nConnection: close\r\n\r\n",
    )
    return status == 200 and b"host=authority.example" in body, f"status={status} body={body[:120]!r}"


def check_http10_te(port):
    status, headers, body = raw_http(
        port,
        b"POST /echo-body HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"
        b"5\r\nhello\r\n0\r\n\r\n",
    )
    return status >= 400 and b"hello" not in body, f"status={status} body={body[:120]!r}"


def check_204_cl(port):
    status, headers, body = raw_http(
        port, b"GET /204-cl HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
    )
    return status == 204 and "content-length" not in headers, f"status={status} headers={headers}"


def check_trailer_filter(port):
    status, headers, body = raw_http(
        port,
        b"POST /trailers HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
        b"Connection: close\r\n\r\n"
        b"0\r\nContent-Length: 100\r\nHost: evil\r\nTE: trailers\r\nX-Ok: 1\r\n\r\n",
    )
    ok = b"x-ok=1" in body and b"content-length=" not in body and b"host=" not in body and b"te=" not in body
    return ok, f"status={status} body={body[:160]!r}"


def check_trailer_bound(port):
    lines = b"".join(b"a: b\r\n" for _ in range(20000))  # ~120 KiB, over max_header_bytes
    try:
        status, headers, body = raw_http(
            port,
            b"POST /trailers HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
            b"Connection: close\r\n\r\n0\r\n" + lines + b"\r\n",
            timeout=6.0,
        )
    except ConnectionResetError:
        # Refusing mid-body and closing with RST is the intended bounded
        # behavior: the connection is not at a request boundary any more.
        return True, "connection reset after the oversized trailer section"
    return status != 200, f"status={status} body_len={len(body)}"


def check_vary_merge(port):
    status, headers, body = raw_http(
        port,
        b"GET /vary HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n",
    )
    vary = header_values(headers, "vary").lower()
    ok = status == 200 and "origin" in vary and "cookie" in vary and "accept-encoding" in vary
    return ok, f"status={status} vary={vary!r}"


def check_cors_fn(port):
    status, headers, body = raw_http(
        port,
        b"GET /cors HTTP/1.1\r\nHost: x\r\nOrigin: https://evil.example\r\nConnection: close\r\n\r\n",
    )
    acao = header_values(headers, "access-control-allow-origin")
    return status == 200 and acao == "", f"status={status} ACAO={acao!r}"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def make_hs256(payload: dict, secret=b"verify-secret", header=None) -> str:
    header = header or {"alg": "HS256", "typ": "JWT"}
    h = b64url(json.dumps(header, separators=(",", ":")).encode())
    p = b64url(json.dumps(payload, separators=(",", ":")).encode())
    sig = hmac.new(secret, f"{h}.{p}".encode(), hashlib.sha256).digest()
    return f"{h}.{p}.{b64url(sig)}"


def check_jwt_unknown_claims(port):
    token = make_hs256({"sub": "alice", "exp": int(time.time()) + 3600, "jti": "abc", "azp": "x"})
    status, headers, body = raw_http(
        port,
        f"GET /jwt HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {token}\r\nConnection: close\r\n\r\n".encode(),
    )
    return status == 200 and body == b"alice", f"status={status} body={body[:120]!r}"


def check_jwks_unknown_fields(port):
    token = make_hs256({"sub": "bob", "exp": int(time.time()) + 3600})
    status, headers, body = raw_http(
        port,
        f"GET /jwks HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {token}\r\nConnection: close\r\n\r\n".encode(),
    )
    return status == 200 and body == b"bob", f"status={status} body={body[:120]!r}"


def check_if_range_weak(port):
    status, headers, body = raw_http(
        port, b"GET /big.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
    )
    if status != 200:
        return False, f"initial GET status={status}"
    etag = header_values(headers, "etag")
    if not etag:
        return False, "no ETag on /big.txt"
    status2, headers2, body2 = raw_http(
        port,
        (
            "GET /big.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=0-3\r\n"
            f"If-Range: W/{etag}\r\nConnection: close\r\n\r\n"
        ).encode(),
    )
    # A weak validator must be ignored: the full representation comes back.
    return status2 == 200 and body2 == b"0123456789", f"status={status2} body={body2[:40]!r} etag={etag!r}"


# ---------------------------------------------------------------- WebSocket

def ws_handshake(port, extensions):
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Extensions: {extensions}\r\n\r\n"
    ).encode()
    s = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
    s.sendall(req)
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    status, headers, _ = parse_http(head + b"\r\n\r\n")
    return s, status, headers, rest


def ws_send(sock, opcode, payload, rsv1=False, rsv2=False, fin=True):
    b0 = (0x80 if fin else 0) | (0x40 if rsv1 else 0) | (0x20 if rsv2 else 0) | opcode
    mask = os.urandom(4)
    n = len(payload)
    if n < 126:
        hdr = bytes([b0, 0x80 | n])
    elif n < 65536:
        hdr = bytes([b0, 0x80 | 126]) + struct.pack("!H", n)
    else:
        hdr = bytes([b0, 0x80 | 127]) + struct.pack("!Q", n)
    masked = bytes(payload[i] ^ mask[i % 4] for i in range(n))
    sock.sendall(hdr + mask + masked)


def ws_recv(sock):
    sock.settimeout(3.0)
    hdr = sock.recv(2)
    if len(hdr) < 2:
        return None
    b0, b1 = hdr[0], hdr[1]
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack("!H", sock.recv(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", sock.recv(8))[0]
    payload = b""
    while len(payload) < n:
        chunk = sock.recv(n - len(payload))
        if not chunk:
            break
        payload += chunk
    return (b0 & 0x0F, payload)


def deflate_message(text: bytes) -> bytes:
    c = zlib.compressobj(6, zlib.DEFLATED, -15)
    out = c.compress(text) + c.flush(zlib.Z_SYNC_FLUSH)
    if out.endswith(b"\x00\x00\xff\xff"):
        out = out[:-4]
    return out


def check_ws_rsv(port):
    sock, status, headers, rest = ws_handshake(
        port, "permessage-deflate; client_max_window_bits"
    )
    ext = header_values(headers, "sec-websocket-extensions").lower()
    if status != 101 or "permessage-deflate" not in ext:
        sock.close()
        return False, f"handshake status={status} ext={ext!r} (deflate not negotiated)"
    ws_send(sock, 0x1, deflate_message(b"hello"), rsv1=True, rsv2=True)
    frame = ws_recv(sock)
    sock.close()
    if frame is None:
        return False, "no frame"
    opcode, payload = frame
    if opcode == 0x1:
        return False, f"accepted a frame with RSV2 set (echo {payload!r})"
    if opcode == 0x8 and len(payload) >= 2:
        code = struct.unpack("!H", payload[:2])[0]
        return code == 1002, f"close code={code}"
    return False, f"opcode={opcode} payload={payload[:20]!r}"


def check_ws_wbits8(port):
    sock, status, headers, rest = ws_handshake(
        port, "permessage-deflate; server_max_window_bits=8"
    )
    sock.close()
    ext = header_values(headers, "sec-websocket-extensions").lower()
    ok = status == 101 and "permessage-deflate" not in ext
    return ok, f"status={status} ext={ext!r}"


# ---------------------------------------------------------------- HTTP/2

def h2_connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
    s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
    settings = hyperframe.frame.SettingsFrame(stream_id=0)
    s.sendall(settings.serialize())
    # Read until the server's SETTINGS, ACK it.
    buf = b""
    while True:
        while len(buf) < 9:
            buf += s.recv(65536)
        length = int.from_bytes(buf[0:3], "big")
        while len(buf) < 9 + length:
            buf += s.recv(65536)
        f, consumed = hyperframe.frame.Frame.parse_frame_header(buf[:9])
        f.parse_body(memoryview(buf[9:9 + length]))
        buf = buf[9 + length:]
        if isinstance(f, hyperframe.frame.SettingsFrame) and "ACK" not in f.flags:
            ack = hyperframe.frame.SettingsFrame(stream_id=0)
            ack.flags.add("ACK")
            s.sendall(ack.serialize())
            break
    return s, buf


def h2_read_frame(s, buf):
    while len(buf) < 9:
        chunk = s.recv(65536)
        if not chunk:
            return None, buf
        buf += chunk
    length = int.from_bytes(buf[0:3], "big")
    while len(buf) < 9 + length:
        chunk = s.recv(65536)
        if not chunk:
            return None, buf
        buf += chunk
    f, _ = hyperframe.frame.Frame.parse_frame_header(buf[:9])
    f.parse_body(memoryview(buf[9:9 + length]))
    return f, buf[9 + length:]


def h2_headers_frame(sid, block, end_stream=False):
    f = hyperframe.frame.HeadersFrame(stream_id=sid)
    f.data = block
    f.flags.add("END_HEADERS")
    if end_stream:
        f.flags.add("END_STREAM")
    return f.serialize()


def check_h2_hpack_skip(port):
    s, buf = h2_connect(port)
    enc = hpack.Encoder()
    dec = hpack.Decoder()
    try:
        base = [(b":method", b"GET"), (b":scheme", b"http"), (b":authority", b"127.0.0.1")]
        block1 = enc.encode(base + [(b":path", b"/slow")])
        s.sendall(h2_headers_frame(1, block1))
        # Stream 3 exceeds the fixture's single-stream limit: the server refuses
        # it. Its block inserts x-poison into the encoder's dynamic table.
        block3 = enc.encode(base + [(b":path", b"/echo-header"), (b"x-poison", b"v")])
        s.sendall(h2_headers_frame(3, block3))
        # Close stream 1's remote half after the handler's response.
        e = hyperframe.frame.DataFrame(stream_id=1)
        e.data = b""
        e.flags.add("END_STREAM")
        s.sendall(e.serialize())

        # Wait for stream 1 to finish and for the refused stream's RST.
        deadline = time.time() + 3
        while time.time() < deadline:
            f, buf = h2_read_frame(s, buf)
            if f is None:
                return False, "connection closed before stream 5"
            if isinstance(f, hyperframe.frame.GoAwayFrame):
                return False, f"premature GOAWAY {f.error_code}"
            if isinstance(f, hyperframe.frame.HeadersFrame) and f.stream_id == 1:
                # Drain its DATA until END_STREAM.
                while True:
                    g, buf = h2_read_frame(s, buf)
                    if g is None:
                        return False, "closed while draining stream 1"
                    if isinstance(g, hyperframe.frame.DataFrame) and g.stream_id == 1 and "END_STREAM" in g.flags:
                        break
                break

        # Stream 5 references the dynamic-table entry inserted by stream 3.
        block5 = enc.encode(base + [(b":path", b"/echo-header"), (b"x-poison", b"v")])
        s.sendall(h2_headers_frame(5, block5, end_stream=True))

        deadline = time.time() + 4
        while time.time() < deadline:
            f, buf = h2_read_frame(s, buf)
            if f is None:
                return False, "connection closed while waiting for stream 5"
            if isinstance(f, hyperframe.frame.GoAwayFrame):
                return False, f"GOAWAY(error_code={f.error_code}) after referencing skipped block"
            if isinstance(f, hyperframe.frame.RstStreamFrame) and f.stream_id == 5:
                return False, f"RST_STREAM({f.error_code}) on stream 5"
            if isinstance(f, hyperframe.frame.HeadersFrame) and f.stream_id == 5:
                headers = dict(dec.decode(f.data))
                status = headers.get(":status", headers.get(b":status"))
                body = b""
                while True:
                    g, buf = h2_read_frame(s, buf)
                    if g is None:
                        break
                    if isinstance(g, hyperframe.frame.DataFrame) and g.stream_id == 5:
                        body += g.data
                        if "END_STREAM" in g.flags:
                            break
                return status in (b"200", "200") and b"value=v" in body, f"status={status} body={body[:80]!r}"
        return False, "timed out waiting for stream 5"
    finally:
        s.close()


# ---------------------------------------------------------------- edge server modes

def check_clean_path_double_decode(binary, root):
    fx = Fixture(binary, {"VERIFY_STATIC_ROOT": root, "VERIFY_CLEAN_PATH": "1"})
    try:
        status, headers, body = raw_http(
            fx.h1, b"GET /%2561dmin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
        )
        return not (status == 200 and b"admin-secret" in body), f"status={status} body={body[:80]!r}"
    finally:
        fx.stop()


def check_group_static(binary, root):
    fx = Fixture(binary, {"VERIFY_STATIC_ROOT": root, "VERIFY_GROUP_STATIC": "1"})
    try:
        status, headers, body = raw_http(
            fx.h1, b"GET /secret.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
        )
        return not (status == 200 and b"top-secret" in body), f"status={status} body={body[:80]!r}"
    finally:
        fx.stop()


# ---------------------------------------------------------------- malicious servers

class OneShotServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, payload):
        self.payload = payload
        super().__init__(("127.0.0.1", 0), OneShotHandler)
        self.port = self.server_address[1]


class OneShotHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        _ = self.request.recv(65536)
        self.request.sendall(self.server.payload)


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            head += chunk
        headers, _, rest = head.partition(b"\r\n\r\n")
        lines = headers.decode("latin-1").split("\r\n")
        body = rest
        cl = 0
        chunked = False
        for line in lines[1:]:
            k, _, v = line.partition(":")
            if k.lower() == "content-length":
                cl = int(v.strip())
            if k.lower() == "transfer-encoding" and "chunked" in v.lower():
                chunked = True
        if chunked:
            payload = b""
            while True:
                while b"\r\n" not in body:
                    body += self.request.recv(65536)
                nl = body.find(b"\r\n")
                size = int(body[:nl].split(b";")[0], 16)
                body = body[nl + 2:]
                if size == 0:
                    break
                while len(body) < size + 2:
                    body += self.request.recv(65536)
                payload += body[:size]
                body = body[size + 2:]
            body = payload
        else:
            while len(body) < cl:
                chunk = self.request.recv(65536)
                if not chunk:
                    break
                body += chunk
            body = body[:cl]
        reply = b"got=" + body
        self.request.sendall(
            b"HTTP/1.1 200 OK\r\nContent-Length: " + str(len(reply)).encode() + b"\r\nConnection: close\r\n\r\n" + reply
        )


class EchoServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), EchoHandler)
        self.port = self.server_address[1]


def gzip_bomb_payload():
    comp = zlib.compressobj(6, zlib.DEFLATED, 16 + 15)
    chunk = b"\x00" * (1 << 20)
    pieces = []
    for _ in range(256):  # 256 MiB of zeros, ~0.25 MiB compressed
        pieces.append(comp.compress(chunk))
    pieces.append(comp.flush())
    gz = b"".join(pieces)
    head = b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
    return head + format(len(gz), "x").encode() + b"\r\n" + gz + b"\r\n0\r\n\r\n"


# ---------------------------------------------------------------- client checks

def run_client(client, *args, limit_virtual_kb=None, timeout=60):
    cmd = [client, *args]
    if limit_virtual_kb:
        cmd = ["bash", "-c", f"ulimit -v {limit_virtual_kb}; exec " + " ".join(subprocess.list2cmdline([a]) for a in cmd)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def check_response_parser(client):
    payload = b"HTTP/1.1 200 OK\n\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"
    srv = OneShotServer(payload)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        code, out, err = run_client(client, "get", f"http://127.0.0.1:{srv.port}/")
    finally:
        srv.shutdown()
        srv.server_close()
    # Correct: the bare-LF blank line ends the head; the rest is body and no
    # Transfer-Encoding is adopted. Inspect the headers line only — the body
    # legitimately contains the literal text "Transfer-Encoding: chunked".
    headers_line = next((line for line in out.splitlines() if line.startswith("headers=")), "")
    ok = code == 0 and "transfer-encoding" not in headers_line.lower() and "body=Transfer-Encoding: chunked" in out
    return ok, f"rc={code} out={out.strip()[:200]!r} err={err.strip()[:120]!r}"


def check_decompress_bomb(client):
    payload = gzip_bomb_payload()
    srv = OneShotServer(payload)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        code, out, err = run_client(
            client, "decompress", f"http://127.0.0.1:{srv.port}/", "1048576"
        )
    finally:
        srv.shutdown()
        srv.server_close()
    rss = 0
    for line in out.splitlines():
        if line.startswith("maxrss_kb="):
            rss = int(line.split("=")[1])
    # Correct: a clean error, not a crash/exception, and no 512 MiB allocation.
    ok = code in (0, 2) and "error=" in out and rss < 200000 and "exception=" not in out
    return ok, f"rc={code} rss_kb={rss} out={out.strip()[:160]!r} err={err.strip()[:120]!r}"


def check_send_stream(client):
    srv = EchoServer()
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        code, out, err = run_client(client, "send_stream", f"http://127.0.0.1:{srv.port}/echo", '{"a":1}')
    finally:
        srv.shutdown()
        srv.server_close()
    echoed = out.split("body=got=", 1)[1].strip() if "body=got=" in out else ""
    ok = code == 0 and bool(echoed) and "a" in echoed
    return ok, f"rc={code} out={out.strip()[:160]!r} err={err.strip()[:120]!r}"


# ---------------------------------------------------------------- main

CHECKS = [
    ("H1 absolute-form authority overrides Host", lambda ctx: check_abs_authority(ctx.h1)),
    ("HTTP/1.0 + Transfer-Encoding is refused", lambda ctx: check_http10_te(ctx.h1)),
    ("204 carries no Content-Length", lambda ctx: check_204_cl(ctx.h1)),
    ("request trailers drop framing/hop fields", lambda ctx: check_trailer_filter(ctx.h1)),
    ("chunked trailer section is bounded", lambda ctx: check_trailer_bound(ctx.h1)),
    ("Vary merge keeps every field", lambda ctx: check_vary_merge(ctx.h1)),
    ("CORS allow_origin_fn-only denies", lambda ctx: check_cors_fn(ctx.h1)),
    ("JWT tolerates unknown claims", lambda ctx: check_jwt_unknown_claims(ctx.h1)),
    ("JWKS tolerates unknown members", lambda ctx: check_jwks_unknown_fields(ctx.h1)),
    ("If-Range ignores a weak validator", lambda ctx: check_if_range_weak(ctx.h1)),
    ("WS rejects RSV2/RSV3", lambda ctx: check_ws_rsv(ctx.h1)),
    ("deflate declines server_max_window_bits=8", lambda ctx: check_ws_wbits8(ctx.h1)),
    ("H2 decodes refused-stream HPACK blocks", lambda ctx: check_h2_hpack_skip(ctx.h2)),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--client", required=True)
    args = ap.parse_args()

    root = tempfile.mkdtemp(prefix="shverify-")
    with open(os.path.join(root, "big.txt"), "w") as f:
        f.write("0123456789")
    with open(os.path.join(root, "admin"), "w") as f:
        f.write("admin-secret")
    with open(os.path.join(root, "secret.txt"), "w") as f:
        f.write("top-secret")

    results = []
    fx = Fixture(args.server, {"VERIFY_STATIC_ROOT": root, "VERIFY_WS_DEFLATE": "1"})
    try:
        for name, fn in CHECKS:
            try:
                ok, detail = fn(fx)
            except Exception as e:  # noqa: BLE001 - report, do not abort the suite
                ok, detail = False, f"{type(e).__name__}: {e}"
            results.append((name, ok, detail))
    finally:
        fx.stop()

    extra = [
        ("clean_path does not double-decode", lambda: check_clean_path_double_decode(args.server, root)),
        ("static_files inside group keeps scope", lambda: check_group_static(args.server, root)),
        ("client parser ends head at the blank line", lambda: check_response_parser(args.client)),
        ("client decompression is bounded", lambda: check_decompress_bomb(args.client)),
        ("send_stream sends the builder body", lambda: check_send_stream(args.client)),
    ]
    for name, fn in extra:
        try:
            ok, detail = fn()
        except Exception as e:  # noqa: BLE001
            ok, detail = False, f"{type(e).__name__}: {e}"
        results.append((name, ok, detail))

    width = max(len(n) for n, _, _ in results)
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name:<{width}}  {detail}")
    failed = sum(1 for _, ok, _ in results if not ok)
    print(f"\n{len(results) - failed}/{len(results)} checks pass")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
