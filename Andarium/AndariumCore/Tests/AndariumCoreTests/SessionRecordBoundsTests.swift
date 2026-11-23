// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// Every file the application keeps of a session, and everything a bug report
/// copies of them, has a size it cannot pass whatever the session did.
///
/// The runtime's record is at most 19200 bytes (`session_record_max_bytes`);
/// the application keeps two of them (the current and the previous), a bug
/// report copies each once, adds the stalls and the network counts in words,
/// the guest's counters and at most 30 MetricKit reports of at most 1 MiB.
struct SessionRecordBoundsTests {
    private func scratch() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-bounds-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    private func write(_ text: String, _ name: String, in directory: URL) throws {
        try text.write(to: directory.appendingPathComponent(name), atomically: true, encoding: .utf8)
    }

    @Test func aRecordLongerThanTheRuntimeWritesIsNeitherReadNorCopiedWhole() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        // A header that parses, then far more than 19200 bytes.
        let text = try StallRecordTests.record(stalls: StallRecordTests.golden())
            + String(repeating: "x", count: 4 << 20)
        try write(text, BugReportAttachments.sessionRecord, in: directory)
        #expect(SessionRecord.read(at: directory.appendingPathComponent(BugReportAttachments.sessionRecord)) == nil)
        // The bug report refuses it exactly as the reader does: a line says so.
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        let current = try #require(entries.first { $0.name == "andarium/session-current.txt" })
        #expect(current.data.count < 1024)
        #expect(String(decoding: current.data, as: UTF8.self).contains("was not copied"))
        // An oversized file is not consumed as the previous session's.
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) == nil)
    }

    @Test func aRecordOfBytesThatAreNotTextStaysWithinTheBoundAfterTheyAreReplaced() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        // Each invalid byte becomes U+FFFD, three bytes: a file of exactly the
        // size the runtime may write would triple.
        var data = Data("uml-session 4\n".utf8)
        data.append(Data(repeating: 0xff, count: SessionRecord.maximumBytes - data.count))
        try data.write(to: directory.appendingPathComponent(BugReportAttachments.sessionRecord))
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        let current = try #require(entries.first { $0.name == "andarium/session-current.txt" })
        #expect(current.data.count <= SessionRecord.maximumBytes)
        #expect(String(data: current.data, encoding: .utf8) != nil)
    }

    @Test func aMetricKitReportIsKeptAndCopiedWithinItsCap() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let cap = BugReportAttachments.maximumMetricKitBytes
        MetricKitReports.keep(Data(repeating: 0x61, count: 3 * cap), named: "big.json", in: directory)
        let kept = directory.appendingPathComponent(BugReportAttachments.metricKitDirectory)
            .appendingPathComponent("big.json")
        #expect(try Data(contentsOf: kept).count == cap)
        // A file an earlier version kept whole is cut when it is copied.
        try Data(repeating: 0x62, count: 2 * cap).write(
            to: kept.deletingLastPathComponent().appendingPathComponent("old.json"))
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.filter { $0.name.hasPrefix("andarium/metrickit/") }.allSatisfy { $0.data.count == cap })
        // Only the newest reports are kept.
        for index in 0..<40 {
            MetricKitReports.keep(Data("{}".utf8), named: String(format: "r%02d.json", index), in: directory)
        }
        let names = try FileManager.default.contentsOfDirectory(
            atPath: kept.deletingLastPathComponent().path)
        #expect(names.count == 30)
    }

    @Test func theStallsAreAnEntryOfTheirOwnWhenARecordHasThem() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let golden = try StallRecordTests.golden()
        var slot = golden
        slot += String(repeating: " ", count: 4095 - slot.utf8.count) + "\n"
        try write(
            StallRecordTests.record(stalls: slot, state: "state ended\nend powered-off\nstatus 0\npid 31\n"),
            BugReportAttachments.previousSessionRecord, in: directory)
        try write(
            StallRecordTests.record(stalls: String(repeating: " ", count: 4095) + "\n"),
            BugReportAttachments.sessionRecord, in: directory)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.map(\.name) == [
            "andarium/session-previous.txt", "andarium/session-current.txt", "andarium/stalls.txt",
        ])
        let words = String(decoding: entries[2].data, as: UTF8.self)
        #expect(words.contains("== The previous session (process 31) =="))
        #expect(words.contains("vCPU 2: waiting for a disk write; the thread ran 0% of a core meanwhile."))
        #expect(words.contains("== The current session (process 77) =="))
        #expect(words.contains("The record has no stall slot."))
        #expect(entries[2].data.count <= 2 * StallRecord.maximumReportBytes + 4096)

        // A runtime from before layout 4 has no stalls to report.
        try FileManager.default.removeItem(at: directory.appendingPathComponent(BugReportAttachments.previousSessionRecord))
        try write("uml-session 3\nstate running\nend running\n", BugReportAttachments.sessionRecord, in: directory)
        #expect(BugReportAttachments.entries(from: directory, containers: []).map(\.name)
            == ["andarium/session-current.txt"])
    }

    @Test func theGuestsCountersAreReadWithinTheirLimits() {
        let paths = GuestProcFiles.sources.map(\.path)
        #expect(paths == [
            "/proc/timer_list", "/proc/interrupts", "/proc/stat", "/proc/pressure/cpu", "/proc/pressure/io",
            "/proc/pressure/memory",
        ])
        for source in GuestProcFiles.sources {
            #expect(GuestProcFiles.command.contains("\(source.path):\(source.limit)"))
        }
        #expect(GuestProcFiles.maximumBytes < 256 << 10)
        #expect(GuestProcFiles.connectLimit + GuestProcFiles.commandLimit <= GuestProcFiles.timeBound)

        let nothing = GuestProcFiles.entry(output: "")
        #expect(String(decoding: nothing.data, as: UTF8.self).contains("printed nothing"))
        let entry = GuestProcFiles.entry(output: String(repeating: "interrupts\n", count: 1 << 20))
        #expect(entry.name == "andarium/guest-proc.txt")
        #expect(entry.data.count == GuestProcFiles.maximumBytes)

        let directory = FileManager.default.temporaryDirectory
        let attached = BugReportAttachments.entries(
            from: directory.appendingPathComponent("nothing-\(UUID().uuidString)"), containers: [], guest: [entry])
        #expect(attached.map(\.name) == ["andarium/guest-proc.txt"])
    }

    @Test func aReadThatCouldNotBeMadeNamesEveryFileAndWhy() {
        let entry = GuestProcFiles.entry(failedBecause: "adb said the connection was refused")
        let text = String(decoding: entry.data, as: UTF8.self)
        #expect(text.contains("adb said the connection was refused"))
        for source in GuestProcFiles.sources { #expect(text.contains("==> \(source.path) <==\n(not read)")) }
        #expect(entry.data.count <= GuestProcFiles.maximumBytes)
    }

    /// The command is written once, in `GuestProcFiles.swift`, between two
    /// markers, and the integration test that runs it in an Android guest reads
    /// it from there; the literal in the source and the one the app sends are
    /// the same text.
    @Test func theCommandInTheSourceIsTheCommandTheAppSends() throws {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<3 { url.deleteLastPathComponent() }
        url.append(path: "Sources/AndariumCore/GuestProcFiles.swift")
        let lines = try String(contentsOf: url, encoding: .utf8).components(separatedBy: "\n")
        let begin = try #require(lines.firstIndex { $0.contains("// BEGIN guest-proc-command") })
        let end = try #require(lines.firstIndex { $0.contains("// END guest-proc-command") })
        let literal = Array(lines[(begin + 2)..<(end - 1)])
        #expect(lines[begin + 1].contains("public static let command = #\"\"\""))
        #expect(lines[end - 1] == "\"\"\"#")
        #expect(literal.joined(separator: "\n") == GuestProcFiles.command)
    }

    /// The command as a shell runs it, with `/proc` swapped for a directory of
    /// files and a stub `su` first in the path (the real one is never run): one
    /// file the shell user reads, one only root reads, one that is not there.
    @Test func theCommandReadsEachFileAsTheShellUserThenAsRootAndSaysWhyNot() throws {
        let directory = try scratch()
        defer {
            try? FileManager.default.setAttributes(
                [.posixPermissions: 0o644], ofItemAtPath: directory.appendingPathComponent("stat").path)
            try? FileManager.default.removeItem(at: directory)
        }
        try String(repeating: "t", count: 200_000).write(
            to: directory.appendingPathComponent("timer_list"), atomically: true, encoding: .utf8)
        try "cpu 1 2 3\n".write(to: directory.appendingPathComponent("stat"), atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0], ofItemAtPath: directory.appendingPathComponent("stat").path)
        let bin = directory.appendingPathComponent("bin")
        try FileManager.default.createDirectory(at: bin, withIntermediateDirectories: true)
        let stub = bin.appendingPathComponent("su")
        try """
            #!/bin/sh
            for last; do :; done
            [ -e "$last" ] || { echo "su: $last: not found" >&2; exit 1; }
            echo "read-by-the-stub-su"
            """.write(to: stub, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: stub.path)

        var command = GuestProcFiles.command
        for (proc, name) in [("/proc/timer_list", "timer_list"), ("/proc/stat", "stat")] {
            command = command.replacingOccurrences(of: proc, with: directory.appendingPathComponent(name).path)
        }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", command]
        process.environment = ["PATH": "\(bin.path):/usr/bin:/bin"]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = FileHandle.nullDevice
        try process.run()
        let watchdog = DispatchWorkItem { process.terminate() }
        DispatchQueue.global().asyncAfter(deadline: .now() + 20, execute: watchdog)
        let output = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        process.waitUntilExit()
        watchdog.cancel()

        // The shell user's file is cut to its limit and says so.
        let timer = try #require(output.range(of: "==> \(directory.appendingPathComponent("timer_list").path) <==\n"))
        let afterTimer = output[timer.upperBound...]
        let body = try #require(afterTimer.range(of: "(read as the shell user)"))
        #expect(afterTimer[..<body.lowerBound].utf8.count == 131_072)
        // Only root reads this one.
        #expect(output.contains("read-by-the-stub-su\n(read as root through su)"))
        // The rest are not there, and the entry says why for both ways.
        #expect(output.contains("==> /proc/interrupts <==\n(not readable: "))
        #expect(output.contains("through su: su: /proc/interrupts: not found"))
        #expect(output.components(separatedBy: "==> ").count - 1 == GuestProcFiles.sources.count)
        #expect(output.utf8.count < GuestProcFiles.maximumBytes)
        #expect(process.terminationStatus == 0)
    }

    /// The most a bug report gains from Andarium, which every part above bounds.
    @Test func theWholeOfWhatABugReportGainsIsBounded() {
        let records = 2 * SessionRecord.maximumBytes
        let stalls = 2 * StallRecord.maximumReportBytes + 4096
        let metrics = 30 * BugReportAttachments.maximumMetricKitBytes
        let total = records + stalls + GuestProcFiles.maximumBytes + metrics
        #expect(total < 32 << 20)
    }
}
