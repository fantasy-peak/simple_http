#!/usr/bin/env python3
"""OpenAPI contract verification for the petshop fixture.

Four independent checks:

  1. The document validates against the official OAS 3.1 metaschema.
  2. Structural invariants: unique operationIds, path parameters that match the
     template, every declared response has a description, every $ref resolves,
     no $defs leak.
  3. Every operation is exercised against the live server with a probe for each
     status it declares; the response Content-Type and JSON Schema are checked.
  4. Swagger UI is reachable and the document is byte-for-byte deterministic.

Usage:
    test/diff/openapi/verify.py --base-url http://127.0.0.1:7795
"""

from __future__ import annotations

import argparse
import http
import copy
import json
import os
import sys
from pathlib import Path
from typing import Any

import httpx
import jsonschema

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "test" / "support"))

from report import Reporter  # noqa: E402

METASCHEMA = HERE / "schema" / "openapi-3.1.json"

# Every route the demo registers, documented or not. The document must contain
# exactly the documented subset; a route added without a doc() is the failure
# this list catches. `/echo/{what}/times/{n}` is deliberately undocumented.
REGISTERED = {
    ("get", "/pets"),
    ("post", "/pets"),
    ("get", "/pets/{id}"),
    ("get", "/pets/search"),
    ("get", "/owners/{owner}/pets/{pet_id}"),
    ("post", "/owners/{owner_id}/pets"),
    ("get", "/orders/{order_id}/items/{item_id}"),
    ("get", "/orders/{id}"),
    ("post", "/orders"),
    ("get", "/geo/{city}/points/{lat}/{lng}/{precision}"),
    ("get", "/lab/{experiment}/{run}/{fast}"),
}
UNDOCUMENTED = {("get", "/echo/{what}/times/{n}")}

AUTH = {"authorization": "Bearer test-token"}

PROBES: list[dict[str, Any]] = [
    # listPets
    {"op": "listPets", "status": 200, "method": "GET", "path": "/pets"},
    # getPet
    {"op": "getPet", "status": 200, "method": "GET", "path": "/pets/42"},
    {"op": "getPet", "status": 404, "method": "GET", "path": "/pets/abc"},
    # searchPets
    {"op": "searchPets", "status": 200, "method": "GET", "path": "/pets/search"},
    # createPet
    {"op": "createPet", "status": 401, "method": "POST", "path": "/pets",
     "json": {"name": "fido"}},
    {"op": "createPet", "status": 201, "method": "POST", "path": "/pets",
     "headers": AUTH, "json": {"name": "fido"}},
    {"op": "createPet", "status": 400, "method": "POST", "path": "/pets",
     "headers": {**AUTH, "content-type": "application/json"}, "content": b"{bad"},
    {"op": "createPet", "status": 409, "method": "POST", "path": "/pets",
     "headers": AUTH, "json": {"name": "taken"}},
    {"op": "createPet", "status": 422, "method": "POST", "path": "/pets",
     "headers": AUTH, "json": {"status": "sold"}},
    # getOwnedPet
    {"op": "getOwnedPet", "status": 200, "method": "GET",
     "path": "/owners/acme-corp/pets/7"},
    {"op": "getOwnedPet", "status": 404, "method": "GET",
     "path": "/owners/acme-corp/pets/0"},
    # getOrderItem
    {"op": "getOrderItem", "status": 200, "method": "GET",
     "path": "/orders/10/items/3"},
    {"op": "getOrderItem", "status": 404, "method": "GET",
     "path": "/orders/10/items/x"},
    # getOrder
    {"op": "getOrder", "status": 200, "method": "GET", "path": "/orders/7"},
    {"op": "getOrder", "status": 404, "method": "GET", "path": "/orders/0"},
    # placeOrder
    {"op": "placeOrder", "status": 401, "method": "POST", "path": "/orders",
     "json": {"pet_id": 10}},
    {"op": "placeOrder", "status": 201, "method": "POST", "path": "/orders",
     "headers": AUTH, "json": {"pet_id": 10}},
    {"op": "placeOrder", "status": 400, "method": "POST", "path": "/orders",
     "headers": {**AUTH, "content-type": "application/json"}, "content": b"{bad"},
    {"op": "placeOrder", "status": 422, "method": "POST", "path": "/orders",
     "headers": AUTH, "json": {}},
    # createOwnedPet
    {"op": "createOwnedPet", "status": 401, "method": "POST",
     "path": "/owners/10/pets", "json": {"name": "spike"}},
    {"op": "createOwnedPet", "status": 201, "method": "POST",
     "path": "/owners/10/pets", "headers": AUTH, "json": {"name": "spike"}},
    {"op": "createOwnedPet", "status": 400, "method": "POST",
     "path": "/owners/10/pets", "headers": {**AUTH, "content-type": "application/json"},
     "content": b"{bad"},
    {"op": "createOwnedPet", "status": 404, "method": "POST",
     "path": "/owners/0/pets", "headers": AUTH, "json": {"name": "spike"}},
    {"op": "createOwnedPet", "status": 422, "method": "POST",
     "path": "/owners/10/pets", "headers": AUTH, "json": {}},
    {"op": "createOwnedPet", "status": 429, "method": "POST",
     "path": "/owners/10/pets", "headers": {**AUTH, "x-rate-limited": "1"},
     "json": {"name": "spike"}},
    # geo
    {"op": "resolveGeo", "status": 200, "method": "GET",
     "path": "/geo/beijing/points/39.9/116.4/8"},
    {"op": "resolveGeo", "status": 404, "method": "GET",
     "path": "/geo/beijing/points/xx/116.4/8"},
    # lab
    {"op": "getLabRun", "status": 200, "method": "GET", "path": "/lab/alpha/7/true"},
    {"op": "getLabRun", "status": 404, "method": "GET", "path": "/lab/alpha/7/1"},
]


def resolve_pointer(document: dict, ref: str) -> Any:
    node: Any = document
    for part in ref.lstrip("#/").split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        node = node[part]
    return node


def deref(node: Any, document: dict, depth: int = 0) -> Any:
    if depth > 32:
        return node
    if isinstance(node, dict):
        ref = node.get("$ref")
        if isinstance(ref, str) and ref.startswith("#/"):
            return deref(resolve_pointer(document, ref), document, depth + 1)
        return {key: deref(value, document, depth + 1) for key, value in node.items()}
    if isinstance(node, list):
        return [deref(value, document, depth + 1) for value in node]
    return node


def walk_refs(node: Any):
    if isinstance(node, dict):
        if "$ref" in node:
            yield node["$ref"]
        for value in node.values():
            yield from walk_refs(value)
    elif isinstance(node, list):
        for value in node:
            yield from walk_refs(value)


def operation_index(document: dict) -> dict[str, tuple[str, str, dict]]:
    index: dict[str, tuple[str, str, dict]] = {}
    for template, methods in document.get("paths", {}).items():
        for method, operation in methods.items():
            if method.lower() not in {"get", "post", "put", "patch", "delete", "head", "options"}:
                continue
            operation_id = operation.get("operationId")
            if operation_id:
                index[operation_id] = (template, method.lower(), operation)
    return index


def check_document(reporter: Reporter, document: dict) -> None:
    # --- metaschema --------------------------------------------------------
    metaschema = json.loads(METASCHEMA.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(metaschema)
    errors = sorted(validator.iter_errors(document), key=lambda error: list(error.path))
    reporter.check(not errors, "document validates against the OAS 3.1 metaschema",
                   "; ".join(error.message for error in errors[:3]))

    reporter.check(str(document.get("openapi", "")).startswith("3.1"),
                   "openapi version is 3.1")
    info = document.get("info") or {}
    reporter.check(bool(info.get("title")) and bool(info.get("version")),
                   "info.title and info.version are present")
    paths = document.get("paths") or {}
    reporter.check(bool(paths), "paths is non-empty")

    # --- structure ---------------------------------------------------------
    operation_ids: list[str] = []
    for template, methods in paths.items():
        for method, operation in methods.items():
            if method.lower() not in {"get", "post", "put", "patch", "delete", "head", "options"}:
                continue
            if operation.get("operationId"):
                operation_ids.append(operation["operationId"])
            responses = operation.get("responses") or {}
            reporter.check(bool(responses), f"{method.upper()} {template} declares responses")
            reporter.check(all(bool(response.get("description")) for response in responses.values()),
                           f"{method.upper()} {template} response descriptions present")
            parameters = operation.get("parameters") or []
            path_parameters = {p.get("name") for p in parameters if p.get("in") == "path"}
            declared = set()
            for segment in template.split("/"):
                if segment.startswith("{") and segment.endswith("}"):
                    declared.add(segment[1:-1])
            reporter.check(path_parameters == declared,
                           f"{method.upper()} {template} path parameters match its template",
                           f"declared={sorted(path_parameters)} template={sorted(declared)}")
            reporter.check(all(p.get("required") is True for p in parameters if p.get("in") == "path"),
                           f"{method.upper()} {template} path parameters are required")
    reporter.check(len(operation_ids) == len(set(operation_ids)),
                   "operationId is unique")

    # --- registered routes are documented ----------------------------------
    documented = {(method, template) for template, methods in paths.items()
                  for method in methods
                  if method.lower() in {"get", "post", "put", "patch", "delete", "head", "options"}}
    reporter.check(documented == REGISTERED,
                   "documented operations match the registered route table",
                   f"missing={sorted(REGISTERED - documented)} extra={sorted(documented - REGISTERED)}")
    reporter.check(not (documented & UNDOCUMENTED),
                   "undocumented route is absent from the document")

    # --- $ref integrity ----------------------------------------------------
    refs = list(walk_refs(document))
    missing = []
    external = []
    for ref in refs:
        if not isinstance(ref, str):
            external.append(str(ref))
            continue
        if not ref.startswith("#/"):
            external.append(ref)
            continue
        try:
            resolve_pointer(document, ref)
        except (KeyError, IndexError, TypeError):
            missing.append(ref)
    reporter.check(not external, "all $refs are local", ", ".join(external[:5]))
    reporter.check(not missing, "all $refs resolve", ", ".join(missing[:5]))
    reporter.check("$defs" not in json.dumps(document), "no $defs leak into the document")

    schemes = ((document.get("components") or {}).get("securitySchemes") or {})
    bearer = schemes.get("bearer") or {}
    reporter.check(bearer.get("type") == "http" and bearer.get("scheme") == "bearer",
                   "bearer security scheme is declared")


def check_swagger(reporter: Reporter, client: httpx.Client, document_bytes: bytes) -> None:
    response = client.get("/swagger")
    reporter.check(response.status_code == http.HTTPStatus.OK, "GET /swagger returns 200")
    reporter.check("text/html" in response.headers.get("content-type", ""),
                   "Swagger UI is HTML")
    reporter.check("openapi.json" in response.text and "swagger" in response.text.lower(),
                   "Swagger UI references the spec")
    spec = client.get("/openapi.json")
    reporter.check(spec.status_code == http.HTTPStatus.OK, "GET /openapi.json returns 200")
    reporter.check(spec.content == document_bytes, "document is deterministic across requests")
    reporter.check(spec.headers.get("content-type", "").startswith("application/json"),
                   "document Content-Type is application/json")


def check_contract(reporter: Reporter, client: httpx.Client, document: dict) -> None:
    index = operation_index(document)
    covered: dict[str, set[int]] = {}
    for probe in PROBES:
        op_id = probe["op"]
        if op_id not in index:
            reporter.check(False, f"probe {op_id}: operation documented")
            continue
        template, method, operation = index[op_id]
        declared = {int(status) for status in (operation.get("responses") or {})}
        covered.setdefault(op_id, set()).add(probe["status"])
        reporter.check(probe["status"] in declared,
                       f"probe {op_id} status {probe['status']} is declared",
                       f"declared={sorted(declared)}")

        kwargs: dict[str, Any] = {"headers": probe.get("headers")}
        if "json" in probe:
            kwargs["json"] = probe["json"]
        if "content" in probe:
            kwargs["content"] = probe["content"]
        response = client.request(probe["method"], probe["path"], **kwargs)
        name = f"{op_id} {probe['method']} {probe['path']} -> {probe['status']}"
        reporter.check(response.status_code == probe["status"],
                       f"{name}: status", f"got {response.status_code}")

        declaration = (operation.get("responses") or {}).get(str(probe["status"]), {})
        content = declaration.get("content") or {}
        if "application/json" in content:
            reporter.check(response.headers.get("content-type", "").startswith("application/json"),
                           f"{name}: Content-Type", response.headers.get("content-type", ""))
            schema = content["application/json"].get("schema")
            if schema:
                try:
                    instance = response.json()
                except ValueError:
                    reporter.check(False, f"{name}: response is JSON", response.text[:120])
                    continue
                try:
                    jsonschema.validate(instance, deref(schema, document))
                    reporter.check(True, f"{name}: body matches the declared schema")
                except jsonschema.ValidationError as error:
                    reporter.check(False, f"{name}: body matches the declared schema",
                                   error.message)

        for header in (declaration.get("headers") or {}):
            reporter.check(header.lower() in {key.lower() for key in response.headers},
                           f"{name}: declared header {header} present")
        if probe["status"] == http.HTTPStatus.UNAUTHORIZED:
            reporter.check(response.headers.get("www-authenticate", "").lower().startswith("bearer"),
                           f"{name}: bearer challenge present")

    # Every declared status must be exercised.
    for op_id, (template, method, operation) in index.items():
        declared = {int(status) for status in (operation.get("responses") or {})}
        exercised = covered.get(op_id, set())
        reporter.check(declared <= exercised,
                       f"every declared status of {op_id} has a probe",
                       f"missing={sorted(declared - exercised)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:7795")
    args = parser.parse_args()

    reporter = Reporter(os.environ.get("SIMPLE_HTTP_TEST_SUITE", "openapi"))
    client = httpx.Client(base_url=args.base_url.rstrip("/"), timeout=10)

    try:
        response = client.get("/openapi.json")
        if response.status_code != http.HTTPStatus.OK:
            reporter.check(False, "GET /openapi.json", f"status {response.status_code}")
            return reporter.summary()
        document = response.json()
        check_document(reporter, document)
        check_swagger(reporter, client, response.content)
        check_contract(reporter, client, document)
    finally:
        client.close()
    return reporter.summary()


if __name__ == "__main__":
    sys.exit(main())
