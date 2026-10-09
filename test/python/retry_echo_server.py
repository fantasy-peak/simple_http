#!/usr/bin/env python3
"""Serves a big fixed-length body and counts requests, to verify the client's
default retry condition (test/retry_check.cpp): a client-side decision error —
e.g. the caller's body cap, body_too_large — must NOT be retried just because
the method is idempotent (GET). With the bug, a GET capped under the body size
is re-sent max_retries+1 times, dragging the whole body each time.

usage: retry_echo_server.py <port> <body_bytes>
"""

import socket
import sys

COUNT = [0]


def main():
    port = int(sys.argv[1])
    nbytes = int(sys.argv[2])
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    print(f"listening on 127.0.0.1:{port} (body {nbytes} B)", flush=True)
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
            COUNT[0] += 1
            print(f"REQUEST #{COUNT[0]}", flush=True)
            head_end = buf.index(b"\r\n\r\n") + 4
            resp = (
                "HTTP/1.1 200 OK\r\n"
                f"Content-Length: {nbytes}\r\n"
                "Content-Type: application/octet-stream\r\n"
                f"X-Request-Count: {COUNT[0]}\r\n"
                "\r\n"
            ).encode("latin1")
            sock.sendall(resp + b"z" * nbytes)
        except (ConnectionResetError, OSError):
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass


if __name__ == "__main__":
    main()