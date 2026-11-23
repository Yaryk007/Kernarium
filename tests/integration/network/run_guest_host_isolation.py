#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove that a guest process reaches nothing of the host it runs on.

The runner listens on the host's loopback and on the host's LAN address, TCP
and UDP, at ports the host picks. A guest process without privileges (uid
65534, as an Android application is) then connects and sends to the SLiRP
gateway address (which SLiRP used to map to the host's 127.0.0.1), to the
limited broadcast address, and to the host's own LAN address. Every attempt
must fail at once, with a refusal or an unreachable error and not a time-out,
and no host listener may have accepted a connection or received a datagram.

The runner first proves that its listeners record what reaches them by
connecting and sending to each itself, so an empty record means silence.

This runs the product launcher, which has no guest forward to a host
listener. A host without a LAN address leaves out the address-specific part
and says so.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import json
import pathlib
import socket
import sys
import threading
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402
from slirp_network import (  # noqa: E402
    start_guest_dhcp_client,
    verify_slirp_lease,
    wait_for_slirp_lease,
)


SCRIPT_PATH = "/opt/uml-test/host_isolation_probe.py"
RESULT_MARKER = "HOST-ISOLATION-RESULT "
# The longest an attempt may take: a refusal needs one round trip of the
# virtual link, so a time-out is a failure of the rule.
PROMPT_SECONDS = 3.0

GUEST_SCRIPT = r'''
import json, os, socket, sys, time
targets = json.loads(sys.argv[1])
os.setgroups([]); os.setgid(65534); os.setuid(65534)
IP_RECVERR = 11
results = []

def outcome(error):
    if isinstance(error, ConnectionRefusedError):
        return "refused"
    if isinstance(error, socket.timeout):
        return "timeout"
    code = getattr(error, "errno", None)
    if code in (101, 113, 111):  # ENETUNREACH, EHOSTUNREACH, ECONNREFUSED
        return "refused" if code == 111 else "unreachable"
    return "error:" + repr(error)

def tcp(name, address, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(%PROMPT%)
    start = time.monotonic()
    try:
        s.connect((address, port))
        s.sendall(b"hello from the guest")
        result = "connected"
    except OSError as error:
        result = outcome(error)
    finally:
        s.close()
    results.append({"name": name, "protocol": "tcp", "address": address,
                    "port": port, "outcome": result,
                    "seconds": round(time.monotonic() - start, 3)})

def udp(name, address, port, broadcast=False):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(%PROMPT%)
    # The error an ICMP unreachable reports is delivered to a socket that
    # asks for it, which is how an application sees a refused datagram.
    s.setsockopt(socket.IPPROTO_IP, IP_RECVERR, 1)
    if broadcast:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    start = time.monotonic()
    try:
        s.sendto(b"datagram from the guest", (address, port))
        s.recvfrom(2048)
        result = "answered"
    except OSError as error:
        result = outcome(error)
    finally:
        s.close()
    results.append({"name": name, "protocol": "udp", "address": address,
                    "port": port, "outcome": result,
                    "seconds": round(time.monotonic() - start, 3)})

for target in targets:
    if target["protocol"] == "tcp":
        tcp(target["name"], target["address"], target["port"])
    else:
        udp(target["name"], target["address"], target["port"],
            target.get("broadcast", False))
print("%MARKER%" + json.dumps(results))
'''


class Listeners:
    """TCP and UDP sockets on one address that record what reaches them."""

    def __init__(self, name: str, address: str) -> None:
        self.name = name
        self.address = address
        self.events: list[str] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._tcp.bind((address, 0))
        self._tcp.listen(8)
        self._tcp.settimeout(0.2)
        self.tcp_port = self._tcp.getsockname()[1]
        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._udp.bind((address, 0))
        self._udp.settimeout(0.2)
        self.udp_port = self._udp.getsockname()[1]
        self._threads = [
            threading.Thread(target=self._serve_tcp, daemon=True),
            threading.Thread(target=self._serve_udp, daemon=True),
        ]
        for thread in self._threads:
            thread.start()

    def _record(self, text: str) -> None:
        with self._lock:
            self.events.append(f"{self.name}: {text}")

    def _serve_tcp(self) -> None:
        while not self._stop.is_set():
            try:
                connection, peer = self._tcp.accept()
            except (TimeoutError, socket.timeout):
                continue
            except OSError:
                return
            self._record(f"tcp connection from {peer[0]}:{peer[1]}")
            connection.close()

    def _serve_udp(self) -> None:
        while not self._stop.is_set():
            try:
                data, peer = self._udp.recvfrom(2048)
            except (TimeoutError, socket.timeout):
                continue
            except OSError:
                return
            self._record(f"datagram from {peer[0]}:{peer[1]}: {data!r}")

    def drain(self) -> list[str]:
        with self._lock:
            events, self.events = self.events, []
        return events

    def close(self) -> None:
        self._stop.set()
        for thread in self._threads:
            thread.join(2)
        self._tcp.close()
        self._udp.close()


def self_check(listeners: Listeners) -> None:
    """The listeners record what reaches them, so silence means nothing did."""
    with socket.create_connection((listeners.address, listeners.tcp_port), 5):
        pass
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
        sender.sendto(b"self-check", (listeners.address, listeners.udp_port))
    deadline = time.monotonic() + 5
    while len(listeners.events) < 2 and time.monotonic() < deadline:
        time.sleep(0.05)
    events = listeners.drain()
    if len(events) != 2:
        raise RuntimeError(
            f"the {listeners.name} listeners did not record the probe's own "
            f"connection and datagram: {events}")


def lan_address() -> str | None:
    """The address the host's traffic leaves by, or None without a route."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            # A connected datagram socket sends nothing; it picks the source.
            probe.connect(("192.0.2.1", 9))
            address = probe.getsockname()[0]
        except OSError:
            return None
    if address.startswith(("127.", "0.", "169.254.")):
        return None
    return address


def targets_for(loopback: Listeners, lan: Listeners | None) -> list[dict]:
    targets = [
        {"name": "gateway tcp", "protocol": "tcp", "address": "10.0.2.2",
         "port": loopback.tcp_port},
        {"name": "gateway udp", "protocol": "udp", "address": "10.0.2.2",
         "port": loopback.udp_port},
        {"name": "broadcast udp", "protocol": "udp",
         "address": "255.255.255.255", "port": loopback.udp_port,
         "broadcast": True},
    ]
    if lan is not None:
        targets += [
            {"name": "host address tcp", "protocol": "tcp",
             "address": lan.address, "port": lan.tcp_port},
            {"name": "host address udp", "protocol": "udp",
             "address": lan.address, "port": lan.udp_port},
        ]
    return targets


def check(results: list[dict], targets: list[dict],
          events: list[str]) -> list[str]:
    failures = [f"host listener reached: {event}" for event in events]
    if len(results) != len(targets):
        failures.append(
            f"the guest reported {len(results)} of {len(targets)} attempts")
    for result in results:
        label = f"{result['name']} ({result['address']}:{result['port']})"
        if result["outcome"] not in ("refused", "unreachable"):
            failures.append(f"{label}: {result['outcome']}, not a refusal")
        elif result["seconds"] > PROMPT_SECONDS:
            failures.append(f"{label}: refused after {result['seconds']} s")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    address = lan_address()
    with contextlib.ExitStack() as stack:
        loopback = stack.enter_context(
            contextlib.closing(Listeners("loopback", "127.0.0.1")))
        self_check(loopback)
        lan = None
        if address is not None:
            lan = stack.enter_context(
                contextlib.closing(Listeners("host address", address)))
            self_check(lan)
        else:
            print("no LAN address on this host: its own address is left out")
        targets = targets_for(loopback, lan)
        script = GUEST_SCRIPT.replace("%MARKER%", RESULT_MARKER).replace(
            "%PROMPT%", repr(PROMPT_SECONDS))
        with Guest(
            arguments.emulator.resolve(),
            arguments.rootfs.resolve(),
            "1024M",
            "tmpfs",
            ("ncpus=2",),
            init_shell=True,
            payloads={SCRIPT_PATH: GuestFile(script.encode())},
        ) as guest:
            guest.boot_to_shell(180)
            start_guest_dhcp_client(guest)
            verify_slirp_lease(*wait_for_slirp_lease(guest))
            output = guest.check_command(
                f"python3 {SCRIPT_PATH} '{json.dumps(targets)}'", 120)
        marker = output.rfind(RESULT_MARKER)
        if marker < 0:
            raise RuntimeError(f"the guest reported no result:\n{output}")
        results = json.loads(
            output[marker + len(RESULT_MARKER):].splitlines()[0])
        # A listener's thread records an event a moment after it arrives.
        time.sleep(0.5)
        events = loopback.drain() + (lan.drain() if lan else [])
        print(json.dumps(results))
        failures = check(results, targets, events)
        if failures:
            raise RuntimeError(
                "the guest reached the host:\n  " + "\n  ".join(failures))
        print("the guest reached nothing of the host")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
