#!/usr/bin/env python3
"""Differential test for the framework layer: simple_http vs Go net/http.

The same requests are sent to test/framework_server.cpp (simple_http) and
test/differential/go/framework_server.go (Go standard library), and observable results are
compared: status, deterministic bodies, auth headers, cookie behavior and
session/CSRF flows. Random token values are not compared across servers; each
side is checked against its own cookie.

Usage:
    test/differential/run.py framework                 # builds and starts both servers (with keys)
    python test/differential/framework_diff.py 7798 7799 [rsa_priv.pem] [ec_priv.pem]
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import sys
import time
from pathlib import Path

import httpx
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

CPP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7798
GO_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 7799
RSA_PRIV = sys.argv[3] if len(sys.argv) > 3 else ""
EC_PRIV = sys.argv[4] if len(sys.argv) > 4 else ""
HOST = "127.0.0.1"

checks = 0
failed = 0


def check(ok: bool, what: str) -> None:
    global checks, failed
    checks += 1
    if ok:
        print(f"PASS  {what}")
    else:
        failed += 1
        print(f"FAIL  {what}")


def base_url(port: int) -> str:
    return f"http://{HOST}:{port}"


def wait_ready(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            response = httpx.get(base_url(port) + "/state", timeout=0.5)
            if response.status_code == 200:
                return
        except Exception:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"server on port {port} did not become ready")


def compare(name: str, cpp: httpx.Response, go: httpx.Response, *, body: bool = True,
            contains: tuple[str, ...] = (), auth: bool = False) -> None:
    ok = cpp.status_code == go.status_code
    detail = f"status {cpp.status_code} vs {go.status_code}"
    if ok and contains:
        ok = all(fragment in cpp.text and fragment in go.text for fragment in contains)
        detail = f"body missing one of {contains}: {cpp.text!r} / {go.text!r}"
    elif ok and body:
        ok = cpp.text == go.text
        detail = f"body {cpp.text!r} vs {go.text!r}"
    if ok and auth:
        cpp_auth = cpp.headers.get("www-authenticate", "")
        go_auth = go.headers.get("www-authenticate", "")
        ok = "invalid_token" in cpp_auth and "invalid_token" in go_auth
        detail = f"www-authenticate {cpp_auth!r} vs {go_auth!r}"
    check(ok, name + ("" if ok else f" ({detail})"))


def client(port: int) -> httpx.Client:
    return httpx.Client(base_url=base_url(port), timeout=5.0, trust_env=False, follow_redirects=False)


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def jwt_token(payload: dict, alg: str, sign) -> str:
    header = b64url(json.dumps({"alg": alg, "typ": "JWT"}, separators=(",", ":")).encode())
    body = b64url(json.dumps(payload, separators=(",", ":")).encode())
    signature = sign(f"{header}.{body}".encode())
    return f"{header}.{body}.{b64url(signature)}"


def hs256_token(payload: dict, secret: str = "secret") -> str:
    return jwt_token(payload, "HS256", lambda data: hmac.new(secret.encode(), data, hashlib.sha256).digest())


def load_private_key(path: str):
    return serialization.load_pem_private_key(Path(path).read_bytes(), password=None)


def rs256_token(payload: dict, key) -> str:
    return jwt_token(payload, "RS256", lambda data: key.sign(data, padding.PKCS1v15(), hashes.SHA256()))


def es256_token(payload: dict, key) -> str:
    def sign(data: bytes) -> bytes:
        der = key.sign(data, ec.ECDSA(hashes.SHA256()))
        r, s = decode_dss_signature(der)
        return r.to_bytes(32, "big") + s.to_bytes(32, "big")

    return jwt_token(payload, "ES256", sign)


def corrupt_signature(token: str) -> str:
    return token[:-2] + ("AA" if not token.endswith("AA") else "BB")


def jwt_payload(scope: str, exp: int | None = None) -> dict:
    return {
        "sub": "alice",
        "scope": scope,
        "roles": ["admin"],
        "iss": "iss",
        "aud": "aud",
        "exp": int(time.time()) + 3600 if exp is None else exp,
        "nbf": 0,
    }


def cookie_names(response: httpx.Response) -> set[str]:
    return set(response.cookies.keys())


def cookie_has(response: httpx.Response, name: str, attribute: str) -> bool:
    prefix = name + "="
    return any(header.startswith(prefix) and attribute.lower() in header.lower()
               for header in response.headers.get_list("set-cookie"))


def main() -> int:
    wait_ready(CPP_PORT)
    wait_ready(GO_PORT)

    # --- state / extractors / validation ----------------------------------
    with client(CPP_PORT) as cpp, client(GO_PORT) as go:
        compare("state", cpp.get("/state"), go.get("/state"))
        compare("path extractor", cpp.get("/users/42"), go.get("/users/42"))
        compare("path extractor rejects a bad int", cpp.get("/users/nope"), go.get("/users/nope"),
                contains=("invalid_path",))
        compare("query extractor", cpp.get("/list?q=x&page=2"), go.get("/list?q=x&page=2"))
        compare("query extractor requires the declared fields", cpp.get("/list?q=x"), go.get("/list?q=x"),
                contains=("invalid_query",))

        compare("json extractor", cpp.post("/json", json={"name": "ada", "age": 36}),
                go.post("/json", json={"name": "ada", "age": 36}))
        compare("json extractor rejects malformed body", cpp.post("/json", content=b"{nope"),
                go.post("/json", content=b"{nope"), contains=("invalid_json",))

        compare("validation accepts a valid body", cpp.post("/valid", json={"name": "ada", "age": 36}),
                go.post("/valid", json={"name": "ada", "age": 36}))
        compare("validation rejects a bad age", cpp.post("/valid", json={"name": "ada", "age": 0}),
                go.post("/valid", json={"name": "ada", "age": 0}), contains=("validation_failed", "age"))
        compare("validation aggregates field errors",
                cpp.post("/valid", json={"name": "", "age": 0}),
                go.post("/valid", json={"name": "", "age": 0}),
                contains=("validation_failed", "name", "age"))

        compare("form extractor", cpp.post("/form", data={"name": "ada", "age": "36"}),
                go.post("/form", data={"name": "ada", "age": "36"}))
        compare("header + cookie extractors",
                cpp.get("/header", headers={"x-page": "3", "cookie": "sid=abc"}),
                go.get("/header", headers={"x-page": "3", "cookie": "sid=abc"}))
        compare("header extractor rejects a missing header",
                cpp.get("/header", headers={"cookie": "sid=abc"}),
                go.get("/header", headers={"cookie": "sid=abc"}), contains=("missing_header",))
        compare("cookie extractor rejects a missing cookie",
                cpp.get("/header", headers={"x-page": "3"}),
                go.get("/header", headers={"x-page": "3"}), contains=("missing_cookie",))

    # --- catch-all / exact / template -------------------------------------
    with client(CPP_PORT) as cpp, client(GO_PORT) as go:
        for path in ("/files/static", "/files/42", "/files/a/b", "/files/"):
            compare(f"file route {path}", cpp.get(path), go.get(path))

    # --- host routing ------------------------------------------------------
    with client(CPP_PORT) as cpp, client(GO_PORT) as go:
        for host in ("api.example.com", "API.EXAMPLE.COM.", "other.example.com",
                     "api.example.com:8080", "api.example.com:1234", "example.com", "evil-example.com"):
            compare(f"host {host}",
                    cpp.get("/who", headers={"host": host}),
                    go.get("/who", headers={"host": host}))

    # --- sessions ----------------------------------------------------------
    with client(CPP_PORT) as cpp, client(GO_PORT) as go:
        first_cpp = cpp.get("/session/get")
        first_go = go.get("/session/get")
        compare("session starts empty", first_cpp, first_go)
        check("sid" in cookie_names(first_cpp) and "sid" in cookie_names(first_go),
              "session establishes an sid cookie on both sides")
        check(cookie_has(first_cpp, "sid", "httponly") and cookie_has(first_go, "sid", "httponly"),
              "session cookie is HttpOnly on both sides")
        check(cookie_has(first_cpp, "sid", "path=/") and cookie_has(first_go, "sid", "path=/"),
              "session cookie has Path=/ on both sides")
        check(cookie_has(first_cpp, "sid", "samesite=lax") and cookie_has(first_go, "sid", "samesite=lax"),
              "session cookie has SameSite=Lax on both sides")
        compare("session set", cpp.post("/session/set"), go.post("/session/set"))
        compare("session persists", cpp.get("/session/get"), go.get("/session/get"))
        old_cpp = cpp.cookies.get("sid")
        old_go = go.cookies.get("sid")
        compare("session rotate", cpp.get("/session/rotate"), go.get("/session/rotate"))
        compare("session after rotate", cpp.get("/session/get"), go.get("/session/get"))

        with httpx.Client(base_url=base_url(CPP_PORT), timeout=5.0, trust_env=False,
                          cookies={"sid": old_cpp}) as stale_cpp, \
             httpx.Client(base_url=base_url(GO_PORT), timeout=5.0, trust_env=False,
                          cookies={"sid": old_go}) as stale_go:
            compare("rotated session invalidates the old id",
                    stale_cpp.get("/session/get"), stale_go.get("/session/get"))

    # --- CSRF --------------------------------------------------------------
    with client(CPP_PORT) as cpp, client(GO_PORT) as go:
        token_cpp = cpp.get("/csrf/token")
        token_go = go.get("/csrf/token")
        check(token_cpp.status_code == token_go.status_code == 200,
              "csrf token endpoint succeeds on both sides")
        check(bool(token_cpp.text) and bool(token_go.text),
              "csrf token endpoint returns a token on both sides")
        check("csrf" in cookie_names(token_cpp) and "csrf" in cookie_names(token_go),
              "csrf endpoint establishes the csrf cookie on both sides")
        compare("csrf accepts a matching header",
                cpp.post("/csrf/submit", headers={"x-csrf-token": token_cpp.text}),
                go.post("/csrf/submit", headers={"x-csrf-token": token_go.text}))
        check(cookie_has(token_cpp, "csrf", "samesite=lax") and cookie_has(token_go, "csrf", "samesite=lax"),
              "csrf cookie has SameSite=Lax on both sides")
        check(not cookie_has(token_cpp, "csrf", "httponly") and not cookie_has(token_go, "csrf", "httponly"),
              "csrf cookie is readable by the page on both sides")
        compare("csrf rejects a missing header", cpp.post("/csrf/submit"), go.post("/csrf/submit"))
        compare("csrf rejects a wrong header",
                cpp.post("/csrf/submit", headers={"x-csrf-token": "wrong"}),
                go.post("/csrf/submit", headers={"x-csrf-token": "wrong"}))
        compare("csrf exempt path", cpp.post("/csrf/exempt"), go.post("/csrf/exempt"))

    # --- JWT ---------------------------------------------------------------
    now = int(time.time())
    with client(CPP_PORT) as cpp, client(GO_PORT) as go:
        valid = hs256_token(jwt_payload("read write"))
        read_only = hs256_token(jwt_payload("read"))
        expired = hs256_token(jwt_payload("read", now - 3600))
        wrong = hs256_token(jwt_payload("read write"), secret="wrong")

        compare("jwt accepts a valid HS256 token",
                cpp.get("/jwt/me", headers={"authorization": "Bearer " + valid}),
                go.get("/jwt/me", headers={"authorization": "Bearer " + valid}))
        compare("jwt rejects a missing token", cpp.get("/jwt/me"), go.get("/jwt/me"),
                contains=("unauthorized",), auth=True)
        compare("jwt rejects a bad signature",
                cpp.get("/jwt/me", headers={"authorization": "Bearer " + wrong}),
                go.get("/jwt/me", headers={"authorization": "Bearer " + wrong}),
                contains=("unauthorized",), auth=True)
        compare("jwt rejects an expired token",
                cpp.get("/jwt/me", headers={"authorization": "Bearer " + expired}),
                go.get("/jwt/me", headers={"authorization": "Bearer " + expired}),
                contains=("unauthorized",), auth=True)
        compare("jwt requires the scope",
                cpp.get("/jwt/write", headers={"authorization": "Bearer " + read_only}),
                go.get("/jwt/write", headers={"authorization": "Bearer " + read_only}),
                contains=("forbidden",))
        compare("jwt allows a matching scope",
                cpp.get("/jwt/write", headers={"authorization": "Bearer " + valid}),
                go.get("/jwt/write", headers={"authorization": "Bearer " + valid}))

        wrong_issuer = hs256_token({**jwt_payload("read"), "iss": "other"})
        compare("jwt rejects a wrong issuer",
                cpp.get("/jwt/me", headers={"authorization": "Bearer " + wrong_issuer}),
                go.get("/jwt/me", headers={"authorization": "Bearer " + wrong_issuer}),
                contains=("unauthorized",), auth=True)

        none_header = b64url(b'{"alg":"none","typ":"JWT"}')
        none_payload = b64url(json.dumps(jwt_payload("read"), separators=(",", ":")).encode())
        none_token = f"{none_header}.{none_payload}.eA"
        compare("jwt rejects the none algorithm",
                cpp.get("/jwt/me", headers={"authorization": "Bearer " + none_token}),
                go.get("/jwt/me", headers={"authorization": "Bearer " + none_token}),
                contains=("unauthorized",), auth=True)

    # --- RS256 / ES256 -----------------------------------------------------
    if RSA_PRIV and EC_PRIV:
        rsa_key = load_private_key(RSA_PRIV)
        ec_key = load_private_key(EC_PRIV)
        with client(CPP_PORT) as cpp, client(GO_PORT) as go:
            for label, prefix, signer in (
                ("RS256", "/jwt/rs256", lambda payload: rs256_token(payload, rsa_key)),
                ("ES256", "/jwt/es256", lambda payload: es256_token(payload, ec_key)),
            ):
                valid = signer(jwt_payload("read write"))
                read_only = signer(jwt_payload("read"))
                expired = signer(jwt_payload("read", now - 3600))

                compare(f"jwt {label} accepts a valid token",
                        cpp.get(prefix + "/me", headers={"authorization": "Bearer " + valid}),
                        go.get(prefix + "/me", headers={"authorization": "Bearer " + valid}))
                compare(f"jwt {label} rejects a missing token",
                        cpp.get(prefix + "/me"), go.get(prefix + "/me"),
                        contains=("unauthorized",), auth=True)
                compare(f"jwt {label} rejects a bad signature",
                        cpp.get(prefix + "/me", headers={"authorization": "Bearer " + corrupt_signature(valid)}),
                        go.get(prefix + "/me", headers={"authorization": "Bearer " + corrupt_signature(valid)}),
                        contains=("unauthorized",), auth=True)
                compare(f"jwt {label} rejects an expired token",
                        cpp.get(prefix + "/me", headers={"authorization": "Bearer " + expired}),
                        go.get(prefix + "/me", headers={"authorization": "Bearer " + expired}),
                        contains=("unauthorized",), auth=True)
                compare(f"jwt {label} requires the scope",
                        cpp.get(prefix + "/write", headers={"authorization": "Bearer " + read_only}),
                        go.get(prefix + "/write", headers={"authorization": "Bearer " + read_only}),
                        contains=("forbidden",))
                compare(f"jwt {label} allows a matching scope",
                        cpp.get(prefix + "/write", headers={"authorization": "Bearer " + valid}),
                        go.get(prefix + "/write", headers={"authorization": "Bearer " + valid}))

    print(f"\n{checks} checks, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
