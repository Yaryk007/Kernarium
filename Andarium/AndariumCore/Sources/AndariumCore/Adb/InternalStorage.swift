// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Whether Android has brought its internal storage up. Android reports the
/// end of its boot before it: for some seconds after that, `/storage/emulated/0`
/// is an empty, read-only folder, and then a mount (the media provider's FUSE
/// daemon) fills it in, creates the standard folders, and lets the shell write.
/// A listing or an import made before is false: an empty root, a refused write.
public enum InternalStorage {
    /// What to run in the guest's shell: it prints `ready` once the storage is
    /// the FUSE mount, the shell can write to it and the media provider has made
    /// a standard folder (the first boot of a guest makes them a moment after
    /// the mount), and `starting` before. A person may delete the standard
    /// folders later, so a guest that has been up for five minutes needs only
    /// the mount and a writable root.
    public static let command = """
        r=/storage/emulated/0; if grep -q ' /storage/emulated fuse' /proc/mounts && [ -w $r ] \
        && { [ -d $r/Download ] || [ -d $r/DCIM ] || [ -d $r/Pictures ] || [ -d $r/Music ] \
        || [ -d $r/Movies ] || [ -d $r/Documents ] || [ "$(cut -d. -f1 /proc/uptime)" -gt 300 ]; }; \
        then echo ready; else echo starting; fi
        """

    /// Whether the storage is ready, from the command's output: nil for
    /// anything else.
    public static func parse(_ output: String) -> Bool? {
        switch output.trimmingCharacters(in: .whitespacesAndNewlines) {
        case "ready": true
        case "starting": false
        default: nil
        }
    }

    /// The same from a finished command: nil when it failed.
    public static func parse(_ result: ShellResult) -> Bool? {
        guard result.exitCode == 0 else { return nil }
        return parse(result.text)
    }
}
