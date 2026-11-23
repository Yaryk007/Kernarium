// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

private let root = GuestFilePath.root

@Suite(.timeLimit(.minutes(1)))
struct GuestRunMarkTests {
    @Test func aMarkIsEightLowercaseHexDigitsAndNothingElse() {
        #expect(GuestRunMark("0a1b2c3d")?.value == "0a1b2c3d")
        #expect(GuestRunMark("ffffffff") != nil && GuestRunMark("00000000") != nil)
        for refused in [
            "", "0a1b2c3", "0a1b2c3d4", "0A1B2C3D", "0a1b2c3g", "0a1b2c3*", "0a1b2c3?", "0a1b2c[d", "0a1b2c'd", "0a1b 2cd",
            "0a1b2c3-", "0a1b2c3/", "0a1b2c\u{e9}d", "\u{0661}\u{0662}\u{0663}\u{0664}",
        ] {
            #expect(GuestRunMark(refused) == nil, "\(refused)")
        }
    }

    @Test func aRandomMarkIsAValidOneAndRunsDiffer() {
        let marks = (0..<64).map { _ in GuestRunMark.random() }
        #expect(marks.allSatisfy { GuestRunMark($0.value) == $0 })
        #expect(Set(marks.map(\.value)).count > 60)
    }

    @Test func everyPartialNameCarriesItsRunsMarkAndIsPartial() throws {
        let run = try #require(GuestRunMark("0a1b2c3d"))
        let path = GuestFiles.temporaryPath(in: root + "/it's here", run: run, token: "ab12cd34")
        #expect(path == root + "/it's here/.andarium-0a1b2c3d-ab12cd34.part")
        let name = GuestFilePath.name(of: path)
        #expect(GuestFiles.isPartial(name) && GuestFiles.isHidden(name))
        #expect(!GuestFiles.isLeftover(name, excluding: run))
        let other = try #require(GuestRunMark("ffffffff"))
        #expect(GuestFiles.isLeftover(name, excluding: other))
    }
}

@Suite(.timeLimit(.minutes(2)))
struct GuestFilesSweepTests {
    private let run = GuestRunMark("0a1b2c3d")!

    @Test func theCommandIsOneFindOfTheRootThatSparesTheRunsOwnFiles() {
        #expect(GuestFiles.sweepCommand(excluding: run)
            == "find '\(root)' -type f -name '.andarium-*.part' ! -name '.andarium-0a1b2c3d-*.part' -print -delete")
        #expect(GuestFiles.sweepCommand(in: "/tmp/it's a \"root\"", excluding: run)
            == "find '/tmp/it'\\''s a \"root\"' -type f -name '.andarium-*.part' ! -name '.andarium-0a1b2c3d-*.part' -print -delete")
    }

    @Test func aFinishedSweepCountsWhatItRemovedAndAnythingElseIsNoAnswer() {
        func result(_ text: String, _ code: Int?) -> ShellResult { ShellResult(stdout: Data(text.utf8), exitCode: code) }
        #expect(GuestFiles.parseSweep(result("", 0)) == 0)
        #expect(GuestFiles.parseSweep(result("\(root)/a/.andarium-1.part\n\(root)/.andarium-2.part\n", 0)) == 2)
        // A folder the shell user cannot open makes find exit 1 after the rest.
        #expect(GuestFiles.parseSweep(result("\(root)/a/.andarium-1.part\n", 1)) == 1)
        #expect(GuestFiles.parseSweep(result("", 2)) == nil)
        #expect(GuestFiles.parseSweep(result("", 127)) == nil)
        #expect(GuestFiles.parseSweep(result("\(root)/a/.andarium-1.part\n", nil)) == nil)
    }

    // MARK: The command, run

    /// A tree like a guest's storage, made on this host, and the command run
    /// by the host's shell (`find` is the same one in the guest's toybox for the
    /// options the command uses; the guest's own test runs it there).
    private struct Tree {
        let base: URL
        let storage: URL
        let outside: URL

        init() throws {
            base = FileManager.default.temporaryDirectory
                .appendingPathComponent("SweepTree-\(UUID().uuidString)", isDirectory: true)
            storage = base.appendingPathComponent("emulated 0 'q'", isDirectory: true)
            outside = base.appendingPathComponent("outside", isDirectory: true)
            try FileManager.default.createDirectory(at: storage, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: outside, withIntermediateDirectories: true)
        }

        func remove() { try? FileManager.default.removeItem(at: base) }

        func file(_ relative: String, in top: URL? = nil, _ body: String = "x") throws {
            let url = (top ?? storage).appendingPathComponent(relative)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(body.utf8).write(to: url)
        }

        func folder(_ relative: String) throws {
            try FileManager.default.createDirectory(
                at: storage.appendingPathComponent(relative), withIntermediateDirectories: true)
        }

        func link(_ relative: String, to target: URL) throws {
            let url = storage.appendingPathComponent(relative)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try FileManager.default.createSymbolicLink(at: url, withDestinationURL: target)
        }

        /// Every path below `top`, relative, folders and links included.
        func listing(_ top: URL? = nil) throws -> Set<String> {
            let top = top ?? base
            let names = try FileManager.default.subpathsOfDirectory(atPath: top.path)
            return Set(names)
        }

        func run(_ command: String) throws -> ShellResult {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/bin/sh")
            process.arguments = ["-c", command]
            let out = Pipe()
            let err = Pipe()
            process.standardOutput = out
            process.standardError = err
            try process.run()
            let stdout = out.fileHandleForReading.readDataToEndOfFile()
            let stderr = err.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return ShellResult(stdout: stdout, stderr: stderr, exitCode: Int(process.terminationStatus))
        }
    }

    @Test func itRemovesExactlyTheLeftoverPartialFilesAndNothingElse() throws {
        let tree = try Tree()
        defer { tree.remove() }
        let mine = ".andarium-0a1b2c3d-5e6f7a8b.part"
        let removed = [
            ".andarium-ab12cd34.part",
            "Download/.andarium-0f1e2d3c.part",
            "Download/Trips/2026/.andarium-9a8b7c6d-00000000.part",
            "Movies/it's a \"folder\" with spaces/.andarium-11223344-55667788.part",
            "Movies/it's a \"folder\" with spaces/deeper $(x) `y`/.andarium-aabbccdd.part",
            "Android/data/com.example.app/files/.andarium-12345678-87654321.part",
            // The mark must be a whole component: neither a longer mark that
            // starts like this run's nor this run's mark in the token's place.
            "Download/.andarium-0a1b2c3d5-5e6f7a8b.part",
            "Download/.andarium-ffffffff-0a1b2c3d-5e6f7a8b.part",
            // A file inside a folder named like a partial file is a partial file.
            "Pictures/.andarium-dir.part/.andarium-deadbeef.part",
            ".andarium-.part",
        ]
        let kept = [
            // This run's own, in the root and nested.
            mine,
            "Download/Trips/\(mine)",
            "Movies/it's a \"folder\" with spaces/\(mine)",
            // Names that look like one and are not.
            "Download/.andarium-x.txt",
            "Download/andarium-1.part",
            "Download/.andarium.part",
            "Download/.Andarium-1.part",
            "Download/.andarium-1.part.bak",
            "Download/clip.part",
            "Download/notes.txt",
            "DCIM/photo.jpg",
        ]
        for relative in removed + kept { try tree.file(relative) }
        // A folder named like one, with an ordinary file in it, stays whole.
        try tree.file("Pictures/.andarium-dir.part/keep.txt")
        try tree.folder("Music/.andarium-empty.part")
        try tree.folder("Music/.andarium-\(run.value)-folder.part")
        // A link named like one stays, and so does what it points at.
        try tree.file("elsewhere.dat", in: tree.outside)
        try tree.link("Download/.andarium-link.part", to: tree.outside.appendingPathComponent("elsewhere.dat"))
        try tree.link("Download/.andarium-folderlink.part", to: tree.outside)
        try tree.link("linked outside", to: tree.outside)
        // Outside the root, whatever it is called, is out of reach.
        try tree.file(".andarium-1.part", in: tree.outside)
        try tree.file("Download/.andarium-2.part", in: tree.outside)

        let before = try tree.listing()
        let result = try tree.run(GuestFiles.sweepCommand(in: tree.storage.path, excluding: run))
        #expect(result.exitCode == 0, "\(String(decoding: result.stderr, as: UTF8.self))")
        #expect(GuestFiles.parseSweep(result) == removed.count, "\(result.text)")

        let after = try tree.listing()
        let gone = before.subtracting(after)
        let expectedGone = Set(removed.map { "emulated 0 'q'/" + $0 })
        #expect(gone == expectedGone, "gone: \(gone.sorted()) expected: \(expectedGone.sorted())")
        #expect(after.subtracting(before).isEmpty)
        // The Swift rule and the shell's agree on every name in the tree.
        for path in before {
            let name = (path as NSString).lastPathComponent
            var isFolder: ObjCBool = false
            let full = tree.base.appendingPathComponent(path).path
            _ = FileManager.default.fileExists(atPath: full, isDirectory: &isFolder)
            let isLink = (try? FileManager.default.destinationOfSymbolicLink(atPath: full)) != nil
            let regular = !isFolder.boolValue && !isLink
            let inside = path.hasPrefix("emulated 0 'q'/")
            #expect((regular && inside && GuestFiles.isLeftover(name, excluding: run)) == gone.contains(path), "\(path)")
        }
        // What it left is what it was told to leave, all of it still readable.
        for relative in kept {
            #expect(try String(contentsOf: tree.storage.appendingPathComponent(relative), encoding: .utf8) == "x", "\(relative)")
        }
        #expect(try String(contentsOf: tree.outside.appendingPathComponent("elsewhere.dat"), encoding: .utf8) == "x")
        #expect(try String(contentsOf: tree.storage.appendingPathComponent("Pictures/.andarium-dir.part/keep.txt"), encoding: .utf8) == "x")
        #expect(FileManager.default.fileExists(atPath: tree.outside.appendingPathComponent(".andarium-1.part").path))
        #expect(FileManager.default.fileExists(atPath: tree.outside.appendingPathComponent("Download/.andarium-2.part").path))
    }

    @Test func aRunThatHasNoPartialFilesChangesNothing() throws {
        let tree = try Tree()
        defer { tree.remove() }
        try tree.file("Download/a.txt")
        try tree.file("Download/.andarium-0a1b2c3d-5e6f7a8b.part")
        let before = try tree.listing()
        let result = try tree.run(GuestFiles.sweepCommand(in: tree.storage.path, excluding: run))
        #expect(GuestFiles.parseSweep(result) == 0)
        #expect(try tree.listing() == before)
    }

    @Test func aRootThatIsNotThereIsNoAnswerOfZeroRemoved() throws {
        let tree = try Tree()
        defer { tree.remove() }
        let result = try tree.run(GuestFiles.sweepCommand(in: tree.base.appendingPathComponent("none").path, excluding: run))
        // find reports a missing root as an error, which `parseSweep` takes as
        // a sweep that finished with nothing to do.
        #expect(GuestFiles.parseSweep(result) == 0 && result.exitCode == 1)
    }

    @Test func aFolderItCannotOpenLeavesTheRestSweptAndTheExitAt1() throws {
        let tree = try Tree()
        defer { tree.remove() }
        try tree.file("Open/.andarium-1.part")
        try tree.file("Locked/.andarium-2.part")
        let locked = tree.storage.appendingPathComponent("Locked").path
        try FileManager.default.setAttributes([.posixPermissions: 0o000], ofItemAtPath: locked)
        defer { try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: locked) }
        let result = try tree.run(GuestFiles.sweepCommand(in: tree.storage.path, excluding: run))
        #expect(result.exitCode == 1, "\(result.exitCode.map(String.init) ?? "none")")
        #expect(GuestFiles.parseSweep(result) == 1)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: locked)
        #expect(try tree.listing(tree.storage) == ["Open", "Locked", "Locked/.andarium-2.part"])
    }
}
