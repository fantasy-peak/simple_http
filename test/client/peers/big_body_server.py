#!/usr/bin/env python3
"""Serves a large fixed-length body, to verify the client-side Response body
cap is honored by the buffered readers (test/body_cap_check.cpp).

usage: big_body_server.py <port> <bytes>
"""

import socket
import sys


def main():
    port = int(sys.argv[1])
    nbytes = int(sys.argv[2])
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
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
            head_end = buf.index(b"\r\n\r\n") + 4
            leftover = buf[head_end:]
            head = buf[:head_end].decode("latin1")
            # Drain the request body if there is one (this server gets GETs).
            resp = (
                "HTTP/1.1 200 OK\r\n"
                f"Content-Length: {nbytes}\r\n"
                "Content-Type: application/octet-stream\r\n"
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