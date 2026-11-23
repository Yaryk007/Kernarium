// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// How the previous process's guest session ended, as the runtime recorded
/// it (darwin/runtime/session_record.hpp).
///
/// The runtime writes "running" when the guest boots and overwrites it with
/// the end on every way the session ends that runs any code: Android's own
/// power-off or restart, a stop, a guest kernel panic with its message, an
/// exit the kernel drives, a hibernated guest whose RAM could not be
/// restored, a crash of a host thread and an exit() of the process. A record
/// still running at the next launch says that the process ended without
/// running any of that code: the system ended it, as an end at the memory
/// limit does, or it crashed outside the guest's paths, which leaves only
/// the system's crash report.
public struct SessionRecord: Equatable, Sendable {
    /// The last of the runtime's memory checkpoints, a few seconds apart.
    public struct Memory: Equatable, Sendable {
        public var at: Date
        public var footprintBytes: UInt64
        public var peakBytes: UInt64
        /// What the process's memory limit still allowed; nil where the host
        /// sets no limit.
        public var availableBytes: UInt64?
    }

    public var running: Bool
    /// The runtime's word for the end: running, powered-off, restarted,
    /// stopped, failed, guest-panic, kernel-abort, kernel-exit,
    /// hibernation-restore-failed, process-exit or host-fault.
    public var end: String
    public var status: Int
    public var processID: Int
    public var started: Date?
    public var ended: Date?
    /// The panic's message, or what else the end said.
    public var message: String?
    /// The guest kernel's last output before a crash, oldest line first.
    public var kernelOutput: String?
    public var memory: Memory?
    /// Whether the guest's console had said that Android finished booting, for
    /// a guest whose console says so; nil in a record that does not say (an
    /// earlier runtime, or a guest that never reports it).
    public var bootFinished: Bool?
    /// How far the guest's boot had got, for a guest whose console reports it,
    /// with "unobserved" kept apart from a record that says nothing of the
    /// boot (nil): a guest that panics before init's first line leaves
    /// "unobserved".
    public var bootProgress: BootProgress?
    /// Which ramdisk the boot started with, when the record says (the runtime
    /// writes it for a guest that reports its boot): the release's own, the
    /// Magisk-patched one, or recovery's. The record of a boot the runtime
    /// never began has none.
    public var ramdisk: BootRamdisk?

    public enum BootProgress: String, Equatable, Sendable {
        case completed, unfinished, unobserved
    }

    public enum BootRamdisk: String, Equatable, Sendable {
        case stock, root, recovery
    }
    /// How many times the guest had restarted in the process before this
    /// session, for a guest whose boot the record follows.
    public var restarts: Int?
    /// The footprint's large terms at the last checkpoint and at the
    /// session's highest one, in MiB, as the runtime words them ("guest G gpu
    /// H/B ..."); nil in a record that has none.
    public var terms: String?
    public var peakTerms: String?
    /// The stall record of the session, from layout 4 on: when the guest's
    /// vCPUs stopped running on the host and what the host was doing then; nil
    /// in an earlier layout or when the slot is of a version this reader does
    /// not know.
    public var stalls: StallRecord?
    /// What the guest's user network counted and whether Andarium's notice that
    /// iOS denies it the network was shown, from layout 5 on; nil in an earlier
    /// layout or when the slot is of a version this reader does not know.
    public var network: NetworkRecord?
    /// How often the guest's storage was settled and how often the guest did
    /// not confirm it, from layout 7 on; nil in an earlier layout, when the
    /// slot is blank (no settle yet) or when it is of a version this reader
    /// does not know.
    public var storage: StorageRecord?
    /// How the platform presented each of the guest's cameras and the stills of
    /// the guest's cameras that did not end in a picture and why, from layout 8
    /// on; nil in an earlier layout, when the slot is blank (no camera presented
    /// and no failure) or when it is of a version this reader does not know.
    public var camera: CameraRecord?

    /// The layouts this reader knows: 1 to 3 keep the session's state right
    /// after the memory slot, 4 puts the stall slot between them, 5 the
    /// network slot after the stall slot, 6 the figures of the run in each
    /// history row of an 8 KiB memory slot, 7 the storage slot after the
    /// network slot and 8 the camera slot after the storage slot. The rows are
    /// for the person reading the bug report; this reader takes none of them.
    static let layouts = [
        "uml-session 1", "uml-session 2", "uml-session 3", "uml-session 4", "uml-session 5", "uml-session 6",
        "uml-session 7", "uml-session 8",
    ]

    /// The read cap, 20 KiB: the runtime writes at most 19200 bytes
    /// (`session_record_max_bytes`), so a file longer than the cap is not one
    /// it wrote, and is neither read nor copied beyond it. The gap is slack for
    /// a later layout, not a limit the runtime reaches.
    public static let maximumBytes = 20 << 10

    /// The output's own text: the runtime pads the state's area with newlines to
    /// its full size, so the output ends with one newline, as the guest's
    /// last line did, and not with the padding.
    private static func withoutPadding(_ output: String) -> String {
        guard output.hasSuffix("\n") else { return output }
        var text = Substring(output)
        while text.hasSuffix("\n") { text.removeLast() }
        return text + "\n"
    }

    /// Reads a record's text; nil when it is not one.
    public init?(text: String) {
        let parts = text.components(separatedBy: "\nkernel-output\n")
        var fields: [String: String] = [:]
        let lines = parts[0].split(separator: "\n", omittingEmptySubsequences: true)
            .filter { !$0.allSatisfy { $0 == " " } }
        guard let format = lines.first, Self.layouts.contains(String(format))
        else { return nil }
        for line in lines.dropFirst() {
            guard let space = line.firstIndex(of: " ") else { continue }
            fields[String(line[..<space])] = String(line[line.index(after: space)...])
        }
        guard let state = fields["state"], let end = fields["end"] else { return nil }
        running = state == "running"
        self.end = end
        status = fields["status"].flatMap { Int($0) } ?? 0
        processID = fields["pid"].flatMap { Int($0) } ?? 0
        started = fields["started"].flatMap { TimeInterval($0) }.map { Date(timeIntervalSince1970: $0) }
        ended = fields["ended"].flatMap { TimeInterval($0) }.map { Date(timeIntervalSince1970: $0) }
        message = fields["message"]
        kernelOutput = parts.count > 1 ? Self.withoutPadding(parts[1]) : nil
        memory = fields["memory"].flatMap(Self.memory(from:))
        bootFinished = fields["boot"].flatMap { ["completed": true, "unfinished": false][$0] }
        bootProgress = fields["boot"].flatMap(BootProgress.init(rawValue:))
        ramdisk = fields["ramdisk"].flatMap(BootRamdisk.init(rawValue:))
        restarts = fields["restarts"].flatMap { Int($0) }
        terms = fields["terms"]
        peakTerms = fields["peak-terms"]
        let hasStalls = ["uml-session 4", "uml-session 5", "uml-session 6", "uml-session 7", "uml-session 8"]
            .contains(String(format))
        stalls = hasStalls ? StallRecord(text: parts[0]) : nil
        let hasNetwork = ["uml-session 5", "uml-session 6", "uml-session 7", "uml-session 8"]
            .contains(String(format))
        network = hasNetwork ? NetworkRecord(text: parts[0]) : nil
        storage = ["uml-session 7", "uml-session 8"].contains(String(format))
            ? StorageRecord(text: parts[0]) : nil
        camera = format == "uml-session 8" ? CameraRecord(text: parts[0]) : nil
    }

    /// "at T footprint F peak P [available A]".
    private static func memory(from line: String) -> Memory? {
        let words = line.split(separator: " ")
        var values: [String: UInt64] = [:]
        var index = 0
        while index + 1 < words.count {
            if let value = UInt64(words[index + 1]) { values[String(words[index])] = value }
            index += 2
        }
        guard let at = values["at"], let footprint = values["footprint"] else { return nil }
        return Memory(
            at: Date(timeIntervalSince1970: TimeInterval(at)), footprintBytes: footprint,
            peakBytes: values["peak"] ?? footprint, availableBytes: values["available"]
        )
    }

    /// The record at `url`, or nil when there is none, it is longer than
    /// `maximumBytes` or it is not a record.
    public static func read(at url: URL) -> SessionRecord? {
        guard let data = BoundedFile.read(url, limit: maximumBytes), data.count <= maximumBytes,
            let text = String(data: data, encoding: .utf8)
        else { return nil }
        return SessionRecord(text: text)
    }

    /// Whether the session ended on its own terms: Android's power-off or
    /// restart, or a stop.
    public var orderly: Bool {
        !running && ["powered-off", "restarted", "stopped"].contains(end)
    }

    /// Below this, at the last checkpoint, a record still running most
    /// likely ended at the memory limit: the GPU and the guest can take that
    /// much between two checkpoints.
    public static let lowMemoryBytes: UInt64 = 256 << 20

    /// Whether the last checkpoint left the process close to its limit.
    public var nearMemoryLimit: Bool {
        guard let available = memory?.availableBytes else { return false }
        return available < Self.lowMemoryBytes
    }

    /// One line for a log or a report.
    public var summary: String {
        summaryOfEnd + bootSentence
    }

    /// What the record adds of the boot when it did not finish.
    private var bootSentence: String {
        guard bootFinished == false else { return "" }
        let times = restarts.map { $0 > 0 ? " and the guest had restarted \($0) time\($0 == 1 ? "" : "s") in this process" : "" } ?? ""
        return " Android had not finished booting\(times)."
    }

    private var summaryOfEnd: String {
        if running {
            var line = "The previous session was still running when the process ended (pid \(processID))"
            if let memory {
                line += "; at the last checkpoint it used \(memory.footprintBytes >> 20) MiB"
                if let available = memory.availableBytes { line += ", \(available >> 20) MiB left" }
            }
            return line + "."
        }
        let detail = message.map { ": \($0)" } ?? ""
        switch end {
        case "guest-panic":
            return "The previous session ended in a guest kernel panic\(detail)."
        case "kernel-abort":
            return "The previous session ended in a guest kernel crash\(detail)."
        case "kernel-exit":
            return "The guest kernel ended the previous process with status \(status)\(detail)."
        case "hibernation-restore-failed":
            return "The previous session ended because its guest RAM could not be restored\(detail)."
        case "process-exit":
            return "The previous process exited while its guest ran\(detail)."
        case "host-fault":
            return "The previous process crashed in Andarium's own code, with a crash report\(detail)."
        default:
            return "The previous session ended: \(end)\(detail)."
        }
    }

    /// What Home says at the next launch, or nil for an orderly end. A record
    /// still running whose process ended out of the foreground is iOS
    /// closing a paused guest, which the pause marker reports instead.
    public func notice(endedOutOfForeground: Bool) -> SessionEndNotice? {
        if orderly || (running && endedOutOfForeground) { return nil }
        switch end {
        case "guest-panic":
            return SessionEndNotice(
                title: String(
                    localized: "Android’s kernel panicked", bundle: .module,
                    comment: "Title of Home's card after Android's kernel panicked, the Linux kernel's own fatal error."),
                text: message.map {
                    String(
                        localized: "Android stopped with a kernel panic: \($0).", bundle: .module,
                        comment: "Home's card after a kernel panic. The argument is the kernel's own message, in English, which stays as it is.")
                } ?? String(
                    localized: "Android stopped with a kernel panic.", bundle: .module,
                    comment: "Home's card after a kernel panic that left no message.")
            )
        case "kernel-abort":
            return SessionEndNotice(
                title: String(
                    localized: "Android’s kernel crashed", bundle: .module,
                    comment: "Title of Home's card after Android's kernel stopped on an error of its own."),
                text: String(
                    localized: "Android stopped on a kernel error.", bundle: .module,
                    comment: "Home's card after Android's kernel stopped on an error of its own."))
        case "kernel-exit":
            return SessionEndNotice(
                title: String(
                    localized: "Android stopped", bundle: .module,
                    comment: "Title of Home's card after Android's kernel ended the session by itself, or after a stop with no known cause."),
                text: String(
                    localized: "Android’s kernel ended Andarium.", bundle: .module,
                    comment: "Home's card after Android's kernel ended the session by itself."))
        case "hibernation-restore-failed":
            return SessionEndNotice(
                title: String(
                    localized: "Android could not resume", bundle: .module,
                    comment: "Title of Home's card after a start that could not bring back the memory Android saved when it was paused."),
                text: String(
                    localized: "Android’s memory could not be restored, so it starts again from boot.", bundle: .module,
                    comment: "Home's card after a start that could not bring back the memory Android saved when it was paused."))
        case "host-fault", "process-exit":
            return SessionEndNotice(
                title: String(
                    localized: "Andarium crashed", bundle: .module,
                    comment: "Title of Home's card after Andarium itself crashed while Android was running."),
                text: String(
                    localized: "Andarium stopped on an error of its own while Android was running.", bundle: .module,
                    comment: "Home's card after Andarium itself crashed while Android was running."))
        case "running" where nearMemoryLimit:
            return SessionEndNotice(
                title: String(
                    localized: "iOS ended Andarium", bundle: .module,
                    comment: "Title of Home's card after iOS ended Andarium because the memory it used came near the limit."),
                text: String(
                    localized: "Android was using almost all the memory iOS allows Andarium. A smaller memory size is safer.",
                    bundle: .module,
                    comment: "Home's card after iOS ended Andarium because the memory it used came near the limit."))
        case "running":
            return SessionEndNotice(
                title: String(
                    localized: "Andarium stopped unexpectedly", bundle: .module,
                    comment: "Title of Home's card after Andarium ended while Android was running, with no known cause."),
                text: String(
                    localized: "Andarium ended while Android was running.", bundle: .module,
                    comment: "Home's card after Andarium ended while Android was running, with no known cause."))
        default:
            return SessionEndNotice(
                title: String(
                    localized: "Android stopped", bundle: .module,
                    comment: "Title of Home's card after Android's kernel ended the session by itself, or after a stop with no known cause."),
                text: String(
                    localized: "Android stopped unexpectedly.", bundle: .module,
                    comment: "Home's card after a stop with no known cause."))
        }
    }
}

/// Home's card after a session that ended badly.
public struct SessionEndNotice: Equatable, Sendable {
    public var title: String
    public var text: String

    public init(title: String, text: String) {
        self.title = title
        self.text = text
    }
}

/// Text that leaves the device in a bug report, with the application's own
/// container paths taken out: they identify this install of Andarium.
public enum DiagnosticRedaction {
    /// `text` with every path in `containers` replaced by `<container>`, and
    /// any other path that names an application container by its identifier
    /// replaced the same way.
    public static func redact(_ text: String, containers: [String]) -> String {
        var result = text
        for container in containers.filter({ $0.count > 1 }).sorted(by: { $0.count > $1.count }) {
            result = result.replacingOccurrences(of: container, with: "<container>")
            // /private/var and /var name the same directory.
            if container.hasPrefix("/private/var/") {
                result = result.replacingOccurrences(
                    of: String(container.dropFirst("/private".count)), with: "<container>")
            }
        }
        let identified = #"(/private)?/var/[^\s"']*?[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"#
        result = result.replacingOccurrences(of: identified, with: "<container>", options: .regularExpression)
        // An iPad app on a Mac lives under the person's home directory.
        return result.replacingOccurrences(
            of: #"/Users/[^/\s"']+"#, with: "/Users/<user>", options: .regularExpression)
    }
}

/// The previous session's record at launch: read, then kept as
/// `previous-session.txt` for the bug report, so the next boot can write its
/// own and a second launch says nothing more. The start's record
/// (`StartRecord`) is kept as `previous-start.txt` the same way.
public enum PreviousSession {
    /// The previous session's record and the notice Home shows for it, left
    /// where the runtime wrote it. `endedOutOfForeground` is whether the pause
    /// marker said the previous process ended paused. Nothing is moved: the
    /// record stays readable until `keep` says what was decided from it is
    /// safe, so that a launch that ends in between reads it again.
    public static func read(in diagnostics: URL, endedOutOfForeground: Bool)
        -> (record: SessionRecord, notice: HomeNotice?)?
    {
        let current = diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord)
        guard let record = SessionRecord.read(at: current) else { return nil }
        return (record, record.notice(endedOutOfForeground: endedOutOfForeground).map { .sessionEnded($0) })
    }

    /// Keeps the record `read` returned as `previous-session.txt` for the bug
    /// report, so that the next boot can write its own and a second launch
    /// says nothing more. The start's own record goes with the session's,
    /// whether or not the runtime got as far as writing the session's. What is
    /// kept follows what this launch found: a session record with no start
    /// record before it leaves no kept start, so the one an earlier session
    /// kept is never read as this session's choice (`BugReportAttachments`
    /// pairs the two again by what they say of themselves). A launch that
    /// finds no readable session record still keeps the start record, marked
    /// as belonging to no session.
    public static func keep(in diagnostics: URL) {
        let start = diagnostics.appendingPathComponent(BugReportAttachments.startRecord)
        let current = diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord)
        let record = SessionRecord.read(at: current)
        let started = FileManager.default.fileExists(atPath: start.path)
        if started || record != nil {
            let keptStart = diagnostics.appendingPathComponent(BugReportAttachments.previousStartRecord)
            try? FileManager.default.removeItem(at: keptStart)
            if started { keep(start, as: keptStart, orphaned: record == nil) }
        }
        guard record != nil else { return }
        let kept = diagnostics.appendingPathComponent(BugReportAttachments.previousSessionRecord)
        try? FileManager.default.removeItem(at: kept)
        try? FileManager.default.moveItem(at: current, to: kept)
    }

    /// Moves the start record to its kept place. With no session record of its
    /// own to go with it, the record is kept only if it can be marked first,
    /// so that no retained session whose process and time happen to fit can
    /// take it for its own: one that cannot be read, or written marked, is
    /// dropped.
    private static func keep(_ start: URL, as kept: URL, orphaned: Bool) {
        guard orphaned else {
            try? FileManager.default.moveItem(at: start, to: kept)
            return
        }
        if var record = StartRecord.read(at: start) {
            record.orphaned = true
            if !record.write(to: kept) { try? FileManager.default.removeItem(at: kept) }
        }
        try? FileManager.default.removeItem(at: start)
    }

    /// At launch, before anything is reported or decided from a record: no run
    /// of this process has begun, so an `earlier-run.txt` is the process
    /// before's (`RunRecord`). When that process left no `session.txt`, its
    /// last run was one whose record was already moved aside, a start with
    /// root that failed and whose pause was saved, and it is the previous
    /// session's: it becomes `previous-session.txt`, and nothing is decided
    /// from it. Otherwise the record at `session.txt` is the previous session's
    /// and the earlier run is dropped. A move or removal that fails leaves the
    /// file where it is, and a bug report does not carry an earlier run that
    /// this process did not retire itself (`BugReportAttachments.entries`).
    public static func settleEarlierRun(in diagnostics: URL) {
        let manager = FileManager.default
        let earlier = diagnostics.appendingPathComponent(BugReportAttachments.earlierRunRecord)
        guard manager.fileExists(atPath: earlier.path) else { return }
        let current = diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord)
        if manager.fileExists(atPath: current.path) {
            try? manager.removeItem(at: earlier)
        } else {
            let kept = diagnostics.appendingPathComponent(BugReportAttachments.previousSessionRecord)
            try? manager.removeItem(at: kept)
            try? manager.moveItem(at: earlier, to: kept)
            // The start that went with that run is kept with it, not marked as
            // belonging to no session: `BugReportAttachments` pairs the two by
            // what they say of themselves.
            let start = diagnostics.appendingPathComponent(BugReportAttachments.startRecord)
            let keptStart = diagnostics.appendingPathComponent(BugReportAttachments.previousStartRecord)
            if manager.fileExists(atPath: start.path) {
                try? manager.removeItem(at: keptStart)
                try? manager.moveItem(at: start, to: keptStart)
            }
        }
    }

    /// `read`, then `keep`: for a reader that decides nothing from the record
    /// that must outlive the launch.
    public static func consume(in diagnostics: URL, endedOutOfForeground: Bool)
        -> (record: SessionRecord, notice: HomeNotice?)?
    {
        let previous = read(in: diagnostics, endedOutOfForeground: endedOutOfForeground)
        keep(in: diagnostics)
        return previous
    }
}

/// The runtime's record of the runs of this process, in the diagnostics
/// directory. A run's record is the file that is there once the run began: the
/// record of an earlier run is moved aside as the new run is asked for
/// (`retire`), so that no time or counter has to tell them apart. A decision
/// made from the earlier record is made and saved by then: a start is not
/// begun while one is owed (`AppModel.start`).
///
/// A record that cannot be moved aside never reads as the run's. It is
/// emptied where it is; and one that can be neither moved nor emptied is not
/// read at all until a later `retire` gets it out of the way. The runtime
/// puts its record in the file by opening it for writing, the access the
/// emptying lacked, so a record that stays has not been replaced by one of
/// this run's either.
public final class RunRecord: @unchecked Sendable {
    /// What `retire` found and did.
    public enum Retirement: Equatable, Sendable {
        /// No record was at the run's place.
        case nothing
        /// The record was moved aside for the bug report.
        case moved
        /// The record could not be moved aside (the copy of the run before
        /// could not be replaced) and was emptied where it was. The bug
        /// report does not carry it.
        case emptied
        /// The record could be neither moved nor emptied; it is not read, and
        /// a run that begins with it in place takes no root decision from a record.
        case stays
    }

    private let directory: URL
    private let lock = NSLock()
    private var staysInPlace = false
    private var movedOne = false

    /// Whether a retirement in this process moved a record aside: the file
    /// `earlier-run.txt` is then this process's own. One a launch found
    /// there is an older process's, and a bug report does not carry it.
    public var retiredAnEarlierRun: Bool { lock.withLock { movedOne } }

    public init(in diagnostics: URL) {
        directory = diagnostics
    }

    /// Takes the record at the run's place away: kept as `earlier-run.txt`
    /// for the bug report, so that the next run writes its own there. The
    /// previous process's record, `previous-session.txt`, is not touched: a
    /// bug report carries it for the whole life of the process.
    @discardableResult
    public func retire() -> Retirement {
        let manager = FileManager.default
        let current = directory.appendingPathComponent(BugReportAttachments.sessionRecord)
        let kept = directory.appendingPathComponent(BugReportAttachments.earlierRunRecord)
        let outcome: Retirement
        if !manager.fileExists(atPath: current.path) {
            outcome = .nothing
        } else {
            try? manager.removeItem(at: kept)
            if (try? manager.moveItem(at: current, to: kept)) != nil {
                outcome = .moved
            } else if Self.empty(current) {
                outcome = .emptied
            } else {
                outcome = .stays
            }
        }
        lock.withLock {
            staysInPlace = outcome == .stays
            if outcome == .moved { movedOne = true }
        }
        return outcome
    }

    private static func empty(_ file: URL) -> Bool {
        guard let handle = try? FileHandle(forWritingTo: file) else { return false }
        defer { try? handle.close() }
        return (try? handle.truncate(atOffset: 0)) != nil
    }

    /// The record as it stands; nil while the runtime has written none. The
    /// runtime rewrites the state in one write, and a read that lands in the
    /// middle of one is read again.
    public func read() -> SessionRecord? {
        if lock.withLock({ staysInPlace }) { return nil }
        let file = directory.appendingPathComponent(BugReportAttachments.sessionRecord)
        for _ in 0..<3 {
            if let record = SessionRecord.read(at: file) { return record }
        }
        return nil
    }
}
