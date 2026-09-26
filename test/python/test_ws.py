"""WebSocket behaviour, driven by the `websockets` library.

The library implements RFC 6455 independently, including the parts simple_http's
own client also implements (masking, fragmentation, control frames). Where they
disagree, one of them is wrong — which is the point of driving the server with
something other than itself.

The route under test is test/server.cpp's /chat, which does two things:
  * echoes every message back with an "echo: " prefix, preserving text/binary;
  * pushes "server-push #N" once a second from a concurrent writer.
The second is why every wait below goes through _recv_echo: a bare recv() races
the push timer, and the failure would look like a wrong echo.
"""

from __future__ import annotations

import asyncio
import time

import websockets

from harness import PLAIN_PORT, check, fail

URL = f"ws://127.0.0.1:{PLAIN_PORT}/chat"
PUSH_PREFIX = "server-push #"


async def _recv_echo(ws, timeout: float = 10.0):
    """The next echo, skipping the server's periodic pushes."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("no echo arrived")
        msg = await asyncio.wait_for(ws.recv(), timeout=remaining)
        if isinstance(msg, str) and msg.startswith(PUSH_PREFIX):
            continue
        return msg


async def _run() -> None:
    # ping_interval=None: the library's own keepalive would interleave Pings with
    # the traffic under test and make a failure ambiguous. Ping/pong is exercised
    # explicitly below instead.
    async with websockets.connect(URL, ping_interval=None, open_timeout=10) as ws:
        await ws.send("hello")
        echoed = await _recv_echo(ws)
        check(echoed == "echo: hello", f"a text message echoes -> {echoed!r}")

        # Binary stays binary: a server that lost the opcode would answer with
        # text and the library would hand back a str.
        await ws.send(b"\x00\x01\x02\xff")
        echoed = await _recv_echo(ws)
        check(echoed == b"echo: \x00\x01\x02\xff",
              f"a binary message echoes as bytes -> {type(echoed).__name__} {echoed!r}")

        # An empty message is legal and must not be mistaken for a close.
        await ws.send("")
        echoed = await _recv_echo(ws)
        check(echoed == "echo: ", f"an empty message echoes -> {echoed!r}")

        # Large enough that the client fragments it (the library splits at 64 KiB
        # by default); the server has to reassemble before echoing.
        big = "x" * 200_000
        await ws.send(big)
        echoed = await _recv_echo(ws, timeout=20)
        check(echoed == "echo: " + big, f"a 200000-byte message survives fragmentation -> {len(echoed)} bytes")

        # Ping must be answered by Pong with the same payload (RFC 6455 §5.5.3).
        pong = await asyncio.wait_for(ws.ping(b"probe"), timeout=5)
        await asyncio.wait_for(pong, timeout=5)
        check(True, "a Ping is answered with a matching Pong")

        # Two messages in flight before either is read: the server reads and
        # echoes in order, so the replies must come back in the same order.
        await ws.send("first")
        await ws.send("second")
        one = await _recv_echo(ws)
        two = await _recv_echo(ws)
        check(one == "echo: first" and two == "echo: second",
              f"messages are echoed in order -> {one!r}, {two!r}")

        # The concurrent writer's pushes have to arrive on the same connection
        # while the read loop is running — that interleaving is the whole reason
        # the server runs a separate writer coroutine.
        pushed = None
        deadline = time.monotonic() + 4.0
        while pushed is None and time.monotonic() < deadline:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=deadline - time.monotonic())
            except (TimeoutError, asyncio.TimeoutError):
                break
            if isinstance(msg, str) and msg.startswith(PUSH_PREFIX):
                pushed = msg
        check(pushed is not None, f"the server pushes while the read loop runs -> {pushed!r}")


async def _closes_cleanly() -> None:
    # A graceful Close handshake: `async with` sends Close and waits for the
    # server's reply. A server that dropped the socket instead would make the
    # library raise rather than return.
    try:
        async with websockets.connect(URL, ping_interval=None, open_timeout=10) as ws:
            await ws.send("bye")
            await _recv_echo(ws)
        check(True, "the close handshake completes")
    except Exception as exc:  # noqa: BLE001 — reporting whatever the library raised
        fail(f"the close handshake raised {exc!r}")


def run() -> None:
    print("\n== WebSocket (websockets) ==")
    try:
        asyncio.run(_run())
        asyncio.run(_closes_cleanly())
    except Exception as exc:  # noqa: BLE001
        fail(f"the WebSocket suite raised {exc!r}")
