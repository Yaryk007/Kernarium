# SPDX-License-Identifier: GPL-2.0-or-later
"""A device and an Xcode that exist only as files, for the iPad automation tests.

`FakeDevice` puts two executables first on PATH: an `xcrun` that implements
the `devicectl device copy` calls the script makes against one directory (the
runner's data container), and an `xcodebuild` that plays the host. Every
process the script starts is therefore real, the supervisor included, and no
call reaches a real device or a real Xcode. What the fake `xcodebuild` does
comes from a queue of scenarios the test sets; each start takes the next one
and the last one repeats.

Scenario kinds:

- `serve`: the runner comes up, beats every 0.1 s and answers commands. With
  `hold` it serves only once the test says `go` for it. The test then ends it
  with `FakeDevice.control(nonce, action)`: `die` (xcodebuild exits),
  `lifetime` (a clean end at the host's own deadline), `relaunch` (xcodebuild
  starts the runner again, which never serves and never says anything),
  `relaunch-quiet` (the same, and xcodebuild ends with no word of whether
  the runner initialized, `quiet_exit_after` seconds later, 0 by default),
  `relaunch-initialized` (the runner initialized: xcodebuild's restart
  notice and the totals of a run of no tests, as its log shows them, and
  xcodebuild ends),
  `relaunch-initialized-then-timeout` (and then the runner started again
  fails as `relaunch-timeout` does; `-authentication` likewise),
  `relaunch-restarting` (the restart notice, and then nothing: a log cut
  in the middle of the pattern), `relaunch-timeout`, `relaunch-authentication` and
  `relaunch-authentication-localized` (the same, and the device's approval
  was not given: XCTest's timeout line, or the authentication cancelled with
  an English or a localized description, `relaunch_after` seconds after the
  runner came up, 0.3 by default), `silent` (the runner stops
  beating and xcodebuild stays) or `busy` (a command that never ends shows in
  the heartbeat, which keeps beating).
- `serve` also takes the control `forged-diagnostic` (a command's failure text
  that carries the lost-connection lines of another process: a healthy host),
  `launch-failed` (xcodebuild reports a failed launch, CoreDevice error 10002,
  once, and the host serves on) and the control
  `connection-lost`: xcodebuild reports the
  connection to the device lost (CoreDevice error 3, as its log shows it) and,
  `lost_relaunch_after` seconds later (6 by default), starts the runner again by
  itself and ends with status 65, unless the supervisor ended it first.
- `approval`: the runner comes up, and 0.3 s later XCTest gives up on the
  prompt with its own line and xcodebuild exits with status 65 (`after` sets
  the delay, `signal` the kind of line: `timeout`, `authentication` or
  `authentication-localized`).
- `exit`: xcodebuild exits at once with `status`, as with no device. With
  `unreachable` set the device also stops answering (every `xcrun` call fails)
  until the test says `FakeDevice.unreachable(False)`: a tunnel that dropped
  before any runner came up.
- `lost-during-start`: the runner comes up and the connection to the device is
  lost (CoreDevice error 3), as in a start the device's tunnel drops under;
  xcodebuild would start the runner again and end `lost_exit_after` seconds
  later (6 by default), with status 65, unless the supervisor ended it first.
  With `test` set the runner got as far as the test that serves (xcodebuild
  says it started) before the loss; with `unreachable` set the device also
  stops answering, as for `exit`.
- `no-runner`: xcodebuild hangs and the runner never comes up.
- `up-silent`: the runner comes up and neither serves nor says why.
- `early-death`: the runner comes up and xcodebuild is killed (SIGKILL) 0.3 s
  later (`after` sets the delay) before it says anything, as when whatever
  started it is killed first.

A fake `caffeinate` records its arguments in `caffeinate.jsonl` and holds, as the
real one does, until the process named by `-w` has ended (or it is signalled).

Any scenario with `stubborn` set makes xcodebuild ignore SIGTERM, as a hung
one does: only SIGKILL ends it. With `child` set it also starts a helper
process (its pid is in the invocation record) that outlives it unless
something ends its whole process group.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import sys
import tempfile
import time

# The block xcodebuild prints when the connection to the device is lost, as its
# log shows it (the launch report ids, the pointers and the device are left
# out); `{stamp}` is the line's time and `{pid}` the pid of the xcodebuild that
# prints it. It appears twice, for the launch and for its group. The block of a failed launch (CoreDevice error 10002) follows.
LOST_BLOCK = [
    "{stamp} xcodebuild[{pid}:2] [MT] IDELaunchReport: 0000000000000000:"
    "0000000000000000:Launching KernariumAutomation Finished with error: An error "
    "occurred while communicating with a remote process.",
    "Domain: com.apple.dt.CoreDeviceError",
    "Code: 3",
    "User Info: {",
    '    DVTErrorCreationDateKey = "2026-11-15 01:34:52 +0000";',
    "    IDERunOperationFailingWorker = IDELaunchCoreDeviceWorker;",
    "}",
    "--",
    "The connection was invalidated.",
    "Domain: com.apple.Mercury.error",
    "Code: 1001",
    "User Info: {",
    '    XPCConnectionDescription = "<RemoteXPCPeerConnection 0x0> { state = canceled }";',
    "}",
    "--",
    "",
    "{stamp} xcodebuild[{pid}:2] [MT] IDELaunchReport: 0000000000000000:"
    "0000000000000000: Finished with error: An error occurred while communicating "
    "with a remote process.",
    "Domain: com.apple.dt.CoreDeviceError",
    "Code: 3",
    "User Info: {",
    "}",
    "--",
    "The connection was invalidated.",
    "Domain: com.apple.Mercury.error",
    "Code: 1001",
    "--",
    "",
]
LAUNCH_FAILED_BLOCK = [
    "{stamp} xcodebuild[{pid}:2]  IDELaunchReport: 0000000000000000:"
    "0000000000000000:Launching KernariumAutomation Finished with error: The "
    "application failed to launch.",
    "Domain: com.apple.dt.CoreDeviceError",
    "Code: 10002",
    "User Info: {",
    "    BundleIdentifier = \"example.AutomationRunner.xctrunner\";",
    "}",
    "--",
    "The request to open \"example.AutomationRunner.xctrunner\" failed.",
    "Domain: FBSOpenApplicationServiceErrorDomain",
    "Code: 1",
    "--",
    "",
]

XCRUN = r'''#!@PYTHON@
import os
import shutil
import sys

root = os.environ["FAKE_ROOT"]
args = sys.argv[1:]
if os.path.exists(os.path.join(root, "unreachable")):
    print("ERROR: the device is not connected")
    sys.exit(1)
if args[:3] == ["devicectl", "device", "copy"] and len(args) > 3:
    sources, destination, index = [], None, 4
    while index < len(args):
        if args[index] == "--source":
            sources.append(args[index + 1])
            index += 2
        elif args[index] == "--destination":
            destination = args[index + 1]
            index += 2
        else:
            index += 1
    container = os.path.join(root, "container")
    if args[3] == "from":
        source = os.path.join(container, sources[0])
        if not os.path.isfile(source):
            print("ERROR: no such file")
            sys.exit(1)
        shutil.copyfile(source, destination)
        sys.exit(0)
    target = os.path.join(container, destination)
    os.makedirs(target, exist_ok=True)
    for source in sources:
        shutil.copyfile(source, os.path.join(target, os.path.basename(source)))
    sys.exit(0)
print("ERROR: unsupported call")
sys.exit(1)
'''

XCODEBUILD = r'''#!@PYTHON@
import fcntl
import json
import os
import signal
import subprocess
import sys
import time

LOST_BLOCK = @LOST_BLOCK@
LAUNCH_FAILED_BLOCK = @LAUNCH_FAILED_BLOCK@
root = os.environ["FAKE_ROOT"]
channel = os.path.join(root, "container", "Documents", "automation")
nonce = os.environ.get("TEST_RUNNER_KERNARIUM_AUTOMATION_NONCE", "")
seconds = float(os.environ.get("TEST_RUNNER_KERNARIUM_AUTOMATION_HOST_SECONDS", "0"))


def next_scenario():
    with open(os.path.join(root, "scenarios.lock"), "a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        path = os.path.join(root, "scenarios.json")
        with open(path) as handle:
            queue = json.load(handle)
        if len(queue) > 1:
            with open(path, "w") as handle:
                json.dump(queue[1:], handle)
        return queue[0]


def say(text):
    print(text, flush=True)


def runner_up():
    stamp = time.strftime("%Y-%m-%d %H:%M:%S") + ".123456+0900"
    say(f"{stamp} KernariumAutomation-Runner[1:2] [Default] Running tests...")


def block(lines, stamp, pid):
    for line in lines:
        say(line.replace("{stamp}", stamp).replace("{pid}", str(pid)))


def connection_lost():
    """What xcodebuild's log shows when the connection to the device is lost."""
    block(LOST_BLOCK, time.strftime("%Y-%m-%d %H:%M:%S") + ".158", os.getpid())


def launch_failed():
    """What xcodebuild's log shows when it could not launch a runner."""
    block(LAUNCH_FAILED_BLOCK, time.strftime("%Y-%m-%d %H:%M:%S") + ".544", os.getpid())


def forged_diagnostic():
    """A command's failure text with the lost-connection block in it, as the
    host printed it when a sender's words carried a line break: the first line
    prefixed, the rest not, and the header naming another process."""
    say("AutomationHost: key synthetic failed: unknown modifier ")
    block(LOST_BLOCK[:3], "2000-01-01 00:00:00.000", 1)


def relaunch_initialized(finish=True):
    """What xcodebuild's log shows when the runner it started again came up
    and initialized for UI testing: its restart notice, then the totals of the
    run, which ran no test."""
    runner_up()
    say("    t =      nans Interface orientation changed to Landscape Right")
    say("")
    say("Restarting after unexpected exit, crash, or test timeout; summary will "
        "include totals from previous launches.")
    say("")
    if not finish:
        return
    say("Test Suite 'Selected tests' started at 2026-11-13 23:38:23.798.")
    say("Test Suite 'KernariumAutomation.xctest' started at 2026-11-13 23:38:23.800.")
    say("Test Suite 'KernariumAutomation.xctest' passed at 2026-11-13 23:38:23.801.")
    say("\t Executed 0 tests, with 0 failures (0 unexpected) in 0.000 (0.000) seconds")
    say("Test Suite 'Selected tests' passed at 2026-11-13 23:38:23.801.")
    say("\t Executed 0 tests, with 0 failures (0 unexpected) in 0.000 (0.003) seconds")


def approval_failure(signal_kind):
    """The line XCTest prints when the device's approval was not given: its
    own 60 s timeout, or the owner's authentication cancelled (the description
    is localized on a device; the domain and the code are not)."""
    stamp = time.strftime("%Y-%m-%d %H:%M:%S") + ".654321+0900"
    if signal_kind == "authentication":
        reason = ('Error Domain=com.apple.LocalAuthentication Code=-4 "UI canceled by '
                  'system." UserInfo={NSDebugDescription=UI canceled by system., '
                  "NSLocalizedDescription=UI canceled by system.}")
    elif signal_kind == "authentication-localized":
        reason = ('Error Domain=com.apple.LocalAuthentication Code=-4 "Authentication '
                  'was cancelled." UserInfo={BiometryType=1, '
                  "NSLocalizedDescription=Authentication was cancelled.}")
    else:
        reason = ('Error Domain=com.apple.dt.XCTest.XCTFuture Code=1000 "Timed out '
                  'while enabling automation mode." UserInfo={NSLocalizedDescription='
                  "Timed out while enabling automation mode.}")
    say(f"{stamp} KernariumAutomation-Runner[1:2] [Default] Failed to initialize for UI "
        f"testing: {reason}")
    say("** TEST EXECUTE FAILED **")


def write_state(started, served, finished=False, command=None, deadline=None):
    state = {"nonce": nonce, "pid": os.getpid(), "started": started,
             "time": time.time(), "deadline": deadline or started + seconds,
             "served": served, "finished": finished}
    if command is not None:
        state["command"] = "stuck"
        state["commandStarted"] = command
    path = os.path.join(channel, "state.json")
    with open(path + ".new", "w") as handle:
        json.dump(state, handle)
    os.replace(path + ".new", path)


def action():
    try:
        with open(os.path.join(root, "control", nonce)) as handle:
            return handle.read().strip()
    except OSError:
        return ""


def answer(served):
    """Answers the commands in the inbox; True after a stop."""
    inbox = os.path.join(channel, "inbox")
    outbox = os.path.join(channel, "outbox")
    stopping = False
    for name in sorted(os.listdir(inbox)):
        if not name.endswith(".ready"):
            continue
        ident = name[:-len(".ready")]
        try:
            with open(os.path.join(inbox, ident + ".json")) as handle:
                command = json.load(handle)
        except (OSError, ValueError):
            continue
        os.remove(os.path.join(inbox, ident + ".json"))
        os.remove(os.path.join(inbox, name))
        with open(os.path.join(outbox, ident + ".json"), "w") as handle:
            json.dump({"ok": True, "op": command.get("op"), "nonce": nonce}, handle)
        served[0] += 1
        stopping = stopping or command.get("op") == "stop"
    return stopping


scenario = next_scenario()
if scenario.get("stubborn"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
child = None
if scenario.get("child"):
    # What xcodebuild leaves behind when it dies: a helper in its group.
    child = subprocess.Popen(["sleep", "3600"]).pid
parent = subprocess.run(["ps", "-o", "command=", "-p", str(os.getppid())],
                        stdout=subprocess.PIPE, text=True).stdout.strip()
with open(os.path.join(root, "invocations.jsonl"), "a") as handle:
    handle.write(json.dumps({
        "pid": os.getpid(), "ppid": os.getppid(), "argv": sys.argv[1:],
        "nonce": nonce, "seconds": seconds, "supervisor": parent,
        "scenario": scenario, "child": child, "time": time.time()}) + "\n")
kind = scenario["kind"]
if kind == "exit":
    if scenario.get("unreachable"):
        open(os.path.join(root, "unreachable"), "w").close()
    say("xcodebuild: error: Unable to find a destination matching the provided destination specifier")
    sys.exit(scenario.get("status", 70))
if kind == "no-runner":
    time.sleep(3600)
runner_up()
if kind == "up-silent":
    time.sleep(3600)
if kind == "lost-during-start":
    if scenario.get("test"):
        say("Test Case '-[KernariumAutomation.AutomationHost testServe]' started.")
    if scenario.get("unreachable"):
        open(os.path.join(root, "unreachable"), "w").close()
    connection_lost()
    time.sleep(scenario.get("lost_exit_after", 6))
    sys.exit(65)
if kind == "early-death":
    time.sleep(scenario.get("after", 0.3))
    os.kill(os.getpid(), signal.SIGKILL)
if kind == "approval":
    time.sleep(scenario.get("after", 0.3))
    approval_failure(scenario.get("signal", "timeout"))
    sys.exit(65)
for sub in ("inbox", "outbox"):
    os.makedirs(os.path.join(channel, sub), exist_ok=True)
for name in os.listdir(os.path.join(channel, "inbox")):
    os.remove(os.path.join(channel, "inbox", name))
while scenario.get("hold") and action() != "go":
    time.sleep(0.05)
say("Test Case '-[KernariumAutomation.AutomationHost testServe]' started.")
started = time.time()
served = [0]
busy = None
reported = False
while True:
    now = action()
    if now == "launch-failed" and not reported:
        reported = True
        launch_failed()
    if now == "forged-diagnostic" and not reported:
        reported = True
        forged_diagnostic()
    if now == "die":
        sys.exit(1)
    if now == "lifetime":
        write_state(started, served[0], finished=True, deadline=time.time())
        sys.exit(0)
    if now == "relaunch":
        runner_up()
        time.sleep(3600)
    if now == "connection-lost":
        # xcodebuild starts the runner again by itself some time later, which
        # never initializes; only an end of xcodebuild before it prevents that.
        connection_lost()
        time.sleep(scenario.get("lost_relaunch_after", 6))
        runner_up()
        time.sleep(0.3)
        say("** TEST EXECUTE FAILED **")
        sys.exit(65)
    if now == "relaunch-initialized":
        relaunch_initialized()
        say("** TEST EXECUTE FAILED **")
        sys.exit(65)
    if now == "relaunch-restarting":
        relaunch_initialized(finish=False)
        time.sleep(3600)
    if now.startswith("relaunch-initialized-then-"):
        # One runner initialized; the next one, started again, asks the device.
        relaunch_initialized()
        time.sleep(scenario.get("relaunch_after", 0.3))
        runner_up()
        time.sleep(scenario.get("relaunch_after", 0.3))
        approval_failure(now[len("relaunch-initialized-then-"):])
        sys.exit(65)
    if now.startswith("relaunch-"):
        # The runner started again asks the device for its approval, or not.
        runner_up()
        if now == "relaunch-quiet":
            say("Test Suite 'Selected tests' passed at 2026-11-13 23:59:35.792.")
            time.sleep(scenario.get("quiet_exit_after", 0))
            sys.exit(65)
        time.sleep(scenario.get("relaunch_after", 0.3))
        approval_failure(now[len("relaunch-"):])
        sys.exit(65)
    if now == "silent":
        time.sleep(3600)
    if now == "busy" and busy is None:
        busy = time.time()
    if answer(served):
        write_state(started, served[0], finished=True)
        sys.exit(0)
    write_state(started, served[0], command=busy)
    time.sleep(0.1)
'''

CAFFEINATE = r'''#!@PYTHON@
import json
import os
import subprocess
import sys
import time

with open(os.path.join(os.environ["FAKE_ROOT"], "caffeinate.jsonl"), "a") as handle:
    handle.write(json.dumps({"pid": os.getpid(), "argv": sys.argv[1:],
                             "time": time.time()}) + "\n")
arguments = sys.argv[1:]
watched = int(arguments[arguments.index("-w") + 1]) if "-w" in arguments else None


def alive(pid):
    shown = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)],
                           stdout=subprocess.PIPE, text=True).stdout.strip()
    return bool(shown) and not shown.startswith("Z")


# The real tool holds until the process named by -w ends, however it ends.
while watched is not None and alive(watched):
    time.sleep(0.05)
'''

GOOD_SETTINGS = {
    "PreferredScreenCaptureFormat": "screenshots",
    "SystemAttachmentLifetime": "keepNever",
    "UserAttachmentLifetime": "keepNever",
}


class FakeDevice:
    """The files, scripts and environment of one fake device."""

    def __init__(self, udid: str = "00000000-0000000000000000") -> None:
        self.udid = udid
        self.scratch = tempfile.TemporaryDirectory(prefix="automation-fake-")
        self.root = Path(self.scratch.name).resolve()
        self.bin = self.root / "bin"
        self.state_dir = self.root / "state"
        self.derived = self.root / "derived"
        self.reports = self.root / "reports"
        self.channel = self.root / "container/Documents/automation"
        self.bin.mkdir()
        for name, template in (("xcrun", XCRUN), ("xcodebuild", XCODEBUILD),
                               ("caffeinate", CAFFEINATE)):
            path = self.bin / name
            path.write_text(template.replace("@PYTHON@", sys.executable)
                            .replace("@LOST_BLOCK@", repr(LOST_BLOCK))
                            .replace("@LAUNCH_FAILED_BLOCK@",
                                     repr(LAUNCH_FAILED_BLOCK)))
            path.chmod(0o755)
        self.environment = dict(
            os.environ, FAKE_ROOT=str(self.root),
            PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}")
        products = self.derived / "Build/Products"
        products.mkdir(parents=True)
        target = {"TestBundlePath": "__TESTHOST__/PlugIns/Host.xctest",
                  **GOOD_SETTINGS}
        with open(products / "KernariumAutomation_fake.xctestrun", "wb") as handle:
            plistlib.dump({"__xctestrun_metadata__": {"FormatVersion": 2},
                           "TestConfigurations": [{"Name": "Test",
                                                   "TestTargets": [target]}]},
                          handle)
        self.scenarios([{"kind": "serve"}])

    def scenarios(self, queue: list[dict]) -> None:
        (self.root / "scenarios.json").write_text(json.dumps(queue))

    def unreachable(self, flag: bool) -> None:
        path = self.root / "unreachable"
        if flag:
            path.write_text("")
        elif path.exists():
            path.unlink()

    def control(self, nonce: str, action: str) -> None:
        """Tells the fake host that has `nonce` to `go` (leave its hold) or to
        `die`, `lifetime`, `relaunch`, `silent` or `busy`."""
        directory = self.root / "control"
        directory.mkdir(exist_ok=True)
        (directory / nonce).write_text(action)

    def silent_for(self) -> float:
        """How long ago the host last beat; infinite before its first beat."""
        state = self.heartbeat()
        return time.time() - state["time"] if state else float("inf")

    def invocations(self) -> list[dict]:
        path = self.root / "invocations.jsonl"
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text().splitlines() if line]

    def caffeinated(self) -> list[dict]:
        """The `caffeinate` processes the script started, in order."""
        path = self.root / "caffeinate.jsonl"
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text().splitlines() if line]

    def heartbeat(self) -> dict | None:
        try:
            return json.loads((self.channel / "state.json").read_text())
        except (OSError, ValueError):
            return None

    def serving(self, nonce: str | None = None) -> bool:
        """Whether the host the heartbeat names runs and has not finished:
        its process is alive, which no wall-clock threshold says."""
        state = self.heartbeat()
        return bool(state and not state.get("finished")
                    and self.alive(state["pid"])
                    and (nonce is None or state["nonce"] == nonce))

    def wait(self, condition, seconds: float, what: str):
        """The condition's value once it holds; fails after `seconds`."""
        deadline = time.monotonic() + seconds
        while True:
            value = condition()
            if value:
                return value
            if time.monotonic() > deadline:
                raise AssertionError(f"not within {seconds} s: {what}")
            time.sleep(0.05)

    def alive(self, pid: int) -> bool:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        shown = subprocess.run(
            ["ps", "-o", "stat=", "-p", str(pid)], stdout=subprocess.PIPE,
            text=True).stdout.strip()
        return bool(shown) and not shown.startswith("Z")

    def kill_hosts(self) -> None:
        """Ends every fake host and supervisor this device started: only a
        process whose command line names this device's own scratch folder."""
        for record in self.invocations():
            for pid in (record["pid"], record["ppid"]):
                shown = subprocess.run(
                    ["ps", "-o", "command=", "-p", str(pid)],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                    text=True).stdout
                if str(self.root) in shown:
                    try:
                        os.killpg(pid, signal.SIGKILL)
                    except (ProcessLookupError, PermissionError):
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except (ProcessLookupError, PermissionError):
                            pass

    def kill_caffeinate(self) -> None:
        for record in self.caffeinated():
            shown = subprocess.run(
                ["ps", "-o", "command=", "-p", str(record["pid"])],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                text=True).stdout
            if str(self.root) in shown:
                try:
                    os.kill(record["pid"], signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass

    def cleanup(self) -> None:
        self.kill_caffeinate()
        self.kill_hosts()
        self.scratch.cleanup()
