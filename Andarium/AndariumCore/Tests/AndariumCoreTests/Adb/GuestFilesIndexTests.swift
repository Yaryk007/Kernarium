// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

private let root = GuestFilePath.root

/// The media index's side of an import: the scan that follows the move.
struct GuestFilesIndexTests {
    @Test func theScanNamesTheFileAsAnEncodedUri() {
        #expect(GuestFiles.scanCommand(for: root + "/Download/Clip.mov")
            == "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://\(root)/Download/Clip.mov'")
        // Bytes a URI reads for something else, and the quote the shell does.
        #expect(GuestFiles.uriPath("/a b#c%d?e'f(g)&h+i;j é/日本.cia")
            == "/a%20b%23c%25d%3Fe%27f%28g%29%26h%2Bi%3Bj%20%C3%A9/%E6%97%A5%E6%9C%AC.cia")
        #expect(GuestFiles.scanCommand(for: root + "/Download/Captain Toad-Treasure Tracker(USA).cia")
            .hasSuffix("-d 'file://\(root)/Download/Captain%20Toad-Treasure%20Tracker%28USA%29.cia'"))
    }

    @Test func theFinishCommandMovesThenScansOnlyAfterTheMove() {
        let command = GuestFiles.finishCommand(
            temporary: root + "/D/.andarium-1.part", path: root + "/D/a b.cia", replacing: false)
        let move = "mv -- '\(root)/D/.andarium-1.part' '\(root)/D/a b.cia'"
        let scan = "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://\(root)/D/a%20b.cia'"
        #expect(command.hasSuffix("\(move) && { \(scan) >/dev/null 2>&1; true; }"))
        // Nothing before the move can scan, and the scan is the last thing.
        let before = command.components(separatedBy: move)[0]
        #expect(!before.contains("am broadcast"))
        #expect(command.components(separatedBy: "am broadcast").count == 2)
    }

    // MARK: The command as a shell runs it

    /// What `finishCommand` does in a real shell, with `mv` as it is and `am`
    /// a script that notes its arguments, in a folder of its own.
    private struct Harness {
        let directory: URL
        var bin: URL { directory.appendingPathComponent("bin") }
        var calls: URL { directory.appendingPathComponent("calls.txt") }

        init() throws {
            directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("FinishCommand-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: bin, withIntermediateDirectories: true)
            let script = bin.appendingPathComponent("am")
            try """
                #!/bin/sh
                for argument in "$@"; do printf '%s\\n' "$argument" >> "\(calls.path)"; done
                printf -- '---\\n' >> "\(calls.path)"
                exit ${AM_EXIT:-0}
                """.write(to: script, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: script.path)
        }

        func remove() { try? FileManager.default.removeItem(at: directory) }

        func path(_ name: String) -> String { directory.appendingPathComponent(name).path }

        func write(_ name: String, _ text: String) throws {
            try text.write(toFile: path(name), atomically: true, encoding: .utf8)
        }

        func read(_ name: String) -> String? { try? String(contentsOfFile: path(name), encoding: .utf8) }

        func exists(_ name: String) -> Bool { FileManager.default.fileExists(atPath: path(name)) }

        /// The arguments of each `am` that ran.
        var scans: [[String]] {
            guard let text = try? String(contentsOf: calls, encoding: .utf8) else { return [] }
            return text.components(separatedBy: "---\n").dropLast().map {
                $0.split(separator: "\n", omittingEmptySubsequences: false).dropLast().map(String.init)
            }
        }

        /// Runs `command` in a shell and returns its exit status.
        func run(_ command: String, amExit: Int = 0) throws -> Int32 {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/bin/sh")
            process.arguments = ["-c", command]
            process.environment = ["PATH": "\(bin.path):/usr/bin:/bin", "AM_EXIT": String(amExit)]
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus
        }

        func finish(replacing: Bool, to name: String = "final.cia", amExit: Int = 0) throws -> Int32 {
            try run(
                GuestFiles.finishCommand(temporary: path(".andarium-1.part"), path: path(name), replacing: replacing),
                amExit: amExit)
        }

        /// What one scan of the file called `name` passes to `am`.
        func scanned(_ name: String) -> [[String]] {
            [[
                "broadcast", "-a", "android.intent.action.MEDIA_SCANNER_SCAN_FILE", "-d",
                "file://" + GuestFiles.uriPath(path(name)),
            ]]
        }
    }

    @Test func aMoveThatWorkedIsFollowedByTheScanOfThatFile() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "body")
        #expect(try harness.finish(replacing: false) == 0)
        #expect(harness.read("final.cia") == "body" && !harness.exists(".andarium-1.part"))
        #expect(harness.scans == harness.scanned("final.cia"))
    }

    @Test func aNameWithSpacesAndQuotesIsMovedAndScannedWhole() throws {
        let harness = try Harness()
        defer { harness.remove() }
        let name = "it's a #1 (copy) 100%.cia"
        try harness.write(".andarium-1.part", "body")
        #expect(try harness.finish(replacing: false, to: name) == 0)
        #expect(harness.read(name) == "body")
        #expect(harness.scans == harness.scanned(name))
        #expect(harness.scans.first?.last?.contains("%20") == true)
    }

    @Test func aNameThatIsTakenIsRefusedAndNothingIsScanned() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "new")
        try harness.write("final.cia", "old")
        #expect(try harness.finish(replacing: false) == Int32(GuestFiles.exitExists))
        #expect(harness.read("final.cia") == "old" && harness.read(".andarium-1.part") == "new")
        #expect(harness.scans.isEmpty)
    }

    @Test func aFolderInTheWayOfAReplaceIsRefusedAndNothingIsScanned() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "new")
        try FileManager.default.createDirectory(atPath: harness.path("final.cia"), withIntermediateDirectories: false)
        #expect(try harness.finish(replacing: true) == Int32(GuestFiles.exitFolder))
        #expect(harness.exists(".andarium-1.part") && harness.scans.isEmpty)
    }

    @Test func aReplaceOfAnIndexedFileScansTheNewOne() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "new")
        try harness.write("final.cia", "old")
        #expect(try harness.finish(replacing: true) == 0)
        #expect(harness.read("final.cia") == "new")
        #expect(harness.scans == harness.scanned("final.cia"))
    }

    @Test func aMoveThatFailsKeepsItsStatusAndNeverScans() throws {
        let harness = try Harness()
        defer { harness.remove() }
        // No partial file: `mv` fails, and its status is the command's.
        #expect(try harness.finish(replacing: false) != 0)
        #expect(!harness.exists("final.cia") && harness.scans.isEmpty)
    }

    @Test func aScanThatFailsDoesNotFailTheImport() throws {
        let harness = try Harness()
        defer { harness.remove() }
        try harness.write(".andarium-1.part", "body")
        #expect(try harness.finish(replacing: false, amExit: 1) == 0)
        #expect(harness.read("final.cia") == "body")
        #expect(harness.scans.count == 1)
    }
}
