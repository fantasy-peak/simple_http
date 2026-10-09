#!/usr/bin/env python3
"""Serves a response head + half a body, then stalls the remaining half for
`pause` seconds before sending it — to verify the client's per-read body-idle
deadline is honoured by read_all() (test/read_all_timeout_check.cpp).

A body-idle budget means "no body bytes within the budget is an error". read()
(enforced via the Stream deadline wrapper) must fail at ~pause if the budget is
shorter; read_all() must fail just the same — pre-fix it ignored the budget and
sat out the whole pause.

usage: slow_body_server.py <port> <body_bytes> <pause_seconds>
"""

import socket
import sys
import time


def main():
    port = int(sys.argv[1])
    nbytes = int(sys.argv[2])
    pause = float(sys.argv[3])
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    print(f"listening on 127.0.0.1:{port} (body {nbytes} B, pause {pause}s)", flush=True)
    while True:
        sock, _ = srv.accept()
        try:
            buf = b""
            while b"\r\n\r\n" not in buf:
                data = sock.recv(4096)
                if not data:
                    break
                buf += data
            if b"\r\n\r\n" not in buf:
                sock.close()
                continue
            half = nbytes // 2
            resp = (
                "HTTP/1.1 200 OK\r\n"
                f"Content-Length: {nbytes}\r\n"
                "Content-Type: application/octet-stream\r\n"
                "\r\n"
            ).encode("latin1")
            sock.sendall(resp + b"a" * half)
            time.sleep(pause)
            sock.sendall(b"b" * (nbytes - half))
        except (ConnectionResetError, BrokenPipeError, OSError):
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass


if __name__ == "__main__":
    main()