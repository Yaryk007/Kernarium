// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// Home's "Translated system libraries": the default, how the choice is kept,
/// what a start states to the emulator and what the session record says of it.
struct TranslatedLibrariesTests {
    private func withDefaults<Result>(_ body: (UserDefaults) -> Result) -> Result {
        let name = "andarium-translated-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        return body(defaults)
    }

    // MARK: The option

    @Test func aFreshInstallAndAnUpdateStartWithTheLibrariesOn() {
        withDefaults { defaults in
            // Neither has a value stored: the key is new in this version.
            #expect(defaults.object(forKey: TranslatedLibrariesPreference.key) == nil)
            let preference = TranslatedLibrariesPreference(defaults: defaults)
            #expect(preference.isOn)
            let start = preference.launch(automated: false)
            #expect(start.isOn && start.keepsChanges)
        }
    }

    @Test func theChoiceSurvivesANewProcessUntilTurnedBackOn() {
        withDefaults { defaults in
            TranslatedLibrariesPreference(defaults: defaults).keep(false)
            let again = TranslatedLibrariesPreference(defaults: defaults)
            #expect(!again.isOn)
            let person = again.launch(automated: false)
            #expect(!person.isOn && person.keepsChanges)
            again.keep(true)
            #expect(TranslatedLibrariesPreference(defaults: defaults).isOn)
        }
    }

    @Test func aHarnessLaunchStartsOnAndLeavesWhatThePersonChoseAlone() {
        withDefaults { defaults in
            let preference = TranslatedLibrariesPreference(defaults: defaults)
            preference.keep(false)
            let harness = preference.launch(automated: true)
            #expect(harness.isOn && !harness.keepsChanges)
            #expect(!preference.isOn, "a launch changed what the person chose")
        }
    }

    // MARK: What a start states

    @Test func aStartStatesTheReleaseOnlyWhileTheOptionIsOnInABuildThatHasIt() {
        for jit in [false, true] {
            // The option is on: the release is stated, and the emulator decides
            // whether the build's artifact fits it.
            #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: true, jitInUse: jit).statesRelease)
            // Off, the launch leaves it out.
            #expect(!TranslatedLibrariesStart(buildCarries: true, optionOn: false, jitInUse: jit).statesRelease)
            // A build without the framework launches as it always did, whatever
            // value an earlier build left stored.
            #expect(TranslatedLibrariesStart(buildCarries: false, optionOn: true, jitInUse: jit).statesRelease)
            #expect(TranslatedLibrariesStart(buildCarries: false, optionOn: false, jitInUse: jit).statesRelease)
        }
    }

    @Test func theRecordSaysWhatTheStartDid() {
        #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: true).word == .on)
        #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: false).word == .off)
        // The JIT does not use them whatever the option says.
        #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: true, jitInUse: true).word == .notUsedWithJit)
        #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: false, jitInUse: true).word == .notUsedWithJit)
        #expect(TranslatedLibrariesStart(buildCarries: false, optionOn: true).word == .unavailable)
        #expect(Set(TranslatedLibrariesStart.Word.allCases.map(\.rawValue))
            == ["on", "off", "not-used-with-jit", "unavailable"])
    }

    // MARK: The start record

    private func scratch() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-start-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    private func start(_ word: TranslatedLibrariesStart.Word, process: Int, at seconds: Int) -> StartRecord {
        StartRecord(translatedLibraries: word, processID: process, at: Date(timeIntervalSince1970: TimeInterval(seconds)))
    }

    private func sessionText(process: Int, started: Int) -> String {
        "uml-session 1\nstate ended\nend powered-off\nstatus 0\npid \(process)\nstarted \(started)\nended \(started + 50)\n"
    }

    private func put(_ text: String, _ file: String, in directory: URL) throws {
        try text.write(to: directory.appendingPathComponent(file), atomically: true, encoding: .utf8)
    }

    private func text(of entry: BugReportArchive.Entry?) -> String? {
        entry.map { String(decoding: $0.data, as: UTF8.self) }
    }

    @Test func theStartRecordReadsBackAndHoldsOnlyItsFields() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent(BugReportAttachments.startRecord)
        for word in TranslatedLibrariesStart.Word.allCases {
            let record = start(word, process: 7, at: 100)
            #expect(record.write(to: url))
            #expect(StartRecord.read(at: url) == record)
        }
        #expect(start(.off, process: 7, at: 100).text == "uml-start 1\ntranslated-libraries off\nprocess 7\nat 100\n")
        // The second is whole, as the session record's times are.
        let whole = StartRecord(
            translatedLibraries: .on, processID: 7, at: Date(timeIntervalSince1970: 100.75))
        #expect(whole.at == Date(timeIntervalSince1970: 100))
    }

    @Test func aFileThatIsNotAStartRecordIsNotOne() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let identity = "process 7\nat 100\n"
        #expect(StartRecord(text: "") == nil)
        #expect(StartRecord(text: "uml-start 1\n") == nil)
        #expect(StartRecord(text: "uml-start 1\ntranslated-libraries maybe\n\(identity)") == nil)
        #expect(StartRecord(text: "uml-start 2\ntranslated-libraries on\n\(identity)") == nil)
        // A record that does not say whose start it is cannot be told from another's.
        #expect(StartRecord(text: "uml-start 1\ntranslated-libraries on\n") == nil)
        #expect(StartRecord(text: "uml-start 1\ntranslated-libraries on\nprocess 7\n") == nil)
        #expect(StartRecord(text: "uml-start 1\ntranslated-libraries on\nat 100\n") == nil)
        #expect(StartRecord(text: "uml-start 1\ntranslated-libraries on\nprocess x\nat 100\n") == nil)
        let long = directory.appendingPathComponent("long.txt")
        try (start(.on, process: 7, at: 100).text + String(repeating: "x", count: 4096))
            .write(to: long, atomically: true, encoding: .utf8)
        #expect(StartRecord.read(at: long) == nil)
        #expect(StartRecord.read(at: directory.appendingPathComponent("missing.txt")) == nil)
        // A field a later version adds does not hide the one this reads.
        #expect(
            StartRecord(text: "uml-start 1\nother 1\ntranslated-libraries off\n\(identity)")?.translatedLibraries == .off)
    }

    @Test func aRecordThatCannotBeWrittenSaysSoAndCostsNothingElse() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let missing = directory.appendingPathComponent("absent").appendingPathComponent("start.txt")
        #expect(!start(.on, process: 7, at: 100).write(to: missing))
    }

    @Test func aWriteReplacesTheEarlierStartsRecord() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent(BugReportAttachments.startRecord)
        #expect(start(.on, process: 7, at: 100).write(to: url))
        #expect(start(.off, process: 8, at: 200).write(to: url))
        #expect(StartRecord.read(at: url) == start(.off, process: 8, at: 200))
    }

    @Test func aStartBelongsToTheSessionItsProcessBeganAtOrAfterIt() {
        let record = start(.off, process: 8, at: 200)
        func session(process: Int, started: Int?) -> SessionRecord {
            SessionRecord(text: "uml-session 1\nstate running\nend running\npid \(process)"
                + (started.map { "\nstarted \($0)" } ?? "") + "\n")!
        }
        #expect(record.belongs(to: session(process: 8, started: 200)))
        #expect(record.belongs(to: session(process: 8, started: 260)))
        // An earlier session of the same process, another process's, and one
        // that says neither when it began.
        #expect(!record.belongs(to: session(process: 8, started: 199)))
        #expect(!record.belongs(to: session(process: 9, started: 260)))
        #expect(!record.belongs(to: session(process: 8, started: nil)))
    }

    @Test func theNextLaunchKeepsTheStartRecordWithTheSessionRecord() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try put(sessionText(process: 7, started: 100), BugReportAttachments.sessionRecord, in: directory)
        start(.off, process: 7, at: 90).write(to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) != nil)
        #expect(StartRecord.read(at: directory.appendingPathComponent(BugReportAttachments.startRecord)) == nil)
        #expect(
            StartRecord.read(at: directory.appendingPathComponent(BugReportAttachments.previousStartRecord))?
                .translatedLibraries == .off)
        // A second launch with no new start says nothing more and keeps it.
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) == nil)
        #expect(
            StartRecord.read(at: directory.appendingPathComponent(BugReportAttachments.previousStartRecord)) != nil)
    }

    @Test func theBugReportCopiesEachStartRecordNextToTheSessionRecordItBelongsTo() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        // No record: the report adds nothing.
        #expect(BugReportAttachments.entries(from: directory, containers: []).isEmpty)
        start(.on, process: 7, at: 90).write(
            to: directory.appendingPathComponent(BugReportAttachments.previousStartRecord))
        start(.notUsedWithJit, process: 8, at: 190).write(
            to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        try put(sessionText(process: 7, started: 100), BugReportAttachments.previousSessionRecord, in: directory)
        try put(sessionText(process: 8, started: 200), BugReportAttachments.sessionRecord, in: directory)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.map(\.name) == [
            "andarium/session-previous.txt", "andarium/session-current.txt",
            "andarium/start-previous.txt", "andarium/start-current.txt",
        ])
        #expect(text(of: entries[2]) == start(.on, process: 7, at: 90).text)
        #expect(text(of: entries[3]) == start(.notUsedWithJit, process: 8, at: 190).text)
        // Text that is not a start record is not copied.
        try put("anything", BugReportAttachments.startRecord, in: directory)
        #expect(!BugReportAttachments.entries(from: directory, containers: []).contains {
            $0.name.hasPrefix("andarium/start-current")
        })
    }

    /// Session A ended and its record rotated to the previous one at the next
    /// launch. That launch's own start, B, wrote its record and then the app
    /// ended before the runtime opened a session record. The launch after
    /// that must not say that A's session was started with B's choice.
    @Test func aStartWhoseSessionNeverBeganIsNotTakenForTheEarlierSessions() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try put(sessionText(process: 7, started: 100), BugReportAttachments.sessionRecord, in: directory)
        start(.on, process: 7, at: 90).write(to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        // The launch of process 8 keeps A's pair, then makes the start B.
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) != nil)
        start(.off, process: 8, at: 200).write(to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        // The next launch finds a start and no session of it.
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) == nil)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        // The session record stays; the choice is not its own.
        #expect(entries.map(\.name).contains("andarium/session-previous.txt"))
        #expect(!entries.map(\.name).contains("andarium/start-previous.txt"))
        #expect(!entries.map(\.name).contains("andarium/start-current.txt"))
        // What the launch found is marked, and the mark is in the copy.
        #expect(text(of: entries.first { $0.name == "andarium/start-previous-unmatched.txt" })
            == StartRecord(
                translatedLibraries: .off, processID: 8, at: Date(timeIntervalSince1970: 200), orphaned: true
            ).text)
    }

    /// The same start, with the process reused and the clock put back: the
    /// retained session's process and time also fit it, and it is still not
    /// that session's.
    @Test func aStartRotatedWithoutASessionStaysUnmatchedWhateverItsProcessAndTimeSay() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try put(sessionText(process: 7, started: 300), BugReportAttachments.sessionRecord, in: directory)
        start(.on, process: 7, at: 290).write(to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) != nil)
        // Process 7 is made again, with the clock behind where session A began,
        // and ends before the runtime opens a session.
        start(.off, process: 7, at: 200).write(to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) == nil)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.map(\.name).contains("andarium/session-previous.txt"))
        #expect(!entries.map(\.name).contains("andarium/start-previous.txt"))
        #expect(text(of: entries.first { $0.name == "andarium/start-previous-unmatched.txt" })?
            .contains("translated-libraries off") == true)
    }

    /// The same sequence with the read of B's start record failing once, as it
    /// does when no file descriptor is free, and succeeding by the time the
    /// report is made: a record that could not be marked is not kept, or it
    /// would be paired with the retained session unmarked.
    @Test func aStartRecordThatCannotBeReadForTheRotationIsNotKept() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try put(sessionText(process: 7, started: 300), BugReportAttachments.sessionRecord, in: directory)
        start(.on, process: 7, at: 290).write(to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) != nil)
        let startFile = directory.appendingPathComponent(BugReportAttachments.startRecord)
        let keptFile = directory.appendingPathComponent(BugReportAttachments.previousStartRecord)
        start(.off, process: 7, at: 200).write(to: startFile)
        try FileManager.default.setAttributes([.posixPermissions: 0], ofItemAtPath: startFile.path)
        try #require(!FileManager.default.isReadableFile(atPath: startFile.path), "the record is still readable")
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) == nil)
        // Readable again by the time the report is made.
        try? FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: keptFile.path)
        let names = BugReportAttachments.entries(from: directory, containers: []).map(\.name)
        #expect(names.contains("andarium/session-previous.txt"))
        #expect(!names.contains("andarium/start-previous.txt"))
        #expect(!FileManager.default.fileExists(atPath: startFile.path))
    }

    /// A time that does not survive being a `Date` would trap when the record
    /// is written back for the rotation, at every launch that finds it.
    @Test func aStartRecordWhoseTimeCannotRoundTripIsNotOne() throws {
        let identity = "uml-start 1\ntranslated-libraries off\nprocess 7\n"
        for time in [Int.max, Int.min, -1, (1 << 53) + 1] {
            #expect(StartRecord(text: identity + "at \(time)\n") == nil, "\(time)")
        }
        for time in [0, 200, 1 << 53] {
            let record = try #require(StartRecord(text: identity + "at \(time)\n"), "\(time)")
            #expect(Int(record.at.timeIntervalSince1970) == time)
        }
        // The launch that finds one rotates nothing and goes on.
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try put(identity + "at \(Int.max)\n", BugReportAttachments.startRecord, in: directory)
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) == nil)
        #expect(BugReportAttachments.entries(from: directory, containers: []).isEmpty)
    }

    @Test func anOrphanedStartRecordReadsBackAndBelongsToNoSession() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent(BugReportAttachments.startRecord)
        let orphan = StartRecord(
            translatedLibraries: .off, processID: 7, at: Date(timeIntervalSince1970: 200), orphaned: true)
        #expect(orphan.text == "uml-start 1\ntranslated-libraries off\nprocess 7\nat 200\norphan 1\n")
        #expect(orphan.write(to: url))
        #expect(StartRecord.read(at: url) == orphan)
        let session = try #require(SessionRecord(text: sessionText(process: 7, started: 300)))
        #expect(!orphan.belongs(to: session))
        var ordinary = orphan
        ordinary.orphaned = false
        #expect(ordinary.belongs(to: session))
        // A record that says nothing of it is not marked.
        #expect(StartRecord(text: "uml-start 1\ntranslated-libraries off\nprocess 7\nat 200\n")?.orphaned == false)
    }

    /// The other way: B's session was written and its start record was not
    /// (the write failed, or the application's earlier record was all there
    /// was). A's choice must not appear as B's.
    @Test(arguments: [7, 8]) func aSessionWithNoStartOfItsOwnDoesNotInheritTheEarlierChoice(process: Int) throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try put(sessionText(process: 7, started: 100), BugReportAttachments.sessionRecord, in: directory)
        start(.on, process: 7, at: 90).write(to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        // The next launch keeps A's pair; its own start's record is never written,
        // and the session B, of this process (the loader reuses one) or of the
        // next, is.
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) != nil)
        try put(sessionText(process: process, started: 300), BugReportAttachments.sessionRecord, in: directory)
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) != nil)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.map(\.name) == ["andarium/session-previous.txt"])
        #expect(text(of: entries.first)?.contains("pid \(process)") == true)
    }

    /// Whatever the files hold, a record is paired by what it says: an earlier
    /// session's choice left beside a later session's record is not its own.
    @Test func aChoiceLeftBesideAnotherProcessesSessionIsLabelledNotPaired() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        start(.on, process: 7, at: 90).write(
            to: directory.appendingPathComponent(BugReportAttachments.previousStartRecord))
        try put(sessionText(process: 8, started: 300), BugReportAttachments.previousSessionRecord, in: directory)
        let names = BugReportAttachments.entries(from: directory, containers: []).map(\.name)
        #expect(names == ["andarium/session-previous.txt", "andarium/start-previous-unmatched.txt"])
    }
}
