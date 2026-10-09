#!/usr/bin/env python3
"""Holds an HTTP/1.1 request open without answering, to verify the client's
h1 half-duplex guard (test/h1_duplex_check.cpp).

Reads the request head and a bit of the body, then deliberately NEVER responds:
in a correct client, reading the response before the request body has ended
(finish()) is an HTTP/1.1 violation — the response cannot start until the
request is complete — so a conforming client must fail fast with a
half-duplex error instead of hanging for its TTFB timeout.

usage: h1_stall_server.py <port>
"""

import socket
import sys


def main():
    port = int(sys.argv[1])
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    print(f"listening on 127.0.0.1:{port}", flush=True)
    while True:
        sock, _ = srv.accept()
        # Read whatever arrives, then stall forever: no response, no close.
        # The client should give up on its own — a conforming one immediately
        # (half-duplex guard), a pre-fix one after its TTFB deadline.
        try:
            while True:
                data = sock.recv(65536)
                if not data:
                    raise EOFError
        except (EOFError, ConnectionResetError, OSError):
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass


if __name__ == "__main__":
    main()