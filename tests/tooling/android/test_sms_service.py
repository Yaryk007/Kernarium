#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The text message service's logic on a plain JVM.

android/lineage-24/overlay/sms is the Java app that delivers a text message the host sends. What it builds
(the SMS-DELIVER PDUs, the request's parsing and the answers, the delivery record, the host channel's frame
and server) does not touch the platform: SmsServiceTest.java drives the PDUs and the bytes, reading the PDUs
back with a parser of its own and the JDK's "UTF-16" decoder, which is what Android's SmsMessage uses for the
user data; RecordTest.java the record (a child JVM is ended at each point a request can end at);
ChannelTest.java the server, and the listener over the descriptor it is given (VsockListener) over scripted stand-ins for the
android.system classes in tests/tooling/android/sms/stubs, which make public only the platform's public API (the
image's module is compiled against stubs without @hide members; surface_problems keeps the list). The sources
compile here without the Android glue (Delivery and HostService, which need the framework); the glue is compiled
by the image build. The golden
bytes are tests/unit/andarium/fixtures/sms-golden-frames.txt, which the host's tests read too.
Skipped (status 77) where the host has no JDK.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
SOURCES = REPOSITORY / "android" / "lineage-24" / "overlay" / "sms" / "src" / "android" / "kernarium" / "sms"
TESTS = HERE / "sms" / "android" / "kernarium" / "sms"
STUBS = HERE / "sms" / "stubs"
GOLDEN = REPOSITORY / "tests" / "unit" / "andarium" / "fixtures" / "sms-golden-frames.txt"
ANDROID_GLUE = ("Delivery.java", "HostService.java")
# VsockListener names only android.system, which the stubs stand in for.
STUB_COMPILED = {"VsockListener.java"}
PLATFORM_FREE = {"Answer.java", "DeliverPdu.java", "DeliveryRecord.java", "DeliveryService.java", "HostFrame.java",
                 "Message.java", "RecordFiles.java", "RecordStore.java", "Server.java"}
# The test classes that run, each printing PASS.
MAINS = ("SmsServiceTest", "RecordTest", "ChannelTest")
MANIFEST = SOURCES.parents[3] / "AndroidManifest.xml"
ANDROID = "{http://schemas.android.com/apk/res/android}"
SKIP = 77

# What the service may use of android.system. The image compiles it against the platform's stubs, which leave out
# everything @hide, and nothing here compiles against them, so the stubs in sms/stubs state the public surface
# (javac refuses a call to a member they do not make public) and this lists it. Every name below was compiled by an
# image build, except accept's public overload, which the platform's API file names. A member the platform hides
# (Os.accept with a SocketAddress, which the first peer-from-accept change called, and which no image build could
# compile) is not public in the stub and is not on this list: the service reaches it by reflection only.
OS_PUBLIC = {
    "FileDescriptor accept(FileDescriptor,InetSocketAddress)", "void close(FileDescriptor)",
    "int fcntlInt(FileDescriptor,int,int)", "SocketAddress getpeername(FileDescriptor)", "int poll(StructPollfd[],int)",
    "int read(FileDescriptor,byte[],int,int)", "void shutdown(FileDescriptor,int)",
    "int write(FileDescriptor,byte[],int,int)",
}
# The service takes the listening socket from the holder of the port and never makes, binds or listens on one: the
# stub has none of these, and no source of the service may name them.
OS_NEVER_CALLED = {"socket", "bind", "listen", "connect"}
OS_HIDDEN = {"FileDescriptor accept(FileDescriptor,SocketAddress)"}
OS_CONSTANTS = {
    "EAGAIN", "ECONNABORTED", "EINTR", "FD_CLOEXEC", "F_SETFD", "F_SETFL", "O_NONBLOCK", "POLLERR", "POLLHUP", "POLLIN",
    "POLLOUT", "SHUT_RDWR", "VMADDR_CID_ANY",
}
VM_SOCKET_ADDRESS_CALLS = {"getSvmCid", "getSvmPort"}
METHOD = re.compile(r"^    (public )?static ([\w\[\]]+) (\w+)\(([^)]*)\)", re.MULTILINE)


def manifest_problems() -> list[str]:
    """The application has one component, a service that is not exported, and is not persistent, so that nothing
    runs between messages and nothing but the holder's wake reaches it (U5). A bare application has no class of its
    own to start, and a service has no filter, permission or process of its own."""
    root = ElementTree.parse(MANIFEST).getroot()
    application = root.find("application")
    if application is None:
        return ["the manifest has no application"]
    problems = [f"declares a <{child.tag}> beside its application" for child in root if child.tag != "application"]
    if application.get(f"{ANDROID}persistent") is not None:
        problems.append("sets android:persistent: nothing runs between messages")
    if application.get(f"{ANDROID}name") is not None:
        problems.append("names an application class")
    components = list(application)
    services = [child for child in components if child.tag == "service"]
    problems += [f"declares a <{child.tag}>" for child in components if child.tag != "service"]
    if len(services) != 1:
        problems.append(f"declares {len(services)} services, not one")
        return problems
    service = services[0]
    if service.get(f"{ANDROID}name") != ".HostService":
        problems.append("does not declare HostService")
    if service.get(f"{ANDROID}exported") != "false":
        problems.append("does not say android:exported=\"false\" for its service")
    if len(service) != 0:
        problems.append("gives its service an intent filter or meta-data")
    problems += [f"gives its service android:{attribute}" for attribute in ("permission", "process", "enabled",
                                                                          "isolatedProcess", "foregroundServiceType")
                 if service.get(f"{ANDROID}{attribute}") is not None]
    return problems


def os_methods(source: str) -> tuple[set[str], set[str]]:
    """The signatures of the stub Os's static methods that are public, and of those that are not."""
    public: set[str] = set()
    hidden: set[str] = set()
    for modifier, result, name, parameters in METHOD.findall(source):
        types = ",".join(" ".join(parameter.split()[:-1]) for parameter in parameters.split(",") if parameter.strip())
        (public if modifier else hidden).add(f"{result} {name}({types})")
    return public, hidden


def surface_problems(sources: list[pathlib.Path]) -> list[str]:
    """The stub's public surface is the list above, and the service's calls into android.system are within it."""
    problems = []
    public, hidden = os_methods((STUBS / "android" / "system" / "Os.java").read_text())
    if public != OS_PUBLIC:
        problems.append(f"the stub Os is public for {sorted(public ^ OS_PUBLIC)}, which differs from the list of the "
                        "platform's public API")
    if hidden != OS_HIDDEN:
        problems.append(f"the stub Os hides {sorted(hidden)}, not {sorted(OS_HIDDEN)}")
    for path in sources:
        text = re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", path.read_text(), flags=re.DOTALL))
        for name in set(re.findall(r"\bOs\.(\w+)\(", text)):
            if name in OS_NEVER_CALLED:
                problems.append(f"{path.name} calls Os.{name}: the holder of the port makes, binds and listens on the "
                                "socket, never the service")
            elif not any(signature.split("(")[0].split()[-1] == name for signature in OS_PUBLIC):
                problems.append(f"{path.name} calls Os.{name}, which the platform's public API does not have")
        for constant in set(re.findall(r"\bOsConstants\.(\w+)", text)):
            if constant not in OS_CONSTANTS:
                problems.append(f"{path.name} uses OsConstants.{constant}, which no image build has compiled")
        for call in set(re.findall(r"\.(getSvm\w+)\(", text)):
            if call not in VM_SOCKET_ADDRESS_CALLS:
                problems.append(f"{path.name} calls VmSocketAddress.{call}, which no image build has compiled")
    return problems


def jdk_tool(name: str) -> str | None:
    """A working javac or java: the macOS /usr/bin stubs exist without a JDK, so ask it."""
    path = shutil.which(name)
    if path is None:
        return None
    probe = subprocess.run([path, "-version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


def main() -> int:
    problems = manifest_problems()
    if problems:
        print(f"FAIL: the service's manifest {', '.join(problems)}", file=sys.stderr)
        return 1
    javac, java = jdk_tool("javac"), jdk_tool("java")
    if javac is None or java is None:
        print("SKIP: no JDK (javac and java) on PATH")
        return SKIP
    sources = sorted(path for path in SOURCES.glob("*.java") if path.name not in ANDROID_GLUE)
    if {path.name for path in sources} != PLATFORM_FREE | STUB_COMPILED:
        print(f"FAIL: the sources outside the Android glue are {sorted(path.name for path in sources)}, "
              f"not {sorted(PLATFORM_FREE | STUB_COMPILED)}", file=sys.stderr)
        return 1
    for name in ANDROID_GLUE:
        if not (SOURCES / name).is_file():
            print(f"FAIL: {name} is missing", file=sys.stderr)
            return 1
    problems = surface_problems(sources)
    if problems:
        print("FAIL: the platform's API surface: " + "; ".join(problems), file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="uml-sms-service-") as scratch:
        classes = pathlib.Path(scratch) / "classes"
        classes.mkdir()
        compiled = subprocess.run(
            [javac, "-Xlint:all", "-Werror", "-encoding", "UTF-8", "-d", str(classes),
             *(str(path) for path in sources), *(str(path) for path in sorted(STUBS.rglob("*.java"))),
             *(str(path) for path in sorted(TESTS.glob("*.java")))],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240,
            check=False)
        if compiled.returncode != 0:
            print(f"FAIL: javac:\n{compiled.stdout}", file=sys.stderr)
            return 1
        for main_class in MAINS:
            ran = subprocess.run([java, "-Dfile.encoding=UTF-8", f"-Dsms.golden={GOLDEN}", "-cp", str(classes),
                                  f"android.kernarium.sms.{main_class}"],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                 timeout=240, check=False)
            print(ran.stdout, end="")
            if ran.returncode != 0 or not ran.stdout.rstrip().endswith("PASS"):
                print(f"FAIL: {main_class}", file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
