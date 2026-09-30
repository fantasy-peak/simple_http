#!/usr/bin/env python3
# Verifies the petshop OpenAPI demo end-to-end with real HTTP requests:
#   1. the /openapi.json document (typed path params, securitySchemes, literals)
#   2. template routing + typed path-parameter parsing (each capture parsed into
#      its field type, echoed back so values must make an exact round-trip)
#   3. methods: 405 keeps the connection alive (same socket), auto-OPTIONS,
#      security (bearer) on write routes
#
# Run after building:
#   xmake build openapi_demo
#   test/python/.venv/bin/python test/openapi_verify.py
# or with any python3 that has httpx.

import json
import os
import socket
import subprocess
import sys
import time

import httpx

PORT = 7795
BASE = f"http://127.0.0.1:{PORT}"
BIN = os.path.join(os.path.dirname(__file__), "..", "build", "linux", "x86_64", "release", "openapi_demo")

fails = 0


def check(name, cond, detail=""):
    global fails
    if cond:
        print(f"PASS  {name}")
    else:
        fails += 1
        print(f"FAIL  {name}  {detail}")


def raw_request_once(sock, raw: bytes) -> bytes:
    """Send one raw HTTP/1.1 request, read the full response (headers + body
    per the Content-Length of that single exchange), into one buffer."""
    sock.sendall(raw)
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    clen = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":", 1)[1].strip())
    while len(rest) < clen:
        rest += sock.recv(65536)
    return head + b"\r\n\r\n" + rest


def main():
    subprocess.run(["pkill", "-x", "openapi_demo"], capture_output=True)
    proc = subprocess.Popen([BIN], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        up = False
        for _ in range(60):
            try:
                httpx.get(f"{BASE}/openapi.json", timeout=1)
                up = True
                break
            except Exception:
                time.sleep(0.2)
        if not up:
            print(f"FAIL  demo did not come up on :{PORT} — is the binary built?")
            sys.exit(1)
        run_checks()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_checks():
    client = httpx.Client(base_url=BASE, timeout=10)

    # --- 1. document -----------------------------------------------------------
    doc = client.get("/openapi.json").json()
    check("document is OAS 3.1", str(doc.get("openapi")).startswith("3.1"))
    sch = doc.get("components", {}).get("securitySchemes", {})
    check("bearer security scheme declared", sch.get("bearer", {}).get("type") == "http"
          and sch.get("bearer", {}).get("scheme") == "bearer")
    paths = doc["paths"]
    check("template routes are document keys",
          "/pets/{id}" in paths and "/geo/{city}/points/{lat}/{lng}/{precision}" in paths
          and "/owners/{owner}/pets/{pet_id}" in paths)
    params = paths["/pets/{id}"]["get"].get("parameters", [])
    idp = next((p for p in params if p.get("name") == "id"), {})
    check("id path param derived from PetParams (path, required, integer)",
          idp.get("in") == "path" and idp.get("required") is True
          and "integer" in json.dumps(idp.get("schema", {})))
    gp = {p["name"]: p for p in paths["/geo/{city}/points/{lat}/{lng}/{precision}"]["get"].get("parameters", [])}
    check("geo derives all four params", set(gp) == {"city", "lat", "lng", "precision"})
    check("geo lat schema is number", "number" in json.dumps(gp["lat"].get("schema", {})))
    check("geo precision schema is integer", "integer" in json.dumps(gp["precision"].get("schema", {})))
    check("write routes carry the bearer security",
          "bearer" in json.dumps(paths["/pets"]["post"].get("security", [])))
    check("literal /pets/search is its own key (not absorbed by {id})", "/pets/search" in paths)
    # A declared request header: /pets/{id} takes x-trace-id (in: header) and
    # echoes it back as a response header.
    getpet_params = paths["/pets/{id}"]["get"].get("parameters", [])
    trace = next((p for p in getpet_params if p.get("name") == "x-trace-id"), {})
    check("request header x-trace-id declared (in: header, string)",
          trace.get("in") == "header" and "string" in json.dumps(trace.get("schema", {})))
    # Field-level metadata from a glaze_json_schema annotation: Pet.status gets
    # a description and an enum list, rendered as a swagger dropdown. The schema
    # is hoisted into components.schemas now, so dereference the $ref first.
    def resolve(node):
        while isinstance(node, dict) and "$ref" in node:
            cur = doc
            for part in node["$ref"].lstrip("#/").split("/"):
                cur = cur.get(part, {})
            node = cur
        return node or {}

    pet_ref = paths["/pets/{id}"]["get"]["responses"]["200"]["content"]["application/json"]["schema"]
    pet_schema = resolve(pet_ref)
    status_schema = pet_schema.get("properties", {}).get("status", {})
    check("Pet.status carries its annotation (description + enum)",
          status_schema.get("description") == "lifecycle state of the pet"
          and status_schema.get("enum") == ["available", "pending", "sold"])
    comps = doc.get("components", {}).get("schemas", {})
    check("object schemas are hoisted into components.schemas and referenced by $ref",
          isinstance(pet_ref, dict) and pet_ref.get("$ref", "").startswith("#/components/schemas/")
          and len(comps) >= 1 and pet_schema.get("type") == "object")
    # Declared response headers: success (201) and an error (429) with Retry-After.
    create_pet = paths["/pets"]["post"]
    hdrs = create_pet.get("responses", {}).get("201", {}).get("headers", {})
    check("success response declares X-Request-Id (string)",
          "X-Request-Id" in hdrs and "string" in json.dumps(hdrs["X-Request-Id"].get("schema", {})))
    owned = paths["/owners/{owner_id}/pets"]["post"]
    rl = owned.get("responses", {}).get("429", {}).get("headers", {})
    check("a 429 error response declares Retry-After (integer)",
          "Retry-After" in rl and "integer" in json.dumps(rl["Retry-After"].get("schema", {})))

    # --- 2. template routing + typed parsing ------------------------------------
    r = client.get("/pets/42")
    check("GET /pets/42 parses id=42 (int64)", r.status_code == 200 and r.json().get("id") == 42)
    r = client.get("/pets/42", headers={"x-trace-id": "trace-17"})
    check("request header x-trace-id is read and echoed back",
          r.status_code == 200 and r.headers.get("x-trace-id") == "trace-17")
    r = client.get("/pets/abc")
    check("GET /pets/abc: non-integer id is a 404", r.status_code == 404)
    r = client.get("/pets/search")
    check("literal /pets/search beats the {id} template",
          r.status_code == 200 and r.json().get("items", [{}])[0].get("name") == "search-hit")

    r = client.get("/owners/acme-corp/pets/7")
    check("GET owners/{slug}/pets/{pet_id}: string+uint64 mix parses",
          r.status_code == 200 and r.json().get("id") == 7)
    r = client.get("/owners//pets/7")
    check("an empty capture segment does not match", r.status_code == 404)

    r = client.get("/geo/beijing/points/39.9/116.4/8")
    g = r.json() if r.status_code == 200 else {}
    check("GET geo: string/double/double/uint32 round-trip exact",
          r.status_code == 200 and g.get("city") == "beijing"
          and g.get("lat") == 39.9 and g.get("lng") == 116.4 and g.get("precision") == 8)
    r = client.get("/geo/beijing/points/39.9/xx/8")
    check("GET geo: unparsable double is a 404", r.status_code == 404)

    r = client.get("/lab/alpha/7/true")
    lab = r.json() if r.status_code == 200 else {}
    check("GET lab: bool 'true' parses (run=7 as int, fast as bool)",
          r.status_code == 200 and lab.get("run") == 7 and lab.get("fast") is True)
    r = client.get("/lab/alpha/7/1")
    check("GET lab: bool '1' is rejected (strict true/false)", r.status_code == 404)

    r = client.get("/orders/10/items/3")
    o = r.json() if r.status_code == 200 else {}
    check("axum example: /orders/{order_id}/items/{item_id}",
          r.status_code == 200 and o.get("id") == 3 and o.get("pet_id") == 10)

    # A plain route (no typed Params, no doc entry) still routes templates and
    # exposes the captures via req->param(name).
    r = client.get("/echo/hi/times/3")
    check("plain route /echo/{what}/times/{n} captures via req->param",
          r.status_code == 200 and r.text == "what=hi n=3")
    r = client.get("/echo/hi/times")
    check("plain template: segment-count mismatch is a 404", r.status_code == 404)

    # --- 3. methods, security, keep-alive ---------------------------------------
    r = client.post("/pets", json={"name": "x"})
    check("POST /pets without token is 401",
          r.status_code == 401 and "bearer" in r.headers.get("www-authenticate", "").lower())
    r = client.post("/pets", json={"name": "fido"}, headers={"authorization": "Bearer t"})
    check("POST /pets with token + valid body is 201",
          r.status_code == 201 and r.json().get("name") == "fido")
    check("POST /pets really sent the declared X-Request-Id header",
          r.headers.get("x-request-id") == "req-42")
    r = client.post("/pets", content=b"{bad json", headers={"authorization": "Bearer t"})
    check("POST /pets with unparsable body is 400", r.status_code == 400)
    r = client.post("/pets", json={"status": "sold"}, headers={"authorization": "Bearer t"})
    check("POST /pets missing required field (name) is 422", r.status_code == 422)
    r = client.post("/pets", json={"name": 123}, headers={"authorization": "Bearer t"})
    check("POST /pets with a type-mismatched field is 400", r.status_code == 400)
    r = client.post("/owners/10/pets", json={"name": "spike"}, headers={"authorization": "Bearer t"})
    check("POST owners/{owner_id}/pets (path+body+security) is 201",
          r.status_code == 201 and r.json().get("id") == 10)

    r = client.options("/pets")
    check("OPTIONS is auto-answered 204 with Allow",
          r.status_code == 204 and "GET" in r.headers.get("allow", "") and "OPTIONS" in r.headers.get("allow", ""))

    r = client.post("/pets/42")
    check("POST to GET-only template is 405 with Allow",
          r.status_code == 405 and "GET" in r.headers.get("allow", "")
          and "HEAD" in r.headers.get("allow", ""))

    # The 405 must NOT kill the connection: the same TCP socket carries the next
    # exchange (httpx/curl would silently reconnect and hide the difference).
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    first = raw_request_once(sock, b"POST /pets/42 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
    second = raw_request_once(sock, b"GET /pets/42 HTTP/1.1\r\nHost: x\r\n\r\n")
    sock.close()
    check("405 is a keep-alive response: the same socket answered the next request",
          first.startswith(b"HTTP/1.1 405") and second.startswith(b"HTTP/1.1 200"))

    r = client.get("/definitely-not-a-route")
    check("unmatched path falls to 404", r.status_code == 404)

    client.close()


if __name__ == "__main__":
    main()
    print(f"{'OK' if fails == 0 else 'FAILED'}  {fails} failed")
    sys.exit(1 if fails else 0)