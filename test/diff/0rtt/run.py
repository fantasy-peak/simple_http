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

A dropped-data case runs the quic-go client through a UDP proxy that removes
every 0-RTT packet. The request is retransmitted at 1-RTT after the handshake,
so both the C++ and the Go server must report `normal`: `used_0rtt()` follows
the handshake state at header completion, not the fact that the connection
once had 0-RTT keys.
"""

from __future__ import annotations

import os
import select
import socket
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
CPP_DROP_PROXY_PORT = 7935
GO_DROP_PROXY_PORT = 7936


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


def _quic_varint(data: bytes, pos: int) -> tuple[int, int]:
    first = data[pos]
    if first < 0x40:
        return first, 1
    if first < 0x80:
        return int.from_bytes(data[pos:pos + 2], "big") & 0x3FFF, 2
    if first < 0xC0:
        return int.from_bytes(data[pos:pos + 4], "big") & 0x3FFFFFFF, 4
    return int.from_bytes(data[pos:pos + 8], "big") & 0x3FFFFFFFFFFFFFFF, 8


class Drops0RttProxy:
    """UDP proxy that drops QUIC v1 long-header type-1 (0-RTT) packets.

    Initial/Handshake/1-RTT traffic is forwarded, so the connection and the
    normal retransmission still reach the server; only the early data is lost.
    """

    VERSION_1 = 0x00000001

    def __init__(self, listen_port: int, upstream_port: int):
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.bind(("127.0.0.1", listen_port))
        self.upstream_addr = ("127.0.0.1", upstream_port)
        self.back_by_client: dict[tuple[str, int], socket.socket] = {}
        self.client_by_back: dict[socket.socket, tuple[str, int]] = {}
        self.dropped = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    @classmethod
    def _split(cls, datagram: bytes) -> list[tuple[int | None, bytes]]:
        """Splits coalesced long-header packets; an unparsed tail is kept whole
        with type None."""
        out: list[tuple[int | None, bytes]] = []
        off = 0
        while off < len(datagram):
            first = datagram[off]
            if not first & 0x80 or off + 5 > len(datagram):
                out.append((None, datagram[off:]))
                break
            if int.from_bytes(datagram[off + 1:off + 5], "big") != cls.VERSION_1:
                out.append((None, datagram[off:]))
                break
            pos = off + 5
            try:
                pos += 1 + datagram[pos]        # DCID length + DCID
                pos += 1 + datagram[pos]        # SCID length + SCID
                ptype = (first >> 4) & 0x03
                if ptype == 0:
                    token_len, n = _quic_varint(datagram, pos)
                    pos += n + token_len
                length, n = _quic_varint(datagram, pos)
                pos += n
            except IndexError:
                out.append((None, datagram[off:]))
                break
            end = pos + length
            if end > len(datagram):
                out.append((None, datagram[off:]))
                break
            out.append((ptype, datagram[off:end]))
            off = end
        return out

    def _run(self) -> None:
        while not self._stop.is_set():
            watch = [self.front, *self.back_by_client.values()]
            try:
                ready, _, _ = select.select(watch, [], [], 0.1)
            except (OSError, ValueError):
                return
            for sock in ready:
                if sock is self.front:
                    data, addr = sock.recvfrom(65535)
                    back = self.back_by_client.get(addr)
                    if back is None:
                        back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        back.bind(("127.0.0.1", 0))
                        self.back_by_client[addr] = back
                        self.client_by_back[back] = addr
                    kept = []
                    for ptype, packet in self._split(data):
                        if ptype == 1:
                            self.dropped += 1
                        else:
                            kept.append(packet)
                    if kept:
                        back.sendto(b"".join(kept), self.upstream_addr)
                else:
                    data, _ = sock.recvfrom(65535)
                    self.front.sendto(data, self.client_by_back[sock])

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2)
        self.front.close()
        for sock in self.back_by_client.values():
            sock.close()


def check_dropped_early(label: str, client: Path, upstream_port: int, proxy_port: int) -> bool:
    """The quic-go client's 0-RTT packets are all dropped; the request reaches
    the server after the handshake, so used_0rtt() must be false on both
    implementations."""
    proxy = Drops0RttProxy(proxy_port, upstream_port)
    try:
        result = run([str(client), f"https://127.0.0.1:{proxy_port}/0rtt", "0", "1"],
                     cwd=ROOT, timeout=60)
        dropped = proxy.dropped
    finally:
        proxy.stop()
    print(f"--- {label} ---")
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    ok = (result.returncode == 0 and "first=normal" in result.stdout and
          "second=normal" in result.stdout and dropped > 0)
    print(f"{label}: {'PASS' if ok else 'FAIL'} (dropped {dropped} 0-RTT packet(s))")
    return ok


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
        # Early packets lost in transit (retransmitted at 1-RTT) must look like
        # a plain request to C++ and quic-go alike.
        for label, upstream, proxy_port in (
            ("go-client -> cpp-server (0-RTT packets dropped)", CPP_ENABLED_PORT, CPP_DROP_PROXY_PORT),
            ("go-client -> go-server (0-RTT packets dropped)", GO_ENABLED_PORT, GO_DROP_PROXY_PORT),
        ):
            try:
                if not check_dropped_early(label, go_client_bin, upstream, proxy_port):
                    failures += 1
            except (subprocess.TimeoutExpired, OSError) as exc:
                print(f"{label}: FAIL ({exc})")
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
