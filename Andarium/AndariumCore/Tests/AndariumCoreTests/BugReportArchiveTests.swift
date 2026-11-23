// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// Andarium's files added to the zip a bug report arrives as, checked with
/// the system's own unzip.
struct BugReportArchiveTests {
    private func scratch() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-archive-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    @discardableResult
    private func run(_ tool: String, _ arguments: [String], in directory: URL) throws -> (Int32, String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = arguments
        process.currentDirectoryURL = directory
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        try process.run()
        let output = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return (process.terminationStatus, String(decoding: output, as: UTF8.self))
    }

    /// A deflated archive as zip(1) makes it, with a comment.
    private func androidLikeArchive(in directory: URL) throws -> URL {
        let content = directory.appendingPathComponent("bugreport.txt")
        try String(repeating: "dumpstate line\n", count: 2000).write(to: content, atomically: true, encoding: .utf8)
        let archive = directory.appendingPathComponent("bugreport.zip")
        let (status, output) = try run("/usr/bin/zip", ["-q", "-9", "bugreport.zip", "bugreport.txt"], in: directory)
        #expect(status == 0, "\(output)")
        return archive
    }

    @Test func entriesGoAfterTheReportsOwn() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let archive = try androidLikeArchive(in: directory)
        try BugReportArchive.append(
            [
                .init(name: "andarium/session-previous.txt", data: Data("state ended\nend guest-panic\n".utf8)),
                .init(name: "andarium/metrickit/metrics-1.json", data: Data("{\"exits\":1}".utf8)),
            ], to: archive)
        let (tested, testOutput) = try run("/usr/bin/unzip", ["-t", archive.path], in: directory)
        #expect(tested == 0, "\(testOutput)")
        let (_, listing) = try run("/usr/bin/unzip", ["-l", archive.path], in: directory)
        #expect(listing.contains("bugreport.txt") && listing.contains("andarium/session-previous.txt")
            && listing.contains("andarium/metrickit/metrics-1.json"))
        let (_, previous) = try run("/usr/bin/unzip", ["-p", archive.path, "andarium/session-previous.txt"], in: directory)
        #expect(previous == "state ended\nend guest-panic\n")
        let (_, original) = try run("/usr/bin/unzip", ["-p", archive.path, "bugreport.txt"], in: directory)
        #expect(original == String(repeating: "dumpstate line\n", count: 2000))
        #expect(!FileManager.default.fileExists(atPath: directory.appendingPathComponent(".bugreport.zip.amending").path))
    }

    @Test func anEmptyArchiveTakesEntries() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let archive = directory.appendingPathComponent("empty.zip")
        var end = Data([0x50, 0x4b, 0x05, 0x06])
        end.append(Data(count: 18))
        try end.write(to: archive)
        try BugReportArchive.append([.init(name: "a.txt", data: Data("one".utf8))], to: archive)
        let (status, output) = try run("/usr/bin/unzip", ["-p", archive.path, "a.txt"], in: directory)
        #expect(status == 0 && output == "one")
    }

    @Test func anythingElseIsLeftAsItWas() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let text = directory.appendingPathComponent("report.zip")
        try Data("not an archive at all, just text".utf8).write(to: text)
        #expect(throws: BugReportArchive.Failure.notAnArchive) {
            try BugReportArchive.append([.init(name: "a", data: Data())], to: text)
        }
        #expect(try Data(contentsOf: text) == Data("not an archive at all, just text".utf8))
        // An end record that says ZIP64.
        let large = directory.appendingPathComponent("zip64.zip")
        var end = Data([0x50, 0x4b, 0x05, 0x06, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF])
        end.append(Data(repeating: 0xFF, count: 8))
        end.append(Data([0, 0]))
        try end.write(to: large)
        #expect(throws: BugReportArchive.Failure.unsupported("a ZIP64 archive")) {
            try BugReportArchive.append([.init(name: "a", data: Data())], to: large)
        }
        #expect(try Data(contentsOf: large) == end)
    }

    @Test func theAttachmentsAreTheRecordsAndReportsRedacted() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let container = directory.path
        try "state ended\nend guest-panic\nNot locking \(container)/data.img\n".write(
            to: directory.appendingPathComponent(BugReportAttachments.previousSessionRecord),
            atomically: true, encoding: .utf8)
        try "state running\nend running\n".write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord), atomically: true, encoding: .utf8)
        for index in 1...3 {
            MetricKitReports.keep(Data("{\"n\":\(index)}".utf8), named: "metrics-\(index).json", in: directory, limit: 2)
        }
        let entries = BugReportAttachments.entries(from: directory, containers: [container])
        #expect(entries.map(\.name) == [
            "andarium/session-previous.txt", "andarium/session-current.txt",
            "andarium/metrickit/metrics-2.json", "andarium/metrickit/metrics-3.json",
        ])
        let previous = String(decoding: entries[0].data, as: UTF8.self)
        #expect(previous.contains("Not locking <container>/data.img") && !previous.contains(container))

        let archive = try androidLikeArchive(in: directory)
        #expect(BugReportAttachments.attach(to: archive, from: directory, containers: [container]))
        let (_, listing) = try run("/usr/bin/unzip", ["-l", archive.path], in: directory)
        #expect(listing.contains("andarium/session-current.txt") && listing.contains("andarium/metrickit/metrics-3.json"))
    }

    @Test func theBugReportCarriesThePreviousProcessAndTheLastEarlierRunThroughoutTheProcess() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let container = directory.path
        let current = directory.appendingPathComponent(BugReportAttachments.sessionRecord)
        // The launch kept the end of the process before, a guest panic.
        try "state ended\nend guest-panic\npid 10\n".write(
            to: directory.appendingPathComponent(BugReportAttachments.previousSessionRecord),
            atomically: true, encoding: .utf8)
        let run = RunRecord(in: directory)
        // Three runs: each is retired as the next is asked for.
        for pid in [11, 12] {
            try "state ended\nend powered-off\npid \(pid)\nNot locking \(container)/data.img\n".write(
                to: current, atomically: true, encoding: .utf8)
            run.retire()
        }
        try "state running\nend running\npid 13\n".write(to: current, atomically: true, encoding: .utf8)
        let entries = BugReportAttachments.entries(from: directory, containers: [container], includingEarlierRun: true)
        #expect(entries.map(\.name) == [
            "andarium/session-previous.txt", "andarium/session-earlier-run.txt", "andarium/session-current.txt",
        ])
        let text = entries.map { String(decoding: $0.data, as: UTF8.self) }
        #expect(text[0].contains("end guest-panic") && text[0].contains("pid 10"))
        // The last earlier run, redacted as every record is.
        #expect(text[1].contains("pid 12") && !text[1].contains("pid 11"))
        #expect(text[1].contains("Not locking <container>/data.img") && !text[1].contains(container))
        #expect(text[2].contains("pid 13"))
    }


    @Test func anEarlierRunThatThisProcessDidNotRetireIsNotCarried() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try "state ended\nend powered-off\npid 12\n".write(
            to: directory.appendingPathComponent(BugReportAttachments.earlierRunRecord), atomically: true, encoding: .utf8)
        try "state running\nend running\npid 13\n".write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord), atomically: true, encoding: .utf8)
        // A file a launch could not file or drop is an older process's.
        #expect(BugReportAttachments.entries(from: directory, containers: []).map(\.name) == [
            "andarium/session-current.txt",
        ])
        #expect(BugReportAttachments.entries(from: directory, containers: [], includingEarlierRun: true).map(\.name) == [
            "andarium/session-earlier-run.txt", "andarium/session-current.txt",
        ])
    }

}
