// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// What the application chose for the start of a session, in a small file
/// beside the runtime's session record (`session.txt`), so that a bug report
/// says whether a feature that changes how Android runs was on for the start
/// it describes. It holds one plain field per choice and nothing that identifies
/// the device or the person. It names the process and the second of the start
/// it describes, which is how a session record is known to be the one the
/// runtime wrote for that start (`belongs(to:)`). The next launch keeps it with
/// the session record (`PreviousSession`), and the bug report copies the two
/// together when they belong to each other (`BugReportAttachments`). A launch
/// that finds a start record and no session record of its own marks the start
/// `orphaned`, which belongs to no session whatever its process and time say.
public struct StartRecord: Equatable, Sendable {
    /// The first line.
    public static let layout = "uml-start 1"

    /// The longest a record is read: it has a few short lines.
    public static let maximumBytes = 1024

    /// The seconds a record may state: the whole numbers a time can hold
    /// exactly, so that a record read is written back as it was. A time past
    /// them is not a time any clock gave.
    public static let seconds = 0...(1 << 53)

    /// Whether the translated system libraries were on for the start.
    public var translatedLibraries: TranslatedLibrariesStart.Word
    /// The process that made the start.
    public var processID: Int
    /// When it made it, to the second, as the session record's own times are.
    public var at: Date
    /// Whether the next launch found this record with no session record of
    /// its own: the runtime never opened the session. Such a start stays
    /// unmatched, though a retained session's process and time may also fit.
    public var orphaned: Bool

    public init(translatedLibraries: TranslatedLibrariesStart.Word, processID: Int, at: Date, orphaned: Bool = false) {
        self.translatedLibraries = translatedLibraries
        self.processID = processID
        self.at = Date(timeIntervalSince1970: at.timeIntervalSince1970.rounded(.down))
        self.orphaned = orphaned
    }

    /// The file's text.
    public var text: String {
        "\(Self.layout)\ntranslated-libraries \(translatedLibraries.rawValue)\n"
            + "process \(processID)\nat \(Int(at.timeIntervalSince1970))\n" + (orphaned ? "orphan 1\n" : "")
    }

    /// Whether `session` is the one the runtime wrote for this start: the same
    /// process, and begun no earlier than the start. A session that began
    /// before it is an earlier one's, and a record the runtime never wrote for
    /// this start (it ended first) has no session to belong to, nor has one a
    /// later launch found without a session of its own (`orphaned`).
    public func belongs(to session: SessionRecord) -> Bool {
        guard !orphaned, let started = session.started else { return false }
        return session.processID == processID && started >= at
    }

    /// Reads a record's text; nil when it is not one.
    public init?(text: String) {
        let lines = text.split(separator: "\n", omittingEmptySubsequences: true)
        guard lines.first == Substring(Self.layout) else { return nil }
        var word: TranslatedLibrariesStart.Word?
        var process: Int?
        var seconds: Int?
        var orphan = false
        for line in lines.dropFirst() {
            guard let space = line.firstIndex(of: " ") else { continue }
            let value = String(line[line.index(after: space)...])
            switch line[..<space] {
            case "translated-libraries": word = TranslatedLibrariesStart.Word(rawValue: value)
            case "process": process = Int(value)
            case "at": seconds = Int(value)
            case "orphan": orphan = value == "1"
            default: break
            }
        }
        // A record that does not say whose start it is cannot be told from
        // another's, so it is not one.
        guard let word, let process, let seconds, Self.seconds.contains(seconds) else { return nil }
        translatedLibraries = word
        processID = process
        at = Date(timeIntervalSince1970: TimeInterval(seconds))
        orphaned = orphan
    }

    /// The record at `url`, or nil when there is none, it is longer than
    /// `maximumBytes` or it is not a record.
    public static func read(at url: URL) -> StartRecord? {
        guard let data = BoundedFile.read(url, limit: maximumBytes), data.count <= maximumBytes,
            let text = String(data: data, encoding: .utf8)
        else { return nil }
        return StartRecord(text: text)
    }

    /// Writes the record, replacing the one there; false when it could not be
    /// written, which costs the bug report this one field and nothing else.
    /// The one there goes first, so that a write that fails leaves no earlier
    /// start's record to be read as this one's.
    @discardableResult
    public func write(to url: URL) -> Bool {
        try? FileManager.default.removeItem(at: url)
        do {
            try Data(text.utf8).write(to: url, options: .atomic)
            return true
        } catch {
            print("Andarium: the start record was not written: \(error.localizedDescription)")
            return false
        }
    }
}
