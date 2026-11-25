#!/usr/bin/env python3
"""HTTP/3 0-RTT differential: C++ and quic-go, 0-RTT on and off.

Enabled cases exercise the full path in both directions:

  C++ client -> Go server   and   C++ client -> C++ server
  Go client  -> C++ server  and   Go client  -> Go server

Disabled cases check that HTTP/3 still works when either side turns 0-RTT off:

  - server disabled, client still requests early -> normal 1-RTT
  - client disabled, server still accepts early  -> normal 1-RTT

Each client makes one normal request (obtaining a ticket), closes the
connection, then sends GET_0RTT on a fresh one. The server handler reports
whether the request was accepted as early data.

A rejection case swaps the server for a fresh process (new ticket keys)
between the two requests; the client must then report
client_errc::zero_rtt_rejected, not a protocol error.
"""

from __future__ import annotations

import os
import subprocess
import sys
import threading
import time
from pathlib import Path

TEST = Path(__file__).resolve().parent
DIFF = TEST.parent
ROOT = DIFF.parent.parent
BIN_DIR = Path(os.environ.get("SIMPLE_HTTP_TEST_BIN_DIR", ROOT / "build/linux/x86_64/release"))
WORK = Path(os.environ.get("SIMPLE_HTTP_TEST_WORKDIR", TEST / ".work"))
WORK.mkdir(parents=True, exist_ok=True)

GO_ENABLED_PORT = 7930
CPP_ENABLED_PORT = 7931
CPP_DISABLED_PORT = 7932
GO_DISABLED_PORT = 7933
CPP_REJECT_PORT = 7934


def run(command: list[str], *, cwd: Path, timeout: int = 60) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=str(cwd), capture_output=True, text=True, timeout=timeout)


def build_go() -> tuple[Path, Path]:
    server = WORK / "go_server"
    client = WORK / "go_client"
    for output, source in ((server, "./0rtt/go/server.go"), (client, "./0rtt/go/client.go")):
        result = run(["go", "build", "-o", str(output), source], cwd=DIFF, timeout=300)
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            raise SystemExit("go build failed")
    return server, client


class Server:
    def __init__(self, command: list[str], name: str):
        self.name = name
        self.process = subprocess.Popen(command, cwd=str(ROOT), stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True)
        time.sleep(1.5)
        if self.process.poll() is not None:
            output = self.process.stdout.read() if self.process.stdout else ""
            raise RuntimeError(f"{name} exited early:\n{output}")

    def stop(self) -> None:
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)


def check_cpp_client(name: str, client: Path, url: str, expect_early: bool,
                     client_0rtt: bool) -> bool:
    result = run([str(client), url, "1" if expect_early else "0", "1" if client_0rtt else "0"],
                 cwd=ROOT, timeout=60)
    print(f"--- {name} ---")
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    observed = "second=early" if expect_early else "second=normal"
    used = "used_0rtt=1" if expect_early else "used_0rtt=0"
    ok = result.returncode == 0 and "first=normal" in result.stdout and \
        observed in result.stdout and used in result.stdout
    print(f"{name}: {'PASS' if ok else 'FAIL'}")
    return ok


def check_go_client(name: str, client: Path, url: str, expect_early: bool,
                    client_0rtt: bool) -> bool:
    result = run([str(client), url, "1" if expect_early else "0", "1" if client_0rtt else "0"],
                 cwd=ROOT, timeout=60)
    print(f"--- {name} ---")
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    observed = "second=early" if expect_early else "second=normal"
    ok = result.returncode == 0 and "first=normal" in result.stdout and observed in result.stdout
    print(f"{name}: {'PASS' if ok else 'FAIL'}")
    return ok


def check_cpp_rejection(server_bin: Path, client_bin: Path) -> bool:
    """A ticket is obtained from server A; A is replaced by a fresh process B
    whose ticket keys differ, so B refuses the early data. The client must
    report client_errc::zero_rtt_rejected rather than a protocol error."""
    label = "cpp-client -> cpp-server (rejected ticket)"
    url = f"https://127.0.0.1:{CPP_REJECT_PORT}/0rtt"
    first = Server([str(server_bin), str(CPP_REJECT_PORT), "1"], "cpp server (reject #1)")
    second = None
    proc = None
    swapped = False
    lines: list[str] = []
    try:
        proc = subprocess.Popen([str(client_bin), url, "0", "1", "1"], cwd=str(ROOT),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        reader = threading.Thread(target=lambda: lines.extend(proc.stdout), daemon=True)
        reader.start()
        # Wait for the first request to finish (and the ticket to be captured)
        # instead of assuming a fixed latency; under sanitizers it can be slow.
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if any(line.startswith("first=") for line in lines):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.05)
        if not any(line.startswith("first=") for line in lines):
            raise RuntimeError("client did not complete its first request")
        # The NewSessionTicket follows the response as a separate flight; give
        # the client a moment to receive and cache it before its source is gone.
        time.sleep(0.75)
        first.stop()
        swapped = True
        second = Server([str(server_bin), str(CPP_REJECT_PORT), "1"], "cpp server (reject #2)")
        proc.wait(timeout=30)
        reader.join(timeout=5)
        out = "".join(lines)
        print(f"--- {label} ---")
        print(out, end="")
        ok = proc.returncode == 0 and "first=normal" in out and "second=rejected" in out
        print(f"{label}: {'PASS' if ok else 'FAIL'}")
        return ok
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        if second is not None:
            second.stop()
        if not swapped:
            first.stop()


def main() -> int:
    cpp_server_bin = BIN_DIR / "diff_0rtt_cpp_server"
    cpp_client_bin = BIN_DIR / "diff_0rtt_cpp_client"
    if not cpp_server_bin.exists() or not cpp_client_bin.exists():
        print(f"missing C++ binaries under {BIN_DIR}", file=sys.stderr)
        return 3
    go_server_bin, go_client_bin = build_go()

    servers = []
    try:
        servers.append(Server([str(cpp_server_bin), str(CPP_ENABLED_PORT), "1"], "cpp server (on)"))
        servers.append(Server([str(cpp_server_bin), str(CPP_DISABLED_PORT), "0"], "cpp server (off)"))
        servers.append(Server([str(go_server_bin), str(GO_ENABLED_PORT), "1"], "go server (on)"))
        servers.append(Server([str(go_server_bin), str(GO_DISABLED_PORT), "0"], "go server (off)"))

        def url(port: int) -> str:
            return f"https://127.0.0.1:{port}/0rtt"

        cases = (
            # 0-RTT enabled end to end, both directions.
            ("cpp-client -> cpp-server (on)", check_cpp_client, cpp_client_bin,
             url(CPP_ENABLED_PORT), True, True),
            ("cpp-client -> go-server (on)", check_cpp_client, cpp_client_bin,
             url(GO_ENABLED_PORT), True, True),
            ("go-client -> cpp-server (on)", check_go_client, go_client_bin,
             url(CPP_ENABLED_PORT), True, True),
            ("go-client -> go-server (on)", check_go_client, go_client_bin,
             url(GO_ENABLED_PORT), True, True),
            # Server disabled: the ticket carries no early-data limit, so the
            # client falls back to a normal 1-RTT request.
            ("cpp-client -> cpp-server (off)", check_cpp_client, cpp_client_bin,
             url(CPP_DISABLED_PORT), False, True),
            ("cpp-client -> go-server (off)", check_cpp_client, cpp_client_bin,
             url(GO_DISABLED_PORT), False, True),
            ("go-client -> cpp-server (off)", check_go_client, go_client_bin,
             url(CPP_DISABLED_PORT), False, True),
            # Client disabled: the server still accepts early data, but this
            # client never enables it.
            ("cpp-client (off) -> cpp-server (on)", check_cpp_client, cpp_client_bin,
             url(CPP_ENABLED_PORT), False, False),
            ("go-client (off) -> cpp-server (on)", check_go_client, go_client_bin,
             url(CPP_ENABLED_PORT), False, False),
        )
        failures = 0
        for case in cases:
            label, check, binary, target, expect_early, client_0rtt = case
            try:
                if not check(label, binary, target, expect_early, client_0rtt):
                    failures += 1
            except subprocess.TimeoutExpired:
                print(f"{label}: FAIL (timeout)")
                failures += 1
        try:
            if not check_cpp_rejection(cpp_server_bin, cpp_client_bin):
                failures += 1
        except (subprocess.TimeoutExpired, RuntimeError) as exc:
            print(f"cpp-client -> cpp-server (rejected ticket): FAIL ({exc})")
            failures += 1
    finally:
        for server in servers:
            server.stop()

    if failures:
        print(f"{failures} 0-RTT differential case(s) failed", file=sys.stderr)
        return 1
    print("all 0-RTT differential cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
