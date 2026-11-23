// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Adds files to the zip Android's bug report arrives as, so that what the
/// host knows travels in the same file.
///
/// The entries go after the archive's last one, stored, and a new central
/// directory follows them: the archive's own entries are not read, moved or
/// rewritten. The work happens on a clone of the file, which replaces the
/// original only once it is complete, so a failure leaves the report as
/// Android made it. Only a plain archive is amended: one disk, no ZIP64
/// records, the central directory right before its end record.
public enum BugReportArchive {
    public struct Entry: Equatable, Sendable {
        public var name: String
        public var data: Data

        public init(name: String, data: Data) {
            self.name = name
            self.data = data
        }
    }

    public enum Failure: Error, Equatable {
        case notAnArchive
        case unsupported(String)
    }

    public static func append(_ entries: [Entry], to url: URL, date: Date = Date()) throws {
        guard !entries.isEmpty else { return }
        let copy = url.deletingLastPathComponent()
            .appendingPathComponent(".\(url.lastPathComponent).amending")
        try? FileManager.default.removeItem(at: copy)
        try FileManager.default.copyItem(at: url, to: copy)
        do {
            try amend(copy, with: entries, date: date)
            _ = try FileManager.default.replaceItemAt(url, withItemAt: copy)
        } catch {
            try? FileManager.default.removeItem(at: copy)
            throw error
        }
    }

    private static let endSignature: UInt32 = 0x0605_4b50
    private static let zip64LocatorSignature: UInt32 = 0x0706_4b50
    private static let centralSignature: UInt32 = 0x0201_4b50
    private static let localSignature: UInt32 = 0x0403_4b50

    private static func amend(_ url: URL, with entries: [Entry], date: Date) throws {
        let handle = try FileHandle(forUpdating: url)
        defer { try? handle.close() }
        let size = try handle.seekToEnd()
        guard size >= 22 else { throw Failure.notAnArchive }
        let tailSize = min(size, 22 + 0xFFFF)
        try handle.seek(toOffset: size - tailSize)
        let tail = try handle.read(upToCount: Int(tailSize)) ?? Data()
        // The end record is the last signature whose comment reaches the end.
        var found: Int?
        var index = tail.count - 22
        while index >= 0 {
            if tail.uint32(at: index) == endSignature,
                index + 22 + Int(tail.uint16(at: index + 20)) == tail.count
            {
                found = index
                break
            }
            index -= 1
        }
        guard let end = found else { throw Failure.notAnArchive }
        let endOffset = size - tailSize + UInt64(end)
        let disk = tail.uint16(at: end + 4), directoryDisk = tail.uint16(at: end + 6)
        let diskEntries = tail.uint16(at: end + 8), count = tail.uint16(at: end + 10)
        let directorySize = tail.uint32(at: end + 12), directoryOffset = tail.uint32(at: end + 16)
        let comment = tail.subdata(in: (end + 22)..<tail.count)
        if disk != 0 || directoryDisk != 0 || diskEntries != count {
            throw Failure.unsupported("a multi-disk archive")
        }
        if count == 0xFFFF || directorySize == 0xFFFF_FFFF || directoryOffset == 0xFFFF_FFFF
            || (end >= 20 && tail.uint32(at: end - 20) == zip64LocatorSignature)
        {
            throw Failure.unsupported("a ZIP64 archive")
        }
        guard UInt64(directoryOffset) + UInt64(directorySize) == endOffset else {
            throw Failure.unsupported("data between the central directory and its end")
        }
        guard Int(count) + entries.count < 0xFFFF else { throw Failure.unsupported("too many entries") }
        try handle.seek(toOffset: UInt64(directoryOffset))
        let directory = try handle.read(upToCount: Int(directorySize)) ?? Data()
        guard directory.count == Int(directorySize), count == 0 || directory.uint32(at: 0) == centralSignature
        else { throw Failure.notAnArchive }

        let (time, day) = dosTimestamp(date)
        var offset = UInt64(directoryOffset)
        var added = Data()
        var central = Data()
        for entry in entries {
            let name = Data(entry.name.utf8)
            let crc = CRC32.checksum(entry.data)
            guard offset + 30 + UInt64(name.count) + UInt64(entry.data.count) < 0xFFFF_FFFF else {
                throw Failure.unsupported("an archive past 4 GiB")
            }
            var local = Data()
            local.append(le: localSignature)
            local.append(le: UInt16(10))  // version needed: stored
            local.append(le: UInt16(0x0800))  // UTF-8 name
            local.append(le: UInt16(0))  // stored
            local.append(le: time)
            local.append(le: day)
            local.append(le: crc)
            local.append(le: UInt32(entry.data.count))
            local.append(le: UInt32(entry.data.count))
            local.append(le: UInt16(name.count))
            local.append(le: UInt16(0))
            local.append(name)
            central.append(le: centralSignature)
            central.append(le: UInt16(0x031E))  // made by Unix, 3.0
            central.append(le: UInt16(10))
            central.append(le: UInt16(0x0800))
            central.append(le: UInt16(0))
            central.append(le: time)
            central.append(le: day)
            central.append(le: crc)
            central.append(le: UInt32(entry.data.count))
            central.append(le: UInt32(entry.data.count))
            central.append(le: UInt16(name.count))
            central.append(le: UInt16(0))  // extra
            central.append(le: UInt16(0))  // comment
            central.append(le: UInt16(0))  // disk
            central.append(le: UInt16(0))  // internal attributes
            central.append(le: UInt32(0o100644) << 16)  // a regular file, rw-r--r--
            central.append(le: UInt32(offset))
            central.append(name)
            added.append(local)
            added.append(entry.data)
            offset += UInt64(local.count + entry.data.count)
        }
        let newDirectorySize = UInt64(directory.count + central.count)
        guard offset + newDirectorySize < 0xFFFF_FFFF else { throw Failure.unsupported("an archive past 4 GiB") }
        var record = Data()
        record.append(le: endSignature)
        record.append(le: UInt16(0))
        record.append(le: UInt16(0))
        record.append(le: UInt16(Int(count) + entries.count))
        record.append(le: UInt16(Int(count) + entries.count))
        record.append(le: UInt32(newDirectorySize))
        record.append(le: UInt32(offset))
        record.append(le: UInt16(comment.count))
        record.append(comment)
        try handle.seek(toOffset: UInt64(directoryOffset))
        try handle.write(contentsOf: added)
        try handle.write(contentsOf: directory)
        try handle.write(contentsOf: central)
        try handle.write(contentsOf: record)
        try handle.truncate(atOffset: try handle.offset())
        try handle.synchronize()
    }

    private static func dosTimestamp(_ date: Date) -> (UInt16, UInt16) {
        let parts = Calendar(identifier: .gregorian).dateComponents(
            [.year, .month, .day, .hour, .minute, .second], from: date)
        let year = max(0, (parts.year ?? 1980) - 1980)
        let hourBits = (parts.hour ?? 0) << 11
        let minuteBits = (parts.minute ?? 0) << 5
        let secondBits = (parts.second ?? 0) / 2
        let time = UInt16(hourBits | minuteBits | secondBits)

        let yearBits = year << 9
        let monthBits = (parts.month ?? 1) << 5
        let day = UInt16(yearBits | monthBits | (parts.day ?? 1))
        return (time, day)
    }
}

/// What Andarium adds to a bug report: the session records, the stalls and the
/// network counts they hold in words and the system's exit and diagnostic
/// reports MetricKit delivered, from `diagnostics`, with the application's
/// container paths taken out; and what the guest's own counters say, when the
/// caller read them.
///
/// Every part is bounded: a session record to `SessionRecord.maximumBytes`, a
/// report of the stalls to a few dozen events per session, a MetricKit report
/// to `maximumMetricKitBytes` and the reports to `MetricKitReports`' limit, a
/// file of the guest's to `GuestProcFiles.maximumBytes`.
public enum BugReportAttachments {
    public static let sessionRecord = "session.txt"
    public static let previousSessionRecord = "previous-session.txt"
    /// The record of the last earlier run of this process, kept apart from
    /// the previous process's (`RunRecord`).
    public static let earlierRunRecord = "earlier-run.txt"
    /// What the application chose for the start of each of the two sessions
    /// (`StartRecord`), kept beside their records.
    public static let startRecord = "start.txt"
    public static let previousStartRecord = "previous-start.txt"
    public static let metricKitDirectory = "metrickit"
    public static let stallReport = "andarium/stalls.txt"
    public static let networkReport = "andarium/network.txt"
    public static let cameraReport = "andarium/camera.txt"
    /// The most of one MetricKit report that is kept, copied or read.
    public static let maximumMetricKitBytes = 1 << 20

    /// `includingEarlierRun` is the caller's word that `earlier-run.txt` is
    /// this process's own: a launch drops or files an older process's
    /// (`PreviousSession.settleEarlierRun`), and one it could not is not carried.
    public static func entries(
        from diagnostics: URL, containers: [String], guest: [BugReportArchive.Entry] = [],
        includingEarlierRun: Bool = false
    ) -> [BugReportArchive.Entry] {
        var entries: [BugReportArchive.Entry] = []
        let files: [(String, String)] = [
            (previousSessionRecord, "andarium/session-previous.txt"),
            (earlierRunRecord, "andarium/session-earlier-run.txt"),
            (sessionRecord, "andarium/session-current.txt"),
        ]
        var records: [SessionRecord?] = []
        for (file, name) in files {
            // An earlier run that this process did not retire is an older process's.
            if file == earlierRunRecord, !includingEarlierRun {
                records.append(nil)
                continue
            }
            let (entry, record) = sessionEntry(diagnostics.appendingPathComponent(file), named: name, containers: containers)
            records.append(record)
            if let entry { entries.append(entry) }
        }
        // The reports in words describe the previous process and the latest run
        // of this one: the earlier run's record stands for it while the run
        // that follows has written none.
        let previous = records[0]
        let latest = records[2] ?? records[1]
        // The choices the application made for each start, next to the records
        // they belong to. A choice whose start left no session record of its
        // own (the application ended first, or the session's was not written)
        // is copied under a name that says so, never as a session's.
        for (file, name, session) in [
            (previousStartRecord, "andarium/start-previous", previous),
            (startRecord, "andarium/start-current", latest),
        ] {
            if let record = StartRecord.read(at: diagnostics.appendingPathComponent(file)) {
                let matched = session.map(record.belongs(to:)) ?? false
                entries.append(.init(name: name + (matched ? ".txt" : "-unmatched.txt"), data: Data(record.text.utf8)))
            }
        }
        if let report = stalls(previous: previous, current: latest) {
            entries.append(.init(name: stallReport, data: Data(DiagnosticRedaction.redact(report, containers: containers).utf8)))
        }
        if let report = network(previous: previous, current: latest) {
            entries.append(.init(name: networkReport, data: Data(DiagnosticRedaction.redact(report, containers: containers).utf8)))
        }
        if let report = cameraStills(previous: previous, current: latest) {
            entries.append(.init(name: cameraReport, data: Data(DiagnosticRedaction.redact(report, containers: containers).utf8)))
        }
        entries.append(contentsOf: guest)
        let metrics = diagnostics.appendingPathComponent(metricKitDirectory)
        let reports = (try? FileManager.default.contentsOfDirectory(atPath: metrics.path)) ?? []
        for report in reports.sorted() where report.hasSuffix(".json") {
            // A longer report is cut, and so no longer JSON: the head is what
            // a person can still read.
            if let text = BoundedFile.text(metrics.appendingPathComponent(report), limit: maximumMetricKitBytes) {
                entries.append(.init(
                    name: "andarium/metrickit/\(report)",
                    data: BoundedFile.capped(
                        DiagnosticRedaction.redact(text, containers: containers), to: maximumMetricKitBytes)))
            }
        }
        return entries
    }

    /// A session record as the bug report copies it, and the record it holds.
    /// A file longer than the runtime ever writes (`SessionRecord.maximumBytes`)
    /// is not copied, exactly as `SessionRecord.read` refuses it, and a line
    /// says so; bytes that are not text become U+FFFD, and the copy is cut to
    /// the same bound after that and after the redaction.
    private static func sessionEntry(
        _ url: URL, named name: String, containers: [String]
    ) -> (BugReportArchive.Entry?, SessionRecord?) {
        guard let data = BoundedFile.read(url, limit: SessionRecord.maximumBytes) else { return (nil, nil) }
        guard data.count <= SessionRecord.maximumBytes else {
            let note = "\(name) was not copied: \(url.lastPathComponent) is longer than \(SessionRecord.maximumBytes) "
                + "bytes, and the runtime writes a record of at most 19200.\n"
            return (.init(name: name, data: Data(note.utf8)), nil)
        }
        let text = String(decoding: data, as: UTF8.self)
        let copy = BoundedFile.capped(
            DiagnosticRedaction.redact(text, containers: containers), to: SessionRecord.maximumBytes)
        return (.init(name: name, data: copy), SessionRecord(text: text))
    }

    /// The stalls of the two sessions the records describe, in words; nil
    /// when neither record has a stall slot (a runtime before layout 4).
    public static func stalls(previous: SessionRecord?, current: SessionRecord?) -> String? {
        guard previous?.stalls != nil || current?.stalls != nil else { return nil }
        var lines = [
            "Stalls of the guest's vCPUs, as the runtime's stall watch recorded them.",
            "",
            "A stall is a vCPU that, for at least 100 ms, was runnable but got under a fifth of a core (starved), or",
            "sat in an uninterruptible wait in the system or in one host call (a device service's answer, a disk,",
            "guest RAM, or a call the runtime does not label) and ran under a fifth of the time meanwhile. The",
            "watch looks every 50 ms. Times are UTC.",
            "andarium/session-previous.txt, andarium/session-earlier-run.txt and andarium/session-current.txt hold the raw records.",
        ]
        for (title, record) in [("The previous session", previous), ("The current session", current)] {
            lines.append("")
            lines.append("== \(title)\(record.map { " (process \($0.processID))" } ?? "") ==")
            if let stalls = record?.stalls {
                lines.append(stalls.report().trimmingCharacters(in: .newlines))
            } else {
                lines.append(record == nil ? "There is no record." : "The record has no stall slot.")
            }
        }
        return lines.joined(separator: "\n") + "\n"
    }

    /// What the guest's user network counted in the two sessions the records
    /// describe, in words; nil when neither record has a network slot (a
    /// runtime before layout 5). The counts are numbers only.
    public static func network(previous: SessionRecord?, current: SessionRecord?) -> String? {
        guard previous?.network != nil || current?.network != nil else { return nil }
        var lines = [
            "What the guest's user network counted during a session, and how often iOS denied Andarium the network",
            "(each denial raises the notice). Numbers only: no address, port, host name or queried name is recorded.",
            "",
            "A DNS query the system refuses (policy denied) is iOS denying the application its network. A TCP",
            "connection is classed by what the guest was told: unreachable (connect() failed at once, but not with",
            "a refusal), reset (refused, timed out on the host after 75 seconds, or failed after it started) or no",
            "answer (under way, given up by the guest, or pushed out of the table of waiting attempts). A refusal by",
            "the system and a path with no route are not told apart. DNS over TCP to the name server or the gateway",
            "is answered by the user network itself and is not counted as a connection.",
            "andarium/session-previous.txt, andarium/session-earlier-run.txt and andarium/session-current.txt hold the raw records.",
        ]
        for (title, record) in [("The previous session", previous), ("The current session", current)] {
            lines.append("")
            lines.append("== \(title)\(record.map { " (process \($0.processID))" } ?? "") ==")
            if let network = record?.network {
                lines.append(network.report())
            } else {
                lines.append(record == nil ? "There is no record." : "The record has no network slot.")
            }
        }
        return lines.joined(separator: "\n") + "\n"
    }

    /// How the platform presented each of the guest's cameras, and the stills of
    /// the guest's cameras that did not end in a picture, and why, in the two
    /// sessions the records describe, in words; nil when neither record has a
    /// camera slot (a runtime before layout 8, or sessions that presented no
    /// camera and had no failed still). Numbers and the runtime's fixed words only.
    public static func cameraStills(previous: SessionRecord?, current: SessionRecord?) -> String? {
        guard previous?.camera != nil || current?.camera != nil else { return nil }
        var lines = [
            "How the platform presented each of the guest's cameras, and the stills of the guest's cameras that did not end",
            "in a picture, with where each one stopped.",
            "Numbers and fixed words only: no name, path or message of the system's is recorded.",
            "",
            "mounting: the platform's fixed angle is the clockwise turn that makes the camera's unturned pictures upright for an",
            "interface orientation. A front camera mounted with its sensor's top at the device's top (portrait 0 or 180)",
            "imitates an earlier front camera: the guest is told 90, the platform turns the video connection, and the photo",
            "output turns its stills (orientation compensation, which stays on).",
            "",
            "refused: the device did not start the capture (a limit, no capture session, or the platform's refusal).",
            "not-started: the platform accepted the capture and could not start it.",
            "framework-error: the system's photo output reported an error (its domain and code follow).",
            "missing-data: the capture ended with no file.",
            "resolved-size: the photo output made a size other than the one asked for, after a second try.",
            "encoded-size: the file states a size other than the one asked for, or none.",
            "file-size: the file is larger than the host takes.",
            "guest-buffer: the file could not be put into the guest's buffer.",
            "timeout: the platform did not end the capture in time.",
            "Orientation compensation is the photo output turning its stills to an earlier model's sensor orientation. The system",
            "answers it only once the capture session's configuration is committed: an answer read before that is not final.",
            "Front devices: the front devices the platform listed, and how many of their formats it did not offer the guest.",
            "andarium/session-previous.txt, andarium/session-earlier-run.txt and andarium/session-current.txt hold the raw records.",
        ]
        for (title, record) in [("The previous session", previous), ("The current session", current)] {
            lines.append("")
            lines.append("== \(title)\(record.map { " (process \($0.processID))" } ?? "") ==")
            if let camera = record?.camera {
                lines.append(camera.report())
            } else {
                lines.append(record == nil ? "There is no record." : "The record names no camera and no failed still.")
            }
        }
        return lines.joined(separator: "\n") + "\n"
    }

    /// Adds the attachments to the report at `file`; a report that cannot take
    /// them is shared as Android made it.
    @discardableResult
    public static func attach(
        to file: URL, from diagnostics: URL, containers: [String], guest: [BugReportArchive.Entry] = [],
        includingEarlierRun: Bool = false
    ) -> Bool {
        let entries = entries(
            from: diagnostics, containers: containers, guest: guest, includingEarlierRun: includingEarlierRun)
        do {
            try BugReportArchive.append(entries, to: file)
            return true
        } catch {
            print("Andarium: the bug report goes without Andarium's records: \(error)")
            return false
        }
    }
}

/// The system's exit and diagnostic reports that MetricKit delivers, kept
/// beside the session records, newest `limit` of them.
public enum MetricKitReports {
    public static func keep(_ json: Data, named name: String, in diagnostics: URL, limit: Int = 30) {
        let directory = diagnostics.appendingPathComponent(BugReportAttachments.metricKitDirectory)
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            // A report longer than the cap keeps its head: no longer JSON, still
            // what a person reads in a bug report.
            try json.prefix(BugReportAttachments.maximumMetricKitBytes)
                .write(to: directory.appendingPathComponent(name), options: .atomic)
            let kept = try FileManager.default.contentsOfDirectory(atPath: directory.path)
                .filter { $0.hasSuffix(".json") }.sorted()
            for old in kept.dropLast(limit) {
                try? FileManager.default.removeItem(at: directory.appendingPathComponent(old))
            }
        } catch {
            print("Andarium: a MetricKit report was not kept: \(error.localizedDescription)")
        }
    }
}

/// The checksum a zip entry carries (IEEE 802.3, reflected).
enum CRC32 {
    private static let table: [UInt32] = (0..<256).map { index in
        var value = UInt32(index)
        for _ in 0..<8 { value = value & 1 != 0 ? 0xEDB8_8320 ^ (value >> 1) : value >> 1 }
        return value
    }

    static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for byte in data { crc = table[Int((crc ^ UInt32(byte)) & 0xFF)] ^ (crc >> 8) }
        return crc ^ 0xFFFF_FFFF
    }
}

extension Data {
    fileprivate func uint16(at index: Int) -> UInt16 {
        UInt16(self[startIndex + index]) | UInt16(self[startIndex + index + 1]) << 8
    }

    fileprivate func uint32(at index: Int) -> UInt32 {
        UInt32(uint16(at: index)) | UInt32(uint16(at: index + 2)) << 16
    }

    fileprivate mutating func append<Value: FixedWidthInteger>(le value: Value) {
        Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) }
    }
}
