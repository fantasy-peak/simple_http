#!/usr/bin/env python3
"""C++/Go differential suites.

Two independent legs, both comparing simple_http against Go's standard library:

  framework  -- the same framework-layer requests against
                test/server/fixtures/framework_server.cpp and
                test/differential/go/framework_server.go; framework_diff.py
                performs the assertions.

  client     -- streaming uploads, 100-continue, request trailers, 429 and
                JWKS. Three runs are diffed line by line:
                  C++ client -> Go server      (C++ client correctness)
                  Go client  -> Go server      (baseline)
                  Go client  -> C++ server     (C++ server correctness)

Usage:
    test/.venv/bin/python test/differential/run.py framework
    test/.venv/bin/python test/differential/run.py client

The test runner sets SIMPLE_HTTP_TEST_BIN_DIR and SIMPLE_HTTP_TEST_WORKDIR.
"""

from __future__ import annotations

import difflib
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
GO_DIR = HERE / "go"

sys.path.insert(0, str(HERE.parent / "support"))
from report import Reporter  # noqa: E402


def find_binary(name: str) -> Path:
    bin_dir = os.environ.get("SIMPLE_HTTP_TEST_BIN_DIR")
    if bin_dir:
        candidate = Path(bin_dir) / name
        if candidate.exists():
            return candidate
    for mode in ("release", "debug"):
        candidate = REPO / "build" / "linux" / "x86_64" / mode / name
        if candidate.exists():
            return candidate
    return REPO / "build" / "linux" / "x86_64" / "release" / name


def workdir() -> Path:
    path = Path(os.environ.get("SIMPLE_HTTP_TEST_WORKDIR", REPO / ".cache" / "differential"))
    path.mkdir(parents=True, exist_ok=True)
    return path


def wait_http(url: str, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=0.5):
                return True
        except Exception:
            time.sleep(0.05)
    return False


def go_build(output: Path, source: Path) -> bool:
    run = subprocess.run(["go", "build", "-o", str(output), str(source)],
                         text=True, capture_output=True, cwd=str(REPO))
    if run.returncode != 0:
        print(run.stderr, file=sys.stderr)
    return run.returncode == 0


def stop(*procs: subprocess.Popen) -> None:
    for proc in procs:
        if proc is None:
            continue
        proc.terminate()
    for proc in procs:
        if proc is None:
            continue
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def make_keys(directory: Path) -> tuple[Path, Path, Path, Path]:
    directory.mkdir(parents=True, exist_ok=True)
    rsa_priv = directory / "rsa_priv.pem"
    rsa_pub = directory / "rsa_pub.pem"
    ec_priv = directory / "ec_priv.pem"
    ec_pub = directory / "ec_pub.pem"
    if not (rsa_priv.exists() and ec_priv.exists()):
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                        "rsa_keygen_bits:2048", "-out", str(rsa_priv)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["openssl", "pkey", "-in", str(rsa_priv), "-pubout", "-out", str(rsa_pub)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout",
                        "-out", str(ec_priv)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["openssl", "pkey", "-in", str(ec_priv), "-pubout", "-out", str(ec_pub)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return rsa_priv, rsa_pub, ec_priv, ec_pub


def framework() -> int:
    reporter = Reporter(os.environ.get("SIMPLE_HTTP_TEST_SUITE", "framework-diff"))
    cache = workdir()
    try:
        rsa_priv, rsa_pub, ec_priv, ec_pub = make_keys(cache / "keys")
    except (OSError, subprocess.CalledProcessError) as error:
        reporter.check(False, f"generate RSA/EC keys ({error})")
        return reporter.summary()

    cpp_bin = find_binary("fixture_framework_server")
    go_bin = cache / "framework_server_go"
    if not cpp_bin.exists():
        reporter.check(False, f"{cpp_bin} not built — run `xmake build fixture_framework_server`")
        return reporter.summary()
    if not go_build(go_bin, GO_DIR / "framework_server.go"):
        reporter.check(False, "build Go framework server")
        return reporter.summary()

    cpp_port, go_port = 7798, 7799
    cpp_log = open(cache / "framework_cpp.log", "w")
    go_log = open(cache / "framework_go.log", "w")
    cpp = subprocess.Popen([str(cpp_bin), str(cpp_port), str(rsa_pub), str(ec_pub)],
                           stdout=cpp_log, stderr=subprocess.STDOUT, cwd=str(REPO))
    go = subprocess.Popen([str(go_bin), str(go_port), str(rsa_pub), str(ec_pub)],
                          stdout=go_log, stderr=subprocess.STDOUT, cwd=str(REPO))
    try:
        cpp_up = wait_http(f"http://127.0.0.1:{cpp_port}/state")
        go_up = wait_http(f"http://127.0.0.1:{go_port}/state")
        reporter.check(cpp_up, f"simple_http framework server up on :{cpp_port}")
        reporter.check(go_up, f"Go framework server up on :{go_port}")
        if cpp_up and go_up:
            run = subprocess.run(
                [sys.executable, str(HERE / "framework_diff.py"),
                 str(cpp_port), str(go_port), str(rsa_priv), str(ec_priv)],
                text=True, capture_output=True, cwd=str(REPO),
            )
            print(run.stdout.rstrip())
            if run.stderr.strip():
                print(run.stderr.rstrip(), file=sys.stderr)
            recorded = False
            for line in run.stdout.splitlines():
                match = re.match(r"^(PASS|FAIL)\s+(.*)$", line.strip())
                if match:
                    reporter.record(match.group(1) == "PASS", match.group(2))
                    recorded = True
            if not recorded:
                reporter.check(run.returncode == 0, "framework differential")
            elif run.returncode != 0:
                reporter.record(False, f"framework_diff.py exited with {run.returncode}")
    finally:
        stop(cpp, go)
        cpp_log.close()
        go_log.close()
    return reporter.summary()


def _probe_lines(path: Path) -> list[str]:
    return [line for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
            if line.startswith(("PASS", "FAIL"))]


def client() -> int:
    reporter = Reporter(os.environ.get("SIMPLE_HTTP_TEST_SUITE", "client-diff"))
    cache = workdir()

    cpp_probe = find_binary("probe_client_diff")
    cpp_server = find_binary("fixture_framework_server")
    if not cpp_probe.exists() or not cpp_server.exists():
        reporter.check(False, "C++ probes not built — run `xmake build probe_client_diff fixture_framework_server`")
        return reporter.summary()

    go_server = cache / "client_diff_server"
    go_probe = cache / "client_diff_probe"
    if not go_build(go_server, GO_DIR / "client_diff_server.go"):
        reporter.check(False, "build Go reference server")
        return reporter.summary()
    if not go_build(go_probe, GO_DIR / "client_diff_probe.go"):
        reporter.check(False, "build Go probe")
        return reporter.summary()

    go_port, baseline_port, cpp_port = 7802, 7803, 7804
    logs = [open(cache / f"{name}.log", "w") for name in ("go_server_a", "go_server_b", "cpp_server")]
    procs = [
        subprocess.Popen([str(go_server), str(go_port)], stdout=logs[0], stderr=subprocess.STDOUT,
                         cwd=str(REPO)),
        subprocess.Popen([str(go_server), str(baseline_port)], stdout=logs[1], stderr=subprocess.STDOUT,
                         cwd=str(REPO)),
        subprocess.Popen([str(cpp_server), str(cpp_port)], stdout=logs[2], stderr=subprocess.STDOUT,
                         cwd=str(REPO)),
    ]
    try:
        ups = [
            wait_http(f"http://127.0.0.1:{go_port}/world"),
            wait_http(f"http://127.0.0.1:{baseline_port}/world"),
            wait_http(f"http://127.0.0.1:{cpp_port}/world"),
        ]
        reporter.check(all(ups), f"reference servers up on :{go_port}, :{baseline_port}, :{cpp_port}")
        if not all(ups):
            return reporter.summary()

        runs = {
            "cpp": [str(cpp_probe), f"http://127.0.0.1:{go_port}"],
            "go": [str(go_probe), f"http://127.0.0.1:{baseline_port}"],
            "go_cpp": [str(go_probe), f"http://127.0.0.1:{cpp_port}"],
        }
        outputs: dict[str, Path] = {}
        for name, command in runs.items():
            path = cache / f"{name}.out"
            run = subprocess.run(command, text=True, capture_output=True, cwd=str(REPO))
            path.write_text(run.stdout + run.stderr, encoding="utf-8")
            print(f"==> {name}\n{run.stdout.rstrip()}")
            outputs[name] = path
            recorded = False
            for line in run.stdout.splitlines():
                match = re.match(r"^(PASS|FAIL)\s+(.*)$", line.strip())
                if match:
                    reporter.record(match.group(1) == "PASS", f"{name}: {match.group(2)}")
                    recorded = True
            if not recorded:
                reporter.check(run.returncode == 0 and not any(
                    line.startswith("FAIL") for line in run.stdout.splitlines()),
                    f"{name} probe reported no failures")
            elif run.returncode != 0:
                reporter.record(False, f"{name} probe exited with {run.returncode}")

        cpp_lines, go_lines = _probe_lines(outputs["cpp"]), _probe_lines(outputs["go"])
        go_cpp_lines = _probe_lines(outputs["go_cpp"])
        diff_cpp = list(difflib.unified_diff(cpp_lines, go_lines, "cpp-client", "go-client", lineterm=""))
        diff_server = list(difflib.unified_diff(go_lines, go_cpp_lines, "go-server", "cpp-server", lineterm=""))
        if diff_cpp:
            print("\n".join(diff_cpp), file=sys.stderr)
        if diff_server:
            print("\n".join(diff_server), file=sys.stderr)
        reporter.check(not diff_cpp, "C++ client and Go client agree")
        reporter.check(not diff_server, "C++ server and Go server agree")
    finally:
        stop(*procs)
        for handle in logs:
            handle.close()
    return reporter.summary()


def main() -> int:
    suite = sys.argv[1] if len(sys.argv) > 1 else ""
    if suite == "framework":
        return framework()
    if suite == "client":
        return client()
    print("usage: run.py framework|client", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
