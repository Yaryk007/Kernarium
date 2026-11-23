// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The root part of a bug report: one small text file inside Android's report,
/// so that whoever reads a report can tell a rooted virtual machine from a
/// stock one before looking at anything else. It holds no identifier, path or
/// package list.
public enum RootReport {
    /// The entry's name in the report's archive.
    public static let name = "andarium/root.txt"

    /// `lastBoot` is the ramdisk the last boot used as the runtime's record of
    /// that boot says (`SessionRecord.ramdisk`), nil when no record says.
    public static func entry(
        state: RootState, release: RootRelease?, lastBoot: SessionRecord.BootRamdisk? = nil
    ) -> BugReportArchive.Entry {
        .init(name: name, data: Data(text(state: state, release: release, lastBoot: lastBoot).utf8))
    }

    public static func text(
        state: RootState, release: RootRelease?, lastBoot: SessionRecord.BootRamdisk? = nil
    ) -> String {
        let booted = lastBoot.map { $0 == .root ? "yes" : "no" } ?? "unknown"
        var lines = [
            "root switch: \(state.on ? "on" : "off")",
            "last start booted the Magisk ramdisk: \(booted)",
        ]
        switch state.attention {
        case .failedStart?: lines.append("paused: a start with root ended before Android booted")
        case .turnedOffAfterFailure?: lines.append("turned off: a start with root ended before Android booted")
        case .stoppedStart?: lines.append("a start with root was stopped before Android booted")
        case nil: break
        }
        lines.append("the release carries root: \(release.map { "Magisk \($0.versionName) (\($0.versionCode))" } ?? "no")")
        lines.append(
            "Magisk app version code the application recorded: \(state.appVersionCode.map(String.init) ?? "none")")
        if let install = state.appInstall {
            let why: String
            switch install.reason {
            case .absent: why = "the app was not in the virtual machine"
            case .older: why = "the app there was older than the release's"
            case .notRecorded: why = "the app there was not recorded as put in by the application for this release"
            }
            lines.append("last Magisk app install: \(install.installed ? "done" : "did not finish"), \(why)")
            if let opened = install.opened {
                lines.append("Magisk app on the screen after that install: \(opened ? "yes" : "no")")
            }
        }
        return lines.joined(separator: "\n") + "\n"
    }
}
