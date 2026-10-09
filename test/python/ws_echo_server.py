#!/usr/bin/env python3
"""A standalone WebSocket echo server for cross-validating simple_http's
client-side WebSocket against an independent RFC 6455 implementation.

The C++ client (test/ws_client_cross.cpp) opens a connection to this and
round-trips text and binary messages. `websockets` is a third-party
implementation (like httpx/h2 in this suite, the point of which is that it does
not share simple_http's spec reading), so a disagreement is one of the two
being wrong.

Usage: test/python/.venv/bin/python test/python/ws_echo_server.py [port]
"""

from __future__ import annotations

import asyncio
import sys

import websockets


async def echo(ws):
    try:
        async for message in ws:
            # websockets delivers str for text frames, bytes for binary; echo
            # back with the same send() which preserves the opcode.
            await ws.send(message)
    except websockets.ConnectionClosed:
        pass


async def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 27920
    async with websockets.serve(echo, "127.0.0.1", port, ping_interval=None):
        print(f"ws echo server listening on 127.0.0.1:{port}", flush=True)
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    asyncio.run(main())