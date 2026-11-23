#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The text-message runner's pure parts, without a guest.

run_android_sms.py plays the host application against a booted release: it frames requests, reads answers
and judges what the guest prints. The tests here run those parts: its encoder and framing against the golden
file that the service and the host's tests read too, its readers against texts in the layout the guest
prints, its judgements of a round of rows, its host and its adb-server client against fakes on loopback,
the catalogue of rows, and a parse of the probe sources (Android's classes are not on the host, so javac
reads them without resolving a name).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import base64
import contextlib
import importlib.util
import io
from pathlib import Path
import re
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_sms.py"
SPEC = importlib.util.spec_from_file_location("run_android_sms", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

PROBE = TESTS / "integration/android/sms"
ID = "00112233445566778899aabbccddeeff"
OTHER = "ffeeddccbbaa99887766554433221100"


def jdk_tool(name: str) -> str | None:
    path = shutil.which(name)
    if path is None:
        return None
    probe = subprocess.run([path, "-version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


class GoldenTests(unittest.TestCase):
    """The runner's wire against the bytes that the service and the host agree on."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.vectors = runner.read_golden()

    def names(self, prefix: str) -> list[str]:
        found = [name for name in self.vectors if name.startswith(prefix)]
        self.assertTrue(found, prefix)
        return found

    def test_requests_encode_to_the_golden_payloads(self) -> None:
        for name in self.names("request-"):
            with self.subTest(name):
                golden = runner.golden_request(self.vectors, name)
                self.assertEqual(
                    runner.request(golden.attempt, golden.identifier, golden.sender, golden.text), golden.payload)

    def test_frames(self) -> None:
        for name in self.names("frame-"):
            with self.subTest(name):
                payload, _, whole = self.vectors[name].partition("=>")
                self.assertEqual(runner.frame(bytes.fromhex(payload.strip())), bytes.fromhex(whole.strip()))

    def test_answers_read_back_and_write_the_same_bytes(self) -> None:
        expected = {
            "answer-handed-over-1": ("handed-over", "1"),
            "answer-handed-over-11": ("handed-over", "11"),
            "answer-handed-over-earlier-1": ("handed-over-earlier", "1"),
            "answer-handed-over-earlier-11": ("handed-over-earlier", "11"),
            "answer-not-handed-over-invalid": ("not-handed-over", "invalid"),
            "answer-not-handed-over-plan": ("not-handed-over", "plan"),
            "answer-not-handed-over-store": ("not-handed-over", "store"),
            "answer-uncertain-interrupted": ("uncertain", "interrupted"),
            "answer-uncertain-unknown": ("uncertain", "unknown"),
            "answer-uncertain-conflict": ("uncertain", "conflict"),
            "answer-unsettled-store": ("unsettled", "store"),
        }
        self.assertEqual(sorted(expected), sorted(n for n in self.names("answer-") if n != "answer-refused-version"))
        for name, (kind, detail) in expected.items():
            with self.subTest(name):
                payload = bytes.fromhex(self.vectors[name].partition("=>")[2].strip())
                answer = runner.parse_answer(payload)
                self.assertEqual((answer.kind, answer.identifier, answer.detail), (kind, ID, detail))
                self.assertEqual(answer.payload(), payload)
        refused = bytes.fromhex(self.vectors["answer-refused-version"].partition("=>")[2].strip())
        self.assertEqual(runner.parse_answer(refused).kind, "refused-version")
        self.assertEqual(runner.answer_bytes("refused-version"), refused)

    def test_the_invalid_answer_has_58_bytes(self) -> None:
        self.assertEqual(len(runner.answer_bytes("not-handed-over", ID, "invalid")), 58)

    def test_answers_that_the_contract_does_not_name(self) -> None:
        for payload in (b"", b"2.handed-over." + ID.encode() + b".0", b"2.handed-over." + ID.encode() + b".12",
                        b"2.handed-over." + ID.encode() + b".01", b"2.uncertain." + ID.encode() + b".other",
                        b"2.unsettled." + ID.encode() + b".plan", b"2.handed-over.x.1", b"3.handed-over." + ID.encode()
                        + b".1", b"2.handed-over." + ID.encode() + b".1.x", b"2.refused-version.x", b"\xff"):
            with self.subTest(payload):
                self.assertEqual(runner.parse_answer(payload).kind, "malformed")

    def test_answer_headers(self) -> None:
        self.assertEqual(runner.frame_length(bytes.fromhex(self.vectors["header-answer-4096"])), 4096)
        self.assertEqual(runner.frame_length(bytes.fromhex(self.vectors["frame-answer-zero"].partition("=>")[2].strip())), 0)
        for name in ("reject-magic", "reject-version", "reject-version-0", "reject-answer-length-4097"):
            with self.subTest(name):
                self.assertIsNone(runner.frame_length(bytes.fromhex(self.vectors[name])))
        self.assertIsNone(runner.frame_length(b"KSMS\x01\x00"))

    def test_the_golden_request_the_rows_use_is_the_first_number(self) -> None:
        golden = runner.golden_request(self.vectors, "request-first-number")
        self.assertEqual((golden.attempt, golden.identifier), ("first", ID))
        self.assertEqual(golden.sender, b"+15551234567")
        self.assertEqual(golden.text, b"hello")

    def test_payload_vectors_for_the_rows_that_send_what_is_not_a_request(self) -> None:
        for name in self.names("refuse-"):
            payload = runner.golden_payload(self.vectors, name)
            self.assertLessEqual(len(payload), runner.MAXIMUM_REQUEST)
        for name in self.names("version-"):
            self.assertTrue(runner.golden_payload(self.vectors, name))

    def test_fresh_identifiers(self) -> None:
        self.assertRegex(runner.new_identifier(), r"[0-9a-f]{32}")
        self.assertNotEqual(runner.new_identifier(), runner.new_identifier())
        self.assertTrue(runner.new_identifier(4001).startswith("00000fa1"))
        with self.assertRaises(ValueError):
            runner.request("again", ID, "a", "b")


class RecordTests(unittest.TestCase):
    """The record file that the proof rows write and read, in the service's format."""

    def test_digest_vectors_of_the_service(self) -> None:
        for sender, text, wanted in (
                ("Bank", "hello", "35dfc8aa85463b0b36567dc603805cfb487b5855dffbbaf6cf70ef0ff9aa4285"),
                ("+15551234567", "café \U0001F600", "4c4d991afaa947c78c569baf4b8f938e9b8608710f68d32e5bf5356d63d540db"),
                ("", "", "d3baf49451436e834d2c79fff543d2f3e0fe317a552d6919b2a4721e91ceb68a")):
            self.assertEqual(runner.digest(sender, text), wanted)
        self.assertNotEqual(runner.digest("a", "bc"), runner.digest("ab", "c"))

    def test_round_trip(self) -> None:
        entries = [("p", ID, runner.digest("a", "b"), 1), ("h", OTHER, runner.digest("c", "d"), 11),
                   ("u", "0" * 32, runner.digest("e", "f"), 0)]
        data = runner.make_record(entries)
        self.assertTrue(data.startswith(b"kernarium-sms-record 1\n"))
        self.assertRegex(data.decode(), r"end [0-9a-f]{8}\n$")
        self.assertEqual(runner.parse_record(data), entries)

    def test_the_largest_record_is_the_documented_size(self) -> None:
        entries = [("u", f"{i:032x}", runner.digest("a", str(i)), 11) for i in range(runner.RECORD_CAPACITY)]
        self.assertEqual(len(runner.make_record(entries)), runner.MAXIMUM_RECORD_BYTES)
        self.assertEqual(runner.MAXIMUM_RECORD_BYTES, 26404)
        with self.assertRaises(runner.Failure):
            runner.parse_record(runner.make_record(entries + [("u", ID, runner.digest("a", "b"), 1)]))
        # Short lines: 257 of them fit the size, so only the count refuses them.
        short = [("u", f"{i:032x}", runner.digest("a", str(i)), 1) for i in range(runner.RECORD_CAPACITY + 1)]
        self.assertLess(len(runner.make_record(short)), runner.MAXIMUM_RECORD_BYTES)
        with self.assertRaises(runner.Failure) as refused:
            runner.parse_record(runner.make_record(short))
        self.assertIn("257 entries", str(refused.exception))

    def test_a_record_the_service_would_refuse(self) -> None:
        good = runner.make_record([("p", ID, runner.digest("a", "b"), 1)])
        for label, data in (("a flipped byte", good.replace(b"p " + ID.encode(), b"h " + ID.encode())),
                            ("no trailer", good[:-13]), ("another header", good.replace(b" 1\n", b" 2\n", 1)),
                            ("a bad line", runner.make_record([("p", ID, "x", 1)]))):
            with self.subTest(label):
                with self.assertRaises(runner.Failure):
                    runner.parse_record(data)


LOG = """\
11-16 12:00:00.100  1000  1100 I AndariumSms: listening on vsock port 5560
11-16 12:00:01.000  1000  1200 I AndariumSms: planned 2 step(s) for a message of 1 part(s), to com.android.messaging
11-16 12:00:01.001  1000  1200 I AndariumSms: step 1 of 00112233
11-16 12:00:01.002  1000  1200 I AndariumSms: step 2 of 00112233
11-16 12:00:02.000  1000  1200 I AndariumSms: step 1 of ffeeddcc
11-16 12:00:02.001  1000  1200 I AndariumSms: step 1 of ffeeddcc failed: SecurityException
11-16 12:00:03.000  1000  1200 I AndariumSms: step 1 of 00112233
11-16 12:00:04.000  1000  1200 W AndariumSms: step 1 of 99999999
"""

EPOCH = """\
1731758405.250  1000  1100 I AndariumSms: record in /data/user_de/0/android.kernarium.sms/files, labelled u:object_r:kernarium_sms_data_file:s0
1731758405.300  1000  1100 I AndariumSms: serving the host's requests
"""

# The holder's own tag, from boot: the line that says it holds the port.
HOLDER_EPOCH = """\
1731758405.300   700   700 I kernarium.sms.listener: listening on vsock port 5560
1731758406.000   900   900 I OtherTag: listening on vsock port 5560
"""

EVENTS = """\
1731758420.100  1000  1000 I boot_progress_enable_screen: 15000
"""

# What init writes to the launcher's console when it starts the holder: from boot, and kept for the whole run.
CONSOLE = """\
[    0.640284][    T1] init: Parsing file /system_ext/etc/init/kernarium_sms_listener.rc...
[    6.674025][    T1] init: starting service 'kernarium_sms_listener'...
[    6.674353][    T1] init: Created socket '/dev/socket/kernarium_sms', mode 660, user 1000, group 1000
[    6.684367][    T1] init: ... started service 'kernarium_sms_listener' has pid 77
"""
STATUS = "Name:\tkernarium_sms_l\nUid:\t9999\t9999\t9999\t9999\nGid:\t9999\t9999\t9999\t9999\nGroups:\t\n"


class ReaderTests(unittest.TestCase):
    def test_step_lines(self) -> None:
        self.assertEqual(runner.step_lines(LOG, "00112233" + "0" * 24), 2)
        self.assertEqual(runner.step_lines(LOG, "00112233" + "0" * 24, 2), 1)
        # A failed step is not a step taken, and neither is a line of another priority.
        self.assertEqual(runner.step_lines(LOG, "ffeeddcc" + "0" * 24), 1)
        self.assertEqual(runner.step_lines(LOG, "99999999" + "0" * 24), 0)
        self.assertEqual(len(runner.STEP_LINE.findall(LOG)), 4)

    def test_epoch_lines(self) -> None:
        self.assertEqual([when for when, _ in runner.epoch_lines(EPOCH, "AndariumSms")], [1731758405.25, 1731758405.3])

    def test_the_holders_start_is_read_from_the_console(self) -> None:
        """The holder's own log line ages out of the log ring; init's console lines stay for the whole run."""
        starts = runner.holder_starts(CONSOLE)
        self.assertEqual((starts.seconds, starts.pids, starts.socket_created), ((6.674025,), (77,), True))
        self.assertEqual(runner.holder_boot_problems(CONSOLE, 77), [])
        self.assertEqual(runner.holder_boot_problems(CONSOLE, None), [], "the pid is compared only when it is known")
        self.assertIn("no init line that started the holder", runner.holder_boot_problems("", 77)[0])
        self.assertIn("pid 78, init started pid 77", runner.holder_boot_problems(CONSOLE, 78)[0])
        twice = CONSOLE + CONSOLE.replace("pid 77", "pid 91")
        self.assertIn("started the holder 2 times (pids [77, 91])", runner.holder_boot_problems(twice, 91)[0])
        self.assertIn("handoff socket with mode 660", runner.holder_boot_problems(
            "\n".join(line for line in CONSOLE.splitlines() if "Created socket" not in line), 77)[0])
        self.assertIn("handoff socket", runner.holder_boot_problems(CONSOLE.replace("mode 660", "mode 666"), 77)[0])
        self.assertIn("handoff socket", runner.holder_boot_problems(CONSOLE.replace("user 1000", "user 2000"), 77)[0])

    def test_the_boot_order_compares_uptimes(self) -> None:
        # H7 keys on the holder, which init starts with the boot; the event's message is the milliseconds since boot.
        self.assertIsNone(runner.check_boot_order(CONSOLE, EVENTS))
        late = CONSOLE.replace("6.674025", "16.500000")
        self.assertIn("after the boot finished at 15.000 s", runner.check_boot_order(late, EVENTS))
        self.assertIn("no init line that started the holder", runner.check_boot_order("", EVENTS))
        self.assertIn("no boot_progress_enable_screen", runner.check_boot_order(CONSOLE, ""))

    def test_the_holders_status(self) -> None:
        self.assertEqual(runner.holder_status_problems(STATUS), [])
        self.assertEqual(runner.holder_status_problems(STATUS.replace("Groups:\t", "Groups:\t9999")), [])
        self.assertIn("group ids are ['0', '0', '0', '0'], not nobody (9999)",
                      runner.holder_status_problems(STATUS.replace("Gid:\t9999\t9999\t9999\t9999", "Gid:\t0\t0\t0\t0"))[0])
        self.assertIn("user ids are", runner.holder_status_problems(STATUS.replace("Uid:\t9999", "Uid:\t1000", 1))[0])
        self.assertIn("supplementary groups ['3003']", runner.holder_status_problems(STATUS.replace("Groups:\t", "Groups:\t3003"))[0])
        self.assertIn("no Gid line", runner.holder_status_problems("Uid:\t9999\t9999\t9999\t9999\n")[0])

    def test_the_record_place(self) -> None:
        self.assertIsNone(runner.check_record_location(EPOCH))
        wrong = EPOCH.replace("kernarium_sms_data_file", "system_app_data_file")
        self.assertIn("not /data/user_de/0/android.kernarium.sms/files labelled", runner.check_record_location(wrong))
        self.assertIn("no 'record in'", runner.check_record_location(""))

    PS = """\
LABEL                          USER           PID   PPID     VSZ    RSS WCHAN            ADDR S NAME
u:r:init:s0                    root             1      0   10000   1000 0                   0 S init
u:r:adbd:s0                    shell          300      1   10000   1000 0                   0 S adbd
u:r:traced:s0                  traced         310      1   10000   1000 0                   0 S traced
u:r:kernarium_gamepad:s0       nobody         320      1   10000   1000 0                   0 S kernarium_gamepad
u:r:hal_thermal_default:s0     system         330      1   10000   1000 0                   0 S thermal
u:r:kernarium_sms_listener:s0  nobody         340      1   10000   1000 0                   0 S kernarium_sms_listener
u:r:system_app:s0              system         950    300   10000   1000 0                   0 S com.android.settings
"""

    # The service runs only while a session does: its line is in the listing then, and not at idle.
    SERVICE = "u:r:kernarium_sms:s0           system         900    300   10000   1000 0                   0 S android.kernarium.sms\n"

    def test_the_listener_inventory(self) -> None:
        self.assertEqual([p.name for p in runner.parse_ps(self.PS)][-2:], ["kernarium_sms_listener", "com.android.settings"])
        # The holder is required; the service is allowed and not required: at idle there is no process of it.
        self.assertEqual(runner.inventory_problems(self.PS), [])
        self.assertEqual(runner.inventory_problems(self.PS + self.SERVICE), [])
        extra = self.PS + "u:r:virtualizationservice:s0   system   960  1  1 1 0 0 S virtmgr\n"
        self.assertIn("virtmgr (pid 960) runs in virtualizationservice, which may use vsock",
                      runner.inventory_problems(extra))
        self.assertIn("a crosvm process runs (pid 961)",
                      runner.inventory_problems(self.PS + "u:r:crosvm:s0  system  961  1  1 1 0 0 S crosvm\n"))
        self.assertIn("no process runs in kernarium_sms_listener",
                      runner.inventory_problems(self.PS.replace("kernarium_sms_listener:s0", "system_app:s0")))
        # The service may not create a vsock socket any more, so a domain list that names it is not the policy's.
        self.assertNotIn("kernarium_sms", runner.VSOCK_TYPES)
        self.assertIn("kernarium_sms_listener", runner.VSOCK_TYPES & runner.RUNNING_VSOCK_TYPES)

    def test_the_port_inventory(self) -> None:
        self.assertEqual(runner.port_problems(set(range(5555, 5561))), [])
        self.assertEqual(runner.port_problems(set(range(5555, 5561)) | {5599}),
                         ["vsock port 5599 answered and is not one of the guest's"])
        self.assertEqual(runner.port_problems(set(range(5555, 5560))), ["vsock port 5560 did not answer"])

    def test_adb_server_replies(self) -> None:
        self.assertEqual(runner.smart_request("host:transport:x"), b"0010host:transport:x")
        self.assertEqual(runner.parse_smart_reply(b"OKAY"), ("OKAY", ""))
        self.assertEqual(runner.parse_smart_reply(b"FAIL0004oops"), ("FAIL", "oops"))
        self.assertEqual(runner.parse_smart_reply(b"OK"), ("SHORT", ""))

    def test_dumps(self) -> None:
        self.assertEqual(runner.parse_total_pss("  Native Heap PSS: 7\n           TOTAL PSS:    18432            TOTAL RSS:    40960"),
                         18432)
        self.assertEqual(runner.parse_total_pss("        TOTAL    18432    1000"), 18432)
        self.assertIsNone(runner.parse_total_pss("nothing"))
        inbox = ("Row: 0 address=+15551234567, body=hello\nRow: 1 address=Bank, body=hello\n"
                 "Row: 2 address=+15551234567, body=hello\n")
        self.assertEqual(runner.inbox_rows(inbox, "+15551234567", "hello"), 2)
        self.assertEqual(runner.inbox_rows(inbox, "+15551234567", "hell"), 0)
        received = "K1Yy aGk=\n"
        self.assertEqual(runner.parse_received(received), [("+V2", "hi")])
        status = "Name:\tadbd\nCapEff:\t0000000000000400\n"
        self.assertTrue(runner.cap_eff_bit(status, 10))
        self.assertFalse(runner.cap_eff_bit(status, 9))
        self.assertIsNone(runner.cap_eff_bit("Name:\tadbd\n", 10))

    def test_the_phone_process_through_a_message(self) -> None:
        self.assertIsNone(runner.phone_problem("1878\n", "1878\r\n", 0, 0))
        self.assertIsNone(runner.phone_problem("1878", "1878", 2, 2))
        self.assertIn("not running before", runner.phone_problem("", "1878", 0, 0))
        self.assertIn("was pid 1878 before the message and is 2041 after it",
                      runner.phone_problem("1878", "2041", 0, 0))
        self.assertIn("is gone after it", runner.phone_problem("1878", "", 0, 0))
        self.assertIn("gained 1 line(s)", runner.phone_problem("1878", "1878", 0, 1))
        crash = ("11-18 20:10:40 E AndroidRuntime: FATAL EXCEPTION: main\n"
                 "11-18 20:10:40 E AndroidRuntime: Process: com.android.phone, PID: 9\n"
                 "11-18 20:10:41 E AndroidRuntime: Process: com.example.other, PID: 10\n")
        self.assertEqual(runner.crash_lines_naming(crash, runner.PHONE), 1)
        self.assertEqual(runner.crash_lines_naming("", runner.PHONE), 0)

    def test_the_kernel_configuration(self) -> None:
        accepted = "CONFIG_VSOCKETS=y\nCONFIG_VIRTIO_VSOCKETS=y\n# CONFIG_VHOST_VSOCK is not set\nCONFIG_OTHER=y\n"
        self.assertEqual(runner.kernel_config_problems(accepted + "CONFIG_X=y\n", accepted), [])
        self.assertEqual(runner.kernel_config_problems("CONFIG_VSOCKETS=y\n# CONFIG_VHOST_VSOCK is not set\n", accepted),
                         ["the kernel lacks 'CONFIG_VIRTIO_VSOCKETS=y'"])
        self.assertTrue(runner.kernel_config_problems(accepted + "CONFIG_VHOST_VSOCK=m\n", accepted)[0]
                        .startswith("the kernel has 'CONFIG_VHOST_VSOCK=m'"))


def answer(kind: str, detail: str = "") -> object:
    return runner.Answer(kind, ID, detail)


def hidden_line(pid: str) -> str:
    return (f"11-18 20:09:43.700  {pid}  {pid} W d.kernarium.sms: Accessing hidden method "
            "Landroid/system/Os;->accept(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)"
            "Ljava/io/FileDescriptor; (unsupported, reflection, denied)\n")


class JudgementTests(unittest.TestCase):
    """What a round of rows may and may not show."""

    def test_never_twice_under_kills(self) -> None:
        problem = runner.retry_problem
        handed, earlier = answer("handed-over", "1"), answer("handed-over-earlier", "1")
        self.assertIsNone(problem(handed, earlier, 1))
        self.assertIsNone(problem(None, earlier, 1))
        self.assertIsNone(problem(None, answer("uncertain", "unknown"), 0))
        self.assertIsNone(problem(None, answer("uncertain", "interrupted"), 0))
        self.assertIsNone(problem(None, answer("uncertain", "interrupted"), 1))
        self.assertIsNone(problem(None, answer("not-handed-over", "plan"), 0))
        self.assertIsNone(problem(None, answer("unsettled", "store"), 1))
        self.assertIn("2 hand-overs", problem(handed, earlier, 2))
        self.assertIn("a retry handed", problem(None, answer("handed-over", "1"), 1))
        self.assertIn("handed-over-earlier with 0", problem(None, earlier, 0))
        self.assertIn("uncertain (unknown) with 1", problem(None, answer("uncertain", "unknown"), 1))
        self.assertIn("answered handed-over with 0", problem(handed, earlier, 0))
        self.assertIn("no answer", problem(handed, None, 1))
        self.assertIn("conflict", problem(None, answer("uncertain", "conflict"), 0))
        self.assertIn("not-handed-over with 1", problem(None, answer("not-handed-over", "store"), 1))

    def test_a_restart_gives_no_wrong_answer(self) -> None:
        def made(kind: str, detail: str = "1", identifier: str = ID) -> object:
            return runner.Exchange("answered", runner.answer_bytes(kind, identifier, detail))

        gone = runner.Exchange("no-answer")
        refused = runner.Exchange("connect-failed")
        good = [made("handed-over"), gone, refused, made("handed-over-earlier"), made("handed-over-earlier")]
        self.assertIsNone(runner.restart_problem(good, ID))
        self.assertIn("after the pid changed", runner.restart_problem([made("handed-over"), gone], ID))
        self.assertIn("wrong answer", runner.restart_problem(good + [made("uncertain", "unknown")], ID))
        self.assertIn("another identifier", runner.restart_problem(good + [made("handed-over-earlier", "1", OTHER)], ID))
        self.assertIn("ended not-taken", runner.restart_problem(good + [runner.Exchange("not-taken")], ID))
        self.assertIn("after the pid changed", runner.restart_problem([], ID))

    TRACE = """\
1500 renameat(AT_FDCWD, "/data/user_de/0/android.kernarium.sms/files/sms-delivery-record.new", AT_FDCWD, "/data/user_de/0/android.kernarium.sms/files/sms-delivery-record") = 0
1500 fsync(7)                          = 0
1500 writev(5, [{iov_base="\\1\\0\\0\\0AndariumSms\\0step 1 of 00112233\\0", iov_len=33}], 1) = 33
1500 ioctl(12, BINDER_WRITE_READ, {write_size=64}) = 0
"""

    def test_the_order_of_the_writes(self) -> None:
        self.assertIsNone(runner.strace_problem(self.TRACE, ID))
        lines = self.TRACE.splitlines()
        self.assertIn("no rename", runner.strace_problem("\n".join(lines[1:]), ID))
        # The file's own fsync before the rename is not the directory's after it.
        self.assertIn("no fsync after", runner.strace_problem("\n".join([lines[1], lines[0], lines[2], lines[3]]), ID))
        self.assertIn("no write of 'step 1 of 00112233' after", runner.strace_problem(
            "\n".join([lines[0], lines[2], lines[1], lines[3]]), ID))
        self.assertIn("no binder call after", runner.strace_problem("\n".join([lines[0], lines[1], lines[3], lines[2]]), ID))


class CatalogueTests(unittest.TestCase):
    def test_every_row_has_a_check_and_every_check_a_row(self) -> None:
        self.assertEqual(sorted(row.name for row in runner.ROWS), sorted(runner.ROW_FUNCTIONS))
        self.assertEqual(len({row.name for row in runner.ROWS}), len(runner.ROWS))
        for row in runner.ROWS:
            self.assertIn(row.mode, ("delivery", "channel", "record", "proof"))
            self.assertTrue(row.needs is None or row.needs in runner.NEEDS, row.name)

    def test_the_designs_rows_are_here_or_named_elsewhere(self) -> None:
        names = {row.name for row in runner.ROWS}
        covered = {c for row in runner.ROWS for c in row.covers}
        for wanted in ("D1", "D2", "G1", "G2a", "G2b", "G2c", "G2d", "G2e", "G3", "G4", "G5", "G6", "G7", "G8", "G9",
                       "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8", "N1", "N2", "N3", "N4", "N5", "N6"):
            self.assertIn(wanted, names)
        for wanted in ("H1", "H2", "H3", "H4", "H5", "H7", "H8", "H10", "H11", "H13"):
            self.assertIn(wanted, covered)
        self.assertEqual(names & {"H6", "H9"}, {"H6", "H9"})
        elsewhere = " ".join(names for names, _ in runner.ELSEWHERE)
        for wanted in ("B1", "H12", "U6"):
            self.assertIn(wanted, elsewhere)

    def test_the_delivery_mode_needs_no_probe_and_no_extras(self) -> None:
        run, skipped = runner.plan_rows("delivery", set())
        # N1 runs before anything is sent: a message starts the service, and a ping would too.
        self.assertEqual([row.name for row in run], ["N1", "D1", "D2", "D3", "N2", "N3", "N4", "N6"])
        self.assertEqual(skipped, [])
        options = runner.build_parser().parse_args(["--mode", "delivery", "--proof-image", "--release", "x"])
        self.assertTrue(options.proof_image)
        self.assertEqual(runner.available_needs(options), {"stock-adbd"})
        self.assertFalse(runner.build_parser().parse_args([]).proof_image)

    def test_what_a_run_without_the_extras_skips(self) -> None:
        run, skipped = runner.plan_rows("channel", {"stock-adbd"})
        self.assertEqual([row.name for row, _ in skipped], ["G2b", "G2c", "G2d", "G2e", "G7", "H9"])
        # The rows that no run of this guest can make are reported as not shown, with the reason, not as not run.
        reasons = {row.name: reason for row, reason in skipped}
        for name in ("G2b", "G2c", "G2d"):
            self.assertTrue(reasons[name].startswith("NOT SHOWN"), reasons[name])
            self.assertIn(name, [row.name for row in runner.ROWS if row.needs in runner.NOT_SHOWN_NEEDS])
        self.assertFalse(reasons["G7"].startswith("NOT SHOWN"))
        self.assertEqual([row.name for row in run][:2], ["N1", "G1"], "N1 comes before the first request")
        self.assertIn("N5", [row.name for row in run])
        self.assertNotIn("N3", [row.name for row in run])
        self.assertIn("G9", [row.name for row in run])
        self.assertIn("H6", [row.name for row in run])
        run, skipped = runner.plan_rows("channel", {"adbd-topic", "kernel-config", "g7"})
        self.assertEqual([row.name for row, _ in skipped], ["G2b", "G2c", "G2d", "H6"])
        run, skipped = runner.plan_rows("channel", {"adbd-topic", "kernel-config", "g7", "vsock-forward"})
        self.assertEqual([row.name for row, _ in skipped], ["H6"])
        run, skipped = runner.plan_rows("record", set())
        self.assertEqual([row.name for row, _ in skipped], ["I7"])
        run, skipped = runner.plan_rows("proof", set())
        self.assertEqual([row.name for row in run], [])
        self.assertEqual({row.name for row, _ in skipped}, {"I3", "I6", "I9"})
        run, _ = runner.plan_rows("proof", {"proof"})
        self.assertEqual([row.name for row in run], ["I3", "I6", "I9"])

    def test_the_option_flags_make_the_rows_available(self) -> None:
        options = runner.build_parser().parse_args(["--adbd-topic", "--g7", "--variant", "no-provider"])
        self.assertEqual(runner.available_needs(options), {"adbd-topic", "g7", "no-provider"})
        options = runner.build_parser().parse_args(["--adbd-vsock-forward"])
        self.assertEqual(runner.available_needs(options), {"stock-adbd", "vsock-forward"})
        options = runner.build_parser().parse_args(["--mode", "proof"])
        self.assertEqual(runner.available_needs(options), {"stock-adbd", "proof"})

    def test_the_row_listing_runs(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            runner.print_rows()
        self.assertIn("G2e", output.getvalue())
        self.assertIn("adbd topic", output.getvalue())
        self.assertIn("B1", output.getvalue())


class FakeService:
    """The service's end of a forward, on loopback: it reads a frame and does what the script says."""

    def __init__(self, script) -> None:
        self.script = script
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen()
        self.port = self.server.getsockname()[1]
        self.received: list[bytes] = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self) -> None:
        while True:
            try:
                connection, _ = self.server.accept()
            except OSError:
                return
            with connection:
                connection.settimeout(5)
                try:
                    header = connection.recv(8, socket.MSG_WAITALL)
                    length = struct.unpack("<HH", header[4:])[1] if len(header) == 8 else 0
                    payload = connection.recv(length, socket.MSG_WAITALL) if length else b""
                except OSError:
                    continue
                self.received.append(header + payload)
                reply = self.script(payload)
                if reply is not None:
                    with contextlib.suppress(OSError):
                        connection.sendall(reply)

    def close(self) -> None:
        self.server.close()


class HostTests(unittest.TestCase):
    def test_outcomes(self) -> None:
        answers = {
            b"first": runner.frame(runner.answer_bytes("handed-over", ID, "1")),
            b"empty": runner.frame(b""),
            b"badmagic": b"KSMT\x01\x00\x05\x00hello",
            b"badversion": b"KSMS\x02\x00\x00\x00",
            b"toolong": b"KSMS\x01\x00\x01\x10",
            b"short": runner.frame(b"hello")[:-2],
            b"headeronly": b"KSM",
            b"close": None,
        }
        service = FakeService(lambda payload: answers[payload])
        self.addCleanup(service.close)
        host = runner.Host(service.port)
        exchange = host.exchange(b"first", seconds=3)
        self.assertEqual((exchange.outcome, runner.parse_answer(exchange.payload).kind), ("answered", "handed-over"))
        self.assertEqual(host.answer(b"first", 3).identifier, ID)
        self.assertEqual(host.exchange(b"empty", seconds=3).outcome, "not-taken")
        for name in ("badmagic", "badversion", "toolong"):
            self.assertEqual(host.exchange(name.encode(), seconds=3).outcome, "bad-header", name)
        for name in ("short", "headeronly", "close"):
            self.assertEqual(host.exchange(name.encode(), seconds=1).outcome, "no-answer", name)
        self.assertEqual(service.received[0], runner.frame(b"first"))
        with self.assertRaises(runner.Failure):
            host.answer(b"close", 1)
        service.close()
        self.assertEqual(runner.Host(service.port).exchange(b"first", seconds=1).outcome, "connect-failed")

    def test_a_refused_stream_ends_with_no_byte(self) -> None:
        service = FakeService(lambda payload: None)
        self.addCleanup(service.close)
        received, closed, elapsed = runner.raw_stream(service.port, runner.frame(b"x"), 3)
        self.assertEqual((received, closed), (b"", True))
        self.assertLess(elapsed, 3)
        talker = FakeService(lambda payload: runner.frame(b"y"))
        self.addCleanup(talker.close)
        received, closed, _ = runner.raw_stream(talker.port, runner.frame(b"x"), 1)
        self.assertEqual(received, runner.frame(b"y"))
        silent = socket.socket()
        silent.bind(("127.0.0.1", 0))
        silent.listen()
        self.addCleanup(silent.close)
        received, closed, elapsed = runner.raw_stream(silent.getsockname()[1], b"x", 1)
        self.assertEqual((received, closed), (b"", False))
        self.assertGreaterEqual(elapsed, 0.9)
        dead = socket.socket()
        dead.bind(("127.0.0.1", 0))
        port = dead.getsockname()[1]
        dead.close()
        with self.assertRaises(runner.Failure):
            runner.raw_stream(port, b"x", 1)


class AdbServerTests(unittest.TestCase):
    """The client of the adb server's own protocol, against a fake that answers as the server does."""

    def serve(self, answers: dict[str, bytes]) -> int:
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen()
        self.addCleanup(server.close)

        def run() -> None:
            while True:
                try:
                    connection, _ = server.accept()
                except OSError:
                    return
                with connection:
                    connection.settimeout(3)
                    try:
                        while True:
                            length = connection.recv(4, socket.MSG_WAITALL)
                            if len(length) < 4:
                                break
                            service = connection.recv(int(length, 16), socket.MSG_WAITALL).decode()
                            reply = answers.get(service, b"FAIL0004nope")
                            if reply == b"HANG":
                                connection.recv(1)
                                break
                            connection.sendall(reply)
                    except OSError:
                        pass

        threading.Thread(target=run, daemon=True).start()
        return server.getsockname()[1]

    def test_okay_fail_and_silence(self) -> None:
        port = self.serve({"host:transport:dev:1": b"OKAY", "vsock:3:5555": b"OKAY", "vsock:3:6000": b"HANG",
                           "vsock:3:7000": b"FAIL0007refused"})

        def ask(serial: str, vsock_port: int, seconds: float = 2) -> str:
            return runner.open_vsock_through_adbd(serial, vsock_port, seconds, port)

        self.assertEqual(ask("dev:1", 5555), "OKAY")
        self.assertEqual(ask("dev:1", 7000), "FAIL")
        self.assertEqual(ask("dev:1", 5556), "FAIL")
        self.assertEqual(ask("dev:1", 6000, 1), "TIMEOUT")
        self.assertEqual(ask("other", 5555), "FAIL")


class StartFailureTests(unittest.TestCase):
    """start_guest owns the emulator it spawned until it returns: whatever fails after the spawn stops it."""

    MARKER = "sms-start-failure-test"

    def start_and_fail(self, forward: object = 12345, proof_image: bool = False, succeed: bool = False,
                       **patched: object) -> list[subprocess.Popen]:
        """Runs start_guest with the launcher's steps replaced; returns the spawned children left running.
        A proof image has no installed-release identity: nothing of it may be read, planned or recorded. With
        ``succeed`` the boot completes, start_guest returns a Guest, and ``self.mocks`` holds what it called."""
        boot = runner.boot_android
        children: list[subprocess.Popen] = []
        self.mocks: dict[str, mock.Mock] = {}
        real_popen = subprocess.Popen

        def spawn(command, *args, **kwargs):
            process = real_popen(command, *args, **kwargs)
            if self.MARKER in " ".join(map(str, command)):
                children.append(process)
            return process

        options = SimpleNamespace(instance=None, release=None, emulator=None, adb_port=5599, cpus=2,
                                  memory="2G", boot_timeout=5, proof_image=proof_image)
        steps = {
            "wait_for_adb": mock.DEFAULT,
            "wait_for_boot_completed": True,
            "provision_guest": mock.DEFAULT,
        }
        steps.update(patched)
        with tempfile.TemporaryDirectory(prefix="uml-sms-t-") as private, contextlib.ExitStack() as stack:
            directory = Path(private)
            stack.enter_context(mock.patch.object(boot, "parse_arguments", return_value=SimpleNamespace(
                emulator="fake", release="fake")))
            stack.enter_context(mock.patch.object(boot, "resolve_executable", return_value=sys.executable))
            stack.enter_context(mock.patch.object(boot, "release_path", return_value=directory))
            unread = AssertionError("a proof image's identity was read")
            for name, owner in (("read_release", boot.android_instance), ("plan_start", boot),
                                ("apply", boot.android_instance), ("booted", boot.android_instance),
                                ("data_capacity", boot)):
                if proof_image:
                    self.mocks[name] = stack.enter_context(mock.patch.object(owner, name, side_effect=unread))
                else:
                    self.mocks[name] = stack.enter_context(mock.patch.object(
                        owner, name, return_value=1 if name == "data_capacity" else object()))
            stack.enter_context(mock.patch.object(boot, "disconnect_adb"))
            self.mocks["emulator_command"] = stack.enter_context(mock.patch.object(
                boot, "emulator_command", return_value=[
                    sys.executable, "-c", f"import time; time.sleep(120)  # {self.MARKER}"]))
            if isinstance(forward, BaseException):
                stack.enter_context(mock.patch.object(runner, "vsock_forward_port", side_effect=forward))
            else:
                stack.enter_context(mock.patch.object(runner, "vsock_forward_port", return_value=forward))
            for name, effect in steps.items():
                if isinstance(effect, BaseException):
                    stack.enter_context(mock.patch.object(boot, name, side_effect=effect))
                elif effect is mock.DEFAULT:
                    stack.enter_context(mock.patch.object(boot, name))
                else:
                    stack.enter_context(mock.patch.object(boot, name, return_value=effect))
            stack.enter_context(mock.patch.object(runner.subprocess, "Popen", side_effect=spawn))
            try:
                if succeed:
                    with mock.patch.object(runner.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
                        # (the settle after the boot is not waited for)
                        self.assertIsInstance(runner.start_guest(options, "adb", directory), runner.Guest)
                    self.instance_made = (directory / "instance").is_dir()
                else:
                    runner.start_guest(options, "adb", directory)
            except (runner.Failure, runner.boot_android.LauncherError):
                if succeed:
                    raise
            else:
                if not succeed:
                    self.fail("start_guest returned although the step failed")
            finally:
                alive = [child for child in children if child.poll() is None]
                for child in alive:
                    child.kill()
                for child in children:
                    child.wait(timeout=10)
        return alive

    def test_a_proof_image_starts_without_an_installed_identity(self) -> None:
        for name, patched in {"Android never boots": {"wait_for_boot_completed": False},
                              "adb never answers": {"wait_for_adb": runner.boot_android.LauncherError("injected")}
                              }.items():
            with self.subTest(name):
                self.assertEqual([], self.start_and_fail(proof_image=True, **patched))

    def test_a_successful_proof_boot_records_nothing_of_an_installation(self) -> None:
        """The control is the installed release, which reads its identity, asks for its capacity and records the
        boot: the same boot of a proof image does none of it, and still makes the instance directory."""
        # The guest that start_guest returns owns the emulator it started: the caller stops it, here the harness.
        self.assertEqual(1, len(self.start_and_fail(succeed=True)))
        for name in ("read_release", "plan_start", "apply", "booted", "data_capacity"):
            self.assertTrue(self.mocks[name].called, f"the installed release does not call {name}")
        self.assertEqual(self.mocks["emulator_command"].call_args.args[4], 1)
        self.assertEqual(1, len(self.start_and_fail(proof_image=True, succeed=True)))
        for name in ("read_release", "plan_start", "apply", "booted", "data_capacity"):
            self.assertFalse(self.mocks[name].called, f"a proof image called {name}")
        self.assertIsNone(self.mocks["emulator_command"].call_args.args[4])
        self.assertTrue(self.instance_made, "the fresh instance's directory must exist for the emulator")

    def test_a_failure_after_the_spawn_leaves_no_emulator(self) -> None:
        boot = runner.boot_android
        cases = {
            "the forward is never announced": {"forward": runner.Failure("injected: no forward")},
            "adb never answers": {"wait_for_adb": boot.LauncherError("injected: waiting for adb")},
            "Android never boots": {"wait_for_boot_completed": False},
            "provisioning fails": {"provision_guest": boot.LauncherError("injected: provisioning")},
        }
        for name, patched in cases.items():
            with self.subTest(name):
                alive = self.start_and_fail(**patched)
                self.assertEqual([], alive, "the emulator it spawned was left running")


class DeliveryRowTests(unittest.TestCase):
    """D1 and D2 over a fake guest whose inbox and phone process the test scripts, and a fake host that stores
    what it is sent: what the rows accept and what they fail, without a guest."""

    SENDER = "+15550100"
    PLAIN = "sms-check plain 000102"

    class Guest:
        forward_port = 0

        def __init__(self, pids: list[str], crash_buffer: str = "") -> None:
            self.rows: list[tuple[int, str, str]] = []
            self.pids, self.crash_buffer = list(pids), crash_buffer
            self.commands: list[str] = []

        def alive(self) -> None:
            pass

        def inbox_ids(self, sender: str, text: str) -> set[int]:
            return {number for number, who, what in self.rows if (who, what) == (sender, text)}

        def shell(self, command: str, timeout: float = 0, check: bool = True) -> str:
            self.commands.append(command)
            return self.pids.pop(0) if len(self.pids) > 1 else self.pids[0]

        def logcat(self, *arguments: str) -> str:
            return self.crash_buffer

    class Host:
        """Answers handed-over and stores ``copies`` rows for each message, as a service that stores it."""

        def __init__(self, guest: "DeliveryRowTests.Guest", copies: int = 1) -> None:
            self.guest, self.copies = guest, copies

        def answer(self, payload: bytes, seconds: float = 0) -> object:
            fields = payload.decode().split(".")
            sender, text = base64.b64decode(fields[3]).decode(), base64.b64decode(fields[4]).decode()
            for _ in range(self.copies):
                number = max((row[0] for row in self.guest.rows), default=0) + 1
                self.guest.rows.append((number, sender, text))
            return runner.Answer("handed-over", fields[2], "1")

    def run_row(self, row, guest: "DeliveryRowTests.Guest", copies: int = 1) -> None:
        run = runner.Run(SimpleNamespace(variant="messaging"), guest, {})
        run.host = self.Host(guest, copies)
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner.time, "sleep"))
            stack.enter_context(mock.patch.object(runner.os, "urandom", side_effect=lambda count: bytes(range(count))))
            stack.enter_context(mock.patch.object(runner, "INBOX_SECONDS", 0.05))
            stack.enter_context(mock.patch.object(runner, "PHONE_WAIT_SECONDS", 0.05))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            row(run)

    def test_the_inbox_query_selects_ids_and_names_no_body_in_its_output(self) -> None:
        command = runner.inbox_ids_command("+1555", "it's 123")
        self.assertIn("--projection _id ", command)
        self.assertNotIn("body:", command)
        self.assertNotIn("--projection _id:", command)
        self.assertEqual(shlex.split(command)[-1], "address='+1555' AND body='it''s 123'")
        self.assertEqual(runner.parse_inbox_ids("Row: 0 _id=12\nRow: 1 _id=7\n"), {12, 7})
        self.assertEqual(runner.parse_inbox_ids("No result found.\n"), set())

    def test_exactly_one_new_row(self) -> None:
        self.assertIsNone(runner.new_row_problem(set(), {4}))
        self.assertIsNone(runner.new_row_problem({1, 2}, {1, 2, 3}))
        self.assertIn("0 new", runner.new_row_problem({1, 2}, {1, 2}))
        self.assertIn("0 new", runner.new_row_problem(set(), set()))
        self.assertIn("2 new", runner.new_row_problem({1}, {1, 2, 3}))

    def test_the_options_that_cannot_qualify_are_refused(self) -> None:
        parse = runner.build_parser().parse_args
        self.assertIn("SMS provider", runner.option_problem(parse(["--mode", "delivery", "--variant", "no-provider"])))
        self.assertIsNone(runner.option_problem(parse(["--mode", "delivery"])))
        self.assertIsNone(runner.option_problem(parse(["--mode", "delivery", "--variant", "no-messaging"])))
        self.assertIsNone(runner.option_problem(parse(["--mode", "record", "--variant", "no-provider"])))
        self.assertIn("--proof-image", runner.option_problem(parse(["--proof-image"])))
        self.assertIn("--proof-image", runner.option_problem(
            parse(["--proof-image", "--release", "x", "--instance", "y"])))
        self.assertIsNone(runner.option_problem(parse(["--proof-image", "--release", "x"])))
        with mock.patch.object(sys, "argv", ["run_android_sms.py", "--mode", "delivery", "--variant", "no-provider"]), \
                contextlib.redirect_stderr(io.StringIO()) as error:
            self.assertEqual(runner.main(), 2)
        self.assertIn("SMS provider", error.getvalue())

    def test_d1_needs_one_new_row(self) -> None:
        self.run_row(runner.row_d1, self.Guest(["100"]))
        old = self.Guest(["100"])
        old.rows = [(1, self.SENDER, self.PLAIN), (2, self.SENDER, self.PLAIN)]
        self.run_row(runner.row_d1, old)  # old matching rows are not a delivery, and a new one is
        self.assertEqual(len(old.rows), 3)
        old = self.Guest(["100"])
        old.rows = [(1, self.SENDER, self.PLAIN)]
        with self.assertRaisesRegex(runner.Failure, "0 new inbox rows"):
            self.run_row(runner.row_d1, old, copies=0)
        with self.assertRaisesRegex(runner.Failure, "2 new inbox rows"):
            self.run_row(runner.row_d1, self.Guest(["100"]), copies=2)
        with self.assertRaisesRegex(runner.Failure, "0 new inbox rows"):
            self.run_row(runner.row_d1, self.Guest(["100"]), copies=0)

    def test_d2_waits_for_the_phone_process_and_fails_plainly_without_it(self) -> None:
        self.run_row(runner.row_d2, self.Guest(["", "", "100", "100\n"]))
        with self.assertRaisesRegex(runner.Failure, r"com.android.phone running: pidof says '' \(after"):
            self.run_row(runner.row_d2, self.Guest([""]))
        with self.assertRaisesRegex(runner.Failure, "was pid 100 before the message and is 200 after it"):
            self.run_row(runner.row_d2, self.Guest(["100", "100", "200"]))
        guest = self.Guest(["100"])
        buffers = ["", "E AndroidRuntime: Process: com.android.phone, PID: 100\n"]
        guest.logcat = lambda *arguments: buffers.pop(0) if len(buffers) > 1 else buffers[0]  # type: ignore
        with self.assertRaisesRegex(runner.Failure, "gained 1 line"):
            self.run_row(runner.row_d2, guest)

    def test_d2_checks_each_message_for_one_new_row(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "a message with a one-time code: 2 new inbox rows"):
            self.run_row(runner.row_d2, self.Guest(["100"]), copies=2)
        with self.assertRaisesRegex(runner.Failure, "a message with a one-time code: 0 new inbox rows"):
            self.run_row(runner.row_d2, self.Guest(["100"]), copies=0)

    LISTENING = ("11-18 20:09:43.609  1402  1480 I AndariumSms: serving the host's requests\n"
                 "11-18 20:09:43.603  1402  1402 I AndariumSms: record in /x, labelled u:object_r:y:s0\n")
    START_UP_DENIALS = (
        "11-18 20:09:44.100  1402  1402 I auditd  : type=1400 audit(0.0:9): avc:  denied  { find } for pid=1402 "
        "name=gpu scontext=u:r:kernarium_sms:s0 tcontext=u:object_r:gpu_service:s0 tclass=service_manager "
        "permissive=0\n"
        "11-18 20:09:44.200  1402  1402 I auditd  : type=1400 audit(0.0:10): avc:  denied  { search } for "
        "name=\"data\" scontext=u:r:kernarium_sms:s0 tcontext=u:object_r:system_data_file:s0 tclass=dir "
        "permissive=0\n")

    def test_what_the_service_log_must_and_must_not_show(self) -> None:
        problems = runner.service_log_problems
        self.assertEqual(problems(self.LISTENING, self.START_UP_DENIALS, "1402"), [])
        self.assertIn("never logged that it serves", problems("", "", "1402")[0])
        # The end of a session at idle is not a failure of the channel, and a later session's pid is the service's too.
        idle = self.LISTENING + ("11-18 20:10:30.000  1402  1480 I AndariumSms: host channel: idle for 30 s, ending "
                                 "the session\n")
        self.assertEqual(problems(idle, "", "1402"), [])
        later = hidden_line("2210")
        self.assertIn("hidden-API access by the service", problems(self.LISTENING, later, {"1402", "2210"})[0])
        self.assertEqual(problems(self.LISTENING, later, {"1402"}), [])
        refused = self.LISTENING + ("11-18 20:10:00.000  1402  1480 I AndariumSms: refused a connection with no "
                                    "peer address: IOException\n")
        self.assertEqual(problems(refused, "", "1402"),
                         ["the service logged 'refused a connection with no peer address: IOException'"])
        cid = self.LISTENING + "11-18 20:10:00.000  1402  1480 I AndariumSms: refused a connection from CID 1\n"
        self.assertEqual(len(problems(cid, "", "1402")), 1)
        failed = self.LISTENING + ("11-18 20:10:00.000  1402  1480 I AndariumSms: host channel: cannot read a "
                                   "peer's CID: NoSuchMethodException\n")
        self.assertIn("host channel: cannot read a peer's CID", problems(failed, "", "1402")[0])
        vsock = ("11-18 20:10:00.000  1402  1480 I auditd  : type=1400 audit(0.0:11): avc:  denied  { accept } for "
                 "scontext=u:r:kernarium_sms:s0 tcontext=u:r:kernarium_sms:s0 tclass=vsock_socket permissive=0\n")
        self.assertEqual(problems(self.LISTENING, vsock, "1402"), ["SELinux denied the service a vsock socket"])
        # The handoff is the service's and the holder's alone: a denial of it, of the holder or of the waker is a failure.
        handoff = vsock.replace("tclass=vsock_socket", "tclass=sock_file")
        self.assertEqual(problems(self.LISTENING, handoff, "1402"), ["SELinux denied the service the handoff (sock_file)"])
        holder = vsock.replace("scontext=u:r:kernarium_sms:s0", "scontext=u:r:kernarium_sms_listener:s0")
        self.assertEqual(problems(self.LISTENING, holder, "1402"), ["SELinux denied kernarium_sms_listener vsock_socket"])
        waker = vsock.replace("scontext=u:r:kernarium_sms:s0", "scontext=u:r:kernarium_sms_wake:s0").replace(
            "vsock_socket", "binder")
        self.assertEqual(problems(self.LISTENING, waker, "1402"), ["SELinux denied kernarium_sms_wake binder"])
        hidden = ("11-18 20:09:43.700  1402  1402 W d.kernarium.sms: Accessing hidden method "
                  "Landroid/system/Os;->accept(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)"
                  "Ljava/io/FileDescriptor; (unsupported, reflection, denied)\n")
        self.assertIn("hidden-API access by the service", problems(self.LISTENING, hidden, "1402")[0])
        self.assertEqual(problems(self.LISTENING, hidden, "999"), [], "another process's warning is not the service's")
        other = hidden.replace("Landroid/system/Os;->accept", "Landroid/app/Activity;->x")
        self.assertEqual(problems(self.LISTENING, other, "1402"), [])

    def test_d3_reads_the_service_and_the_holder_without_needing_a_process(self) -> None:
        guest = self.Guest(["100"])
        logs = {"-s kernarium.sms.listener:I": ""}
        console = [CONSOLE.replace("pid 77", "pid 100")]
        guest.service_log = lambda: self.LISTENING  # type: ignore[method-assign]
        guest.logcat = lambda *arguments: logs.get(" ".join(arguments), self.START_UP_DENIALS)  # type: ignore[method-assign]
        guest.log = SimpleNamespace(read_text=lambda errors="strict": console[0])  # type: ignore[attr-defined]
        guest.holder_pid = lambda: 100  # type: ignore[attr-defined]
        # No process of the service: it ended at idle, and its log is the evidence. The holder's own log line has
        # aged out of the ring by now: init's console line is its start.
        self.run_row(runner.row_d3, guest)
        guest.service_log = lambda: ""  # type: ignore[method-assign]
        with self.assertRaisesRegex(runner.Failure, "never logged that it serves"):
            self.run_row(runner.row_d3, guest)
        guest.service_log = lambda: self.LISTENING  # type: ignore[method-assign]
        console[0] = ""
        with self.assertRaisesRegex(runner.Failure, "no init line that started the holder"):
            self.run_row(runner.row_d3, guest)
        console[0] = CONSOLE.replace("pid 77", "pid 100")
        logs["-s kernarium.sms.listener:I"] = (
            "11-18 20:09:31.000   700   700 E kernarium.sms.listener: cannot listen on vsock port 5560: errno 98\n")
        with self.assertRaisesRegex(runner.Failure, "the holder logged an error"):
            self.run_row(runner.row_d3, guest)

    def test_delivery_runs_d1_before_d2(self) -> None:
        rows, _ = runner.plan_rows("delivery", set())
        names = [row.name for row in rows]
        self.assertLess(names.index("D1"), names.index("D2"))
        self.assertLess(names.index("D2"), names.index("D3"))


class Clock:
    """The clock the rows' waits advance: a row that waits out a 30 s window takes no time."""

    def __init__(self) -> None:
        self.now = 1000.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class Simulation:
    """The guest of the on-demand service, as the rows see it: the holder always runs, and the service has a
    process only from a request until its idle window and its two seconds' linger have passed. Every request
    starts it when it has none (after the wake's delay), records one step line and one inbox row for a message,
    and restarts the window. The knobs break one thing each."""

    def __init__(self, idle: float = 30.0, wake: float = 0.5) -> None:
        self.clock = Clock()
        self.idle, self.wake, self.linger = idle, wake, 2.0
        self.pid: int | None = None
        self.last = 0.0
        self.counter = 4000
        self.holder = 77
        self.holder_restarts = False    # the holder ends and init starts it again: another pid at every look
        self.holder_looks = 0
        self.holder_log = ""            # the holder logs its listening once, early: the ring does not keep it
        self.console = CONSOLE          # init's lines from boot, which the launcher's console keeps
        self.holder_status = STATUS
        self.su = False                 # a user build has no su
        self.killed = 0
        self.kill_succeeds = True
        self.starts = 0
        self.rows: list[tuple[int, str, str]] = []
        self.step_log: list[str] = []
        self.record_label = "u:object_r:kernarium_sms_data_file:s0"   # what the service reads on its record directory
        self.record_lines: list[str] = []   # the service's line about its record, one per session of this boot
        self.commands: list[str] = []
        self.keep_running = False       # the service never ends (a persistent one)
        self.hand_over_copies = 1       # step lines and inbox rows a message gets
        self.answer_kind = "handed-over"
        self.screen_off = False
        self.wake_locks = ""
        self.property_output: dict[str, str] = {}

    def session(self) -> int | None:
        if self.pid is not None and not self.keep_running and self.clock.now - self.last >= self.idle + self.linger:
            self.pid = None
        return self.pid

    def touch(self) -> None:
        # A request after the window's end is not accepted by the session that is ending: the holder starts another.
        over = self.session() is not None and not self.keep_running and self.clock.now - self.last >= self.idle
        if self.session() is None or over:
            self.clock.sleep(self.wake)
            self.counter += 1
            self.pid = self.counter
            self.starts += 1
            self.record_lines.append(
                f"{1731758405 + self.clock.now:.3f}  {self.pid}  {self.pid + 8} I AndariumSms: record in "
                f"{runner.RECORD_DIRECTORY}, labelled {self.record_label}\n")
        self.last = self.clock.now


class SimulatedGuest:
    serial = "127.0.0.1:5578"
    forward_port = 0
    private = Path("/nonexistent")

    def __init__(self, simulation: Simulation) -> None:
        self.sim = simulation

    def alive(self) -> None:
        pass

    def strict_pid(self) -> int | None:
        return self.sim.session()

    def pid(self) -> int | None:
        return self.sim.session()

    def pids(self) -> set[int]:
        pid = self.sim.session()
        return set() if pid is None else {pid}

    @property
    def log(self) -> SimpleNamespace:
        return SimpleNamespace(read_text=lambda errors="strict": self.sim.console)

    def has_su(self) -> bool:
        return self.sim.su

    def su(self, command: str, timeout: float = 0, check: bool = True) -> str:
        return self.shell(command, timeout, check)

    def kill_service(self) -> bool:
        """am force-stop: the process ends at once."""
        self.sim.commands.append(f"am force-stop {runner.PACKAGE}")
        self.sim.killed += 1
        if self.sim.kill_succeeds:
            self.sim.pid = None
        return self.sim.kill_succeeds

    def holder_pid(self) -> int | None:
        self.sim.holder_looks += 1
        if self.sim.holder is not None and self.sim.holder_restarts:
            return self.sim.holder + self.sim.holder_looks - 1
        return self.sim.holder

    def logcat(self, *arguments: str) -> str:
        text = " ".join(arguments)
        if f"-s {runner.TAG}:I" in text:
            return "".join(self.sim.record_lines)
        return self.sim.holder_log if f"-s {runner.HOLDER_TAG}:I" in text else ""

    def service_log(self) -> str:
        return "".join(self.sim.step_log)

    def inbox_ids(self, sender: str, text: str) -> set[int]:
        return {number for number, who, what in self.sim.rows if (who, what) == (sender, text)}

    def inbox(self) -> str:
        return "".join(f"Row: {number} address={who}, body={what}\n" for number, who, what in self.sim.rows)

    def adb_run(self, *arguments: str, timeout: float = 0) -> SimpleNamespace:
        self.sim.commands.append("adb " + " ".join(arguments))
        return SimpleNamespace(returncode=0, stdout="")

    def shell(self, command: str, timeout: float = 0, check: bool = True) -> str:
        self.sim.commands.append(command)
        for needle, output in self.sim.property_output.items():
            if needle in command:
                return output
        if "keyevent KEYCODE_SLEEP" in command:
            self.sim.screen_off = True
        if "keyevent KEYCODE_WAKEUP" in command:
            self.sim.screen_off = False
        if command.startswith("dumpsys power"):
            return (f"  mWakefulness={'Asleep' if self.sim.screen_off else 'Awake'}\n"
                    f"Wake Locks: size={len(self.sim.wake_locks.splitlines())}\n{self.sim.wake_locks}")
        if command.startswith("cat /proc/") and command.endswith("/status"):
            return self.sim.holder_status
        if command.startswith("ps -A -Z"):
            listing = PS_AT_IDLE if self.sim.holder else PS_AT_IDLE.replace("kernarium_sms_listener:s0", "system_app:s0")
            return listing + (SERVICE_PS_LINE.format(pid=self.sim.session()) if self.sim.session() else "")
        return ""


PS_AT_IDLE = """\
LABEL                          USER           PID   PPID     VSZ    RSS WCHAN            ADDR S NAME
u:r:init:s0                    root             1      0   10000   1000 0                   0 S init
u:r:kernarium_sms_listener:s0  nobody          77      1   10000   1000 0                   0 S kernarium_sms_listener
"""
SERVICE_PS_LINE = "u:r:kernarium_sms:s0           system        {pid}    300   10000   1000 0                   0 S android.kernarium.sms\n"


class SimulatedHost:
    def __init__(self, simulation: Simulation) -> None:
        self.sim = simulation

    def exchange(self, payload: bytes, seconds: float = 0) -> object:
        self.sim.touch()
        return runner.Exchange("answered", runner.answer_bytes("refused-version"))

    def answer(self, payload: bytes, seconds: float = 0) -> object:
        fields = payload.decode().split(".")
        sender, text = base64.b64decode(fields[3]).decode(), base64.b64decode(fields[4]).decode()
        self.sim.touch()
        if self.sim.answer_kind == "handed-over":
            for _ in range(self.sim.hand_over_copies):
                self.sim.step_log.append(f"11-18 20:09:43.609  1402  1480 I AndariumSms: step 1 of {fields[2][:8]}\n")
                self.sim.rows.append((len(self.sim.rows) + 1, sender, text))
        self.sim.clock.sleep(0.05)
        return runner.Answer(self.sim.answer_kind, fields[2], "1")


class RecordingHost(SimulatedHost):
    """A host whose service keeps a record: a first message is handed over once, and its retry, and a first message
    sent again, are answered as handed over earlier, as the record does across a process that ended."""

    def __init__(self, simulation: Simulation) -> None:
        super().__init__(simulation)
        self.known: set[str] = set()

    def reply(self, payload: bytes) -> tuple[str, str, str]:
        fields = payload.decode().split(".")
        if fields[1] == "ping":
            return "refused-version", "", ""
        identifier = fields[2]
        if identifier in self.known:
            return "handed-over-earlier", identifier, "1"
        if fields[1] == "retry":
            return "uncertain", identifier, "unknown"
        self.known.add(identifier)
        sender, text = base64.b64decode(fields[3]).decode(), base64.b64decode(fields[4]).decode()
        self.sim.step_log.append(f"11-18 20:09:43.609  1402  1480 I AndariumSms: step 1 of {identifier[:8]}\n")
        self.sim.rows.append((len(self.sim.rows) + 1, sender, text))
        return "handed-over", identifier, "1"

    def exchange(self, payload: bytes, seconds: float = 0) -> object:
        self.sim.touch()
        kind, identifier, detail = self.reply(payload)
        data = runner.answer_bytes(kind) if kind == "refused-version" else runner.answer_bytes(kind, identifier, detail)
        return runner.Exchange("answered", data)

    def answer(self, payload: bytes, seconds: float = 0) -> object:
        self.sim.touch()
        kind, identifier, detail = self.reply(payload)
        return runner.Answer(kind, identifier, detail)


class OnDemandTests(unittest.TestCase):
    """What the service's on-demand life adds: the readers of its process, its socket and its wake locks, and the
    rows N1 to N6, G2e, G4 and G5 over a simulated guest."""

    def run_row(self, row, simulation: Simulation, host: type = SimulatedHost, **options: object) -> "runner.Run":
        guest = SimulatedGuest(simulation)
        run = runner.Run(SimpleNamespace(variant="messaging", vsockprobe_dex=Path("/x/vsockprobe.dex"), **options),
                         guest, {})
        run.host = host(simulation)
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner.time, "monotonic", simulation.clock.monotonic))
            stack.enter_context(mock.patch.object(runner.time, "sleep", simulation.clock.sleep))
            stack.enter_context(mock.patch.object(runner.os, "urandom", side_effect=self.unique_bytes))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            row(run)
        self.run = run
        return run

    counter = 0

    def unique_bytes(self, count: int) -> bytes:
        """Different bytes at every call, so that each message of a row has an identifier and a text of its own."""
        self.counter += 1
        return (self.counter.to_bytes(4, "big") + bytes(count))[:count]  # the first four bytes name a message in a log

    # ---- the readers

    def test_the_end_of_the_window_is_judged(self) -> None:
        self.assertIsNone(runner.idle_timing_problem(32.5))
        self.assertIsNone(runner.idle_timing_problem(27.0))
        self.assertIsNone(runner.idle_timing_problem(45.0))
        self.assertIn("after only 12.0 s", runner.idle_timing_problem(12.0))
        self.assertIn("46.0 s after the last request", runner.idle_timing_problem(46.0))

    def test_the_service_is_not_persistent(self) -> None:
        listed = "  PID mappings:\n    PID #4000: ProcessRecord{abc 4000:android.kernarium.sms/1000}\n"
        self.assertIsNone(runner.persistent_problem(listed, "    flags=[ SYSTEM HAS_CODE ALLOW_CLEAR_USER_DATA ]\n"))
        self.assertIn("persistent", runner.persistent_problem(listed + "      persistent=true\n", ""))
        self.assertIn("persistent", runner.persistent_problem("  *PERS* UID 1000 ProcessRecord{abc}\n", ""))
        self.assertIn("persistent", runner.persistent_problem(listed, "    flags=[ SYSTEM PERSISTENT HAS_CODE ]\n"))

    def test_the_holder_and_the_service_processes(self) -> None:
        self.assertEqual(runner.holder_problems(PS_AT_IDLE), [])
        self.assertIn("no process runs in kernarium_sms_listener",
                      runner.holder_problems(PS_AT_IDLE.replace("kernarium_sms_listener:s0", "system_app:s0"))[0])
        self.assertIn("runs as root, not nobody",
                      runner.holder_problems(PS_AT_IDLE.replace("nobody ", "root   "))[0])
        self.assertEqual(len(runner.holder_problems(PS_AT_IDLE + PS_AT_IDLE.splitlines()[-1] + "\n")), 1)
        self.assertIsNone(runner.service_process_problem(PS_AT_IDLE + SERVICE_PS_LINE.format(pid=9)))
        self.assertIn("not u:r:kernarium_sms:s0, system",
                      runner.service_process_problem(PS_AT_IDLE + SERVICE_PS_LINE.format(pid=9).replace(
                          "kernarium_sms:s0", "system_app:s0")))
        self.assertIn("no process", runner.service_process_problem(PS_AT_IDLE))

    def test_the_handoff_socket_listing(self) -> None:
        good = "srw-rw---- 1 system system u:object_r:kernarium_sms_socket:s0 0 2026-11-20 10:00 /dev/socket/kernarium_sms\n"
        self.assertIsNone(runner.socket_listing_problem(good))
        for wrong in (good.replace("srw-rw----", "srw-rw-rw-"), good.replace("system system", "shell shell"),
                      good.replace("kernarium_sms_socket", "device"), "ls: No such file or directory\n"):
            self.assertIsNotNone(runner.socket_listing_problem(wrong), wrong)

    def test_the_wake_locks(self) -> None:
        none = "Wake Locks: size=0\n"
        self.assertEqual(runner.wakelock_problems(none, {4001, 77}), [])
        other = "Wake Locks: size=1\n  PARTIAL_WAKE_LOCK 'RILJ' ACQ=-3s (uid=1001 pid=611 ws=null)\n"
        self.assertEqual(runner.wakelock_problems(other, {4001, 77}), [])
        ours = "Wake Locks: size=1\n  PARTIAL_WAKE_LOCK 'x' ACQ=-1s (uid=1000 pid=4001 ws=null)\n"
        self.assertEqual(len(runner.wakelock_problems(ours, {4001, 77})), 1)
        named = "Wake Locks: size=1\n  PARTIAL_WAKE_LOCK 'AndariumSms:deliver' ACQ=-1s (uid=1000 pid=500 ws=null)\n"
        self.assertEqual(len(runner.wakelock_problems(named, {4001, 77})), 1)
        self.assertEqual(runner.wakefulness("  mWakefulness=Asleep\n"), "Asleep")
        self.assertEqual(runner.wakefulness("  mWakefulness=Awake\n"), "Awake")
        self.assertIsNone(runner.wakefulness("nothing"))

    # ---- N1 to N6

    def test_n1_wants_no_process_and_the_holder(self) -> None:
        simulation = Simulation()
        self.run_row(runner.row_n1, simulation)
        self.assertEqual(simulation.starts, 0, "N1 sends nothing: a ping would start the service")
        simulation.pid, simulation.last, simulation.keep_running = 4001, simulation.clock.now, True
        with self.assertRaisesRegex(runner.Failure, r"runs \(pid 4001\) at idle"):
            self.run_row(runner.row_n1, simulation)
        simulation = Simulation()
        simulation.holder = None  # type: ignore[assignment]
        with self.assertRaisesRegex(runner.Failure, "kernarium_sms_listener does not run"):
            self.run_row(runner.row_n1, simulation)

    def test_n1_wants_a_holder_that_stays_up_and_logs_no_error(self) -> None:
        """The holder that ends at once and is started again by init shows as a process most of the time (the first
        image of this change did, on a handoff socket it never listened on), so N1 looks twice, reads init's console
        lines for its starts, and reads its log for errors. The holder does not repeat its listening line: the
        console's start line is the evidence that remains."""
        simulation = Simulation()
        simulation.holder_restarts = True
        with self.assertRaisesRegex(runner.Failure, r"pid 78 after \d+ s, was 77: it ends and init starts it again"):
            self.run_row(runner.row_n1, simulation)
        simulation = Simulation()
        simulation.console += CONSOLE.replace("pid 77", "pid 91").replace("6.67", "9.67")
        with self.assertRaisesRegex(runner.Failure, r"init started the holder 2 times \(pids \[77, 91\]\)"):
            self.run_row(runner.row_n1, simulation)
        simulation = Simulation()
        simulation.holder_log += ("11-20 10:00:01.000    77    77 E kernarium.sms.listener: "
                                  "the handoff socket failed (events 0x10)\n")
        with self.assertRaisesRegex(runner.Failure, "the holder logged an error: 'the handoff socket failed"):
            self.run_row(runner.row_n1, simulation)
        simulation = Simulation()
        simulation.console = ""
        with self.assertRaisesRegex(runner.Failure, "no init line that started the holder"):
            self.run_row(runner.row_n1, simulation)
        # No log line of the holder at all, which is what a ring that has rotated shows: not a failure.
        simulation = Simulation()
        simulation.holder_log = ""
        self.run_row(runner.row_n1, simulation)

    def test_n2_a_first_message_at_idle(self) -> None:
        simulation = Simulation()
        simulation.touch()  # a session from an earlier row: N2 waits for it to end
        run = self.run_row(runner.row_n2, simulation)
        self.assertEqual(simulation.starts, 2)
        self.assertEqual(len(simulation.rows), 1)
        self.assertTrue(any("answered in 0.5" in note for note in run.notes), run.notes)
        simulation = Simulation()
        simulation.keep_running = True
        simulation.touch()
        with mock.patch.object(runner, "GONE_SECONDS", 5):
            with self.assertRaisesRegex(runner.Failure, "the service ending: it still runs"):
                self.run_row(runner.row_n2, simulation)
        simulation = Simulation()
        simulation.hand_over_copies = 2
        with self.assertRaisesRegex(runner.Failure, "2 new inbox rows"):
            self.run_row(runner.row_n2, simulation)

    def test_n3_the_process_ends_and_the_next_request_starts_another(self) -> None:
        simulation = Simulation()
        run = self.run_row(runner.row_n3, simulation)
        self.assertEqual(simulation.starts, 2)
        self.assertTrue(any("ended" in note for note in run.notes), run.notes)
        early = Simulation(idle=5)
        with self.assertRaisesRegex(runner.Failure, "after only"):
            self.run_row(runner.row_n3, early)
        resident = Simulation()
        resident.keep_running = True
        with self.assertRaisesRegex(runner.Failure, "the service ending: it still runs"):
            self.run_row(runner.row_n3, resident)
        moved = Simulation()
        original = moved.touch

        def restart_holder() -> None:
            original()
            moved.holder = 78

        moved.touch = restart_holder  # type: ignore[method-assign]
        with self.assertRaisesRegex(runner.Failure, "the holder was pid 77 and is pid 78"):
            self.run_row(runner.row_n3, moved)

    def test_n4_requests_around_the_end_of_the_window(self) -> None:
        simulation = Simulation()
        run = self.run_row(runner.row_n4, simulation)
        self.assertEqual(len(simulation.rows), len(runner.BOUNDARY_OFFSETS) + 1)
        self.assertEqual(len(simulation.step_log), len(simulation.rows), "one step line for each message")
        self.assertGreaterEqual(simulation.starts, 3, "some requests found the process gone")
        self.assertTrue(any("offset +1.0" in note for note in run.notes), run.notes)
        twice = Simulation()
        twice.hand_over_copies = 2
        with self.assertRaisesRegex(runner.Failure, r"2 step-1 lines|2 new inbox rows"):
            self.run_row(runner.row_n4, twice)
        refusing = Simulation()
        refusing.answer_kind = "not-handed-over"
        with self.assertRaisesRegex(runner.Failure, "answer"):
            self.run_row(runner.row_n4, refusing)

    def test_n5_the_shell_cannot_reach_the_handoff(self) -> None:
        """Where there is su the file's mode, owner and label are read; on a user build the shell cannot stat
        /dev/socket, so init's console line is the evidence and the connect probe is the rest. The probe runs
        whatever the first half found."""
        listing = ("srw-rw---- 1 system system u:object_r:kernarium_sms_socket:s0 0 2026-11-20 10:00 "
                   "/dev/socket/kernarium_sms\n")
        simulation = Simulation()
        simulation.property_output = {"VsockProbe unix": "connect: errno=13 EACCES\n"}
        run = self.run_row(runner.row_n5, simulation)
        self.assertEqual(simulation.starts, 0)
        self.assertTrue(any("VsockProbe unix /dev/socket/kernarium_sms" in command for command in simulation.commands))
        self.assertFalse(any("ls -lZ" in command for command in simulation.commands), "a user build cannot stat it")
        self.assertTrue(any("no su" in note for note in run.notes), run.notes)
        for output, problem in (("connect: ok\n", "connected to"), ("connect: errno=2 ENOENT\n", "errno=2"),
                                ("socket: errno=13 EACCES\n", "older vsockprobe.dex")):
            simulation = Simulation()
            simulation.property_output = {"VsockProbe unix": output}
            with self.assertRaisesRegex(runner.Failure, problem):
                self.run_row(runner.row_n5, simulation)
        # The console line is missing: that is reported, and the probe ran all the same.
        simulation = Simulation()
        simulation.console = CONSOLE.replace("mode 660", "mode 666")
        simulation.property_output = {"VsockProbe unix": "connect: errno=13 EACCES\n"}
        with self.assertRaisesRegex(runner.Failure, "handoff socket with mode 660"):
            self.run_row(runner.row_n5, simulation)
        self.assertTrue(any("VsockProbe unix" in command for command in simulation.commands))
        simulation = Simulation()
        simulation.console = ""
        simulation.property_output = {"VsockProbe unix": "connect: ok\n"}
        with self.assertRaisesRegex(runner.Failure, "handoff socket with mode 660.*connected to"):
            self.run_row(runner.row_n5, simulation)
        # With su, the listing is read as before, and the probe still runs when it is wrong.
        simulation = Simulation()
        simulation.su = True
        simulation.property_output = {"VsockProbe unix": "connect: errno=13 EACCES\n", "ls -lZ": listing}
        self.run_row(runner.row_n5, simulation)
        self.assertTrue(any("ls -lZ" in command for command in simulation.commands))
        simulation = Simulation()
        simulation.su = True
        simulation.property_output = {"VsockProbe unix": "connect: errno=13 EACCES\n",
                                      "ls -lZ": listing.replace("srw-rw----", "srw-rw-rw-")}
        with self.assertRaisesRegex(runner.Failure, "the handoff socket"):
            self.run_row(runner.row_n5, simulation)

    def test_n6_a_message_while_the_screen_is_off(self) -> None:
        simulation = Simulation()
        run = self.run_row(runner.row_n6, simulation)
        self.assertFalse(simulation.screen_off, "the screen is switched on again")
        self.assertIn("input keyevent KEYCODE_SLEEP", simulation.commands)
        self.assertTrue(any("screen off" in note for note in run.notes), run.notes)
        locked = Simulation()
        locked.touch()
        locked.wake_locks = f"  PARTIAL_WAKE_LOCK 'x' ACQ=-1s (uid=1000 pid={locked.pid + 1} ws=null)\n"
        with self.assertRaisesRegex(runner.Failure, "wake lock"):
            self.run_row(runner.row_n6, locked)
        self.assertFalse(locked.screen_off, "the screen is switched on again after a failure too")
        stuck = Simulation()
        stuck.property_output = {"dumpsys power": "  mWakefulness=Awake\n"}
        with mock.patch.object(runner, "SCREEN_SECONDS", 3):
            with self.assertRaisesRegex(runner.Failure, "the screen turning off: it is awake"):
                self.run_row(runner.row_n6, stuck)

    # ---- the rows that changed

    def test_g2e_runs_its_second_pass_with_the_service_stopped(self) -> None:
        class Reverse(SimulatedGuest):
            def adb_run(self, *arguments: str, timeout: float = 0) -> SimpleNamespace:
                self.sim.commands.append("adb " + " ".join(arguments))
                if arguments[:1] == ("reverse",) and "--list" not in arguments and "--remove" not in arguments:
                    refused = arguments[1].startswith(("vsock", "acceptfd"))
                    return SimpleNamespace(returncode=1 if refused else 0, stdout="refused" if refused else "")
                return SimpleNamespace(returncode=0, stdout="")

            def shell(self, command: str, timeout: float = 0, check: bool = True) -> str:
                if command == "echo adbd-works":
                    return "adbd-works\n"
                if command.startswith("pm disable-user"):
                    self.sim.pid = None  # disabling the package ends its process, which nothing restarts
                return super().shell(command, timeout, check)

        def go(simulation: Simulation) -> list[str]:
            guest = Reverse(simulation)
            run = runner.Run(SimpleNamespace(variant="messaging"), guest, {})
            run.host = SimulatedHost(simulation)
            run.ping = lambda: True  # type: ignore[method-assign]
            with contextlib.ExitStack() as stack:
                stack.enter_context(mock.patch.object(runner.time, "monotonic", simulation.clock.monotonic))
                stack.enter_context(mock.patch.object(runner.time, "sleep", simulation.clock.sleep))
                stack.enter_context(mock.patch.object(runner.os, "urandom", side_effect=lambda count: bytes(range(count))))
                runner.row_g2e(run)
            return simulation.commands

        simulation = Simulation()
        simulation.touch()  # the service runs when the row starts, as after the rows before it
        commands = go(simulation)
        disabled = commands.index(f"pm disable-user {runner.PACKAGE}")
        attempts = [index for index, command in enumerate(commands) if command.startswith("adb reverse vsock:5560 tcp:")]
        self.assertEqual(len(attempts), 2, "one pass before the package is disabled and one after")
        self.assertLess(attempts[0], disabled)
        self.assertLess(disabled, attempts[1])
        self.assertIn(f"pm enable {runner.PACKAGE}", commands[disabled:])
        # A package whose process survives the disabling is the old pass that never ran with the service stopped.
        resident = Simulation()
        resident.touch()
        resident.keep_running = True

        class Survives(Reverse):
            def shell(self, command: str, timeout: float = 0, check: bool = True) -> str:
                if command.startswith("pm disable-user"):
                    return SimulatedGuest.shell(self, command, timeout, check)
                return super().shell(command, timeout, check)

        guest = Survives(resident)
        run = runner.Run(SimpleNamespace(variant="messaging"), guest, {})
        run.host = SimulatedHost(resident)
        run.ping = lambda: True  # type: ignore[method-assign]
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner.time, "monotonic", resident.clock.monotonic))
            stack.enter_context(mock.patch.object(runner.time, "sleep", resident.clock.sleep))
            stack.enter_context(mock.patch.object(runner.os, "urandom", side_effect=lambda count: bytes(range(count))))
            with self.assertRaisesRegex(runner.Failure, "the service stopped: it still runs"):
                runner.row_g2e(run)
        self.assertIn(f"pm enable {runner.PACKAGE}", resident.commands, "the package is enabled again after a failure")

    def test_g4_the_holder_from_boot_and_a_service_that_is_not_persistent(self) -> None:
        epoch = HOLDER_EPOCH + EPOCH
        events = EVENTS

        class Guest4(SimulatedGuest):
            def logcat(self, *arguments: str) -> str:
                joined = " ".join(arguments)
                if "boot_progress_enable_screen" in joined:
                    return events
                if f"-s {runner.HOLDER_TAG}:I" in joined:
                    return HOLDER_EPOCH
                if "-s AndariumSms:I" in joined:
                    return EPOCH
                return ""

            def shell(self, command: str, timeout: float = 0, check: bool = True) -> str:
                if command.startswith("dumpsys activity processes"):
                    return self.sim.property_output.get("persistent", "    PID #4001: ProcessRecord{x 4001:android.kernarium.sms/1000}\n")
                if command.startswith("dumpsys package"):
                    return "    flags=[ SYSTEM HAS_CODE ]\n"
                return super().shell(command, timeout, check)

        def go(simulation: Simulation) -> None:
            guest = Guest4(simulation)
            run = runner.Run(SimpleNamespace(variant="messaging", vsockprobe_dex=Path("/x")), guest, {})
            run.host = SimulatedHost(simulation)
            run.vectors = {"request-first-number": "x"}
            with contextlib.ExitStack() as stack:
                stack.enter_context(mock.patch.object(runner.time, "monotonic", simulation.clock.monotonic))
                stack.enter_context(mock.patch.object(runner.time, "sleep", simulation.clock.sleep))
                stack.enter_context(mock.patch.object(runner, "golden_request", lambda vectors, name: SimpleNamespace(
                    identifier="00112233" + "0" * 24)))
                runner.row_g4(run)

        simulation = Simulation()
        simulation.step_log.append("11-16 12:00:01.001  1000  1200 I AndariumSms: step 1 of 00112233\n")
        go(simulation)
        self.assertEqual(simulation.starts, 1, "G4 starts a session to look at it")
        persistent = Simulation()
        persistent.step_log = list(simulation.step_log)
        persistent.property_output = {"persistent": "  *PERS* UID 1000 ProcessRecord{x}\n"}
        with self.assertRaisesRegex(runner.Failure, "persistent"):
            go(persistent)
        nobody = Simulation()
        nobody.step_log = list(simulation.step_log)
        nobody.holder = None  # type: ignore[assignment]
        with self.assertRaisesRegex(runner.Failure, "kernarium_sms_listener"):
            go(nobody)
        # The holder's group: init leaves it at root when the rc names none.
        root = Simulation()
        root.step_log = list(simulation.step_log)
        root.holder_status = STATUS.replace("Gid:\t9999\t9999\t9999\t9999", "Gid:\t0\t0\t0\t0")
        with self.assertRaisesRegex(runner.Failure, "group ids are"):
            go(root)

    def test_g4_runs_every_check_and_reports_every_problem(self) -> None:
        """The service's checks no longer wait behind the holder's: a holder with no console line AND a persistent
        service are both reported, and the checks after the failed one ran."""
        class Guest4(SimulatedGuest):
            def logcat(self, *arguments: str) -> str:
                joined = " ".join(arguments)
                if "boot_progress_enable_screen" in joined:
                    return EVENTS
                if "-s AndariumSms:I" in joined:
                    return EPOCH
                return ""

            def shell(self, command: str, timeout: float = 0, check: bool = True) -> str:
                if command.startswith("dumpsys activity processes"):
                    return "  *PERS* UID 1000 ProcessRecord{x}\n"
                if command.startswith("dumpsys package"):
                    return "    flags=[ SYSTEM HAS_CODE ]\n"
                return super().shell(command, timeout, check)

        simulation = Simulation()
        simulation.console = ""
        simulation.step_log.append("11-16 12:00:01.001  1000  1200 I AndariumSms: step 1 of 00112233\n")
        guest = Guest4(simulation)
        run = runner.Run(SimpleNamespace(variant="messaging", vsockprobe_dex=Path("/x")), guest, {})
        run.host = SimulatedHost(simulation)
        run.vectors = {"request-first-number": "x"}
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner.time, "monotonic", simulation.clock.monotonic))
            stack.enter_context(mock.patch.object(runner.time, "sleep", simulation.clock.sleep))
            stack.enter_context(mock.patch.object(runner, "golden_request", lambda vectors, name: SimpleNamespace(
                identifier="00112233" + "0" * 24)))
            with self.assertRaises(runner.Failure) as caught:
                runner.row_g4(run)
        message = str(caught.exception)
        self.assertIn("no init line that started the holder", message)
        self.assertIn("persistent", message, "the service's checks ran after the holder's failed")
        self.assertIn("the launcher's console has no init line", message)

    # ---- I4, I2, I9, G6 and the rows that this guest cannot show

    def test_i4_counts_only_rounds_that_ended_a_process(self) -> None:
        """Each round starts from a process that a ping started, ends it with am force-stop, and the next request is
        answered by another pid. A kill that did nothing is a failure, and so is a service with no process to end."""
        simulation = Simulation()
        with mock.patch.object(runner, "KILL_ROUNDS", 6):
            run = self.run_row(runner.row_i4, simulation, RecordingHost)
        self.assertEqual(simulation.killed, 6, "a kill in every round")
        self.assertTrue(all(command.startswith("am force-stop") for command in simulation.commands
                            if "force-stop" in command or "crash" in command), "no crash: it leaves the process serving")
        self.assertFalse(any("am crash" in command for command in simulation.commands))
        self.assertTrue(any("6 rounds, each of which ended a process" in note for note in run.notes), run.notes)
        # A kill that does not end the process (the command succeeds and nothing dies, as with a crash dialog) is not
        # a round: the next request is answered by the same pid, which the row does not accept.
        survives = Simulation()
        original = SimulatedGuest.kill_service

        def spared(self: SimulatedGuest) -> bool:
            original(self)
            self.sim.pid = self.sim.pid or self.sim.counter  # still the same process
            return True

        with mock.patch.object(SimulatedGuest, "kill_service", spared), mock.patch.object(runner, "KILL_ROUNDS", 3):
            with self.assertRaisesRegex(runner.Failure, "the service restarting: the process .* that ran before is still "
                                                        "listed"):
                self.run_row(runner.row_i4, survives, RecordingHost)
        failing = Simulation()
        failing.kill_succeeds = False
        with mock.patch.object(runner, "KILL_ROUNDS", 3):
            with self.assertRaisesRegex(runner.Failure, "'am force-stop android.kernarium.sms' did not succeed"):
                self.run_row(runner.row_i4, failing, RecordingHost)
        # A successor that pidof lists beside the process that was ended has not seen that process die: the round
        # counts only when the old pid is absent from the whole set. A listing that clears is waited out; one that
        # does not clear fails the row.
        class Lingering(SimulatedGuest):
            polls_listing_the_old_one = 2

            def kill_service(self) -> bool:
                self.old = self.sim.pid
                self.polls = 0
                return super().kill_service()

            def pids(self) -> set[int]:
                found = super().pids()
                self.polls += 1
                if found and self.polls <= self.polls_listing_the_old_one:
                    found = found | {self.old}
                return found

        for lingers, stays in ((2, False), (10 ** 6, True)):
            simulation = Simulation()
            guest = Lingering(simulation)
            guest.polls_listing_the_old_one = lingers
            run = runner.Run(SimpleNamespace(variant="messaging"), guest, {})
            run.host = RecordingHost(simulation)
            with contextlib.ExitStack() as stack:
                stack.enter_context(mock.patch.object(runner.time, "monotonic", simulation.clock.monotonic))
                stack.enter_context(mock.patch.object(runner.time, "sleep", simulation.clock.sleep))
                stack.enter_context(mock.patch.object(runner, "KILL_ROUNDS", 2))
                if stays:
                    with self.assertRaisesRegex(runner.Failure, "the process .* that ran before is still listed"):
                        runner.row_i4(run)
                else:
                    runner.row_i4(run)
        # A round whose ping leaves no process (the service answered from none) has nothing to end.
        class NoProcess(SimulatedGuest):
            def pid(self) -> int | None:
                return None

        simulation = Simulation()
        guest = NoProcess(simulation)
        run = runner.Run(SimpleNamespace(variant="messaging"), guest, {})
        run.host = RecordingHost(simulation)
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner.time, "monotonic", simulation.clock.monotonic))
            stack.enter_context(mock.patch.object(runner.time, "sleep", simulation.clock.sleep))
            with self.assertRaisesRegex(runner.Failure, "round 0: no process of android.kernarium.sms after the service "
                                                        "answered"):
                runner.row_i4(run)

    def test_i2_restarts_the_instance_cleanly_and_judges_every_part(self) -> None:
        """A clean power-off and a new launcher on the same data image: the console's line of the holder, no process at
        idle, the retry and a new message answered, and the process gone after the window."""
        simulation = Simulation()

        def restart(run: "runner.Run") -> None:
            simulation.commands.append("restart the instance")
            simulation.pid = None  # the new boot has no service process, and its log has no line of the old session
            simulation.step_log.clear()
            simulation.record_lines.clear()

        with mock.patch.object(runner, "restart_instance", restart):
            self.run_row(runner.row_i2, simulation, RecordingHost)
        self.assertIn("restart the instance", simulation.commands)
        self.assertEqual(simulation.starts, 2, "the first message, and the session that the retry starts")
        # The new message after the restart is counted on its own: two handovers of it beside one reply are a failure
        # that the count of the first message's lines cannot show.
        class Doubled(RecordingHost):
            def reply(self, payload: bytes) -> tuple[str, str, str]:
                answer = super().reply(payload)
                if answer[0] == "handed-over" and len(self.known) == 2:
                    self.sim.step_log.append(f"11-18 20:09:43.609  1402  1480 I AndariumSms: step 1 of {answer[1][:8]}\n")
                return answer

        doubled = Simulation()
        with mock.patch.object(runner, "restart_instance", lambda run: (setattr(doubled, "pid", None),
                                                                        doubled.step_log.clear(),
                                                                        doubled.record_lines.clear())):
            with self.assertRaisesRegex(runner.Failure, "2 step-1 lines for the new message after the restart, "
                                                        "expected 1"):
                self.run_row(runner.row_i2, doubled, Doubled)
        # A record that did not survive the restart answers the retry as uncertain, which is reported (and the new
        # message is still judged): the finding of the second boot of the first guest series.
        class Forgetful(RecordingHost):
            def reply(self, payload: bytes) -> tuple[str, str, str]:
                if payload.decode().split(".")[1] == "retry":
                    return "unsettled", payload.decode().split(".")[2], "store"
                return super().reply(payload)

        forgetful = Simulation()
        with mock.patch.object(runner, "restart_instance",
                               lambda run: (forgetful.step_log.clear(), forgetful.record_lines.clear())):
            with self.assertRaisesRegex(runner.Failure, "the retry after the restart was answered .*unsettled"):
                self.run_row(runner.row_i2, forgetful, Forgetful)
        # The holder's start is judged on the new console.
        nothing = Simulation()

        def restart_without_a_console_line(run: "runner.Run") -> None:
            nothing.console = ""
            nothing.step_log.clear()
            nothing.record_lines.clear()

        with mock.patch.object(runner, "restart_instance", restart_without_a_console_line):
            with self.assertRaisesRegex(runner.Failure, "no init line that started the holder"):
                self.run_row(runner.row_i2, nothing, RecordingHost)
        # The finding of the second boot (BOOT2): the service could not search the path of its record, read no label
        # ("labelled null") and was denied directories. Each is reported on its own.
        blind = Simulation()

        def restart_blind(run: "runner.Run") -> None:
            blind.pid = None
            blind.step_log.clear()
            blind.record_lines.clear()
            blind.record_label = "null"

        with mock.patch.object(runner, "restart_instance", restart_blind):
            with self.assertRaisesRegex(runner.Failure, "after the restart, the record's place is .*labelled null"):
                self.run_row(runner.row_i2, blind, RecordingHost)
        denied = ("1731758405.000  1  1 I auditd  : type=1400 audit(0.0:130): avc:  denied  { search } for  "
                  "comm=\"d.kernarium.sms\" name=\"0\" dev=\"ubdb3\" ino=390146 scontext=u:r:kernarium_sms:s0 "
                  "tcontext=u:object_r:system_data_file:s0:c512,c768 tclass=dir permissive=0")
        self.assertEqual(len(runner.storage_denial_problems(denied)), 1)
        self.assertEqual(runner.storage_denial_problems(denied.replace("tclass=dir", "tclass=service_manager")), [])
        self.assertEqual(runner.storage_denial_problems(denied.replace("kernarium_sms:s0", "system_app:s0")), [])

    def test_i9_ends_the_holder_and_wants_the_port_back(self) -> None:
        simulation = Simulation()
        simulation.su = True

        class Killing(SimulatedGuest):
            def su(self, command: str, timeout: float = 0, check: bool = True) -> str:
                self.sim.commands.append(command)
                if command.startswith("kill -9"):
                    self.sim.holder += 40
                    self.sim.console += CONSOLE.replace("pid 77", f"pid {self.sim.holder}").replace("6.67", "90.67")
                return ""

        guest = Killing(simulation)
        run = runner.Run(SimpleNamespace(variant="messaging", vsockprobe_dex=Path("/x")), guest, {})
        run.host = SimulatedHost(simulation)
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner.time, "monotonic", simulation.clock.monotonic))
            stack.enter_context(mock.patch.object(runner.time, "sleep", simulation.clock.sleep))
            stack.enter_context(mock.patch.object(runner.os, "urandom", side_effect=self.unique_bytes))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            runner.row_i9(run)
        self.assertIn("kill -9 77", simulation.commands)
        self.assertTrue(simulation.rows, "a message after the holder's restart was handed over")
        # Init that does not start it again (the pid does not change) fails, with the wait bounded.
        dead = Simulation()
        dead.su = True
        guest = SimulatedGuest(dead)
        run = runner.Run(SimpleNamespace(variant="messaging", vsockprobe_dex=Path("/x")), guest, {})
        run.host = SimulatedHost(dead)
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner.time, "monotonic", dead.clock.monotonic))
            stack.enter_context(mock.patch.object(runner.time, "sleep", dead.clock.sleep))
            with self.assertRaisesRegex(runner.Failure, "the holder started again: it has no new process"):
                runner.row_i9(run)

    def test_g6_names_the_sweep_it_does_not_make(self) -> None:
        simulation = Simulation()
        simulation.property_output = {"test -x /apex/com.android.virt/bin/vm": "", "pidof crosvm": "",
                                      "pm list packages": ""}

        class Guest6(SimulatedGuest):
            def probe_call(self, call: str, seconds: float = 0) -> str:
                return "vm: refused"

        guest = Guest6(simulation)
        run = runner.Run(SimpleNamespace(variant="messaging", adbd_vsock_forward=False), guest, {})
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner.time, "monotonic", simulation.clock.monotonic))
            stack.enter_context(mock.patch.object(runner.time, "sleep", simulation.clock.sleep))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(mock.patch.object(runner, "open_vsock_through_adbd",
                                                  side_effect=AssertionError("the sweep ran")))
            runner.row_g6(run)
        self.assertTrue(any("port sweep is not shown" in note for note in run.notes), run.notes)

    def test_g5_reports_the_session_and_the_holder(self) -> None:
        simulation = Simulation()
        simulation.property_output = {"dumpsys meminfo": "        TOTAL    18432    1000\n"}
        run = self.run_row(runner.row_g5, simulation)
        self.assertEqual(simulation.starts, 1, "the session's cost needs a session")
        self.assertTrue(any("18432 kB" in note and "session" in note for note in run.notes), run.notes)
        self.assertTrue(any("holder" in note for note in run.notes), run.notes)
        simulation = Simulation()
        with self.assertRaisesRegex(runner.Failure, "shows no total"):
            self.run_row(runner.row_g5, simulation)


class ProbeSourceTests(unittest.TestCase):
    def test_the_probe_sources_parse_and_name_what_the_runner_calls(self) -> None:
        sources = sorted(PROBE.glob("**/*.java"))
        self.assertEqual([path.name for path in sources], ["CommandReceiver.java", "ReceivedReceiver.java",
                                                         "VsockProbe.java"])
        manifest = (PROBE / "app/AndroidManifest.xml").read_text()
        self.assertIn(f'package="{runner.PROBE_PACKAGE}"', manifest)
        for name in ("RECEIVE_SMS", "MANAGE_VIRTUAL_MACHINE", "BROADCAST_SMS", "Telephony.SMS_RECEIVED",
                     ".CommandReceiver", ".ReceivedReceiver", 'android:debuggable="true"'):
            self.assertIn(name, manifest)
        for name in ('"vsock"', '"vm"', '"reset"', "vsock.txt", "vm.txt", "socket: errno=", '"unix"', "connect: errno="):
            self.assertIn(name, "".join(path.read_text() for path in sources))
        javac = jdk_tool("javac")
        if javac is None:
            self.skipTest("no JDK on PATH")
        with tempfile.TemporaryDirectory(prefix="sms-probe-") as classes:
            self.assertEqual(self.syntax_errors(javac, classes, sources), [])
            broken = Path(classes) / "Broken.java"
            broken.write_text("class Broken { int x = ; }\n")
            self.assertEqual(len(self.syntax_errors(javac, classes, [broken])), 1, "a syntax error must be seen")

    # What javac says of a name that Android's classes would have resolved: not a syntax error.
    UNRESOLVED = re.compile(r"cannot find symbol|package \S+ does not exist|does not override or implement a method")

    def syntax_errors(self, javac: str, classes: str, sources: list[Path]) -> list[str]:
        """The errors of a compile that are not unresolved names. The Android classes are not on the host. A JDK of 17
        or later stops after parsing with the options below (the older option name is for the JDKs before it, which
        ignore both); JDK 11 goes on and names every unresolved class, which this filters out."""
        compiled = subprocess.run(
            [javac, "-proc:none", "-XDshould-stop.ifNoError=PARSE", "-XDshould-stop.ifError=PARSE", "-d", classes,
             *(str(path) for path in sources)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120,
            check=False)
        errors = [line.split(" error: ", 1)[1] for line in compiled.stdout.splitlines() if " error: " in line]
        if compiled.returncode != 0 and not errors:
            self.fail(compiled.stdout)
        return [error for error in errors if not self.UNRESOLVED.search(error)]

    PROBE_STUBS = TESTS / "tooling/android/sms/probe-stubs"

    def test_the_shell_probe_uses_only_the_public_surface_and_answers_as_the_runner_reads(self) -> None:
        """build-apk.sh compiles the shell's probe against the platform's public android.jar, which leaves out every
        @hide class (android.system.UnixSocketAddress among them). The stubs in tooling/android/sms/probe-stubs
        declare only the public members the probe uses, so a call to anything else does not compile here; the probe
        then runs against them for the answers that row N5 and the shell's vsock row read."""
        javac, java = jdk_tool("javac"), jdk_tool("java")
        if javac is None or java is None:
            self.skipTest("no JDK on PATH")
        sources = [PROBE / "tool/VsockProbe.java", *sorted(self.PROBE_STUBS.glob("**/*.java"))]
        with tempfile.TemporaryDirectory(prefix="sms-probe-run-") as classes:
            compiled = subprocess.run(
                [javac, "-source", "11", "-target", "11", "-Xlint:all", "-encoding", "UTF-8", "-d", classes,
                 *(str(path) for path in sources)],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120,
                check=False)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)

            def answer(arguments: list[str], **properties: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [java, *(f"-D{name.replace('_', '.')}={value}" for name, value in properties.items()),
                     "-cp", classes, "VsockProbe", *arguments],
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120,
                    check=False)

            handoff = "/dev/socket/kernarium_sms"
            refused = answer(["unix", handoff], probe_connect="Permission denied")
            self.assertEqual(refused.stdout, "connect: errno=13 EACCES\n", refused)
            self.assertIn(f"LocalSocket.connect FILESYSTEM {handoff}", refused.stderr)
            self.assertEqual(answer(["unix", handoff], probe_connect="Connection refused").stdout,
                             "connect: errno=111 ECONNREFUSED\n")
            self.assertEqual(answer(["unix", handoff], probe_connect="No such file or directory").stdout,
                             "connect: errno=2 ENOENT\n")
            self.assertEqual(answer(["unix", handoff]).stdout, "connect: ok\n")
            self.assertEqual(answer(["unix", handoff], probe_connect="Broken pipe").stdout,
                             "connect: error: Broken pipe\n")
            self.assertEqual(answer([]).stdout, "socket: ok\n")
            self.assertEqual(answer([], probe_socket="errno:13").stdout, "socket: errno=13 EACCES\n")

    def test_the_build_script_names_the_probe_and_its_dex(self) -> None:
        script = (PROBE / "build-apk.sh").read_text()
        self.assertIn("AndariumSmsProbe.apk", script)
        self.assertIn("vsockprobe.dex", script)
        self.assertIn(runner.PROBE_PACKAGE, script)
        syntax = subprocess.run(["bash", "-n", str(PROBE / "build-apk.sh")], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
        self.assertEqual(syntax.returncode, 0, syntax.stdout)

    def test_the_verifier_keeps_the_probe_out_of_an_image(self) -> None:
        verifier = (TESTS.parent / "android/lineage-24/tools/verify-target-files.py").read_text()
        self.assertIn('"AndariumSmsProbe"', verifier)


if __name__ == "__main__":
    unittest.main()
