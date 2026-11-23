// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCoreTestSupport
import Foundation
import Testing
@testable import AndariumCore

/// The durable notice behind "a message may have arrived": its bytes, the
/// sequence of every write (replace, flush the file, flush the directory),
/// what a failed write leaves, and what survives a session.
struct TextMessageNoticeTests {
    private static let x = "00000000000000000000000000000001"
    private static let y = "00000000000000000000000000000002"
    private static let z = "00000000000000000000000000000003"

    typealias RecordingFiles = RecordingNoticeFiles

    private let directory = URL(fileURLWithPath: "/instance")
    private var url: URL { TextMessageNotice.url(in: directory) }

    private func notice(_ files: RecordingFiles) -> TextMessageNotice {
        TextMessageNotice(directory: directory, files: files)
    }

    private func text(_ files: RecordingFiles) -> String {
        files.contents(url).map { String(decoding: $0, as: UTF8.self) } ?? "(none)"
    }

    private let header = "kernarium-sms-pending 1\n"

    // MARK: The bytes

    @Test func theFileIsTheHeaderTheFlagAndTheEntries() async throws {
        let files = RecordingFiles()
        let notice = notice(files)
        try await notice.add(Self.x)
        try await notice.add(Self.y)
        #expect(text(files) == header + Self.x + "\n" + Self.y + "\n")
        notice.endSession()
        try await notice.add(Self.z)
        #expect(text(files) == header + "cut-off\n" + Self.z + "\n")
        // The most it can be: the flag and seventeen entries, 593 bytes.
        let full = TextMessageNotice.encode(
            cutOff: true, entries: (0..<17).map { String(format: "%032x", $0) })
        #expect(full.count == 593)
    }

    /// The session ended, and its send task was cancelled first: the add that was already on its way holds
    /// nothing, so no identifier of a session that is gone joins the next session's memory or file.
    @Test func anAddThatArrivesAfterItsTaskWasCancelledHoldsNothing() async throws {
        let files = RecordingFiles()
        let notice = notice(files)
        let task = Task {
            while !Task.isCancelled { await Task.yield() }
            try await notice.add(Self.x)
        }
        task.cancel()
        notice.endSession()
        await #expect(throws: CancellationError.self) { try await task.value }
        #expect(notice.currentEntries.isEmpty)
        #expect(text(files) == "(none)")
        // A task that was not cancelled still adds.
        try await notice.add(Self.y)
        #expect(notice.currentEntries == [Self.y])
    }

    @Test func aFileThatIsNotExactlyTheFormatCountsAsALeftover() {
        let good = [header, header + "cut-off\n", header + Self.x + "\n", header + "cut-off\n" + Self.x + "\n" + Self.y + "\n"]
        for body in good {
            #expect(TextMessageNotice.decode(Data(body.utf8)) != nil, Comment(rawValue: body))
        }
        let seventeen = (0..<17).map { String(format: "%032x", $0) + "\n" }.joined()
        let eighteen = seventeen + String(format: "%032x", 17) + "\n"
        let bad: [(String, Data)] = [
            ("empty", Data()),
            ("garbage", Data("garbage".utf8)),
            ("another header", Data("kernarium-sms-pending 2\n".utf8)),
            ("no header", Data((Self.x + "\n").utf8)),
            ("no final line end", Data((header + Self.x).utf8)),
            ("a blank line", Data((header + "\n").utf8)),
            ("the flag after an entry", Data((header + Self.x + "\ncut-off\n").utf8)),
            ("the flag twice", Data((header + "cut-off\ncut-off\n").utf8)),
            ("an upper case entry", Data((header + Self.x.uppercased().replacing("0", with: "A") + "\n").utf8)),
            ("a short entry", Data((header + String(Self.x.dropLast()) + "\n").utf8)),
            ("a repeated entry", Data((header + Self.x + "\n" + Self.x + "\n").utf8)),
            ("eighteen entries", Data((header + eighteen).utf8)),
            ("a carriage return", Data((header + Self.x + "\r\n").utf8)),
            ("a byte above 0x7e", Data((header + Self.x + "\n").utf8) + Data([0xC3, 0xA9, 0x0A])),
            ("a trailing space", Data((header + "cut-off \n").utf8)),
        ]
        for (name, data) in bad {
            #expect(TextMessageNotice.decode(data) == nil, Comment(rawValue: name))
        }
        #expect(TextMessageNotice.decode(Data((header + seventeen).utf8)) != nil)
    }

    // MARK: Writes (S13)

    @Test func everyWriteIsReplaceFlushTheFileFlushTheDirectory() async throws {
        let files = RecordingFiles()
        let notice = notice(files)
        try await notice.add(Self.x)
        #expect(files.calls == ["replace", "flushFile", "flushDirectory"])
        files.clearCalls()
        // The removal after presentation runs the same three.
        await notice.resolve(Self.x)
        #expect(files.calls == ["replace", "flushFile", "flushDirectory"])
        #expect(text(files) == header)
        files.clearCalls()
        await notice.clearCutOff()
        #expect(files.calls == ["replace", "flushFile", "flushDirectory"])
    }

    @Test func aFailureOfAnyStepOfTheAddRefusesItAndHoldsNothing() async throws {
        for step in ["replace", "flushFile", "flushDirectory"] {
            let files = RecordingFiles()
            let notice = notice(files)
            files.fail(step)
            await #expect(throws: POSIXError.self, Comment(rawValue: step)) { try await notice.add(Self.x) }
            #expect(files.fired == [step], Comment(rawValue: step))
            #expect(notice.currentEntries.isEmpty, Comment(rawValue: step))
            #expect(!notice.cutOffPending, Comment(rawValue: step))
        }
    }

    /// X unresolved, the add of Y fails, then the add of Z succeeds: the file still held X after the failure and
    /// holds X and Z after the success, written from memory.
    @Test func aFailedAddKeepsEarlierEntriesAndTheNextWriteComesFromMemory() async throws {
        let files = RecordingFiles()
        let notice = notice(files)
        try await notice.add(Self.x)
        files.fail("replace")
        await #expect(throws: POSIXError.self) { try await notice.add(Self.y) }
        files.stop()
        #expect(text(files) == header + Self.x + "\n")
        try await notice.add(Self.z)
        #expect(text(files) == header + Self.x + "\n" + Self.z + "\n")
    }

    @Test func aFailedRemovalChangesNothingForTheSessionAndTheNextOpeningSaysCutOffOnce() async throws {
        let files = RecordingFiles()
        let notice = notice(files)
        try await notice.add(Self.x)
        files.fail("replace")
        await notice.resolve(Self.x)
        files.stop()
        // This session holds nothing; the file still says X.
        #expect(notice.currentEntries.isEmpty && !notice.cutOffPending)
        #expect(text(files) == header + Self.x + "\n")
        // A new launch reads it as a leftover, shows it once, and then there is nothing.
        let relaunched = self.notice(files)
        #expect(relaunched.cutOffPending)
        await relaunched.clearCutOff()
        #expect(!relaunched.cutOffPending)
        #expect(!self.notice(files).cutOffPending)
        #expect(text(files) == header)
    }

    // MARK: What survives (S10)

    @Test func whatASessionLeavesBecomesTheFlagAndSurvivesALaunch() async throws {
        let files = RecordingFiles()
        let notice = notice(files)
        try await notice.add(Self.x)
        // The run ended, or the guest restarted, with the answer still awaited.
        notice.endSession()
        #expect(notice.cutOffPending && notice.currentEntries.isEmpty)
        // A session that ends without anyone opening the bar changes nothing.
        notice.endSession()
        #expect(notice.cutOffPending)
        // Another application launch over the same instance reads the file the first wrote.
        #expect(self.notice(files).cutOffPending)
        // A later add writes the flag with it: the file says both.
        try await notice.add(Self.y)
        #expect(text(files) == header + "cut-off\n" + Self.y + "\n")
        // Showing the flag clears it and keeps the session's own entry.
        await notice.clearCutOff()
        #expect(text(files) == header + Self.y + "\n")
        #expect(!notice.cutOffPending)
    }

    @Test func aDamagedOrUnreadableFileCountsAsALeftoverAndTheNextWriteReplacesIt() async throws {
        let files = RecordingFiles()
        files.put(Data("garbage".utf8), at: url)
        let damaged = notice(files)
        #expect(damaged.cutOffPending)
        await damaged.clearCutOff()
        #expect(text(files) == header)
        #expect(!notice(files).cutOffPending)
        // A file that cannot be read.
        final class Unreadable: NoticeFiles, @unchecked Sendable {
            func read(_ url: URL) throws -> Data? { throw POSIXError(.EACCES) }
            func replace(_ data: Data, at url: URL) throws {}
            func flushFile(at url: URL) throws {}
            func flushDirectory(at url: URL) throws {}
        }
        #expect(TextMessageNotice(directory: directory, files: Unreadable()).cutOffPending)
        // No file, no leftover.
        #expect(!notice(RecordingFiles()).cutOffPending)
    }

    @Test func aResultThatWasNotShownBeforeTheBarClosedLeavesTheEntryAsTheFlag() async throws {
        let files = RecordingFiles()
        let notice = notice(files)
        try await notice.add(Self.x)
        try await notice.add(Self.y)
        notice.leaveForCutOff(Self.x)
        #expect(notice.cutOffPending && notice.currentEntries == [Self.y])
        // The identifier that left is no leftover's: a second call changes nothing.
        notice.leaveForCutOff(Self.x)
        #expect(notice.currentEntries == [Self.y])
    }

    @Test func anEighteenthEntryIsRefused() async throws {
        let files = RecordingFiles()
        let notice = notice(files)
        for number in 0..<TextMessageNotice.capacity { try await notice.add(String(format: "%032x", number)) }
        await #expect(throws: TextMessageNotice.Failure.full) { try await notice.add(String(format: "%032x", 99)) }
        // The same identifier again changes nothing.
        try await notice.add(String(format: "%032x", 0))
        #expect(notice.currentEntries.count == TextMessageNotice.capacity)
    }

    // MARK: On the disk

    private func scratch() throws -> URL {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("notice-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }

    @Test func theRealFilesWriteAndReadTheSameBytes() async throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let notice = TextMessageNotice(directory: root.appendingPathComponent("Guest"))
        try await notice.add(Self.x)
        let written = try Data(contentsOf: TextMessageNotice.url(in: root.appendingPathComponent("Guest")))
        #expect(written == Data((header + Self.x + "\n").utf8))
        #expect(TextMessageNotice(directory: root.appendingPathComponent("Guest")).cutOffPending)
    }

    @Test func aFlushIsFullSyncAndFallsBackToSyncAndFailsWhenBothFail() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("f")
        try Data("x".utf8).write(to: file)
        final class Calls: @unchecked Sendable {
            private let lock = NSLock()
            private var log: [String] = []
            func add(_ name: String) { lock.withLock { log.append(name) } }
            var value: [String] { lock.withLock { log } }
        }
        let works = Calls()
        try DiskNoticeFiles(fullSync: { _ in works.add("full"); return 0 }, sync: { _ in works.add("sync"); return 0 }).flushFile(at: file)
        #expect(works.value == ["full"])
        let falls = Calls()
        try DiskNoticeFiles(fullSync: { _ in falls.add("full"); return -1 }, sync: { _ in falls.add("sync"); return 0 }).flushFile(at: file)
        #expect(falls.value == ["full", "sync"])
        // The directory the same way, and both failing is a failure.
        let both = Calls()
        #expect(throws: POSIXError.self) {
            try DiskNoticeFiles(fullSync: { _ in both.add("full"); return -1 }, sync: { _ in both.add("sync"); return -1 }).flushDirectory(at: root)
        }
        #expect(both.value == ["full", "sync"])
        #expect(throws: POSIXError.self) { try DiskNoticeFiles().flushFile(at: root.appendingPathComponent("missing")) }
    }

    // MARK: Erase and a new data disk (S10h, S10n)

    @Test func theNoticeGoesWithTheDataDiskOnEraseAndOnANewDisk() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let notice = TextMessageNotice.url(in: files.directory)
        #expect(InstanceFiles.textMessageNotice == notice.lastPathComponent)
        try FileManager.default.createDirectory(at: files.directory, withIntermediateDirectories: true)
        try Data("android data".utf8).write(to: files.data)
        try Data((header + "cut-off\n").utf8).write(to: notice)
        try files.removeAll()
        #expect(!FileManager.default.fileExists(atPath: notice.path))
        // A new data disk has no inbox to check.
        try Data((header + "cut-off\n").utf8).write(to: notice)
        let release = ReleaseIdentity(
            releaseID: "rel-a", buildFingerprint: "fp-rel-a", buildNumber: 100, securityPatch: "2026-10-01",
            systemDiskSHA256: String(repeating: "a", count: 64), dataLayout: 1, minimumGiB: 4, defaultGiB: 8, maximumGiB: 64)
        let decision = try files.decide(for: release)
        #expect(decision.action == .create)
        try files.apply(decision, release: release)
        #expect(!FileManager.default.fileExists(atPath: notice.path))
    }
}
