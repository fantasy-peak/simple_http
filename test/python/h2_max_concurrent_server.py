#!/usr/bin/env python3
"""Control / limit-announcing HTTP/2 server for the client-side
SETTINGS_MAX_CONCURRENT_STREAMS check (test/h2_limit_check.cpp).

Two modes:
  --limit N   announce SETTINGS_MAX_CONCURRENT_STREAMS=N and serve 200 normally
              for every request whose stream is under the limit
  --limit 0   announce SETTINGS_MAX_CONCURRENT_STREAMS=0 (= "no new streams
              may be opened", RFC 9113 §6.5.2) and reset any arriving stream
              with REFUSED_STREAM

The point of the harness is the *server side log*: with a conforming client,
`--limit 0` must never receive a HEADERS (RequestReceived) at all — the client
refuses locally before anything reaches the wire. With a non-conforming one it
gets one RequestReceived per attempt and an RST back.

Usage:
  test/python/.venv/bin/python test/python/h2_max_concurrent_server.py --port 7813 --limit 0
"""

import argparse
import socket
import sys

from h2.config import H2Configuration
from h2.connection import H2Connection
from h2.events import (
    ConnectionTerminated,
    DataReceived,
    RequestReceived,
    StreamEnded,
)
from h2.exceptions import ProtocolError
from h2.settings import SettingCodes
from h2.errors import ErrorCodes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--limit", type=int, required=True)
    ap.add_argument("--die-on-request", action="store_true",
                    help="kill the socket on the first RequestReceived instead of replying — "
                         "exercises the client's connection-drop-while-in-read_head() path")
    args = ap.parse_args()

    config = H2Configuration(client_side=False, header_encoding="utf-8")
    conn = H2Connection(config=config)
    conn.update_settings({SettingCodes.MAX_CONCURRENT_STREAMS: args.limit})

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(4)
    print(f"listening on 127.0.0.1:{args.port} limit={args.limit}", flush=True)

    conns = 0
    while True:
        sock, _ = srv.accept()
        conns += 1
        try:
            # hyper-h2's FrameBuffer strips the 24-byte client preface itself
            # (server-side), so everything on the wire goes to receive_data();
            # the preface matching the first bytes is validated internally.
            #
            # Careful: `initiate_connection()` queues a SETTINGS frame built
            # from hyper-h2's *default* local settings (MAX_CONCURRENT_STREAMS
            # 100), which would override the explicit update_settings() frame
            # below (a later SETTINGS wins per-setting). So the default frame is
            # discarded and only the authoritative one is sent.
            conn.initiate_connection()
            conn.data_to_send()  # drop the default SETTINGS frame
            conn.update_settings({SettingCodes.MAX_CONCURRENT_STREAMS: args.limit})
            print(f"[conn {conns}] server up (max_concurrent={args.limit})", flush=True)

            requests_seen = 0
            got_preface = False
            while True:
                data = sock.recv(65536)
                if not data:
                    sock.close()
                    break
                try:
                    events = conn.receive_data(data)
                except ProtocolError as exc:
                    print(f"[conn {conns}] ProtocolError: {exc}", flush=True)
                    sock.close()
                    break
                # drain the outgoing buffer exactly once per turn — data_to_send()
                # is destructive; calling it twice would lose frames.
                out = conn.data_to_send()
                if out and not got_preface:
                    got_preface = True
                    print(f"[conn {conns}] client preface ok; server SETTINGS on the wire", flush=True)
                if out:
                    sock.sendall(out)

                for event in events:
                    if isinstance(event, RequestReceived):
                        requests_seen += 1
                        sid = event.stream_id
                        print(f"[conn {conns}] RequestReceived stream_id={sid} "
                              f"(#requests-seen={requests_seen})", flush=True)
                        if args.die_on_request:
                            print(f"[conn {conns}] killing socket mid-exchange", flush=True)
                            sock.close()
                            break
                        if args.limit == 0:
                            conn.reset_stream(sid, error_code=ErrorCodes.REFUSED_STREAM)
                        else:
                            conn.send_headers(
                                sid,
                                [(":status", "200"), ("content-type", "text/plain")],
                                end_stream=False,
                            )
                            conn.send_data(sid, b"ok", end_stream=True)
                        out = conn.data_to_send()
                        if out:
                            sock.sendall(out)
                    elif isinstance(event, DataReceived):
                        print(f"[conn {conns}] DataReceived stream_id={event.stream_id}", flush=True)
                    elif isinstance(event, StreamEnded):
                        print(f"[conn {conns}] StreamEnded stream_id={event.stream_id}", flush=True)
                    elif isinstance(event, ConnectionTerminated):
                        print(f"[conn {conns}] ConnectionTerminated", flush=True)
        except (EOFError, ConnectionResetError):
            sock.close()


if __name__ == "__main__":
    main()