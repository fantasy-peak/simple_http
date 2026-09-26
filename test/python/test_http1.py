"""HTTP/1.1 behaviour, driven by httpx.

httpx is a separate implementation of the client half, which is the point: where
it and simple_http disagree about the framing rules, one of them is wrong.
"""

from __future__ import annotations

import httpx

from harness import BASE, check, fail


def run() -> None:
    print("\n== HTTP/1.1 (httpx) ==")
    # An explicit HTTP/1.1 client: httpx would otherwise be free to negotiate h2
    # where it can, and then this would silently be testing something else.
    with httpx.Client(http2=False, timeout=10.0) as client:
        _routes(client)
        _framing(client)
        _keep_alive(client)
        _errors(client)


def _routes(client: httpx.Client) -> None:
    r = client.get(f"{BASE}/world")
    check(r.status_code == 200 and r.text == "hello from HTTP/1.1",
          f"GET /world -> {r.status_code} {r.text!r}")

    r = client.get(f"{BASE}/headers")
    check(r.status_code == 200, "GET /headers -> 200")

    r = client.get(f"{BASE}/api/anything")
    check(r.status_code == 200, f"a regex route matches -> {r.status_code}")

    r = client.get(f"{BASE}/definitely-not-a-route")
    check(r.status_code == 404, f"an unknown path falls back -> {r.status_code}")


def _framing(client: httpx.Client) -> None:
    # A body bigger than one read: the response has to be framed, not just
    # delivered in whatever chunk the socket happened to produce.
    r = client.get(f"{BASE}/big", params={"n": 100_000})
    check(r.status_code == 200 and len(r.content) == 100_000,
          f"a 100000-byte body arrives whole -> {len(r.content)} bytes")

    # Request body with a known length, echoed back verbatim.
    payload = "x" * 5000
    r = client.post(f"{BASE}/echo", content=payload)
    check(r.status_code == 200 and r.text == payload,
          f"a 5000-byte request body is echoed whole -> {len(r.text)} bytes")

    # Chunked request: httpx streams the body without a Content-Length. The server
    # has to reassemble it, so a missing terminator shows up as a short echo.
    def gen():
        for _ in range(10):
            yield b"chunky"

    r = client.post(f"{BASE}/echo", content=gen())
    check(r.status_code == 200 and r.text == "chunky" * 10,
          f"a chunked request body is reassembled -> {len(r.text)} bytes")

    # A chunked *response* — /hello streams one frame per request-body frame, then
    # a terminator. httpx decodes the framing, so a wrong chunk size or a missing
    # final chunk surfaces as a decode error or a truncated body.
    r = client.post(f"{BASE}/hello", content="one two three".encode())
    check(r.status_code == 200 and "done," in r.text and "one two three" in r.text,
          f"a chunked response decodes -> {r.text!r}")

    # HEAD: headers only, no body — and the framing must not promise one.
    r = client.head(f"{BASE}/world")
    check(r.status_code == 200 and r.content == b"",
          f"HEAD returns no body -> {len(r.content)} bytes")

    # 204 carries neither a body nor a Content-Length.
    r = client.get(f"{BASE}/empty")
    check(r.status_code == 204 and r.content == b"",
          f"204 has no body -> {r.status_code}")

    # A response the handler sends without a known length, written in pieces.
    r = client.get(f"{BASE}/sync")
    check(r.status_code == 200, f"GET /sync -> {r.status_code}")


def _keep_alive(client: httpx.Client) -> None:
    # Twenty requests over one pooled connection: if the server closed after any
    # of them, httpx would transparently reconnect and this would still pass — so
    # what is being checked is that no response is lost, which is what a framing
    # desync on a reused connection looks like from here.
    ok = True
    for i in range(20):
        r = client.get(f"{BASE}/world")
        if r.status_code != 200 or r.text != "hello from HTTP/1.1":
            ok = False
            fail(f"keep-alive request {i} -> {r.status_code} {r.text!r}")
            break
    if ok:
        check(True, "20 requests on a pooled connection all answered correctly")

    # Connection: close must be honoured, and the next request must still work
    # (on a new connection, which httpx does silently).
    r = client.get(f"{BASE}/world", headers={"Connection": "close"})
    check(r.status_code == 200, "Connection: close is accepted")
    r = client.get(f"{BASE}/world")
    check(r.status_code == 200, "the client recovers after Connection: close")


def _errors(client: httpx.Client) -> None:
    r = client.get(f"{BASE}/delay", params={"ms": 10})
    check(r.status_code == 200, f"a delayed handler still answers -> {r.status_code}")

    # A handler that throws must become a 500 rather than dropping the connection:
    # this is the check that found the missing try/catch in invoke_handler, which
    # C++ clients did not surface (they report a bare disconnect, and the suite
    # never asked for a 500).
    r = client.get(f"{BASE}/throw")
    check(r.status_code == 500, f"a throwing handler becomes a 500 -> {r.status_code}")

    # Malformed request bytes deliberately do not appear here: httpx refuses to
    # send them (Illegal header value), so the server would never see them. The
    # regression suite drives those over a raw socket, which is the only way to
    # put them on the wire.
