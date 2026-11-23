#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive UI automation on a connected iPad through one long-lived host.

Each XCTest session that turns UI automation on can make the device ask for
its owner's passcode or biometrics. The KernariumAutomation UI-test target
keeps one session alive for hours and executes commands this script places in
its runner's data container over `devicectl`, so one approval at the device
serves every later command. Build the host once, start it, approve the prompt
at the device, and then send commands:

  ipad-automation.py --device UDID build
  ipad-automation.py --device UDID start --hours 8
  ipad-automation.py --device UDID launch --env KERNARIUM_ROOTFS=none.erofs
  ipad-automation.py --device UDID tap --id displayModeButton
  ipad-automation.py --device UDID orientation landscapeLeft
  ipad-automation.py --device UDID --bundle com.apple.springboard drag 500 700 400 600 1
  ipad-automation.py --device UDID screenshot -o shot.png
  ipad-automation.py --device UDID stop --force

Every command has a bounded wait. A command fails with exit status 3 when the
host is not running. UI automation stays approved only while some XCTest
session keeps it on, so a new host normally needs a new approval at the
device: never stop a working host without someone to approve its successor.
Run `start` only while someone can approve, because XCTest gives up 60 s after
the runner starts and leaves the prompt on screen. The host targets
com.imxieyi.Kernarium unless --bundle names another application, such as
com.apple.springboard. A `launch` of Andarium (com.imxieyi.andarium) keeps the
phone silent by stating ANDARIUM_HAPTICS=0; `launch --haptics` leaves that out
for a run that tests haptics. `activate` of an Andarium that is not running is
refused, since the host starts it with no environment: use `launch`
(`activate --haptics` starts it for a run that tests haptics).

A coordinate tap or drag names points as offsets from the origin of that
application's frame, which `tree` prints; springboard's frame is the whole
screen. A drag presses its start for the given seconds before moving, which is
what picks up a window's resize handle. `orientation` rotates the device and
reports the orientation it reached; without a name it only reports.

No command ends the host: a failed command reports its error, and the host
prints it to its log. If every command starts taking about 60 s, a system
keyboard animation has left XCTest's wait for an idle application stuck;
relaunch the application. The host is not in the Kernarium scheme, so
reinstalling Kernarium or running other tests leaves it serving, and its log
records each such termination as a Kernarium crash.

A host ends on its own now and then: its runner dies under memory pressure
on a small device, its lifetime runs out, or the process that started it goes
away. Automation mode ends with the runner, and the next runner turns it on
again without the owner while the system still holds the last approval, however
soon it starts; it needs the owner only when the system lost it. xcodebuild
starts a runner that iOS ended again at once, and its log says which case it
was: "Restarting after unexpected exit" then "Executed 0 tests" means the new
runner initialized (a new host then serves with nobody at the device), and
"Failed to initialize for UI testing" means the owner is needed. A runner that
xcodebuild starts after the connection to the device was lost (CoreDevice error
3, as when the Mac's CoreDevice service crashes) cannot initialize, so the
host's supervisor tries to end xcodebuild as soon as its log reports that
error, before the runner is started again (best effort, not a guarantee): the
host's death is then a plain one. `watch` keeps one device's host serving:

  ipad-automation.py --device UDID watch --hours 8
  ipad-automation.py --device UDID watch --detach
  mkdir -p STATE_DIR/UDID
  ipad-automation.py --device UDID watch --print-launchd-job LABEL > job.plist
  ipad-automation.py --device UDID watch --print-launchd-job LABEL --launchd-log PATH

The same `watch` runs in a terminal's foreground, detached by `--detach` (its
log is watch-<time>.log in the state directory), or as a job manager's job
(`--print-launchd-job` prints a launchd job with absolute paths and its own
PATH, which runs once and is never restarted by launchd: a watch that crashes
stays gone, its host keeps serving, and `status` shows the watch missing;
printing creates nothing, so the `mkdir` above, for the job's log directory,
comes before the job loads, and the job's registration is the owner's).
launchd must be able to open the job file and the log itself, so both belong on
a volume with ownership enabled (the internal one) while the state directory
may be anywhere: `--launchd-log` names the log, which is otherwise
STATE_DIR/UDID/watch-launchd.log.

The printed job asks launchd for interactive scheduling (ProcessType
Interactive, LegacyTimers), and a running watch holds an idle-sleep assertion
(`caffeinate -i -w PID`: never the display) that ends with it. The watch logs a
line, with the gap, whenever a turn of its loop starts more than 15 s after the
one before it.

The watch acts only on evidence. A wrong restart ends a host that works, or
puts an approval prompt on the device that nobody answers; a missed one costs
the next look at `status`. Where it cannot tell, it replaces nothing, says
what it could not establish and ends with a status of its own.

- A host serves when its heartbeat advances between two reads, and a serving
  host is never stopped or restarted. A host is dead when its heartbeat says
  it finished, or when 3 reads in a row all worked and all showed the same
  beat, the last one started 20 s after the first one ended (5 s once its
  supervisor ended or xcodebuild started the runner again), or when one
  command has run longer than 30 minutes and than the process that sent it
  waits for. A read's time is when it started, so a late delivery of an old
  beat is not silence; a read that fails or shows another host's beat is
  unknown and starts the count again, whether the watch has run for a day or
  has just started, and a sleep of the Mac does the same. A host's heartbeat
  cannot be read for 15 minutes: the watch ends with status 7 and the state
  cannot-observe, having replaced nothing. A device that never had a host
  (nothing to read) is one it cannot tell: `start` makes the first.
- Retiring the old host and starting the next run under the launch lock that
  `start` takes, from before the watch checks again that the heartbeat is still
  the one it judged, and that no start was published meanwhile (either one
  changed: it looks again), until the new host is published. The old host's
  whole process group (supervisor, xcodebuild and what else is in it) must
  be listed and empty before anything starts; what the watch cannot show is the
  host's, cannot list, or cannot name at all (the supervisor records its own
  identity before it spawns anything) stops it with status 8 and the state
  remainder, naming the pids, and nothing starts beside it. A group is signalled only
  while the watch holds the supervisor it started, or while a member's command
  line carries the host's nonce, and then all of it, with SIGKILL for what
  ignores SIGTERM.
- The device may have asked for an approval that nobody gave whenever that
  cannot be excluded: the log shows XCTest's line "Timed out while enabling
  automation mode", or the "Failed to initialize for UI testing" line with a
  LocalAuthentication error of code -4 or -2 (the owner's authentication
  cancelled, by the system or the owner), or a runner came up that the log does
  not show initialized: one that xcodebuild started again after the host's
  runner died and whose restart notice and "Executed 0 tests" the log does not
  show (a relaunch that initialized is no risk, and the host is replaced at
  once; a log that ends before it says is not shown), or one of a launch that
  never served and whose test (`testServe`, which runs only after the runner
  initialized for UI testing) never started. That is recorded (the
  file approval.json in the state directory) before anything is ended and read
  again after, and the watch ends with status 4 and the state needs-approval,
  which `status` reports. It starts nothing again until a host serves: a
  `start` with the owner at the device that serves, or a heartbeat that
  advances. A host's heartbeat is written by the test that runs only after
  XCTest initialized the runner for UI testing, so a serving host has its
  approval; the next runner after it dies may not.
- A replacement serves when a heartbeat with its own new nonce advances. One
  that was started and does not serve ends the watch; the watch starts no
  second host after one death, except after a start that asked nothing of the
  device: one refused before anything started (device not connected, report
  slots short), and one that ended with the device's connection lost or
  unreadable before its runner came up or after its test started (a runner
  that came up and lost the connection before the test started may have asked
  for an approval, and is the owner's). After a refusal the watch waits 5, 10
  and 20 s; after a host that ended it waits up to 180 s for the device to
  answer (a connected device with no heartbeat to read answers), then 5, 10 and
  20 s; it makes 4 starts at most.
  Any other launch whose runner never came up (xcodebuild ended, or the runner
  did not come up in --wait), and a device that does not answer in that time,
  end the watch with status 5 and start-failed, and no latch. No built host, or
  a run file that lets Xcode record the screen, ends it at once with the state
  cannot-start; more than 6 replacements (--max-restarts) in an hour, counted
  across a watch that ended mid-replacement too, end it with restart-storm.

Status 6: another watch holds this device's lock. `status` shows the watch and
its last end. A watch also waits for a host that a `start` is still bringing up,
for as long as that `start` waits, and `start` and `watch` launch one at a time.

A command sent while the watch is starting a host, or has not seen the host
beat for 15 s, waits for a host that beats, up to 180 s (a hard bound), and
fails with status 3 as before when no watch runs or the watch has ended. A
command that is running or waiting when its host ends is not repeated for the
replacement, which starts with an empty inbox: it may have run, and its
sender gets the same status 3. `stop --force` ends the watch first (only the
process that holds this device's watch lock), so that it does not start the host
again, and then the host; `start` refuses while a watch runs. Ending the watch
alone (SIGTERM, or the job manager's stop) leaves a serving host running.

The host's test run records no video of the screen and keeps no attachments: a
host runs for hours, and Xcode's defaults would store a recording of whatever
the device shows. The KernariumAutomation scheme turns both off, `start`
refuses a built test run file that does not say so, and the run's result
bundle lives next to the host log in --state-dir (a failed run collects no
device diagnostics into it). `start`, `launch` and
`activate` first make room for diagnostic reports on the device
(tests/integration/support/ios_device_reports.py), so a kill during the run
leaves a report.
"""

import argparse
import contextlib
import fcntl
import json
import os
import pathlib
import plistlib
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid
from xml.parsers import expat

RUNNER = "com.imxieyi.KernariumAutomation.xctrunner"
KERNARIUM = "com.imxieyi.Kernarium"
CHANNEL = "Documents/automation"
TEST = "KernariumAutomation/AutomationHost/testServe"
HEARTBEAT_STALE = 20
EXIT_FAILED = 1
EXIT_NO_HOST = 3
# `watch` ends with one of these when it gave up: the owner is needed at the
# device, or it stopped trying for a reason its record names; when another
# watch for the same device and state directory already runs; or when it could
# not establish what it needs to act (it acts only on evidence, and otherwise
# says what it could not establish and ends).
EXIT_NEEDS_APPROVAL = 4
EXIT_WATCH_GAVE_UP = 5
EXIT_WATCH_LOCKED = 6
EXIT_WATCH_CANNOT_OBSERVE = 7
EXIT_WATCH_REMAINDER = 8
# How long `stop` lets the supervisor end on its own before signalling it.
SUPERVISOR_EXIT_WAIT = 60

# The watch (see `watch` in the module's help). Every wait it makes is bounded
# by these, in seconds.
#
# How often it looks at the host's processes and log, and how often it reads
# the heartbeat when those show nothing wrong.
WATCH_POLL = 2
WATCH_PULL_EVERY = 5
# The host beats every 2 s from a queue its commands never block. A host is
# dead only on evidence: WATCH_DEAD_PULLS consecutive reads that all worked
# and all showed the same beat, the last started this long after the first
# ended: WATCH_GONE_AGE once its supervisor ended or xcodebuild started the
# runner again, HEARTBEAT_STALE otherwise. A read that fails or shows another
# host's beat says nothing, and starts the count again.
WATCH_GONE_AGE = 5
WATCH_DEAD_PULLS = 3
# The heartbeat cannot be read for this long: the watch ends without having
# replaced anything, as it does not know the host is gone.
WATCH_UNKNOWN_LIMIT = 900
# One command that has run longer than this, and longer than the process that
# sent it waits for it, is a host whose main thread is hung.
WATCH_BUSY_LIMIT = 1800
# The wall clock gaining this much on the monotonic one (which stops while the
# Mac sleeps) is a sleep or a clock change: what the watch saw before it says
# nothing about the host now.
WATCH_CLOCK_GAP = 10
# The watch polls every WATCH_POLL; a turn of its loop that starts this much
# later than the one before it was not scheduled (or was held by a read of the
# device), and the watch says so, with the gap.
WATCH_STALL = 15
# After "Running tests..." XCTest gives the device's approval prompt 60 s: a
# runner that is up and neither serves nor says why within this is ended, and
# what it may have asked of the device is not known.
WATCH_RUNNER_WAIT = 90
# Attempts to start a host that does not come to anything the device could
# have been asked for: one that is refused before anything starts (the device
# is not connected, the report store has no room) and one that ended with the
# device's connection lost before its runner came up or after its test started;
# and the pauses between them. After an attempt that ended, the watch first
# waits up to WATCH_RECONNECT_WAIT for the device to answer again.
WATCH_ATTEMPTS = 4
WATCH_BACKOFF = (5, 10, 20)
WATCH_RECONNECT_WAIT = 180
# A read of the device's heartbeat made while the watch replaces a host (or
# waits for the device) is given this long, so that a stop is not held up by
# one that never returns.
WATCH_READ_WAIT = 20
# More replacements than this in WATCH_STORM_WINDOW end the watch.
WATCH_MAX_RESTARTS = 6
WATCH_STORM_WINDOW = 3600
# How long a command waits for a replacement the watch is making (a hard
# bound), how recently the watch must have seen the host's beat advance for a
# command to be sent, and how long `stop --force` lets the watch end and
# `watch --detach` lets the new watch take its lock.
WATCH_COMMAND_WAIT = 180
WATCH_ALIVE_FRESH = 15
WATCH_END_WAIT = 90
WATCH_DETACH_WAIT = 20
# How often a command waiting for its result looks at the host's heartbeat.
SEND_LIVENESS_EVERY = 10
# How long a launch waits for another launch of the same device (`start` or
# the watch) to finish its transition.
LAUNCH_LOCK_WAIT = 300
# How long a supervisor lets xcodebuild, and then what else is in its process
# group, obey SIGTERM before SIGKILL; and how long what ends a supervisor
# waits for it to be done (its two waits, with room).
CHILD_TERM_WAIT = 10
SUPERVISOR_TERM_WAIT = 30
# How long a process group that shows nothing of being the host's is looked at
# again before it is called another group's.
FOREIGN_SETTLE = 2.0
# xcodebuild's own lines: the runner is up, and XCTest gave up on the prompt.
RUNNER_UP_LINE = "Running tests..."
# xcodebuild's line for the start of the test that serves. The test runs only
# after the runner initialized for UI testing, so the device's approval was
# held or given by then.
TEST_STARTED_LINE = ("Test Case '-[KernariumAutomation.AutomationHost testServe]' "
                     "started.")
APPROVAL_TIMEOUT_LINE = "Timed out while enabling automation mode"
INIT_FAILURE_LINE = "Failed to initialize for UI testing"
# What xcodebuild prints when the runner it started again after the first one
# died has initialized: its restart notice, then the totals of that run, which
# ran no test (the one that serves is not run again).
RESTART_LINE = "Restarting after unexpected exit"
NO_TESTS_LINE = "Executed 0 tests"
# The same line when the owner's authentication for it was cancelled: by the
# owner (LAError -2) or by the system (-4, "UI canceled by system", as when the
# screen locks during the prompt). The description is localized; the domain
# and the code are not.
AUTHENTICATION_CANCELLED = re.compile(
    r"Error Domain=com\.apple\.LocalAuthentication Code=-([24])\b")
EXIT_STATUS_LINE = "ipad-automation: xcodebuild exited with status"
# xcodebuild reports the end of the runner's launch session with a block of
# its own: a header line (its time, process and "IDELaunchReport", ending in
# "Finished with error: <text>"), then the error's domain and code on the next
# two lines. The block of a lost connection to the device is CoreDevice error
# 3 ("An error occurred while communicating with a remote process", nested
# "The connection was invalidated"): xcodebuild then starts the runner again
# by itself, 18 s later in the one observed case, and that runner cannot
# initialize. Any other CoreDevice error does not match (code 10002, "The
# application failed to launch", is xcodebuild's own failed relaunch: it ends
# by itself), and no line of a healthy run does: only xcodebuild's own lines
# start with its time and process name. The supervisor takes a header only
# from the xcodebuild it started (its pid), and a line ends only at a newline:
# text the runner prints (a diagnostic with line breaks in it) cannot form the
# block unless it knows that pid and writes the three lines.
LAUNCH_ERROR_LINE = re.compile(
    r"^\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)\.\d+ xcodebuild\[(\d+):\d+\] +"
    r"(?:\[MT\] )?IDELaunchReport: .*Finished with error: ")
CORE_DEVICE_DOMAIN = "Domain: com.apple.dt.CoreDeviceError"
CONNECTION_LOST_CODE = "3"
# The time at the start of a line xcodebuild or the runner prints.
LOG_TIME = re.compile(r"^\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)\.\d+")
# Why a verdict no longer stands when the watch checks it under the launch
# lock; these two are gaps in what the watch has seen of the host.
UNREADABLE_NOW = "the heartbeat cannot be read now"
OTHER_HOST_NOW = "the heartbeat is another host's now"
# The watch record's states: the first three while the watch runs, the others
# when it has ended.
WATCHING, RESTARTING, STARTING = "watching", "restarting", "starting"
NEEDS_APPROVAL, START_FAILED = "needs-approval", "start-failed"
RESTART_STORM, CANNOT_START = "restart-storm", "cannot-start"
CANNOT_OBSERVE, REMAINDER = "cannot-observe", "remainder"
STOPPED, WATCH_ERROR = "stopped", "error"
ACTIVE_STATES = (WATCHING, RESTARTING, STARTING)
# What the watch's record says for each ended state.
STATE_TEXT = {
    NEEDS_APPROVAL: "needs approval at the device",
    START_FAILED: "could not start a host",
    RESTART_STORM: "gave up: too many restarts",
    CANNOT_START: "cannot start a host here",
    CANNOT_OBSERVE: "could not observe the host, and replaced nothing",
    REMAINDER: "a remainder of the old host is still there: nothing was started",
    STOPPED: "ended on request",
    WATCH_ERROR: "ended on an error",
}
# What the host's test run file must say (the KernariumAutomation scheme sets
# it). Xcode's defaults record the device's screen for the whole session and
# keep every attachment, and a host runs for hours: that filled the internal
# disk with a video of whatever the device showed.
RECORDING_SETTINGS = {
    "PreferredScreenCaptureFormat": "screenshots",
    "SystemAttachmentLifetime": "keepNever",
    "UserAttachmentLifetime": "keepNever",
}
SCRIPT = pathlib.Path(__file__).resolve()
CHECKOUT = SCRIPT.parents[2]
sys.path.insert(0, str(CHECKOUT / "tests" / "integration" / "support"))
import andarium  # noqa: E402
import ios_device_reports  # noqa: E402

# The processes whose diagnostic reports a host session needs free slots for:
# the applications it drives and its own runner.
REPORT_PROCESSES = (*ios_device_reports.APPLICATION_PROCESSES,
                    "KernariumAutomation-Runner")
# The process an application's reports are named for, by bundle identifier;
# any other bundle is taken to be named like the last part of its identifier.
BUNDLE_PROCESSES = {"com.imxieyi.Kernarium": "Kernarium",
                    "com.imxieyi.andarium": "Andarium"}
# The host operations that start an application on the device.
STARTING_COMMANDS = ("launch", "activate")

# XCTest types a key from the character it produces. On a device it delivers
# nothing for the characters of XCUIKeyboardKey.return ("\r"), .delete
# ("\x7f") and .escape ("\x1b"), or for a backspace ("\b"): no event reaches
# any responder of the application, a plain UITextField included, and the
# command still succeeds. "\n" produces the real Return key (HID usage 0x28),
# modifiers included. Key names therefore resolve to characters that arrive,
# and the characters that would be dropped are refused with a working route.
# The arrows are the iOS values of XCUIKeyboardKey, the arrow symbols; the
# macOS values, U+F700 to U+F703, press nothing on a device.
KEYS = {
    "return": "\n",
    "tab": "\t",
    "space": " ",
    "up": "\u2191",
    "down": "\u2193",
    "left": "\u2190",
    "right": "\u2192",
}
UNDELIVERABLE = {
    "delete": "XCTest drops a synthesized Delete key; `type $'\\x7f'` deletes "
              "through the text input system instead",
    "escape": "XCTest drops a synthesized Escape key; the terminal's key bar "
              "sends one with `tap --label Escape`",
}
# The key each dropped character stands for.
DROPPED = {"\r": "return", "\x7f": "delete", "\b": "delete", "\x1b": "escape"}
# The UIDeviceOrientation cases XCUIDevice can be set to.
ORIENTATIONS = ("portrait", "portraitUpsideDown", "landscapeLeft", "landscapeRight")


class NoHost(Exception):
    pass


def log(message):
    print(f"ipad-automation: {message}", file=sys.stderr, flush=True)


def devicectl(args, device, timeout):
    """Runs one devicectl device subcommand; returns (status, output)."""
    command = ["xcrun", "devicectl", "device", *args, "--device", device]
    try:
        done = subprocess.run(
            command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, f"devicectl timed out after {timeout} s"
    except OSError as error:
        return 127, f"devicectl cannot run: {error}"
    return done.returncode, done.stdout


def container_args(options):
    return ["--domain-type", "appDataContainer",
            "--domain-identifier", options.runner]


def pull(options, source, destination, timeout=60):
    status, _ = devicectl(
        ["copy", "from", "--source", source, "--destination", str(destination),
         *container_args(options)], options.device, timeout)
    return status == 0 and pathlib.Path(destination).exists()


def push(options, sources, destination, timeout=60):
    args = ["copy", "to"]
    for source in sources:
        args += ["--source", str(source)]
    status, output = devicectl(
        [*args, "--destination", destination, *container_args(options)],
        options.device, timeout)
    if status != 0:
        raise NoHost(f"cannot copy into the runner's container: {output.strip()[-400:]}")


class Clock:
    """The time the watch measures with; a test steps its own by hand."""

    def monotonic(self):
        return time.monotonic()

    def time(self):
        return time.time()

    def sleep(self, seconds):
        time.sleep(seconds)

    def wait(self, event, seconds):
        """True when `event` is set within `seconds`."""
        return event.wait(seconds)


CLOCK = Clock()


def write_json(path, data):
    """Writes `data` whole and moves it into place: a reader never sees half
    of it."""
    temporary = path.with_name(f"{path.name}.{os.getpid()}.{threading.get_ident()}")
    temporary.write_text(json.dumps(data))
    os.replace(temporary, path)


def read_json(path):
    try:
        value = json.loads(path.read_text())
    except (OSError, ValueError):
        return {}
    return value if isinstance(value, dict) else {}


def host_state(options, timeout=60):
    """The device-side heartbeat, or None when there is none (or it cannot be
    read: the two are not told apart)."""
    with tempfile.TemporaryDirectory() as scratch:
        local = pathlib.Path(scratch) / "state.json"
        if not pull(options, f"{CHANNEL}/state.json", local, timeout):
            return None
        try:
            state = json.loads(local.read_text())
        except ValueError:
            return None
        return state if isinstance(state, dict) else None


# What devicectl says when the file it was asked for is not on the device, as
# against what it says of a device that is not connected.
MISSING_FILE = re.compile(r"no such file|NSCocoaErrorDomain Code=260|does not exist",
                          re.IGNORECASE)


def second_read(options, timeout=60):
    """(answered, state): a second copy of the heartbeat file, made after the
    first gave nothing, and what it tells. A copy that works gives the
    heartbeat in it (none when it is not a heartbeat). The device answers with
    no heartbeat when the file is not there (no host ever wrote one, or its
    container was cleared). A device that is not connected says something
    else."""
    with tempfile.TemporaryDirectory() as scratch:
        local = pathlib.Path(scratch) / "state.json"
        status, output = devicectl(
            ["copy", "from", "--source", f"{CHANNEL}/state.json",
             "--destination", str(local), *container_args(options)],
            options.device, timeout)
        state = None
        if status == 0:
            try:
                state = json.loads(local.read_text())
            except (OSError, ValueError):
                state = None
    if status == 0:
        return True, state if isinstance(state, dict) else None
    return bool(MISSING_FILE.search(output)), None


def device_reply(options, timeout=60, clock=CLOCK, until=None, cancelled=None):
    """(answered, state): whether the device answers a read, and the heartbeat
    it gave, from whichever read worked. It answers by giving its heartbeat, or
    by saying that there is none to give: a connected device with no host to
    read answers (with no state). Both reads are made within `timeout` in all,
    and none begins at or after `until` (on `clock`) or once `cancelled()` says
    the caller was told to end: the second read is not made, and the device is
    not taken to answer."""
    began = clock.monotonic()
    state = host_state(options, timeout)
    if state is not None:
        return True, state
    now = clock.monotonic()
    left = timeout - (now - began)
    if until is not None:
        left = min(left, until - now)
    if left <= 0 or (cancelled is not None and cancelled()):
        return False, None
    return second_read(options, left)


def liveness(state):
    """Why the host is not serving, or None when it is."""
    if state is None:
        return "no heartbeat in the runner's container"
    if state.get("finished"):
        return "the host has finished"
    age = time.time() - state.get("time", 0)
    if age > HEARTBEAT_STALE:
        return f"the heartbeat is {age:.0f} s old"
    return None


def state_dir(options):
    directory = pathlib.Path(options.state_dir) / options.device
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def state_path(options):
    """The device's state directory, without creating it."""
    return pathlib.Path(options.state_dir) / options.device


def local_record(options):
    return read_json(state_dir(options) / "host.json")


def process_table():
    """Every process as (pid, pgid, stat, command); None when `ps` fails."""
    try:
        done = subprocess.run(
            ["ps", "-ax", "-o", "pid=,pgid=,stat=,command="],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, errors="replace", timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if done.returncode != 0:
        return None
    rows = []
    for line in done.stdout.splitlines():
        parts = line.split(None, 3)
        if len(parts) >= 3 and parts[0].isdigit() and parts[1].isdigit():
            rows.append((int(parts[0]), int(parts[1]), parts[2],
                         parts[3] if len(parts) > 3 else ""))
    return rows


def group_members(pgid):
    """The live processes of a process group as (pid, command), without the
    `ps` that lists them; None when the processes cannot be listed."""
    table = process_table()
    if table is None:
        return None
    members = []
    for pid, group, stat, command in table:
        words = command.split()
        if group != pgid or stat.startswith("Z"):
            continue
        if command.startswith("(") and command.endswith(")"):
            # A process that is exiting: the kernel shows its name alone.
            continue
        if len(words) > 1 and os.path.basename(words[0]) == "ps" and words[1] == "-ax":
            continue
        members.append((pid, command))
    return members


_budget = threading.local()


class Budget:
    """Inside it, a blocking question about a process waits no longer than
    `remaining()` says is left of the caller's own bound."""

    def __init__(self, remaining):
        self.remaining = remaining

    def __enter__(self):
        self.outer = getattr(_budget, "remaining", None)
        _budget.remaining = self.remaining
        return self

    def __exit__(self, *exception):
        _budget.remaining = self.outer


def query_timeout(default):
    """How long one question about a process may take: `default`, or what is
    left of the enclosing Budget when that is less (never less than 0.1 s)."""
    remaining = getattr(_budget, "remaining", None)
    return default if remaining is None else max(0.1, min(default, remaining()))


def process_instance(pid):
    """(start time, command) of a process: the start time tells it from a
    later process that has the same pid. None when there is none."""
    try:
        done = subprocess.run(
            ["ps", "-o", "lstart=", "-o", "command=", "-p", str(pid)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, errors="replace",
            env=dict(os.environ, LC_ALL="C"), timeout=query_timeout(10))
    except (OSError, subprocess.TimeoutExpired):
        return None
    line = done.stdout.rstrip("\n")
    if done.returncode != 0 or len(line) < 25:
        return None
    return line[:24], line[25:].strip()


def find_supervisor(nonce):
    """The pid of the supervisor that was started with this nonce, or None."""
    for pid, group, stat, command in process_table() or []:
        words = command.split()
        if stat.startswith("Z") or "_supervise" not in words:
            continue
        for index, word in enumerate(words[:-1]):
            if word == "--nonce" and words[index + 1] == nonce:
                return pid
    return None


def recorded_supervisor(record):
    """The pid `start` recorded, while that process is still its supervisor.

    Nothing clears the record when the host ends, and by then the pid can
    name any process, including another agent's process-group leader. The
    pid is trusted only while its command line still carries `_supervise`
    and the record's nonce."""
    pid = record.get("supervisor")
    nonce = record.get("nonce")
    if not nonce:
        return None
    if pid is None:
        # The launch that wrote the record ended before it knew the pid.
        return find_supervisor(nonce)
    if not isinstance(pid, int) or pid <= 0:
        return None
    try:
        done = subprocess.run(
            ["ps", "-o", "command=", "-p", str(pid)], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            timeout=query_timeout(10))
    except (OSError, subprocess.TimeoutExpired):
        return None
    words = done.stdout.split()
    if done.returncode != 0 or "_supervise" not in words:
        return None
    for index, word in enumerate(words[:-1]):
        if word == "--nonce" and words[index + 1] == nonce:
            return pid
    return None


# What end_host found. ENDED: the host's process group was listed and is
# empty (or is another group under a reused pid), so nothing of the host
# remains. Everything else says it is not known to be gone: FOREIGN, a group
# the record names holds nothing that shows it is the host's; STUCK, something
# that is the host's would not end; UNKNOWN, the processes could not be listed.
ENDED, FOREIGN, STUCK, UNKNOWN = "ended", "foreign", "stuck", "unknown"


def carries(nonce, command):
    """Whether a command line is the host's own: the supervisor's `--nonce`
    or xcodebuild's result bundle, which is named for the nonce."""
    words = command.split()
    if "_supervise" in words:
        return any(word == "--nonce" and words[index + 1] == nonce
                   for index, word in enumerate(words[:-1]))
    # (a script run by its interpreter shows the interpreter first)
    return any(os.path.basename(word) == "xcodebuild" for word in words[:2]) and any(
        word.endswith(f"{nonce}.xcresult") for word in words)


def supervisor_identity_path(directory, nonce):
    return pathlib.Path(directory) / f"supervisor-{nonce}.json"


def publish_supervisor_identity(options):
    """The supervisor says who it is before it spawns anything, in a file of
    its own for this host: the starter that records the supervisor's pid can
    die between spawning it and writing the pid, and a later launch replaces
    the record. The group the supervisor leads is the host's."""
    write_json(supervisor_identity_path(state_dir(options), options.nonce), {
        "pid": os.getpid(), "pgid": os.getpgrp(), "nonce": options.nonce,
        "started": time.time()})


def host_group(record, process, directory=None):
    """(pgid, known): the process group of the host the record names. It is
    the supervisor's (`start_new_session` made its pid the group id): the pid
    in the record, or the one the supervisor published itself in `directory`.
    A host whose group cannot be recovered that way is (None, False): what
    carries its nonce shows the group only while some process still does, and
    that nothing does proves nothing, as a helper that carries no nonce can
    stay. (None, True) is only for a record that names no host at all, or a
    pid that cannot be a supervisor's."""
    nonce = record.get("nonce")
    pid = process.pid if process is not None else record.get("supervisor")
    if isinstance(pid, int) and pid > 1:
        return pid, True
    if not nonce:
        return None, True
    # A pid that cannot be a host's supervisor (launchd's, or none) was not
    # written by a start that spawned one: only a record with no pid at all
    # may stand for a host that was spawned and not yet recorded.
    spawned = pid is None
    if directory is not None:
        published = read_json(supervisor_identity_path(directory, nonce)).get("pgid")
        if isinstance(published, int) and published > 1:
            return published, True
    table = process_table()
    if table is None:
        return None, False
    for _, group, stat, command in table:
        if not stat.startswith("Z") and carries(nonce, command):
            return group, True
    return None, not spawned


def pid_reused(pid, started):
    """Whether the process that has `pid` now started after the host did
    (`started`, epoch seconds): then the host's group, which the supervisor's
    pid named, is gone and this is another one. A group whose leader is gone,
    or older than the host, is not shown to be another."""
    instance = process_instance(pid)
    if instance is None or not isinstance(started, (int, float)):
        return False
    try:
        began = time.mktime(time.strptime(instance[0], "%a %b %d %H:%M:%S %Y"))
    except ValueError:
        return False
    return began >= started - 2


def end_host(record, process=None, wait=None, remainder=None, directory=None):
    """Ends the whole of a host: its supervisor, xcodebuild and what else is
    in their process group, with SIGTERM and then SIGKILL, and returns ENDED
    only once the group was listed and is empty. Anything else (see above)
    appends what is left, as (pid, command), to `remainder`. A host whose
    group cannot be identified (no pid in the record, none published by its
    supervisor in `directory`, nothing that carries its nonce) is UNKNOWN.

    A group is signalled only while `process`, the supervisor this process
    started and has not reaped, holds the pid, or while a member's command
    line carries the record's nonce. Ownership shown once holds through the
    two signals of this call: the group was not empty at any look between
    them, so it is the one that was shown, and its remains (a helper that
    ignores SIGTERM) are ended with SIGKILL. A group that never showed it is
    left alone."""
    nonce = record.get("nonce")
    pid, known = host_group(record, process, directory)
    if not known:
        return UNKNOWN
    if pid is None:
        return ENDED
    owned = False
    for sig, limit in ((signal.SIGTERM, wait or SUPERVISOR_TERM_WAIT),
                       (signal.SIGKILL, 10)):
        if process is not None:
            process.poll()
        members = group_members(pid)
        if members is None:
            return UNKNOWN
        if not members:
            return ENDED
        owned = owned or (process is not None and process.returncode is None) or (
            bool(nonce) and any(carries(nonce, command) for _, command in members))
        if not owned:
            # What ended a moment ago is still being torn down: look again
            # for a little before calling the rest another group's.
            settle = time.monotonic() + FOREIGN_SETTLE
            while not owned and time.monotonic() < settle:
                time.sleep(0.2)
                members = group_members(pid)
                if members is None:
                    return UNKNOWN
                if not members:
                    return ENDED
                owned = bool(nonce) and any(
                    carries(nonce, command) for _, command in members)
        if not owned:
            started = record.get("started")
            if started is None and directory is not None and nonce:
                started = read_json(supervisor_identity_path(directory, nonce)).get("started")
            if pid_reused(pid, started):
                return ENDED
            if remainder is not None:
                remainder.extend(members)
            return FOREIGN
        try:
            os.killpg(pid, sig)
        except ProcessLookupError:
            pass
        except PermissionError:
            # A process that has just exited and is not reaped yet refuses
            # signals too (macOS): give the group a moment to empty before
            # calling it one that may not be signalled.
            settle = time.monotonic() + FOREIGN_SETTLE
            while time.monotonic() < settle:
                if process is not None:
                    process.poll()
                members = group_members(pid)
                if members is None:
                    return UNKNOWN
                if not members:
                    return ENDED
                time.sleep(0.2)
            if remainder is not None:
                remainder.extend(members)
            return FOREIGN
        deadline = time.monotonic() + limit
        while True:
            if process is not None:
                process.poll()
            members = group_members(pid)
            if members is None:
                return UNKNOWN
            if not members:
                return ENDED
            if time.monotonic() >= deadline:
                break
            time.sleep(0.2)
    if remainder is not None:
        remainder.extend(members)
    return STUCK


class WatchLock:
    """The one-watch-per-device rule: an exclusive lock on `watch.lock` in the
    state directory, held while the watch runs and released by the system
    when its process ends, however it ends. The file then says who holds it:
    pid, the process's start time (which tells it from a later process with
    the pid), device and state directory; the lock itself is the proof that
    some watch runs, and these say which."""

    def __init__(self, options):
        self.options = options
        self.path = state_dir(options) / "watch.lock"
        self.fd = None

    def acquire(self):
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
        # A command looking for the watch holds the lock for an instant.
        for _ in range(5):
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                time.sleep(0.05)
        else:
            os.close(fd)
            return False
        instance = process_instance(os.getpid())
        identity = {"pid": os.getpid(), "start": instance[0] if instance else "",
                    "device": self.options.device,
                    "state": os.path.realpath(state_path(self.options))}
        os.ftruncate(fd, 0)
        os.pwrite(fd, (json.dumps(identity) + "\n").encode(), 0)
        self.fd = fd
        return True

    def release(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


def lock_owner(fd, options):
    """The pid of the process the lock file names, when that process is the
    one that wrote it for this device and state directory; else None. Until
    the holder has written, the file still names a watch that ended, whose pid
    may belong to any process now."""
    try:
        identity = json.loads(os.pread(fd, 512, 0).decode(errors="replace"))
    except ValueError:
        return None
    if not isinstance(identity, dict):
        return None
    pid = identity.get("pid")
    if (not isinstance(pid, int) or pid <= 0 or not identity.get("start")
            or identity.get("device") != options.device
            or identity.get("state") != os.path.realpath(state_path(options))):
        return None
    instance = process_instance(pid)
    if instance is None or instance[0] != identity["start"]:
        return None
    return pid


def watcher_pid(options):
    """The pid of the watch running for this device, or None; 0 when one runs
    and cannot be identified (it may not have written yet). The held lock is
    the proof of a running watch, never a pid in a file."""
    try:
        fd = os.open(state_path(options) / "watch.lock", os.O_RDONLY | os.O_CLOEXEC)
    except OSError:
        return None
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            return None
        for _ in range(20):
            pid = lock_owner(fd, options)
            if pid:
                return pid
            time.sleep(0.05)
        return 0
    finally:
        os.close(fd)


def read_watch_record(options):
    return read_json(state_path(options) / "watch.json")


class WatchRecord:
    """`watch.json` in the state directory: what `status` and a later `watch`
    read."""

    def __init__(self, options):
        self.path = state_dir(options) / "watch.json"
        self.data = read_watch_record(options)

    def update(self, **fields):
        if fields.get("state", self.data.get("state")) != self.data.get("state"):
            fields["since"] = time.time()
        self.data.update(fields, updated=time.time())
        write_json(self.path, self.data)


class ApprovalLatch:
    """`approval.json` in the state directory: the device may be asking for
    an approval nobody gave, so no watch starts a host until one serves. It
    is set when an attempt shows XCTest's timeout ("observed") or ended where
    that could not be told ("possible"), and cleared only by a host that
    serves: a stopped, failed or restarted watch leaves it as it is."""

    def __init__(self, options):
        self.options = options

    def path(self):
        return state_path(self.options) / "approval.json"

    def read(self):
        return read_json(self.path()) or None

    def set(self, kind, nonce, log_path, detail):
        existing = self.read() or {}
        if existing.get("kind") == "observed":
            kind = "observed"
        write_json(state_dir(self.options) / "approval.json", {
            "kind": kind, "since": existing.get("since", time.time()),
            "nonce": nonce, "log": str(log_path), "detail": detail})

    def clear(self):
        try:
            self.path().unlink()
        except FileNotFoundError:
            pass


def forget_approval_failure(options):
    """A host started with the owner at the device serves: the watch that
    failed for approval may run again."""
    ApprovalLatch(options).clear()
    if (read_watch_record(options).get("state") == NEEDS_APPROVAL
            and watcher_pid(options) is None):
        WatchRecord(options).update(
            state=STOPPED, detail="cleared by a `start` that served")


def watch_summary(options):
    """What `status` says of the watch."""
    pid = watcher_pid(options)
    record = read_watch_record(options)
    summary = {"running": pid is not None}
    if pid:
        summary["pid"] = pid
    state = record.get("state")
    if state:
        summary["state"] = state
        if state not in ACTIVE_STATES:
            summary["meaning"] = STATE_TEXT.get(state, state)
        elif pid is None:
            summary["meaning"] = "the watch ended without recording why"
        for key in ("detail", "restarts", "last_end"):
            if record.get(key) not in (None, ""):
                summary[key] = record[key]
        if record.get("since"):
            summary["since"] = time.strftime(
                "%Y-%m-%d %H:%M:%S", time.localtime(record["since"]))
    latch = ApprovalLatch(options).read()
    if latch:
        summary["approval"] = (
            "the device may be waiting for an approval nobody gave "
            f"({latch.get('kind')}): no watch starts a host until `start` "
            "serves with the owner at the device")
    return summary


def wait_for_replacement(options, clock=None):
    """With a watch running, a command waits while the watch says it is
    starting a host, or has not seen the host's heartbeat advance lately (the
    watch may not have noticed a death yet), until it has, or the watch ends;
    it returns at once, and fails as it always did, when no watch runs. The
    wait is a hard bound: its deadline starts before the first question about
    a process, each of those gets what is left of it, and an expired deadline
    fails the command even when the record just turned ready. It makes no
    device read, and reads only the watch's own record, whose times are the
    Mac's."""
    clock = clock or CLOCK
    deadline = clock.monotonic() + WATCH_COMMAND_WAIT
    announced = False
    with Budget(lambda: deadline - clock.monotonic()):
        while True:
            record = read_watch_record(options)
            if (watcher_pid(options) is None
                    or record.get("state") not in ACTIVE_STATES):
                return
            if clock.monotonic() >= deadline:
                raise NoHost(f"the host was still being replaced after "
                             f"{WATCH_COMMAND_WAIT} s")
            if (record.get("state") == WATCHING
                    and clock.time() - record.get("alive_at", 0) <= WATCH_ALIVE_FRESH):
                return
            if not announced:
                log("the host is not serving and its watch is starting another; "
                    f"waiting up to {WATCH_COMMAND_WAIT} s")
                announced = True
            clock.sleep(1)


def inflight_directory(options):
    return state_path(options) / "inflight"


class Inflight:
    """What a sender tells the watch while it waits for a command's result:
    how long it waits, so that a command the host has run for as long as that
    is hung and one it has not is not. A sender that ended leaves a file that
    its process's start time shows is stale."""

    def __init__(self, options, ident, limit):
        self.options = options
        self.path = inflight_directory(options) / f"{ident}.json"
        instance = process_instance(os.getpid())
        self.data = {"pid": os.getpid(), "limit": limit,
                     "start": instance[0] if instance else ""}

    def __enter__(self):
        state_dir(self.options)
        self.path.parent.mkdir(exist_ok=True)
        write_json(self.path, self.data)
        return self

    def __exit__(self, *exception):
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


def inflight_limit(options, command):
    """The wait, in seconds, of the sender of `command` (the id the heartbeat
    names) while it still waits for that command's result; 0 when the command
    has no such sender. Another command's sender says nothing of this one."""
    if (not isinstance(command, str) or command.startswith(".")
            or not re.fullmatch(r"[A-Za-z0-9._-]+", command)):
        return 0
    entry = read_json(inflight_directory(options) / f"{command}.json")
    pid, limit = entry.get("pid"), entry.get("limit")
    if (not isinstance(pid, int) or not entry.get("start")
            or not isinstance(limit, (int, float))):
        return 0
    instance = process_instance(pid)
    return limit if instance is not None and instance[0] == entry["start"] else 0


def send(options, command, timeout, clock=None):
    """Sends one command and waits at most `timeout` s for its result.

    A command already sent to a host that ends is not repeated for the
    replacement: it may have run, and the replacement clears its inbox, so
    the sender is told the host is gone (status 3), as for any dead host."""
    clock = clock or CLOCK
    wait_for_replacement(options, clock)
    ident = f"{time.time_ns():020d}-{uuid.uuid4().hex[:8]}"
    command = dict(command, bundle=command.get("bundle") or options.bundle)
    deadline = clock.monotonic() + timeout + 30

    def remaining():
        return max(1, min(60, deadline - clock.monotonic()))

    sent_to = host_state(options, remaining())
    host = sent_to.get("nonce") if sent_to else None
    with Inflight(options, ident, timeout + 30), \
            tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        body = scratch / f"{ident}.json"
        marker = scratch / f"{ident}.ready"
        body.write_text(json.dumps(command))
        marker.write_text("")
        push(options, [body, marker], f"{CHANNEL}/inbox")
        result = scratch / "result.json"
        next_check = clock.monotonic() + SEND_LIVENESS_EVERY
        while clock.monotonic() < deadline:
            if pull(options, f"{CHANNEL}/outbox/{ident}.json", result, remaining()):
                reply = json.loads(result.read_text())
                if "file" in reply:
                    attachment = scratch / reply["file"]
                    if not pull(options, f"{CHANNEL}/outbox/{reply['file']}", attachment, 120):
                        raise NoHost("the host wrote an attachment that cannot be copied")
                    reply["data"] = attachment.read_bytes()
                return reply
            if clock.monotonic() >= next_check:
                state = host_state(options, remaining())
                reason = liveness(state)
                if reason is not None:
                    raise NoHost(f"the host is not running: {reason}" + (
                        "; its watch replaces it, but this command is not "
                        "repeated because it may have run"
                        if watcher_pid(options) is not None else ""))
                if host and state.get("nonce") != host:
                    raise NoHost("the host this command was sent to has been "
                                 "replaced, and the replacement started with an "
                                 "empty inbox; the command is not repeated "
                                 "because it may have run")
                next_check = clock.monotonic() + SEND_LIVENESS_EVERY
            clock.sleep(0.5)
    raise NoHost(f"no result for {command['op']} within {timeout} s")


def command_status(options):
    state = host_state(options)
    reason = liveness(state)
    record = local_record(options)
    summary = {"serving": reason is None}
    if reason is not None:
        summary["reason"] = reason
    if state:
        now = time.time()
        summary.update({
            "heartbeat_age_s": round(now - state.get("time", 0), 1),
            "uptime_s": round(now - state.get("started", now)),
            "remaining_s": round(state.get("deadline", now) - now),
            "served": state.get("served"),
            "busy": state.get("command"),
        })
    if record:
        summary["xcodebuild_log"] = record.get("log")
        summary["result_bundle"] = record.get("result_bundle")
        summary["supervisor_running"] = recorded_supervisor(record) is not None
    summary["watch"] = watch_summary(options)
    print(json.dumps(summary, indent=2))
    return 0 if reason is None else EXIT_NO_HOST


def derived_data(options):
    return pathlib.Path(options.derived_data)


def command_build(options):
    command = [
        "xcodebuild", "build-for-testing",
        "-project", str(CHECKOUT / "Kernarium/Kernarium.xcodeproj"),
        "-scheme", "KernariumAutomation",
        "-destination", "generic/platform=iOS",
        "-derivedDataPath", str(derived_data(options)),
        "-allowProvisioningUpdates", *options.xcodebuild_arg]
    log("building the automation host")
    try:
        return subprocess.run(command, stdin=subprocess.DEVNULL,
                              timeout=options.timeout).returncode
    except subprocess.TimeoutExpired:
        log(f"the build took longer than {options.timeout} s")
        return EXIT_FAILED


def recording_problems(xctestrun):
    """What in the test run file lets Xcode record the session, as messages.

    Both run file formats are enumerated target by target, and every target
    must have a bundle path of its own and state each setting. A target of any
    other shape, such as one that runs the destination's installed bundle
    (UseDestinationArtifacts), is refused: this check cannot read its
    settings."""
    try:
        with open(xctestrun, "rb") as handle:
            plan = plistlib.load(handle)
    except (OSError, ValueError, expat.ExpatError) as error:
        return [f"{xctestrun} cannot be read: {error}"]
    if not isinstance(plan, dict):
        return [f"{xctestrun} is not a test run file"]
    metadata = plan.get("__xctestrun_metadata__")
    version = metadata.get("FormatVersion") if isinstance(metadata, dict) else None
    problems = set()
    targets = []
    if version == 2:
        configurations = plan.get("TestConfigurations")
        if not isinstance(configurations, list):
            return [f"{xctestrun} has no test configurations"]
        for configuration in configurations:
            entries = (configuration.get("TestTargets")
                       if isinstance(configuration, dict) else None)
            if not isinstance(entries, list):
                problems.add("a test configuration lists no targets")
                continue
            targets.extend(entries)
    elif version in (None, 1):
        targets = [value for key, value in plan.items()
                   if key != "__xctestrun_metadata__"]
    else:
        return [f"{xctestrun} is in a test run file format this check "
                "does not know"]
    if not targets:
        return [f"{xctestrun} names no test target"]
    for target in targets:
        if not isinstance(target, dict) or not isinstance(
                target.get("TestBundlePath"), str):
            problems.add("a test target has no bundle path of its own "
                         "(a destination-artifact or unknown target shape)")
            continue
        if target.get("UseDestinationArtifacts"):
            problems.add("a test target uses the destination's artifacts")
        for key, wanted in RECORDING_SETTINGS.items():
            if target.get(key) != wanted:
                problems.add(f"{key} is {target.get(key)!r}, not {wanted!r}")
    return sorted(problems)


def refuse_recording(xctestrun):
    """True, after saying why, when Xcode would record this run file's
    session."""
    problems = recording_problems(xctestrun)
    if problems:
        log(f"{xctestrun} does not turn Xcode's recording off "
            f"({'; '.join(problems)}): it would record the device's screen "
            "for the whole session and keep attachments; run `build` again "
            "with the current KernariumAutomation scheme")
    return bool(problems)


def check_report_slots(options, processes):
    """Makes room for diagnostic reports before something starts an
    application; False, after saying why, when it cannot."""
    try:
        ios_device_reports.preflight(options, [], extra_processes=processes,
                                     emit=log)
    except ios_device_reports.DeviceError as error:
        log(str(error))
        return False
    return True


def clear_group(wait):
    """Ends what else is in the supervisor's process group (it leads one of its
    own), so that the supervisor ends last and nothing of the host outlives
    it."""
    group = os.getpgrp()
    if group != os.getpid():
        return

    def others():
        return [pid for pid, _ in (group_members(group) or []) if pid != os.getpid()]

    if not others():
        return
    try:
        os.killpg(group, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + wait
    while others() and time.monotonic() < deadline:
        time.sleep(0.2)
    for pid in others():
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def command_supervise(options):
    """Runs xcodebuild for the host's lifetime; started detached by `start`.

    The result bundle goes where `start` says, under the state directory,
    never to the default under the internal disk's DerivedData. This is where
    xcodebuild starts, so the run file and the report store are checked here,
    whoever called.

    The supervisor stays responsible for the whole host until nothing of it is
    left: SIGTERM (what ends a host) ends xcodebuild, with SIGKILL for one
    that ignores it, and then whatever else xcodebuild left in the group; it
    exits last.

    It also ends xcodebuild itself as soon as xcodebuild's log (`--log`)
    reports the connection to the device lost. xcodebuild would start the
    runner again by itself some seconds later, a runner that cannot
    initialize when the device is locked and may leave the device asking for an
    approval nobody gives; with xcodebuild ended first, the host's death is a
    plain one and the watch may replace it. This is best effort: the log is
    polled, so xcodebuild may have started the runner before the supervisor
    reads the error, and the watch then handles that death as it does any
    other. xcodebuild's own relaunch never yields a host that serves (the
    test that serves is not run again). The error is taken only from lines
    of the xcodebuild this supervisor started."""
    publish_supervisor_identity(options)
    if refuse_recording(options.xctestrun):
        return EXIT_FAILED
    if not check_report_slots(options, REPORT_PROCESSES):
        return EXIT_FAILED
    environment = dict(os.environ,
                       TEST_RUNNER_KERNARIUM_AUTOMATION_HOST="1",
                       TEST_RUNNER_KERNARIUM_AUTOMATION_HOST_SECONDS=str(options.seconds),
                       TEST_RUNNER_KERNARIUM_AUTOMATION_NONCE=options.nonce)
    command = ["xcodebuild", "test-without-building",
               "-xctestrun", options.xctestrun,
               "-destination", f"platform=iOS,id={options.device}",
               "-resultBundlePath", options.result_bundle,
               # A failed run would otherwise collect the device's diagnostics
               # (devicectl diagnose) into the result bundle.
               "-collect-test-diagnostics", "never",
               f"-only-testing:{TEST}"]
    stopping = []
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, lambda *_: stopping.append(number))
    # xcodebuild stays in this process group, which `start`, `stop` and the
    # watch signal as a whole.
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, env=environment)
    deadline = time.monotonic() + options.seconds + 900
    scan = LogScan(options.log, xcodebuild_pid=process.pid)
    while (process.poll() is None and not stopping
           and time.monotonic() < deadline):
        scan.read()
        if scan.connection_lost:
            print(f"ipad-automation: {scan.lost_connection_text()}: ending "
                  "xcodebuild before it starts the runner again", flush=True)
            break
        time.sleep(0.2)
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=options.term_wait)
        except subprocess.TimeoutExpired:
            process.kill()
    status = process.wait()
    clear_group(options.term_wait)
    print(f"ipad-automation: xcodebuild exited with status {status}", flush=True)
    return status


class LaunchRefused(Exception):
    """Nothing was started. A permanent refusal is one that trying again
    cannot change; `cancelled` says the caller asked to end first; `settle`
    says a host another launch started is still coming up."""

    def __init__(self, message, permanent=False, cancelled=False, settle=False):
        super().__init__(message)
        self.permanent = permanent
        self.cancelled = cancelled
        self.settle = settle


class Launch:
    """One host that `launch_host` started."""

    def __init__(self, process, log_path, nonce):
        self.process = process
        self.log_path = log_path
        self.nonce = nonce


class LaunchLock:
    """The launch transition of one device: from the check that no host is
    coming up to the host's record on disk. `start` and the watch each make it
    under this lock, so that two of them never launch side by side. It is an
    exclusive `flock` on `launch.lock` in the state directory, and waiting for
    it is bounded and ends when `cancelled()` says so."""

    def __init__(self, options, cancelled=None):
        self.path = state_dir(options) / "launch.lock"
        self.cancelled = cancelled
        self.fd = None

    def __enter__(self):
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
        deadline = time.monotonic() + LAUNCH_LOCK_WAIT
        waiting = False
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if not waiting:
                    log("another launch of this device is in progress; "
                        "waiting for it")
                    waiting = True
            if self.cancelled is not None and self.cancelled():
                os.close(fd)
                raise LaunchRefused("the launch was cancelled while it waited "
                                    "for another launch", cancelled=True)
            if time.monotonic() >= deadline:
                os.close(fd)
                raise LaunchRefused("another launch of this device did not "
                                    f"finish within {LAUNCH_LOCK_WAIT} s")
            time.sleep(0.1)
        self.fd = fd
        return self

    def __exit__(self, *exception):
        os.close(self.fd)


def host_unsettled(record, default_wait):
    """Whether the host the record names is still coming up: its supervisor
    runs, and the wait its starter recorded (plus the runner's own) has not
    run out."""
    if recorded_supervisor(record) is None:
        return False
    wait = record.get("wait") or default_wait
    return time.time() < record.get("started", 0) + wait + WATCH_RUNNER_WAIT


def launch_conflict(options):
    """Why `start` must not launch a host now, or None."""
    if watcher_pid(options) is not None:
        return ("a watch is running for this device: it replaces a host that "
                "ends, and a second host would ask for approval; `stop "
                "--force` ends the watch and the host")
    if liveness(host_state(options)) is None:
        return "a host is already serving this device; use it or stop it first"
    if host_unsettled(local_record(options), options.wait):
        return ("a host started a moment ago is still coming up; wait for it, "
                "or `stop --force`")
    return None


def start_guard(options):
    conflict = launch_conflict(options)
    return LaunchRefused(conflict) if conflict else None


def collection_options(options, label):
    """The options for one collection of the device's diagnostic reports. The
    collector refuses a destination that exists, so each collection of a
    watch, which collects again and again, gets a directory of its own
    beneath the one configured."""
    if not options.device_reports_dir:
        return options
    fresh = argparse.Namespace(**vars(options))
    fresh.device_reports_dir = pathlib.Path(options.device_reports_dir) / label
    return fresh


def launch_host(options, cancelled=None, guard=None, record=None, locked=False):
    """Starts a supervisor, and with it xcodebuild and a new host, and records
    it in `host.json`. Raises LaunchRefused, before anything starts, when the
    host cannot be started.

    The transition is made under the device's launch lock (the caller holds it
    already when `locked`): `guard()` there returns a LaunchRefused to raise
    (or None), `cancelled()` is checked again after the report preflight,
    which can take minutes, and `record` is told the host's nonce and log
    before the host starts. A record that exists before the supervisor does is
    written whole, and a crash between the two leaves it findable by its
    nonce."""
    runs = sorted(derived_data(options).glob("Build/Products/KernariumAutomation_*.xctestrun"))
    if not runs:
        raise LaunchRefused(
            f"no built host under {derived_data(options)}; run `build` first",
            permanent=True)
    # The supervisor checks both again where xcodebuild starts; here the
    # caller sees a refusal before anything is started.
    if refuse_recording(runs[-1]):
        raise LaunchRefused("the built host's run file lets Xcode record the "
                            "device's screen", permanent=True)
    seconds = int(options.hours * 3600)
    nonce = uuid.uuid4().hex
    directory = state_dir(options)
    # The nonce keeps two starts in one second apart: xcodebuild refuses a
    # result bundle that exists.
    name = f"host-{time.strftime('%Y%m%d-%H%M%S')}-{nonce}"
    log_path = directory / f"{name}.log"
    result_bundle = directory / f"{name}.xcresult"
    with contextlib.nullcontext() if locked else LaunchLock(options, cancelled):
        refusal = guard() if guard is not None else None
        if refusal is not None:
            raise refusal
        # The preflight lists the device's reports, which can take longer than
        # a stop allows: a launch told to end does not begin it.
        if cancelled is not None and cancelled():
            raise LaunchRefused("the launch was cancelled before the host started",
                                cancelled=True)
        # Before anything starts: a kill during the session must leave a report.
        if not check_report_slots(collection_options(options, f"{name}-launch"),
                                  REPORT_PROCESSES):
            raise LaunchRefused("the device's report store has too few free slots "
                                "or cannot be read")
        if cancelled is not None and cancelled():
            raise LaunchRefused("the launch was cancelled before the host started",
                                cancelled=True)
        if record is not None:
            record(nonce, log_path)
        reports = []
        supervised = collection_options(options, f"{name}-supervise")
        if supervised.device_reports_dir:
            reports = ["--device-reports-dir", str(supervised.device_reports_dir)]
        if options.leave_device_reports:
            reports.append("--leave-device-reports")
        host = {"supervisor": None, "log": str(log_path),
                "result_bundle": str(result_bundle), "nonce": nonce,
                "started": time.time(), "seconds": seconds, "wait": options.wait}
        # The identity files of hosts older than the one this replaces name
        # groups that were ended (or that no later step asks about).
        keep = {supervisor_identity_path(directory, each).name
                for each in (nonce, local_record(options).get("nonce")) if each}
        for stale in directory.glob("supervisor-*.json"):
            if stale.name not in keep:
                try:
                    stale.unlink()
                except OSError:
                    pass
        write_json(directory / "host.json", host)
        with open(log_path, "w") as output:
            supervisor = subprocess.Popen(
                [sys.executable, str(SCRIPT), "--device", options.device,
                 "--runner", options.runner, "--state-dir", options.state_dir,
                 "_supervise", "--xctestrun", str(runs[-1]),
                 "--result-bundle", str(result_bundle), "--log", str(log_path),
                 "--seconds", str(seconds), "--nonce", nonce,
                 "--term-wait", str(CHILD_TERM_WAIT), *reports],
                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                start_new_session=True)
        write_json(directory / "host.json", dict(host, supervisor=supervisor.pid))
    return Launch(supervisor, log_path, nonce)


def command_start(options):
    conflict = launch_conflict(options)
    if conflict:
        log(conflict)
        return EXIT_FAILED
    try:
        launch = launch_host(options, guard=lambda: start_guard(options))
    except LaunchRefused as refusal:
        log(str(refusal))
        return EXIT_FAILED
    supervisor, log_path, nonce = launch.process, launch.log_path, launch.nonce
    log(f"started xcodebuild under supervisor {supervisor.pid}; log {log_path}")
    deadline = time.monotonic() + options.wait
    announced = False
    while time.monotonic() < deadline:
        if supervisor.poll() is not None:
            break
        if not announced and RUNNER_UP_LINE in log_path.read_text(errors="replace"):
            # XCTest gives up on enabling automation 60 s after this line.
            log("the runner is up: approve Enable UI Automation at the device within 60 s")
            announced = True
        state = host_state(options)
        if state and state.get("nonce") == nonce and liveness(state) is None:
            log("the host is serving")
            forget_approval_failure(options)
            return 0
        time.sleep(3)
    scan = LogScan(str(log_path))
    scan.read()
    if scan.approval_failure:
        log("the device did not enable UI automation: the approval it asked for "
            "was not given (the prompt can stay on the screen until someone "
            f"dismisses it); {scan.approval_failure}")
    elif supervisor.poll() is not None:
        log("xcodebuild ended before the host served; see the log")
    else:
        log(f"the host did not serve within {options.wait} s; see the log")
    if supervisor.poll() is None:
        # A supervisor that ended on its own cleared its group first.
        end_host(local_record(options), supervisor)
    return EXIT_NO_HOST


def signal_watcher(pid):
    os.kill(pid, signal.SIGTERM)


def watcher_is_a_watch(pid):
    """Whether `pid` is a `watch` of this script, the last check before a
    signal."""
    instance = process_instance(pid)
    if instance is None:
        return False
    words = instance[1].split()
    return "watch" in words and any(
        word.endswith(SCRIPT.name) for word in words)


def end_watch(options):
    """Ends the watch running for this device, before its host is stopped:
    left running it would start the host again. False, after saying why,
    when it cannot be ended, and then the host stays.

    Only the process that holds this device's lock for this state directory,
    as the lock file names it with its start time, is signalled: a watch for
    another device, or any other process that has the pid of one that ended,
    is not."""
    pid = watcher_pid(options)
    if pid is None:
        return True
    if pid <= 0 or not watcher_is_a_watch(pid):
        log(f"a watch holds the lock but pid {pid} is not one: not stopping the "
            "host, which that watch would start again")
        return False
    log(f"ending the watch (pid {pid}) first, so that it does not restart the host")
    try:
        signal_watcher(pid)
    except ProcessLookupError:
        pass
    except PermissionError:
        log(f"cannot signal the watch (pid {pid}): not stopping the host, "
            "which it would start again")
        return False
    deadline = time.monotonic() + WATCH_END_WAIT
    while time.monotonic() < deadline:
        if watcher_pid(options) is None:
            return True
        time.sleep(0.2)
    log(f"the watch (pid {pid}) did not end within {WATCH_END_WAIT} s: not "
        "stopping the host, which it would start again")
    return False


def command_stop(options):
    if not options.force:
        log("refusing to stop: automation stays approved only while a session runs, "
            "so a new host will ask for approval at the device again; pass --force "
            "when someone can approve it")
        return EXIT_FAILED
    if not end_watch(options):
        return EXIT_FAILED
    record = local_record(options)
    try:
        send(options, {"op": "stop"}, 30)
        log("the host stopped")
    except NoHost as error:
        log(str(error))
    if recorded_supervisor(record) is not None:
        deadline = time.monotonic() + SUPERVISOR_EXIT_WAIT
        while time.monotonic() < deadline:
            if recorded_supervisor(record) is None:
                break
            time.sleep(1)
        if recorded_supervisor(record) is not None:
            log(f"terminating supervisor {record.get('supervisor')}")
    if end_host(record, directory=state_dir(options)) in (STUCK, UNKNOWN):
        log("part of the host would not end, or its processes could not be "
            "listed; see `ps` for its process group")
        return EXIT_FAILED
    return 0


class LogScan:
    """Reads a host's xcodebuild log as it grows and keeps what the watch
    needs of it. A line is taken once it is complete, and ends only at a
    newline. With `xcodebuild_pid`, the launch error of a lost connection is
    taken only from the lines of that xcodebuild (the supervisor's own)."""

    def __init__(self, path, xcodebuild_pid=None):
        self.path = path
        self.xcodebuild_pid = xcodebuild_pid
        self.offset = 0
        self.runner_up = 0
        # When each runner came up ("HH:MM:SS", or "" when the line has no
        # time), in order.
        self.runner_times = []
        # What the log shows of each runner after the first, in order:
        # "pending" (it came up and nothing more), "restarted" (xcodebuild's
        # restart notice), "initialized" (and the totals of a run of no tests)
        # or "failed" (XCTest's initialization failure).
        self.relaunches = []
        # The first runner's test that serves started: it initialized.
        self.test_started = False
        self.approval_failure = None
        # What the approval failure says went wrong, for the record.
        self.approval_cause = None
        self.init_failure = None
        self.exit_status = None
        self.note = None
        # The time xcodebuild reported the connection to the device lost, once
        # it has; and where the block of a launch error it is reading stands:
        # None, ("domain", time) after its header, ("code", time) after the
        # domain line.
        self.connection_lost = None
        self.launch_error = None

    def unconfirmed_relaunches(self):
        """How many runners started again are not shown to have initialized."""
        return sum(1 for outcome in self.relaunches if outcome != "initialized")

    def read(self):
        if not self.path:
            return
        try:
            with open(self.path, "rb") as handle:
                handle.seek(self.offset)
                data = handle.read()
        except OSError:
            return
        complete = data.rfind(b"\n") + 1
        self.offset += complete
        # Not splitlines(): it also ends a line at \r, \x0b, \x0c, \x1c-\x1e,
        # \x85, \u2028 and \u2029, which text printed by the runner may carry.
        for line in data[:complete].decode(errors="replace").split("\n")[:-1]:
            self.take(line.rstrip())

    def take_launch_error(self, line):
        """Follows xcodebuild's block for the end of the runner's launch
        session: a header, then the domain, then the code, on consecutive
        lines. Only CoreDevice error 3 is a lost connection."""
        state, self.launch_error = self.launch_error, None
        header = LAUNCH_ERROR_LINE.match(line)
        if header and self.xcodebuild_pid not in (None, int(header.group(2))):
            header = None
        if header:
            self.launch_error = ("domain", header.group(1))
        elif state and state[0] == "domain" and line.rstrip() == CORE_DEVICE_DOMAIN:
            self.launch_error = ("code", state[1])
        elif (state and state[0] == "code" and self.connection_lost is None
              and line.rstrip() == f"Code: {CONNECTION_LOST_CODE}"):
            self.connection_lost = state[1]

    def take(self, line):
        self.take_launch_error(line)
        if line.endswith(RUNNER_UP_LINE):
            self.runner_up += 1
            stamp = LOG_TIME.match(line)
            self.runner_times.append(stamp.group(1) if stamp else "")
            if self.runner_up > 1:
                self.relaunches.append("pending")
        elif self.relaunches:
            text = line.strip()
            if text.startswith(RESTART_LINE) and self.relaunches[-1] == "pending":
                self.relaunches[-1] = "restarted"
            elif text.startswith(NO_TESTS_LINE) and self.relaunches[-1] == "restarted":
                self.relaunches[-1] = "initialized"
        if line.strip() == TEST_STARTED_LINE and self.runner_up == 1:
            self.test_started = True
        if INIT_FAILURE_LINE in line:
            if self.relaunches:
                self.relaunches[-1] = "failed"
            failure = line[line.index(INIT_FAILURE_LINE):][:160]
            cancelled = AUTHENTICATION_CANCELLED.search(line)
            if APPROVAL_TIMEOUT_LINE in line or cancelled:
                # The device asked for an approval and did not get it.
                self.approval_failure = failure
                if cancelled and cancelled.group(1) == "4":
                    self.approval_cause = ("the system cancelled the approval "
                                           "prompt, as when the screen locks "
                                           "during it")
                elif cancelled:
                    self.approval_cause = "the owner cancelled the approval prompt"
                else:
                    self.approval_cause = ("the approval prompt was not "
                                           "answered in time")
            else:
                self.init_failure = failure
        if line.startswith(EXIT_STATUS_LINE):
            self.exit_status = line[len(EXIT_STATUS_LINE):].strip()
        elif line.startswith("ipad-automation:"):
            self.note = line[:200]

    def lost_connection_text(self):
        return (f"the device connection was lost at {self.connection_lost} "
                f"(CoreDevice error {CONNECTION_LOST_CODE})")

    def describe(self):
        """Why the host's run ended, as far as its log says."""
        parts = []
        if self.exit_status:
            parts.append(f"xcodebuild exited with status {self.exit_status}")
        if self.connection_lost:
            parts.append(self.lost_connection_text())
        if self.approval_failure:
            parts.append(f"{self.approval_cause}: {self.approval_failure}")
        elif self.init_failure:
            parts.append(self.init_failure)
        elif self.note and not self.exit_status:
            parts.append(self.note)
        return "; ".join(parts) or "its log says nothing"

    def runner_text(self, served):
        """What the log shows of the runner that may have asked the device for
        an approval nobody gave, for the record: for a host that served, the
        first runner that xcodebuild started again and the log does not show
        initialized; for a launch that never served, its runner."""
        if not served:
            return (f"a runner came up{self.at(self.runner_times[:1])} that did "
                    "not serve, and its log does not say why")
        index = next(number for number, outcome in enumerate(self.relaunches)
                     if outcome != "initialized")
        shown = {"pending": "shows neither a restart notice nor a run of no "
                            "tests",
                 "restarted": "shows the restart notice and not the run of no "
                              "tests that follows",
                 "failed": f"shows an initialization failure ({self.init_failure})",
                 }[self.relaunches[index]]
        parts = [f"xcodebuild started the runner again"
                 f"{self.at(self.runner_times[index + 1:index + 2])} after the "
                 f"host's runner ended; its log {shown}"]
        if self.connection_lost:
            parts.insert(0, self.lost_connection_text())
        return "; ".join(parts)

    @staticmethod
    def at(times):
        return f" at {times[0]}" if times and times[0] else ""


class SleepWatch:
    """Tells that the Mac slept: calling it says whether the wall clock gained
    on the monotonic one (which stops while the Mac sleeps) since the call
    before, by more than WATCH_CLOCK_GAP."""

    def __init__(self, clock):
        self.clock = clock
        self.mono, self.wall = clock.monotonic(), clock.time()

    def __call__(self):
        mono, wall = self.clock.monotonic(), self.clock.time()
        drift = (wall - self.wall) - (mono - self.mono)
        self.mono, self.wall = mono, wall
        return abs(drift) > WATCH_CLOCK_GAP


class HostObserver:
    """Tells, from what a host and its processes show, whether it is dead. It
    says so only on evidence.

    The host writes its heartbeat every 2 s from a queue that no command
    blocks, so a busy host keeps beating, and only a beat that stops advancing
    ends a host. The watch measures that itself, on its own monotonic clock,
    and never subtracts the device's clock from the Mac's: a host is dead when
    WATCH_DEAD_PULLS reads in a row all worked and all showed the same beat,
    and the last one started at least the silence bound after the first one
    ended. A read's time is when it started (a late delivery of an old beat
    is not silence). A read that fails, or shows another host's heartbeat, is
    unknown: it says nothing, the count starts again, and so does a sleep of
    the Mac (a gap between the wall clock and the monotonic one). The same
    reads serve a watch that has just started (it follows the first host it
    can read) and one that has watched for a day.
    The processes only hurry the verdict: a runner that still beats serves,
    whatever happened to the process that started it."""

    def __init__(self, options, nonce, state, process=None, scan=None,
                 clock=CLOCK, on_progress=None, seen=None):
        self.options = options
        self.nonce = nonce
        self.state = state or {}
        self.process = process
        self.scan = scan or LogScan(None)
        self.runners = self.scan.runner_up
        self.clock = clock
        self.on_progress = on_progress
        now = clock.monotonic()
        self.last_mono, self.last_wall = now, clock.time()
        self.last_pull = now
        # When the first read of the current stretch of reads that did not
        # work started.
        self.unknown_since = None
        self.candidate = None
        self.relaunched = False
        # The beat advanced since the watch began to look; the host it
        # follows changed.
        self.advanced = False
        self.switched = False
        self.beat = None
        # (end of the first read of this beat, start of the latest, count).
        self.run = None
        # What the last death verdict was about.
        self.verdict = None
        if state:
            started, ended = seen or (now, now)
            self.note(state, started, ended)

    def supervisor_gone(self):
        if self.process is not None:
            return self.process.poll() is not None
        record = local_record(self.options)
        return bool(self.nonce) and record.get("nonce") == self.nonce and (
            recorded_supervisor(record) is None)

    def progressed(self):
        if self.on_progress is not None:
            self.on_progress()

    def reset(self, forget=False):
        """A verdict that did not stand: count again. With `forget`, the
        verdict was dropped for a gap in what was read (see `forget`)."""
        if forget:
            self.forget()
        self.run = None

    def forget(self):
        """After a gap in what was read (a read that failed, a sleep of the Mac,
        another host's beat, a long wait elsewhere), nothing read before says
        what the host does now: not the count, the beat, or a candidate. The
        next read that works is a first one, and the heartbeat is progress only
        when a read after it advances. A host that beat once more before it died
        shows that last beat on the first read, which proves nothing."""
        self.beat = None
        self.run = None
        self.candidate = None

    def note(self, state, started, ended):
        """A read of this host's heartbeat that worked."""
        beat = state.get("time", 0)
        self.state = state
        self.unknown_since = None
        if self.run is not None and beat == self.beat:
            first_end, _, count = self.run
            self.run = (first_end, started, count + 1)
            return
        if self.beat is not None and beat != self.beat:
            self.advanced = True
            self.progressed()
        self.beat = beat
        self.run = (ended, started, 1)

    def lose(self, started):
        """A read that said nothing about this host. The beat read before it
        goes too: the next read may show a beat the host wrote just before it
        died, which is not progress."""
        self.beat = None
        self.run = None
        if self.unknown_since is None:
            self.unknown_since = started

    def pull(self, since=None):
        """One read of the heartbeat. `since` is the (monotonic, wall) time at
        which the turn that makes it began. A sleep of the Mac in the lookup of
        the host's processes, before the copy, makes the copy a first read: it
        is of the host now. A sleep during the copy throws it away: it may be of
        what the host wrote before the sleep, and the host may have died after
        it."""
        started = self.clock.monotonic()
        wall = self.clock.time()
        self.last_pull = started
        since_mono, since_wall = since or (started, wall)
        if abs((wall - since_wall) - (started - since_mono)) > WATCH_CLOCK_GAP:
            self.forget()
        state = host_state(self.options)
        ended = self.clock.monotonic()
        if abs((self.clock.time() - wall) - (ended - started)) > WATCH_CLOCK_GAP:
            self.forget()
            state = None
        if state is None:
            self.candidate = None
            self.lose(started)
        elif self.nonce is None:
            self.follow(state, started, ended)
        elif state.get("nonce") != self.nonce:
            self.lose(started)
            self.consider(state, started, ended)
        else:
            self.note(state, started, ended)

    def follow(self, state, started, ended):
        """Observes the host this heartbeat is of, from now on."""
        nonce = state.get("nonce")
        record = local_record(self.options)
        self.nonce, self.process = nonce, None
        self.scan = LogScan(record.get("log") if record.get("nonce") == nonce else None)
        self.scan.read()
        self.runners = self.scan.runner_up
        self.beat = self.run = self.candidate = self.unknown_since = None
        self.note(state, started, ended)

    def consider(self, state, started, ended):
        """The heartbeat of another host: it is followed once its beat
        advances between two reads. What an earlier host left behind does not."""
        nonce = state.get("nonce")
        if not nonce or state.get("finished"):
            self.candidate = None
            return
        seen = self.candidate
        self.candidate = (nonce, state.get("time", 0))
        if seen is None or seen[0] != nonce or seen[1] == state.get("time", 0):
            return
        self.follow(state, started, ended)
        self.switched = True
        self.progressed()

    def check(self):
        """None while the host is not shown to be dead, else why it is."""
        clock = self.clock
        now = clock.monotonic()
        wall = clock.time()
        slept = abs((wall - self.last_wall) - (now - self.last_mono)) > WATCH_CLOCK_GAP
        self.last_mono, self.last_wall = now, wall
        if slept:
            # What was read before says nothing of the host now.
            self.forget()
        gone = self.supervisor_gone()
        self.scan.read()
        relaunched = self.scan.runner_up > self.runners
        self.relaunched = relaunched
        quick = gone or relaunched
        if (self.nonce is None or slept or quick
                or now - self.last_pull >= WATCH_PULL_EVERY):
            self.pull((now, wall))
            # A sleep inside the turn is dealt with: the next turn measures
            # from here, and does not take it for another.
            self.last_mono, self.last_wall = clock.monotonic(), clock.time()
        self.verdict = None
        if self.nonce is None:
            return None
        state = self.state
        reason = None
        extra = {}
        if state.get("finished"):
            if state.get("time", 0) >= state.get("deadline", 0) - WATCH_GONE_AGE:
                reason = "the host reached its lifetime"
            else:
                reason = "the host reported that it finished"
            extra = {"finished": True}
        elif self.run is not None:
            first_end, last_start, count = self.run
            silent = last_start - first_end
            if count >= WATCH_DEAD_PULLS and silent >= (
                    WATCH_GONE_AGE if quick else HEARTBEAT_STALE):
                if gone:
                    reason = (f"the supervisor ended ({self.scan.describe()}) and "
                              f"the heartbeat has not advanced for {silent:.0f} s")
                elif relaunched:
                    reason = ("xcodebuild started the runner again, so the host's "
                              "runner ended; the heartbeat has not advanced for "
                              f"{silent:.0f} s")
                else:
                    reason = f"the heartbeat has not advanced for {silent:.0f} s"
        if reason is None and state.get("command") and self.unknown_since is None:
            # The device's own clock, both times: how long it has run.
            running = state.get("time", 0) - state.get("commandStarted", state.get("time", 0))
            if (running > WATCH_BUSY_LIMIT
                    and running > inflight_limit(self.options, state["command"])):
                reason = f"one command has run for {running:.0f} s: the host is hung"
                extra = {"hung": True, "command": state["command"]}
        if reason is not None:
            self.verdict = dict(
                extra, nonce=self.nonce, beat=self.beat, relaunched=relaunched,
                reason=reason,
                record_nonce=local_record(self.options).get("nonce"))
        return reason

    def blind_for(self):
        """How long no read of the heartbeat has worked, in seconds."""
        return 0 if self.unknown_since is None else self.clock.monotonic() - self.unknown_since


class Cadence:
    """Tells when the watch's loop was not scheduled. A turn that starts more
    than WATCH_STALL after the one before it (on the monotonic clock, which
    stops while the Mac sleeps, or on the wall clock, which does not) is
    described with the time its wait took and the time the rest of the turn
    took, which tell a loop that was not run from one held by a read of the
    device."""

    def __init__(self, clock=CLOCK):
        self.clock = clock
        self.last = None
        self.waited = 0.0

    def turn(self):
        """The message for a turn that starts late, or None. Call at the start
        of every turn."""
        now, wall = self.clock.monotonic(), self.clock.time()
        last, self.last = self.last, (now, wall)
        if last is None:
            return None
        gap, wall_gap = now - last[0], wall - last[1]
        if max(gap, wall_gap) <= WATCH_STALL:
            return None
        return (f"the poll loop did not run for {gap:.1f} s by the monotonic "
                f"clock and {wall_gap:.1f} s by the wall clock (a poll comes "
                f"every {WATCH_POLL} s): its wait took {self.waited:.1f} s and "
                f"the rest of the turn {max(0.0, gap - self.waited):.1f} s")

    def wait(self, event, seconds):
        """`clock.wait`, timed. True when `event` is set within `seconds`."""
        began = self.clock.monotonic()
        try:
            return self.clock.wait(event, seconds)
        finally:
            self.waited = self.clock.monotonic() - began

    def reset(self):
        """What happened until now is not a poll (a replacement, which takes
        minutes): the next turn is late only by what comes after this call,
        so a wait that follows it is still timed."""
        self.last = (self.clock.monotonic(), self.clock.time())
        self.waited = 0.0


# What Watch.replace returns when the verdict it was given did not stand and
# the watch looks again.
AGAIN = "again"


class Watch:
    """Keeps one device's host serving; see `watch` in the module's help. It
    acts only on evidence: where it cannot tell, it replaces nothing and ends
    with a status that says what it could not establish.

    `stop` is set to end it; the watch then leaves a serving host as it is."""

    def __init__(self, options, stop=None, clock=CLOCK):
        self.options = options
        self.stop = stop or threading.Event()
        self.clock = clock
        self.record = WatchRecord(options)
        self.latch = ApprovalLatch(options)
        self.observer = None
        self.adopted = False
        self.pending = None
        self.cadence = Cadence(clock)
        self.blind_said = False
        # The hosts the replacement in hand retired (see `replace`).
        self.retired = set()
        self.restarts = 0
        self.restart_times = [t for t in self.record.data.get("restart_times", [])
                              if isinstance(t, (int, float))]

    def say(self, message):
        log(f"watch {time.strftime('%H:%M:%S')}: {message}")

    def progress(self):
        """The observer saw the heartbeat advance: commands may be sent."""
        self.record.update(alive_at=self.clock.time())

    def run(self):
        self.record.update(state=STARTING, detail="", pid=os.getpid(),
                           started=self.clock.time(), restarts=0,
                           restart_times=self.restart_times)
        # A host another launch is still bringing up is that launch's to
        # finish: the owner may be approving it now.
        self.settle_last_launch()
        self.reconcile_last_launch()
        self.observer = HostObserver(self.options, None, None, clock=self.clock,
                                     on_progress=self.progress)
        while not self.stop.is_set():
            late = self.cadence.turn()
            if late:
                self.say(late)
            reason = self.observer.check()
            if self.observer.switched or (self.observer.advanced and not self.adopted):
                self.observer.switched = False
                self.adopt()
            # Whatever the observer concludes from what it saw before, a
            # heartbeat that cannot be read for too long ends the watch.
            code = self.watch_blind()
            if code is not None:
                return code
            if reason is None:
                self.cadence.wait(self.stop, WATCH_POLL)
                continue
            code = self.replace(reason)
            self.cadence.reset()
            if code is AGAIN:
                self.cadence.wait(self.stop, WATCH_POLL)
            elif code is not None:
                return code
        return self.stopped()

    def watch_blind(self):
        """Says so when the heartbeat cannot be read, and ends the watch, with
        nothing replaced, when that lasts too long."""
        blind = self.observer.blind_for()
        if blind >= WATCH_UNKNOWN_LIMIT:
            latch = self.latch.read()
            return self.give_up(
                CANNOT_OBSERVE,
                f"the host's heartbeat could not be read for {blind:.0f} s, so "
                "it is not known whether a host serves; nothing was replaced"
                + (f"; an approval that nobody gave is recorded ({latch.get('kind')}: "
                   f"{latch.get('detail')}); a `start` with the owner at the "
                   "device clears it" if latch else ""),
                EXIT_WATCH_CANNOT_OBSERVE)
        if blind >= 60 and not self.blind_said:
            self.say(f"the host's heartbeat cannot be read for {blind:.0f} s; "
                     "a host that cannot be seen is left alone")
            self.blind_said = True
        elif blind == 0:
            self.blind_said = False
        return None

    def settle_last_launch(self):
        """Before the watch looks at anything: a host another launch is still
        bringing up is waited for, for as long as the wait its starter
        recorded, and no longer once it beats (the observation then decides)
        or its supervisor ended."""
        options = self.options
        record = local_record(options)
        nonce = record.get("nonce")
        if not nonce:
            return
        coming = record.get("started", 0) + (record.get("wait") or options.wait) \
            + WATCH_RUNNER_WAIT
        announced = False
        while not self.stop.is_set():
            if recorded_supervisor(record) is None or self.clock.time() >= coming:
                return
            state = host_state(options)
            if state and state.get("nonce") == nonce:
                return
            if not announced:
                self.say(f"a host that started "
                         f"{self.clock.time() - record.get('started', 0):.0f} s "
                         "ago does not serve yet; waiting for it")
                announced = True
            self.clock.wait(self.stop, WATCH_POLL)

    def reconcile_last_launch(self):
        """What the last launch's own log says of the device's approval,
        whether or not a heartbeat can be read: a failure that nobody answered
        is recorded for good, and kept until a host serves (adopting one
        clears it)."""
        record = local_record(self.options)
        if not record.get("nonce"):
            return
        scan = LogScan(record.get("log"))
        scan.read()
        if scan.approval_failure:
            self.latch.set("observed", record["nonce"], record.get("log"),
                           scan.describe())

    def adopt(self):
        """The host the observer follows serves (its beat advanced): it is
        taken over, touching nothing."""
        observer = self.observer
        state = observer.state
        nonce = observer.nonce
        now = self.clock.time()
        self.say(f"following the host that serves (up {now - state.get('started', now):.0f} s, "
                 f"{state.get('deadline', now) - now:.0f} s left); it is not touched")
        counted = {}
        pending = self.record.data.get("pending")
        if (isinstance(pending, dict) and pending.get("nonce") == nonce
                and isinstance(pending.get("time"), (int, float))):
            # The host a watch that ended began to start: its replacement
            # counts, once, now that it is known to serve.
            self.restart_times = [t for t in self.restart_times
                                  if now - t < WATCH_STORM_WINDOW] + [pending["time"]]
            self.restarts += 1
            counted = {"restart_times": self.restart_times, "restarts": self.restarts}
        # A host serves, so whatever the device was asked is answered.
        self.latch.clear()
        self.adopted = True
        self.pending = None
        self.record.update(state=WATCHING, detail="adopted a serving host",
                           host=nonce, pending=None, alive_at=now, **counted)

    def note_pending(self, nonce, log_path, replacement):
        """Recorded before the host starts: a watch that ends in between
        leaves the replacement to be counted when its host is adopted."""
        self.pending = {"nonce": nonce, "log": str(log_path),
                        "time": self.clock.time(), "replacement": replacement}
        self.record.update(pending=self.pending)

    def recent_restarts(self):
        now = self.clock.time()
        return [t for t in self.restart_times if now - t < WATCH_STORM_WINDOW]

    def recheck(self, verdict):
        """Under the launch lock, before anything is signalled: why the
        verdict no longer stands, or None. It was about one host, and a
        start published since (its record, or a host it is still bringing up)
        is another."""
        options = self.options
        record = local_record(options)
        if record.get("nonce") != verdict.get("record_nonce"):
            return "a start was published meanwhile"
        if record.get("nonce") != verdict["nonce"] and host_unsettled(record, options.wait):
            return "a host another launch started is still coming up"
        state = host_state(options)
        if state is None:
            return UNREADABLE_NOW
        if state.get("nonce") != verdict["nonce"]:
            return OTHER_HOST_NOW
        if verdict.get("hung"):
            if state.get("command") != verdict["command"]:
                return "the command that kept the host busy has ended"
        elif verdict.get("finished"):
            if not state.get("finished"):
                return "the host does not say it finished now"
        elif state.get("time", 0) != verdict["beat"]:
            return "the heartbeat advanced"
        return None

    def approval_scans(self, verdict):
        """The logs that say what the device may have been asked: the host the
        verdict is about (which served, so its first runner needed nothing
        more) and, when the last launch was another one, that launch (which
        never served: the heartbeat is not its)."""
        scans = [(verdict["nonce"], self.observer.scan, True)]
        record = local_record(self.options)
        if record.get("nonce") and record["nonce"] != verdict["nonce"]:
            last = LogScan(record.get("log"))
            scans.append((record["nonce"], last, False))
        return scans

    @staticmethod
    def approval_risk(scans):
        """(kind, nonce, detail) of what a device may have been asked, or
        None. Observed: a log says the approval was not given. Possible: a
        runner came up that is not known to have gone through
        initialization: one started again after the host served whose log
        does not show it initialized (the restart notice, then the totals of
        a run of no tests; a log cut before that says nothing, and nothing is
        not taken for safe), or one of a launch that never served and whose
        test never started. A runner started again that initialized is no
        risk: the system still held the approval, and a new host serves
        without the owner. The first runner of a launch whose test started is
        no risk either: that test runs only after the runner initialized."""
        found = None
        for nonce, scan, served in scans:
            scan.read()
            if scan.approval_failure:
                return "observed", nonce, scan.describe(), scan.path
            initialized = served or scan.test_started
            if found is None and (scan.unconfirmed_relaunches() if initialized
                                  else scan.runner_up):
                found = ("possible", nonce,
                         f"{scan.runner_text(initialized)}, so the device may "
                         "have asked for an approval nobody gave", scan.path)
        return found

    def replace(self, reason):
        """The host the observer follows is dead. Returns the status the watch
        ends with, None once its replacement serves, or AGAIN when what the
        verdict was about changed and the watch looks again.

        Everything that touches the device's host happens under the launch
        lock a `start` takes: recheck the verdict, record what the device may
        have been asked, end what is left of the host, start the next one. A
        started host that does not serve ends the watch: it is not tried
        again, with one exception. A start that was refused before anything
        began, or that ended with the device's connection lost or unreadable
        and nothing asked of the device (its runner never came up, or its test
        had started), is tried again after the device answers again: up to
        WATCH_ATTEMPTS starts in all, each under the lock, and the wait for the
        device, up to WATCH_RECONNECT_WAIT, outside it."""
        options = self.options
        verdict = dict(self.observer.verdict or {})
        self.say(f"the host stopped serving: {reason}")
        self.record.update(state=RESTARTING, detail=reason,
                           last_end={"reason": reason, "time": self.clock.time()})
        detail = ""
        expected = None
        # The hosts this watch ended: what the device's heartbeat may name
        # without being another start's host.
        retired = self.retired = {verdict.get("nonce"), verdict.get("record_nonce")}
        for number in range(1, WATCH_ATTEMPTS + 1):
            try:
                with LaunchLock(options, self.stop.is_set):
                    step = (self.transition(verdict) if number == 1
                            else self.restart(expected, retired))
            except LaunchRefused as refusal:
                if refusal.cancelled:
                    return self.stopped()
                self.say(f"{refusal}; looking again")
                self.observer.reset()
                return AGAIN
            refused = isinstance(step, tuple) and step[0] == "retry"
            if isinstance(step, tuple):
                # Nothing was started, and nothing asked of the device: a start
                # refused before anything began, or one not made because the
                # device could not be checked.
                detail = step[1]
            elif isinstance(step, Launch):
                retired.add(step.nonce)
                outcome = self.await_launch(step)
                launched = (self.pending or {}).get("time", self.clock.time())
                self.pending = None
                kind = outcome[0]
                if kind == "served":
                    self.observer = outcome[1]
                    self.adopted = True
                    self.restarts += 1
                    self.restart_times = self.recent_restarts() + [launched]
                    self.say(f"the replacement serves (restart {self.restarts})")
                    # One write: the pending record is what lets a watch that
                    # ends before it adopts the host count it, so it is
                    # cleared in the update that counts it, not before.
                    self.record.update(
                        state=WATCHING, detail="", host=step.nonce, pending=None,
                        restarts=self.restarts, restart_times=self.restart_times,
                        alive_at=self.clock.time())
                    return None
                self.record.update(pending=None)
                if kind == "stopped":
                    return self.stopped()
                if kind == "approval":
                    return self.give_up(NEEDS_APPROVAL, outcome[1], EXIT_NEEDS_APPROVAL)
                if kind == "remainder":
                    return self.give_up(REMAINDER, outcome[1], EXIT_WATCH_REMAINDER)
                if kind != "retry":
                    return self.give_up(START_FAILED, outcome[1], EXIT_WATCH_GAVE_UP)
                detail = outcome[1]
            else:
                return step
            # What is left of this attempt is ended, and the device was asked
            # for nothing: the one failure that is tried again.
            expected = local_record(options).get("nonce")
            said = "was refused" if refused else "started no host"
            self.say(f"attempt {number} of {WATCH_ATTEMPTS} {said}: {detail}")
            self.record.update(detail=f"attempt {number} {said}: {detail}")
            if number == WATCH_ATTEMPTS:
                break
            # A start refused before anything began waits the backoff alone,
            # for whatever refused it may pass; a host that ended with the
            # connection lost waits first for the device to answer.
            waited = None if refused else self.await_device(detail)
            if waited == "foreign":
                self.say("not starting again: the device's heartbeat names a "
                         "host this watch did not start; looking again")
                self.observer.reset(forget=True)
                return AGAIN
            if waited == "timeout":
                return self.give_up(
                    START_FAILED,
                    f"the device did not answer within {WATCH_RECONNECT_WAIT} s "
                    f"after attempt {number} ({detail})", EXIT_WATCH_GAVE_UP)
            if waited == "stopped" or self.clock.wait(
                    self.stop, WATCH_BACKOFF[number - 1]):
                return self.stopped()
        return self.give_up(
            START_FAILED, f"{WATCH_ATTEMPTS} attempts to start a host "
            f"{'were refused' if refused else 'did not succeed'}; the last: "
            f"{detail}", EXIT_WATCH_GAVE_UP)

    def await_device(self, detail):
        """Outside the launch lock: waits until the device answers a read, or
        WATCH_RECONNECT_WAIT has passed, counted from before the first read and
        covering every read and sleep. None once it answers in time, else
        "timeout", or "stopped" when the watch was told to end. A stop is
        looked for before each read, and no read is longer than
        WATCH_READ_WAIT or than what is left of the wait; an answer that comes
        after the deadline is no answer. "foreign" when the device answers with
        the heartbeat of a host outside `self.retired`."""
        options = self.options
        deadline = self.clock.monotonic() + WATCH_RECONNECT_WAIT
        announced = False
        while True:
            if self.stop.is_set():
                return "stopped"
            left = deadline - self.clock.monotonic()
            if left <= 0:
                return "timeout"
            answered, state = device_reply(
                options, min(WATCH_READ_WAIT, left), clock=self.clock,
                until=deadline, cancelled=self.stop.is_set)
            if self.stop.is_set():
                return "stopped"
            if self.clock.monotonic() >= deadline:
                return "timeout"
            if answered:
                # The read that finds the device answering may show a host
                # another start made meanwhile.
                named = state.get("nonce") if state else None
                if named and named not in self.retired:
                    return "foreign"
                return None
            if not announced:
                self.say(f"the device is not answering ({detail}); waiting up to "
                         f"{WATCH_RECONNECT_WAIT} s for the connection to return")
                self.record.update(
                    state=RESTARTING,
                    detail=f"waiting for device connection: {detail}")
                announced = True
            left = deadline - self.clock.monotonic()
            if self.clock.wait(self.stop, max(0, min(WATCH_PULL_EVERY, left))):
                return "stopped"

    def restart(self, expected, retired):
        """Under the launch lock, for an attempt after the first, once the
        device answered: the start is made again only if nothing changed what
        the first one rested on. A latch set meanwhile refuses it, and so does
        a start published since (its host is another launch's, not this
        watch's to replace). A start from another state directory is not in
        this one's record, so the device's own heartbeat is read as well: one
        that names a host outside `retired` is another start's, and the watch
        goes back to observing it."""
        if self.latch.read():
            return self.refuse_after_approval_failure()
        if local_record(self.options).get("nonce") != expected:
            self.say("not starting again: a start was published meanwhile; "
                     "looking again")
            self.observer.reset(forget=True)
            return AGAIN
        if self.stop.is_set():
            return self.stopped()
        answered, state = device_reply(
            self.options, WATCH_READ_WAIT, clock=self.clock,
            cancelled=self.stop.is_set)
        if self.stop.is_set():
            # Told to end while the check read: nothing more is begun.
            return self.stopped()
        named = state.get("nonce") if state else None
        if named and named not in retired:
            self.say(f"not starting again: the device's heartbeat names a host "
                     f"this watch did not start ({named[:8]}); looking again")
            self.observer.reset(forget=True)
            return AGAIN
        if not answered:
            # A read that failed says nothing of whether another host serves,
            # so it is no permission to start; a device that says there is no
            # heartbeat is a confirmed absence, and is.
            return ("unverified", "the device did not answer the check that no "
                    "other host serves")
        return self.start_attempt()

    def start_attempt(self):
        """Under the launch lock: one start. A Launch; ("retry", why) when
        nothing was started and trying again may help; else what `replace`
        returns."""
        try:
            return launch_host(
                self.options, cancelled=self.stop.is_set, locked=True,
                record=lambda nonce, path: self.note_pending(nonce, path, True))
        except LaunchRefused as refusal:
            if refusal.cancelled:
                return self.stopped()
            if refusal.permanent:
                return self.give_up(CANNOT_START, str(refusal), EXIT_WATCH_GAVE_UP)
            return ("retry", str(refusal))

    def transition(self, verdict):
        """Under the launch lock: retires the dead host and makes the first
        start; what `start_attempt` returns, or what `replace` returns."""
        options = self.options
        observer = self.observer
        why = self.recheck(verdict)
        if why:
            self.say(f"not replacing it: {why}; looking again")
            observer.reset(forget=why in (UNREADABLE_NOW, OTHER_HOST_NOW))
            return AGAIN
        if self.latch.read():
            return self.refuse_after_approval_failure()
        scans = self.approval_scans(verdict)
        risk = self.approval_risk(scans)
        if risk:
            # Before anything is ended: a crash from here on leaves it.
            self.latch.set(risk[0], risk[1], risk[3], risk[2])
        elif scans[0][1].relaunches:
            self.say("xcodebuild started the runner again and it initialized, "
                     "so the device needs no approval: replacing the host")
        elif scans[0][1].connection_lost:
            self.say(f"{scans[0][1].lost_connection_text()} and xcodebuild "
                     "started no runner again, so the device was asked for "
                     "nothing: replacing the host")
        remainder = []
        status = self.end_all(verdict, remainder)
        # What the ended processes printed last counts.
        after = self.approval_risk(scans)
        if after and (not risk or after[0] != risk[0]):
            self.latch.set(after[0], after[1], after[3], after[2])
        if status != ENDED:
            pids = ", ".join(f"{pid} ({command[:120]})" for pid, command in remainder)
            return self.give_up(
                REMAINDER,
                f"what is left of the host that stopped serving is not known to "
                f"be gone ({status}{': ' + pids if pids else ''}); nothing was "
                "started beside it", EXIT_WATCH_REMAINDER)
        # What was unknown before the cleanup and shows now is not undone: the
        # risk that was recorded stands.
        refusal = after or risk
        if refusal:
            return self.refuse_after_approval_failure(
                f"{refusal[2]} (the host that ended: {reason_of(verdict)})")
        recent = self.recent_restarts()
        if len(recent) >= options.max_restarts:
            return self.give_up(
                RESTART_STORM,
                f"{len(recent)} replacements in the last {WATCH_STORM_WINDOW // 60} "
                f"minutes, and the bound is {options.max_restarts}; the host ended "
                "again", EXIT_WATCH_GAVE_UP)
        return self.start_attempt()

    def end_all(self, verdict, remainder):
        """Ends what is left of the host the verdict names and of the last
        launch when that was another; ENDED only when each group was listed
        and is empty."""
        nonce = verdict["nonce"]
        record = local_record(self.options)
        if record.get("nonce") == nonce:
            targets = [(record, self.observer.process)]
        else:
            targets = [({"nonce": nonce}, self.observer.process)]
            if record.get("nonce"):
                targets.append((record, None))
        status = ENDED
        for target, process in targets:
            result = end_host(target, process, remainder=remainder,
                              directory=state_dir(self.options))
            if result != ENDED and status == ENDED:
                status = result
        return status

    def await_launch(self, launch):
        """Waits for the host just started to serve: ("served", observer), or
        ("stopped",), ("approval", why), ("remainder", why) or ("failed", why)
        once it ended.

        Served is a heartbeat of this launch's nonce that advances between two
        reads with no gap between them (the nonce is new, so nothing older can
        show it, but a read that failed or a sleep of the Mac makes the next
        beat a first one again), while the launch's supervisor runs. The log is
        read again after every wait that can take long."""
        options = self.options
        scan = LogScan(launch.log_path)
        deadline = self.clock.monotonic() + options.wait
        runner_up_at = None
        first = None
        unreadable = False
        slept = SleepWatch(self.clock)
        while True:
            if self.stop.is_set():
                return self.finish(launch, scan, "stopped")
            scan.read()
            # A sleep of the Mac since the last look: the next beat is a first
            # one again.
            gap = slept()
            started = self.clock.monotonic()
            state = host_state(options, timeout=WATCH_READ_WAIT)
            unreadable = unreadable or state is None
            ended = self.clock.monotonic()
            if slept():
                # The copy was begun before a sleep and finished after it: it
                # can hold what the host wrote before, so it is thrown away, not
                # kept as the first beat.
                state = None
            if self.stop.is_set():
                # Told to end while the read blocked: nothing more is read.
                return self.finish(launch, scan, "stopped")
            scan.read()
            if scan.approval_failure:
                return self.finish(launch, scan, "approval")
            # A supervisor that ended ended with its xcodebuild: a beat read
            # now is not proof of a host.
            if launch.process.poll() is not None:
                return self.finish(launch, scan, "exited", unreadable)
            if (state and state.get("nonce") == launch.nonce
                    and not state.get("finished")):
                beat = state.get("time", 0)
                if gap or first is None:
                    first = beat
                elif beat != first:
                    return ("served", HostObserver(
                        options, launch.nonce, state, launch.process, scan,
                        self.clock, self.progress, seen=(started, ended)))
            else:
                # A read that failed, or shows another host: the next beat of
                # this launch is a first one again.
                first = None
            now = self.clock.monotonic()
            if scan.runner_up and runner_up_at is None:
                runner_up_at = now
                deadline = now + WATCH_RUNNER_WAIT
            if now > deadline:
                return self.finish(launch, scan, "overdue")
            self.clock.wait(self.stop, WATCH_POLL)

    def finish(self, launch, scan, why, unreadable=False):
        """A launch ended without a serving host. What it may have asked of
        the device is recorded first (a runner that came up and said nothing
        counts, unless its test started), then what is left of it is ended,
        then its log is read once more. Only a launch whose runner never came
        up, or whose test started, is no risk; one of those that ended with the
        device's connection lost or unreadable ("unreadable": a read of its
        heartbeat failed while it started) is told so as ("retry", why)."""
        scan.read()
        scans = [(launch.nonce, scan, False)]
        risk = self.approval_risk(scans)
        if risk:
            self.latch.set(risk[0], risk[1], launch.log_path, risk[2])
        remainder = []
        status = end_host(local_record(self.options), launch.process,
                          remainder=remainder, directory=state_dir(self.options))
        after = self.approval_risk(scans)
        if after and (not risk or after[0] != risk[0]):
            self.latch.set(after[0], after[1], launch.log_path, after[2])
        if status != ENDED:
            pids = ", ".join(f"{pid} ({command[:120]})" for pid, command in remainder)
            return ("remainder", f"what is left of the host that was starting is "
                    f"not known to be gone ({status}{': ' + pids if pids else ''}); "
                    "nothing else was started")
        if why == "stopped":
            return ("stopped",)
        # What was latched before the cleanup stands, whatever the last scan
        # shows (as in `transition`): the record blocks the next watch.
        refusal = after or risk
        if refusal:
            return ("approval", self.approval_text(refusal, launch))
        if why == "exited":
            ended = ("after its test started" if scan.test_started
                     else "before the runner came up")
            detail = (f"xcodebuild ended {ended} ({scan.describe()}); "
                      f"log {launch.log_path}")
            if self.stop.is_set():
                return ("stopped",)
            if (scan.connection_lost or unreadable
                    or host_state(self.options, timeout=WATCH_READ_WAIT) is None):
                return ("retry", detail)
            return ("failed", detail)
        if scan.test_started:
            return ("failed", "the runner came up and its test started, but the "
                    f"heartbeat did not advance within {WATCH_RUNNER_WAIT} s; "
                    f"log {launch.log_path}")
        return ("failed", f"the runner did not come up within {self.options.wait} s; "
                f"log {launch.log_path}")

    @staticmethod
    def approval_text(risk, launch):
        kind, _, detail, _ = risk
        if kind == "observed":
            return ("the device asked for an approval and it was not given "
                    f"({detail}); run `start` with the owner at the device, "
                    "then `watch` again")
        return (f"{detail}: the host started for the replacement ended before "
                f"it served (log {launch.log_path}); run `start` with the owner "
                "at the device, then `watch` again")

    def give_up(self, state, detail, code):
        self.say(f"{STATE_TEXT[state]}: {detail}")
        self.record.update(state=state, detail=detail)
        return code

    def refuse_after_approval_failure(self, detail=None):
        """A watch is not run again after an approval failure by whatever
        starts it again (a job manager, a loop): each try would put another
        prompt on the device. The latch on disk, not this watch's memory, is
        what refuses, and only a host that serves clears it."""
        latch = self.latch.read() or {}
        detail = detail or latch.get("detail", "")
        self.say("a host started for this device may have needed an approval "
                 f"nobody gave ({latch.get('kind')}), and no host serves: run "
                 "`start` with the owner at the device, then `watch` again; "
                 f"starting nothing. {detail}")
        self.record.update(state=NEEDS_APPROVAL, detail=detail,
                           since=latch.get("since", self.clock.time()))
        return EXIT_NEEDS_APPROVAL

    def stopped(self):
        self.say("ending on request; a serving host is left as it is")
        self.record.update(state=STOPPED, detail="ended on request")
        return 0


def reason_of(verdict):
    """What the verdict was about, for the record: why the observer called the
    host dead."""
    return verdict.get("reason") or (
        "finished" if verdict.get("finished") else (
            "hung" if verdict.get("hung") else "its heartbeat stopped"))


def launchd_path():
    """The PATH a job manager's job needs: where this shell finds the device
    tools, then the system's. A job inherits no login shell's."""
    directories = []
    for tool in ("xcrun", "xcodebuild", ios_device_reports.MOVE_TOOL):
        found = shutil.which(tool)
        if found:
            # A relative PATH entry names a directory of this shell's
            # working directory, which the job does not have.
            directories.append(str(pathlib.Path(found).absolute().parent))
    directories += ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]
    return ":".join(dict.fromkeys(directories))


def launchd_job(options, label):
    """The launchd job description that runs this watch.

    The job runs the same foreground `watch` and relies on nothing of the
    environment it is made in: absolute interpreter, script, state directory
    and derived data, and a PATH built for it. launchd starts it once and
    never restarts it (no KeepAlive): every end the watch reports (approval,
    giving up, not being able to tell) is a status, and a job that came back
    after those would put a new prompt on the device. The price is that a
    watch that crashes is not replaced by launchd: its host is left serving,
    and the next poll of `status` shows the watch gone. Processes the watch
    started are left running when it ends.

    The job asks launchd for interactive scheduling: ProcessType Interactive
    (without it a job gets light resource limits that throttle its CPU and
    I/O, which the supervisors and xcodebuild it starts inherit) and
    LegacyTimers (its timers are not coalesced with other processes'), because
    a watch of the default kind was once not run for over an hour while the
    Mac idled with its display off. Nothing else is asked of the system: the
    idle-sleep assertion is the watch's own (`keep_awake`)."""
    state = pathlib.Path(options.state_dir).resolve()
    arguments = [sys.executable, str(SCRIPT), "--device", options.device,
                 "--state-dir", str(state),
                 "--derived-data", str(pathlib.Path(options.derived_data).resolve())]
    if options.runner != RUNNER:
        arguments += ["--runner", options.runner]
    arguments += ["watch", "--hours", f"{options.hours:g}",
                  "--wait", str(options.wait),
                  "--max-restarts", str(options.max_restarts)]
    if options.device_reports_dir:
        # Absolute without resolving: the report collector refuses a path
        # with a symlink in it, and a resolved one would hide that.
        arguments += ["--device-reports-dir",
                      str(pathlib.Path(options.device_reports_dir).absolute())]
    if options.leave_device_reports:
        arguments.append("--leave-device-reports")
    environment = {"PATH": launchd_path()}
    if os.environ.get("DEVELOPER_DIR"):
        environment["DEVELOPER_DIR"] = str(
            pathlib.Path(os.environ["DEVELOPER_DIR"]).absolute())
    # Absolute, as every other path of the job is: launchd's working directory
    # is not this one.
    log_path = (os.path.abspath(options.launchd_log) if options.launchd_log
                else str(state / options.device / "watch-launchd.log"))
    return {
        "Label": label,
        "ProgramArguments": arguments,
        "EnvironmentVariables": environment,
        "RunAtLoad": True,
        "AbandonProcessGroup": True,
        "LimitLoadToSessionType": "Aqua",
        "ProcessType": "Interactive",
        "LegacyTimers": True,
        "StandardOutPath": log_path,
        "StandardErrorPath": log_path,
    }


def detach_watch(options):
    """Runs the same `watch` in a session of its own, with its output in the
    state directory, and returns once it holds the watch lock."""
    if watcher_pid(options) is not None:
        log("a watch already runs for this device and state directory")
        return EXIT_WATCH_LOCKED
    arguments = [word for word in options.argv if word != "--detach"]
    directory = state_dir(options)
    log_path = directory / f"watch-{time.strftime('%Y%m%d-%H%M%S')}.log"
    with open(log_path, "ab") as output:
        child = subprocess.Popen(
            [sys.executable, str(SCRIPT), *arguments], stdin=subprocess.DEVNULL,
            stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
    deadline = time.monotonic() + WATCH_DETACH_WAIT
    while time.monotonic() < deadline:
        status = child.poll()
        if status is not None:
            log(f"the watch ended at once with status {status}; see {log_path}")
            return status
        if watcher_pid(options) == child.pid:
            log(f"watching under pid {child.pid}; log {log_path}")
            return 0
        time.sleep(0.2)
    log(f"the watch (pid {child.pid}) did not take its lock within "
        f"{WATCH_DETACH_WAIT} s; see {log_path}")
    return EXIT_FAILED


def keep_awake():
    """Holds an idle-sleep assertion for as long as this watch runs, and
    returns the process that holds it (None when it cannot be started: the
    watch runs without it). `caffeinate -i` keeps the Mac from idle sleep and
    nothing else: never the display (`-d`), and it does not wake it (`-u`). With
    `-w` it also ends by itself when this process does, however it ends."""
    tool = shutil.which("caffeinate")
    if tool is None:
        log("no caffeinate: the watch runs without an idle-sleep assertion")
        return None
    try:
        return subprocess.Popen(
            [tool, "-i", "-w", str(os.getpid())], stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as error:
        log(f"caffeinate cannot start ({error}): the watch runs without an "
            "idle-sleep assertion")
        return None


def release_awake(holder):
    """Ends what `keep_awake` started; waits for it, so that nothing is left."""
    if holder is None:
        return
    try:
        holder.terminate()
        holder.wait(timeout=10)
    except subprocess.TimeoutExpired:
        holder.kill()
        holder.wait()
    except OSError:
        pass


def command_watch(options, stop=None):
    if options.print_launchd_job:
        job = launchd_job(options, options.print_launchd_job)
        # Printing creates nothing, and launchd will not create the directory
        # its log goes to.
        log(f"the job logs to {job['StandardOutPath']}: before loading it, run "
            f"`mkdir -p {shlex.quote(str(pathlib.Path(job['StandardOutPath']).parent))}`")
        sys.stdout.write(plistlib.dumps(job).decode())
        return 0
    if options.detach:
        return detach_watch(options)
    lock = WatchLock(options)
    if not lock.acquire():
        log("another watch already runs for this device and state directory")
        return EXIT_WATCH_LOCKED
    stop = stop or threading.Event()
    saved = {}
    if threading.current_thread() is threading.main_thread():
        for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            saved[number] = signal.signal(number, lambda *_: stop.set())
    watch = None
    awake = None
    try:
        awake = keep_awake()
        watch = Watch(options, stop)
        return watch.run()
    except Exception as error:  # the watch must say why it ended
        log(traceback.format_exc().rstrip())
        if watch is not None:
            watch.record.update(state=WATCH_ERROR,
                                detail=f"{type(error).__name__}: {error}")
        return EXIT_FAILED
    finally:
        release_awake(awake)
        for number, handler in saved.items():
            signal.signal(number, handler)
        lock.release()


def element_fields(options):
    fields = {}
    if options.id:
        fields["id"] = options.id
    if options.label:
        fields["label"] = options.label
    return fields


def key_character(key):
    """The character the host types for a `key` argument: a key name or one
    character. Raises ValueError for a key XCTest would silently drop."""
    key = DROPPED.get(key, key)
    if key in UNDELIVERABLE:
        raise ValueError(UNDELIVERABLE[key])
    if key in KEYS:
        return KEYS[key]
    # XCTest's typeKey raises for anything but one UTF-16 code unit, so a
    # character outside the Basic Multilingual Plane is refused as well.
    if len(key.encode("utf-16-le")) != 2:
        raise ValueError(f"unknown key {key!r}: name one of "
                         f"{', '.join(sorted(KEYS))} or give one character")
    return key


def build_command(options):
    """The host command for an application operation."""
    op = options.command
    command = {"op": op}
    if op == "launch":
        environment = dict(item.split("=", 1) for item in options.env)
        if options.bundle == andarium.BUNDLE_ID and not options.haptics:
            # An automated launch keeps the phone silent unless it tests haptics.
            environment[andarium.HAPTICS_SETTING] = andarium.HAPTICS_OFF
        command["environment"] = environment
        command["arguments"] = options.arg
    elif op in ("tap", "wait", "exists"):
        command.update(element_fields(options))
        if op == "tap" and options.x is not None:
            command.update(x=options.x, y=options.y)
        if op == "wait":
            command["gone"] = options.gone
        command["timeout"] = options.element_timeout
    elif op in ("type", "siri"):
        command["text"] = options.text
    elif op == "key":
        command["key"] = key_character(options.key)
        command["modifiers"] = [item for item in options.modifiers.split(",") if item]
    elif op == "orientation":
        if options.orientation is not None:
            command["orientation"] = options.orientation
    elif op == "drag":
        if options.seconds < 0 or options.hold < 0:
            raise ValueError("a drag's press and hold durations cannot be negative")
        command.update(x1=options.x1, y1=options.y1, x2=options.x2, y2=options.y2,
                       press=options.seconds, hold=options.hold)
        if options.velocity is not None:
            if options.velocity <= 0:
                raise ValueError("a drag's velocity is a positive number of points per second")
            command["velocity"] = options.velocity
    return command


def bundle_process(bundle):
    """The process name the reports of an application are filed under."""
    return BUNDLE_PROCESSES.get(bundle) or bundle.rsplit(".", 1)[-1]


# XCUIApplication.State of an application that is not running (unknown is 0).
NOT_RUNNING_STATES = (0, 1)


def activation_keeps_phone_silent(options):
    """Whether `activate` may go to the host. The host's activation starts a
    stopped application with no launch environment, so a stopped Andarium
    would start with its haptics on (and a host that is running is never
    rebuilt to carry one): it is refused, and `launch` is the way to start it,
    unless the run tests haptics. A running application is only brought to the
    front."""
    if options.bundle != andarium.BUNDLE_ID or options.haptics:
        return True
    reply = send(options, {"op": "state"}, options.timeout)
    if not reply.get("ok"):
        log(f"cannot tell whether {options.bundle} is running: {reply.get('error', reply)}")
        return False
    state = reply.get("state")
    if state is None or state in NOT_RUNNING_STATES:
        log(f"refusing to activate {options.bundle}: it is not running, and an "
            f"activation starts it without {andarium.HAPTICS_SETTING}="
            f"{andarium.HAPTICS_OFF}, so its haptics would play; use `launch`, "
            "or `activate --haptics` for a run that tests haptics")
        return False
    return True


def run_op(options):
    op = options.command
    try:
        command = build_command(options)
    except ValueError as error:
        log(str(error))
        return EXIT_FAILED
    if op in STARTING_COMMANDS and not check_report_slots(
            options, [bundle_process(command.get("bundle") or options.bundle)]):
        return EXIT_FAILED
    if op == "activate" and not activation_keeps_phone_silent(options):
        return EXIT_FAILED
    reply = send(options, command, options.timeout)
    data = reply.pop("data", None)
    if op == "screenshot" and data is not None:
        pathlib.Path(options.output).write_bytes(data)
        reply["file"] = options.output
    if op == "tree" and reply.get("ok"):
        print(reply.pop("tree"))
    print(json.dumps(reply))
    return 0 if reply.get("ok") else EXIT_FAILED


def collections_help(parser):
    """`start` and the watch collect the device's reports once or more per
    host, each into a new directory beneath the one named."""
    for action in parser._actions:
        if "--device-reports-dir" in action.option_strings:
            action.help = ("where the reports moved off the device go, outside "
                           "every checkout and with no symlink in the path: "
                           "each collection makes a new directory of its own "
                           "in it (default: a per-user folder)")


def parse(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default=os.environ.get("KERNARIUM_AUTOMATION_DEVICE"),
                        help="device UDID (default: $KERNARIUM_AUTOMATION_DEVICE)")
    parser.add_argument("--runner", default=RUNNER, help=argparse.SUPPRESS)
    parser.add_argument("--bundle", default=KERNARIUM,
                        help=f"application a command acts on (default: {KERNARIUM})")
    parser.add_argument("--state-dir", default=os.environ.get(
        "KERNARIUM_AUTOMATION_STATE_DIR", str(CHECKOUT / ".build/kernarium-automation")),
        help="where `start` keeps the host's xcodebuild log, result bundle "
                             "and supervisor pid")
    parser.add_argument("--derived-data", default=str(CHECKOUT / ".build/kernarium-automation/derived"),
                        help="derived data of the host build")
    parser.add_argument("--timeout", type=int, default=120,
                        help="seconds to wait for a command's result (default: 120)")
    commands = parser.add_subparsers(dest="command", required=True)

    build = commands.add_parser("build", help="build the host for iOS devices")
    build.add_argument("--xcodebuild-arg", action="append", default=[],
                       help="extra xcodebuild argument, such as a package cache path")
    start = commands.add_parser("start", help="start the host; the device may ask for approval")
    start.add_argument("--hours", type=float, default=8, help="host lifetime (default: 8)")
    start.add_argument("--wait", type=int, default=240,
                       help="seconds to wait for the host to serve (default: 240)")
    ios_device_reports.add_arguments(start)
    collections_help(start)
    supervise = commands.add_parser("_supervise")
    supervise.add_argument("--xctestrun", required=True)
    supervise.add_argument("--result-bundle", required=True)
    supervise.add_argument("--log", help="xcodebuild's log (this process's own "
                           "output), read for a lost connection to the device")
    supervise.add_argument("--seconds", type=int, required=True)
    supervise.add_argument("--nonce", required=True)
    supervise.add_argument("--term-wait", type=float, default=CHILD_TERM_WAIT,
                           help=argparse.SUPPRESS)
    ios_device_reports.add_arguments(supervise)
    watch = commands.add_parser(
        "watch", help="keep the host serving: start the next one once the old one "
                      "is shown to be dead, and say so when that cannot be shown")
    watch.add_argument("--hours", type=float, default=8,
                       help="lifetime of each host the watch starts (default: 8)")
    watch.add_argument("--wait", type=int, default=240,
                       help="seconds to wait for a new host's runner to come up "
                            "(default: 240)")
    watch.add_argument("--max-restarts", type=int, default=WATCH_MAX_RESTARTS,
                       help="replacements allowed per hour (default: "
                            f"{WATCH_MAX_RESTARTS})")
    watch.add_argument("--detach", action="store_true",
                       help="run the watch in a session of its own and return")
    watch.add_argument("--print-launchd-job", metavar="LABEL",
                       help="print the launchd job that runs this watch, and "
                            "exit; it creates nothing, and the job's log "
                            "directory (the one of --launchd-log, by default "
                            "STATE_DIR/UDID) must exist when it loads: "
                            "`mkdir -p` it first. launchd never "
                            "restarts the job (it has no KeepAlive): a watch "
                            "that crashes stays gone, its host keeps serving, "
                            "and `status` shows the watch missing")
    watch.add_argument("--launchd-log", metavar="PATH",
                       help="the file the printed job logs to (default: "
                            "STATE_DIR/UDID/watch-launchd.log); launchd must be "
                            "able to open the job file and the log itself, so "
                            "both belong on a volume with ownership enabled "
                            "(the internal one) while the state directory may "
                            "be anywhere")
    ios_device_reports.add_arguments(watch)
    collections_help(watch)
    commands.add_parser("status", help="report whether the host is serving")
    stop = commands.add_parser(
        "stop", help="end the host session; starting another needs a new approval")
    stop.add_argument("--force", action="store_true",
                      help="stop even though a new host must be approved at the device")

    commands.add_parser("ping", help="round-trip an empty command")
    launch = commands.add_parser("launch", help="launch (or relaunch) the application")
    launch.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    launch.add_argument("--arg", action="append", default=[])
    launch.add_argument(
        "--haptics", action="store_true",
        help="this run tests haptics: an Andarium launch then states no haptics "
        "setting, so the application's own Haptics switch decides (without it "
        f"the launch states {andarium.HAPTICS_SETTING}={andarium.HAPTICS_OFF} "
        "and the phone stays silent); Kernarium plays none, and ignores it")
    ios_device_reports.add_arguments(launch)
    for name, text in (("activate", "bring the application to the foreground"),
                       ("terminate", "terminate the application"),
                       ("state", "report the application's run state"),
                       ("tree", "print the application's element tree")):
        sub = commands.add_parser(name, help=text)
        if name == "activate":
            sub.add_argument(
                "--haptics", action="store_true",
                help="this run tests haptics: a stopped Andarium may then be "
                "started by activation, with its own Haptics switch deciding "
                "(without it activation of a stopped Andarium is refused: "
                "use launch)")
            ios_device_reports.add_arguments(sub)
    for name, text in (("tap", "tap an element or a point in the application"),
                       ("wait", "wait for an element to appear or disappear"),
                       ("exists", "report whether an element exists")):
        sub = commands.add_parser(name, help=text)
        sub.add_argument("--id", help="accessibility identifier")
        sub.add_argument("--label", help="accessibility label")
        sub.add_argument("--element-timeout", type=float, default=10)
        if name == "tap":
            sub.add_argument("--x", type=float, help="point offset from the application's origin")
            sub.add_argument("--y", type=float)
        if name == "wait":
            sub.add_argument("--gone", action="store_true", help="wait for disappearance")
    drag = commands.add_parser(
        "drag", help="press a point, drag it to another and lift (points from the "
                     "application's origin)")
    for name in ("x1", "y1", "x2", "y2"):
        drag.add_argument(name, type=float)
    drag.add_argument("seconds", type=float, nargs="?", default=0.5,
                      help="how long to press before dragging (default: 0.5)")
    drag.add_argument("--velocity", type=float,
                      help="drag speed in points per second (default: XCTest's)")
    drag.add_argument("--hold", type=float, default=0.0,
                      help="seconds to hold at the end before lifting (default: 0)")
    orientation = commands.add_parser(
        "orientation", help="rotate the device, or report its orientation")
    orientation.add_argument("orientation", nargs="?", choices=ORIENTATIONS)
    type_ = commands.add_parser("type", help="type text into the focused element")
    type_.add_argument("text")
    siri = commands.add_parser(
        "siri", help="start a Siri request with typed text (interrupts the app's audio)")
    siri.add_argument("text")
    key = commands.add_parser("key", help="press one hardware-keyboard key")
    key.add_argument("key", help="one character, or return, tab, space, up, down, left, "
                                 "right; XCTest cannot press delete or escape")
    key.add_argument("--modifiers", default="", help="comma list of shift, control, option, command")
    shot = commands.add_parser("screenshot", help="save a screenshot of the device")
    shot.add_argument("-o", "--output", required=True)

    options = parser.parse_args(argv)
    options.argv = list(argv)
    if not options.device:
        parser.error("name the device with --device or $KERNARIUM_AUTOMATION_DEVICE")
    if options.command == "tap" and (options.x is None) != (options.y is None):
        parser.error("tap needs both --x and --y")
    if options.command == "launch" and any(
            item.partition("=")[0] == andarium.HAPTICS_SETTING for item in options.env):
        parser.error(f"{andarium.HAPTICS_SETTING} is the --haptics option's, not an --env pair")
    if options.command == "watch" and options.launchd_log and not options.print_launchd_job:
        parser.error("--launchd-log goes with --print-launchd-job")
    return options


def main(argv):
    options = parse(argv)
    handlers = {"build": command_build, "start": command_start, "stop": command_stop,
                "status": command_status, "watch": command_watch,
                "_supervise": command_supervise}
    try:
        handler = handlers.get(options.command, run_op)
        if options.command == "build":
            options.timeout = max(options.timeout, 1800)
        return handler(options)
    except NoHost as error:
        log(str(error))
        return EXIT_NO_HOST


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
