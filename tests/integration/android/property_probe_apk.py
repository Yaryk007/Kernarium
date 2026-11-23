# SPDX-License-Identifier: GPL-2.0-or-later
"""An application that reads the guest's properties as an ordinary application, built without Android's SDK.

run_android_installation_identity.py asks what an ordinary application can read of the guest's
properties and what its interface listing shows. `adb shell run-as` is not that reader: it runs in
`runas_app`, a domain of its own. The package here has one exported broadcast receiver, so
`am broadcast -n` makes the zygote fork a normal application process, which runs in `untrusted_app_29`
(the package targets API 29, the highest a host can sign without Android's tools: the JAR signature
of orientation_probe_apk.py is scheme v1). The receiver's code is property_probe_dex.py; it starts a
shell in the application's own uid and domain that writes its context, whether the control and the key
read as non-empty, and which interfaces the application's interface listing gives an IPv6 link-local
address, into `files/result`. The package is debuggable so that the runner reads the result with
`run-as ... cat`; that read is the runner's, not the probe's.

The manifest is Android's binary XML (orientation_probe_apk.encode_manifest); the signature is a
throwaway key's, so the result belongs to no one.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib

import orientation_probe_apk as binary_xml
import property_probe_dex

PACKAGE = "org.example.andarium.propertyprobe"
RECEIVER = property_probe_dex.CLASS_NAME
COMPONENT = f"{PACKAGE}/{RECEIVER}"
#: Relative to the application's data directory, where `run-as` starts.
RESULT_FILE = "files/result"


def manifest_tree() -> binary_xml.Element:
    """A debuggable application with the INTERNET permission and one exported receiver that has no
    intent filter."""
    return binary_xml.Element("manifest", (("package", PACKAGE), ("versionCode", 1)), (
        binary_xml.Element("uses-sdk", (("minSdkVersion", binary_xml.MIN_SDK),
                                        ("targetSdkVersion", binary_xml.TARGET_SDK))),
        # An application without it has no network group, so its interface listing fails: the probe
        # holds what the applications that matter hold.
        binary_xml.Element("uses-permission", (("name", "android.permission.INTERNET"),)),
        binary_xml.Element("application", (("label", "Property Probe"), ("debuggable", True)), (
            binary_xml.Element("receiver", (("name", RECEIVER), ("exported", True))),
        )),
    ))


def build_apk(destination: pathlib.Path, control: str, key: str,
              workdir: pathlib.Path | None = None) -> pathlib.Path:
    """The signed package at `destination`: the property names the receiver's script reads are
    `control` and `key`."""
    dex = property_probe_dex.build_dex(PACKAGE, control, key)
    return binary_xml.build_apk(destination, workdir, manifest_tree(), {"classes.dex": dex})
