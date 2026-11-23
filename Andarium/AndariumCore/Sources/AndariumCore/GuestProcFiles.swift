// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The guest kernel's own counters that bear on a stall, read over adb when a
/// bug report is made and added to it: the timers and interrupts it saw, the
/// CPU time it accounted and the pressure-stall time of its CPU, I/O and
/// memory (`/proc/timer_list`, `/proc/interrupts`, `/proc/stat`,
/// `/proc/pressure/*`). They cover the guest's whole run, so they show with
/// the host's stall record (`StallRecord`) whether time went missing inside
/// the guest or outside it.
///
/// One shell command reads the files and each is cut to its own limit, so the
/// entry is never longer than `maximumBytes`. A file the shell user may not
/// read is tried as root (`su 0`), and one that neither can read says why in
/// its place. The read, connecting included, adds at most `timeBound` to a bug
/// report, and when it cannot be done the entry says so and names each file.
public enum GuestProcFiles {
    public struct Source: Equatable, Sendable {
        public var path: String
        public var limit: Int
    }

    public static let entryName = "andarium/guest-proc.txt"

    /// How long the read may add to a bug report: connecting, then the
    /// command, each within half of it.
    public static let timeBound = Duration.seconds(10)
    public static var connectLimit: Duration { timeBound / 2 }
    public static var commandLimit: Duration { timeBound / 2 }

    /// The command for the guest's shell. The integration test that runs it in
    /// an Android guest (`tests/integration/android/run_android_guest_proc.py`)
    /// takes it from this literal, between the two markers, so the string is
    /// written once. It uses no function and no word that the guest's shell
    /// aliases (Android's mksh aliases `r`, for one), and nothing but POSIX
    /// parameter expansion.
    // BEGIN guest-proc-command
    public static let command = #"""
for entry in /proc/timer_list:131072 /proc/interrupts:32768 /proc/stat:32768 /proc/pressure/cpu:4096 /proc/pressure/io:4096 /proc/pressure/memory:4096; do
path=${entry%:*}; limit=${entry#*:}
echo "==> $path <=="
if head -c $limit "$path" 2>/dev/null; then echo "(read as the shell user)"
elif su 0 head -c $limit "$path" 2>/dev/null; then echo "(read as root through su)"
else echo "(not readable: $(head -c 1 "$path" 2>&1 >/dev/null | head -n 1); through su: $(su 0 head -c 1 "$path" 2>&1 >/dev/null | head -n 1))"
fi
done
"""#
    // END guest-proc-command

    /// The files the command reads, from its first line.
    public static let sources: [Source] = {
        guard let first = command.split(separator: "\n").first,
            let start = first.range(of: "for entry in "), let end = first.range(of: "; do")
        else { return [] }
        return first[start.upperBound..<end.lowerBound].split(separator: " ").compactMap { entry in
            guard let colon = entry.lastIndex(of: ":"), let limit = Int(entry[entry.index(after: colon)...]) else {
                return nil
            }
            return Source(path: String(entry[..<colon]), limit: limit)
        }
    }()

    /// The most the entry holds: the files at their limits and each one's
    /// marker lines, or the reason it was not read.
    public static var maximumBytes: Int { sources.reduce(0) { $0 + $1.limit } + sources.count * 1024 }

    /// The entry for what the command printed.
    public static func entry(output: String) -> BugReportArchive.Entry {
        guard !output.isEmpty else { return entry(failedBecause: "the guest's shell printed nothing") }
        return BugReportArchive.Entry(name: entryName, data: BoundedFile.capped(output, to: maximumBytes))
    }

    /// The entry for a read that could not be made: why, and every file as not
    /// read.
    public static func entry(failedBecause reason: String) -> BugReportArchive.Entry {
        var text = "The guest's /proc files were not read for this report: \(reason).\n"
        for source in sources { text += "==> \(source.path) <==\n(not read)\n\n" }
        return BugReportArchive.Entry(name: entryName, data: BoundedFile.capped(text, to: maximumBytes))
    }
}
