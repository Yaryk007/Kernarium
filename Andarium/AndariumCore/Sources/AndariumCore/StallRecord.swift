// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The stall record of a session: when the guest's vCPUs stopped running on
/// the host, and what was true of the host then, as the runtime wrote it into
/// the session record's stall slot (darwin/runtime/stall_record.hpp).
///
/// A vCPU is stalled when its carrier thread, for 100 ms or more, was
/// runnable and got under a fifth of the time (starved), sat in an
/// uninterruptible wait, or sat in one host wait. The slot holds the newest
/// events that fit in 4 KiB and the last changes of the thermal state, of Low
/// Power Mode with the core count, and of the memory pressure level; every
/// time in it is milliseconds after `base`. A line this reader does not know
/// is skipped and a line with more fields than it knows is read as far as it
/// goes, so a later runtime can add fields at the end of a line.
public struct StallRecord: Equatable, Sendable {
    /// What a stalled vCPU waited for, by the letter the slot spells it with.
    public enum Cause: Character, Equatable, Sendable {
        case starved = "s"
        case fault = "f"
        case other = "o"
        case answer = "a"
        case receive = "r"
        case send = "n"
        case diskRead = "d"
        case diskWrite = "w"
        case flush = "y"
        case memory = "m"

        /// Whether the system, not the guest's host call, held the thread.
        var isSystem: Bool { self == .starved || self == .fault }
    }

    public struct Stalled: Equatable, Sendable {
        public var vcpu: Int
        public var cause: Cause
        /// The share of a core the carrier got meanwhile, in percent.
        public var sharePercent: Int
    }

    /// Cores, cores on performance cores and watts, each in hundredths.
    public struct Usage: Equatable, Sendable {
        public var cores: Int
        public var performanceCores: Int
        public var watts: Int
    }

    public struct Event: Equatable, Sendable {
        public var sequence: Int
        /// Milliseconds after the record's base.
        public var start: Int
        public var duration: Int
        /// The event was still going when the slot was written.
        public var open: Bool
        /// The vCPUs stalled; none for a stall of the whole process, which
        /// the watch's own late tick shows.
        public var stalled: [Stalled]
        /// The worst lateness of the main queue; nil where it was never
        /// serviced (no run loop).
        public var mainQueueLate: Int?
        public var watchLate: Int
        /// 0 nominal, 1 fair, 2 serious, 3 critical.
        public var thermalStart: Int
        public var thermalEnd: Int
        public var lowPowerMode: Bool
        public var processorsStart: Int
        public var processorsEnd: Int
        /// The process's use in the five seconds before the event and during it.
        public var before: Usage
        public var during: Usage
        /// The process's involuntary context switches during the event.
        public var involuntarySwitches: Int
        public var footprintMiB: Int
        /// What the process's memory limit still allowed; nil for none.
        public var leftMiB: Int?
        /// The change of the process's compressed memory during the event.
        public var compressedChangeMiB: Int
        public var pageins: Int
        public var faults: Int
        public var systemPageins: Int
        public var systemDecompressions: Int
        /// 0 normal, 1 warning, 2 critical.
        public var pressure: Int

        /// Fields of an event line that this reader knows: the line's tokens.
        static let fieldCount = 22
    }

    /// A change of the thermal state (levels as `Event.thermalStart`).
    public struct ThermalChange: Equatable, Sendable {
        public var at: Int
        public var from: Int
        public var to: Int
    }

    /// A change of Low Power Mode or of the active core count.
    public struct PowerChange: Equatable, Sendable {
        public var at: Int
        public var lowPowerMode: Bool
        public var processors: Int
    }

    public struct PressureChange: Equatable, Sendable {
        public var at: Int
        /// The level the system moved to.
        public var level: Int
    }

    /// What the runtime's slot holds: the newest 32 events and the newest 8
    /// changes of each kind, and an event names at most 16 vCPUs. A record
    /// that says more is not the runtime's, and only this much of it is read.
    public static let eventCapacity = 32
    public static let transitionCapacity = 8
    public static let vcpuCapacity = 16

    /// The most `report()` takes. A full slot, with every vCPU stalled and the
    /// widest numbers, is about 62 KiB in words; a longer text is cut.
    public static let maximumReportBytes = 80 << 10

    /// The wall clock at the base of every time in the record.
    public var base: Date
    /// When the slot was last written.
    public var written: Date
    /// The events the session ever had, and how many of them the slot holds.
    public var total: Int
    public var shown: Int
    public var events: [Event]
    public var thermal: [ThermalChange]
    public var power: [PowerChange]
    public var pressure: [PressureChange]
    /// How often the watch saw a vCPU thread in the idle wait the runtime
    /// marks, and in a wait nothing marks; nil in a slot that does not count.
    public var idleSamples: Int?
    public var unmarkedSamples: Int?

    /// Reads the stall slot out of a session record's text (or the slot
    /// alone); nil when the text has none, as the records of the earlier
    /// layouts, or one of a version this reader does not know.
    public init?(text: String) {
        var header: [Substring]?
        var events: [Event] = []
        var thermal: [ThermalChange] = []
        var power: [PowerChange] = []
        var pressure: [PressureChange] = []
        // The kernel's quoted output is not the record's own lines.
        let own = text.components(separatedBy: "\nkernel-output\n")[0]
        for line in own.split(separator: "\n", omittingEmptySubsequences: true) {
            let words = line.split(separator: " ", omittingEmptySubsequences: true)
            guard let first = words.first else { continue }
            switch first {
            case "stalls":
                if header == nil { header = words }
            case "stall":
                if let event = Self.event(words) { events.append(event) }
            case "thermal":
                if words.count >= 4, let at = Int(words[1]), let from = Int(words[2]), let to = Int(words[3]) {
                    thermal.append(ThermalChange(at: at, from: from, to: to))
                }
            case "power":
                if words.count >= 4, let at = Int(words[1]), let low = Int(words[2]), let cores = Int(words[3]) {
                    power.append(PowerChange(at: at, lowPowerMode: low != 0, processors: cores))
                }
            case "pressure":
                if words.count >= 3, let at = Int(words[1]), let level = Int(words[2]) {
                    pressure.append(PressureChange(at: at, level: level))
                }
            default:
                break
            }
        }
        // stalls 1 base B at T total N shown M
        guard let header, header.count >= 10, header[1] == "1", header[2] == "base", header[4] == "at",
            header[6] == "total", header[8] == "shown",
            let base = TimeInterval(header[3]), let written = TimeInterval(header[5]),
            abs(base) < 1e11, abs(written) < 1e11,
            let total = Int(header[7]), let shown = Int(header[9])
        else { return nil }
        self.base = Date(timeIntervalSince1970: base)
        self.written = Date(timeIntervalSince1970: written)
        self.total = total
        self.shown = shown
        // Only what the slot can hold: the newest of each.
        self.events = Array(events.suffix(Self.eventCapacity))
        self.thermal = Array(thermal.suffix(Self.transitionCapacity))
        self.power = Array(power.suffix(Self.transitionCapacity))
        self.pressure = Array(pressure.suffix(Self.transitionCapacity))
        // idle I unmarked U, after the fields every reader knows.
        func counter(_ name: Substring) -> Int? {
            guard let at = header.firstIndex(of: name), at + 1 < header.count else { return nil }
            return Int(header[at + 1])
        }
        idleSamples = counter("idle")
        unmarkedSamples = counter("unmarked")
    }

    /// `stall SEQ START DUR[+] CAUSES SHARES UI WATCH THERMAL LPM CPUS BEFORE
    /// DURING SWITCHES FOOTPRINT LEFT COMPRESSED PAGEINS FAULTS SYSPAGEINS
    /// SYSDECOMP PRESSURE`.
    private static func event(_ words: [Substring]) -> Event? {
        guard words.count >= Event.fieldCount, let sequence = Int(words[1]), let start = Int(words[2]) else {
            return nil
        }
        var durationText = words[3]
        let open = durationText.hasSuffix("+")
        if open { durationText = durationText.dropLast() }
        guard let duration = Int(durationText) else { return nil }
        // Each cause letter but '.' has a two-digit share, in order.
        var shares: [Int] = []
        let digits = Array(words[5].prefix(2 * vcpuCapacity))
        if digits != ["-"] {
            var at = 0
            while at + 1 < digits.count {
                shares.append(Int(String(digits[at...at + 1])) ?? 0)
                at += 2
            }
        }
        var stalled: [Stalled] = []
        var next = 0
        for (vcpu, letter) in words[4].prefix(vcpuCapacity).enumerated() {
            guard let cause = Cause(rawValue: letter) else { continue }
            stalled.append(Stalled(vcpu: vcpu, cause: cause, sharePercent: next < shares.count ? shares[next] : 0))
            next += 1
        }
        let thermal = Array(words[8])
        let processors = words[10].split(separator: ">")
        guard thermal.count >= 2, processors.count == 2, let thermalStart = Int(String(thermal[0])),
            let thermalEnd = Int(String(thermal[1])), let low = Int(words[9]),
            let processorsStart = Int(processors[0]), let processorsEnd = Int(processors[1]),
            let before = usage(words[11]), let during = usage(words[12]),
            let footprint = Int(words[14]), let compressed = Int(words[16]),
            let switches = Int(words[13]), let pageins = Int(words[17]), let faults = Int(words[18]),
            let systemPageins = Int(words[19]), let decompressions = Int(words[20]), let pressure = Int(words[21])
        else { return nil }
        return Event(
            sequence: sequence, start: start, duration: duration, open: open, stalled: stalled,
            mainQueueLate: words[6] == "-" ? nil : Int(words[6]), watchLate: Int(words[7]) ?? 0,
            thermalStart: thermalStart, thermalEnd: thermalEnd, lowPowerMode: low != 0,
            processorsStart: processorsStart, processorsEnd: processorsEnd, before: before, during: during,
            involuntarySwitches: switches, footprintMiB: footprint, leftMiB: words[15] == "-" ? nil : Int(words[15]),
            compressedChangeMiB: compressed, pageins: pageins, faults: faults, systemPageins: systemPageins,
            systemDecompressions: decompressions, pressure: pressure)
    }

    private static func usage(_ word: Substring) -> Usage? {
        let parts = word.split(separator: ",")
        guard parts.count == 3, let cores = Int(parts[0]), let performance = Int(parts[1]), let watts = Int(parts[2])
        else { return nil }
        return Usage(cores: cores, performanceCores: performance, watts: watts)
    }
}

// MARK: - Words

extension StallRecord {
    /// The record in words for a person reading a bug report: each event with
    /// the vCPUs it stopped, what they waited for, how long, how much of the
    /// time they ran meanwhile, and the thermal, power and memory state around
    /// it, newest last, then the changes of the host's state. Times are UTC.
    /// The text is never longer than `maximumReportBytes`; what does not fit is
    /// cut at a line, with a line that says so.
    public func report() -> String {
        var lines: [String] = []
        lines.append("The session began \(Self.stamp(base)); this slot was last written \(Self.stamp(written)).")
        lines.append(
            total == 0
                ? "No stall was recorded."
                : "\(total) stall\(total == 1 ? "" : "s") recorded; "
                    + (shown == total ? "all are listed." : "the newest \(shown) are listed."))
        if let idle = idleSamples, let unmarked = unmarkedSamples {
            var seen = "The watch saw vCPU threads in the idle wait the runtime marks \(idle) times and in a wait it "
                + "does not mark \(unmarked) times"
            if idle == 0 && unmarked > 0 {
                seen += "; with no marked idle wait seen, the runtime's wait markers may not be reaching its wrappers"
            }
            lines.append(seen + ".")
        }
        if !events.isEmpty { lines.append("") }
        for event in events {
            lines.append(contentsOf: words(for: event))
            lines.append("")
        }
        var changes: [(Int, String)] = []
        for change in thermal {
            changes.append((change.at, "thermal state \(Self.thermalName(change.from)) to \(Self.thermalName(change.to))"))
        }
        for change in power {
            changes.append(
                (change.at, "Low Power Mode \(change.lowPowerMode ? "on" : "off"), \(change.processors) active cores"))
        }
        for change in pressure {
            changes.append((change.at, "memory pressure \(Self.pressureName(change.level))"))
        }
        if !changes.isEmpty {
            lines.append("Changes of the host's state, oldest first:")
            for (at, text) in changes.sorted(by: { $0.0 < $1.0 }) {
                lines.append("  \(Self.stamp(at: at, in: self)) (\(Self.since(at))): \(text)")
            }
        }
        while lines.last == "" { lines.removeLast() }
        return Self.cut(lines, to: Self.maximumReportBytes)
    }

    /// The lines joined, whole lines only, in at most `limit` bytes.
    static func cut(_ lines: [String], to limit: Int) -> String {
        let note = "(the rest of the record is not shown)\n"
        var text = ""
        var bytes = 0
        for line in lines {
            let size = line.utf8.count + 1
            if bytes + size > limit - note.utf8.count {
                return text + note
            }
            text += line + "\n"
            bytes += size
        }
        return text
    }

    /// One event in words.
    public func words(for event: Event) -> [String] {
        var lines: [String] = []
        var headline = "Stall \(event.sequence), \(Self.stamp(at: event.start, in: self)) "
        headline += "(\(Self.since(event.start)) into the session): \(Self.duration(event.duration))"
        headline += event.open ? ", still going when this was written." : "."
        lines.append(headline)

        // What stopped, and how much of the time each thread ran meanwhile.
        if event.stalled.isEmpty {
            lines.append(
                "  No vCPU thread showed a stall of its own, and the watch's own wake-up was "
                    + "\(event.watchLate) ms late: the whole process was not running, or not "
                    + "scheduled, for that time.")
        } else {
            for group in Dictionary(grouping: event.stalled, by: \.cause).sorted(by: { $0.value[0].vcpu < $1.value[0].vcpu })
            {
                let cpus = Self.list(group.value.map { "vCPU \($0.vcpu)" })
                let shares = Set(group.value.map(\.sharePercent))
                let ran = shares.count == 1 ? "\(shares.first ?? 0)%" : "\(shares.min() ?? 0)% to \(shares.max() ?? 0)%"
                lines.append(
                    "  \(cpus): \(Self.waitWords(group.key)); "
                        + "\(group.value.count == 1 ? "the thread" : "each thread") ran \(ran) of a core meanwhile.")
            }
            if event.watchLate >= 150 {
                lines.append(
                    "  The watch's own wake-up was \(event.watchLate) ms late, so the whole process was held up, "
                        + "not only the vCPU threads.")
            }
        }
        if let late = event.mainQueueLate {
            if late >= 100 { lines.append("  The app's main queue answered \(late) ms late.") }
        } else {
            lines.append("  The main queue's lateness was not measured during this event.")
        }

        // The host around it.
        var host = "  Host: thermal state \(Self.thermalName(event.thermalStart))"
        if event.thermalEnd != event.thermalStart { host += " to \(Self.thermalName(event.thermalEnd))" }
        host += ", Low Power Mode \(event.lowPowerMode ? "on" : "off")"
        host += ", \(event.processorsStart) active cores"
        if event.processorsEnd != event.processorsStart { host += " to \(event.processorsEnd)" }
        host += "."
        lines.append(host)
        lines.append(
            "  The process used \(Self.cores(event.before.cores)) cores (\(Self.cores(event.before.performanceCores)) "
                + "on performance cores, \(Self.watts(event.before.watts)) W) in the 5 s before and "
                + "\(Self.cores(event.during.cores)) cores (\(Self.cores(event.during.performanceCores)) on "
                + "performance cores, \(Self.watts(event.during.watts)) W) during it; the system took the CPU from it "
                + "\(event.involuntarySwitches) times.")
        var memory = "  Memory: the process held \(event.footprintMiB) MiB"
        if let left = event.leftMiB { memory += " with \(left) MiB left of its limit" }
        memory += ", memory pressure \(Self.pressureName(event.pressure))"
        memory += ", compressed memory \(event.compressedChangeMiB >= 0 ? "+" : "")\(event.compressedChangeMiB) MiB"
        memory += "; page-ins \(event.pageins) and faults \(event.faults) in the process, "
        memory += "\(event.systemPageins) page-ins and \(event.systemDecompressions) decompressions in the system."
        lines.append(memory)

        // One sentence of what this adds up to.
        lines.append("  Reading: \(Self.reading(event))")
        return lines
    }

    /// Memory counts as tight at the system's warning or critical pressure
    /// level, when the process's compressed memory grew by 64 MiB or more
    /// during the event, or when it paged in 1000 pages (16 MiB) or more.
    static func memoryIsTight(_ event: Event) -> Bool {
        event.pressure >= 1 || event.compressedChangeMiB >= 64 || event.pageins >= 1000
    }

    private static func reading(_ event: Event) -> String {
        let hot = max(event.thermalStart, event.thermalEnd) >= 2
        let held = event.lowPowerMode || event.processorsEnd < event.processorsStart
        var context = ""
        if hot { context += ", while the device was hot" }
        if held { context += ", with the cores held back" }
        if memoryIsTight(event) { context += ", with memory tight" }
        if event.stalled.isEmpty {
            return "the whole process stopped" + context + "; the system did not run it."
        }
        var parts: [String] = []
        if event.stalled.contains(where: { $0.cause == .starved }) {
            parts.append("the system did not run the vCPU threads" + context)
        }
        if event.stalled.contains(where: { $0.cause == .fault }) {
            parts.append(
                "vCPU threads sat in an uninterruptible wait in the system, usually a page fault or storage" + context)
        }
        let blocked = Set(event.stalled.map(\.cause).filter { !$0.isSystem })
        if !blocked.isEmpty {
            parts.append(
                "vCPU threads waited on the host (" + blocked.map { waitWords($0) }.sorted().joined(separator: "; ") + ")")
        }
        return parts.joined(separator: ", and ") + "."
    }

    private static func waitWords(_ cause: Cause) -> String {
        switch cause {
        case .starved: "runnable but starved of CPU"
        case .fault: "in an uninterruptible wait in the system (usually a page fault or storage)"
        case .other: "blocked in a host call the runtime does not label"
        case .answer: "waiting for a device service's answer"
        case .receive: "waiting to read or receive on a host descriptor"
        case .send: "waiting to write or send on a host descriptor"
        case .diskRead: "waiting for a disk read"
        case .diskWrite: "waiting for a disk write"
        case .flush: "waiting for a flush to disk"
        case .memory: "waiting while guest RAM was committed or released"
        }
    }

    static func thermalName(_ level: Int) -> String {
        switch level {
        case 0: "nominal"
        case 1: "fair"
        case 2: "serious"
        case 3: "critical"
        default: "level \(level)"
        }
    }

    static func pressureName(_ level: Int) -> String {
        switch level {
        case 0: "normal"
        case 1: "warning"
        case 2: "critical"
        default: "level \(level)"
        }
    }

    private static func list(_ items: [String]) -> String {
        switch items.count {
        case 0: ""
        case 1: items[0]
        default: items.dropLast().joined(separator: ", ") + " and " + items[items.count - 1]
        }
    }

    private static func cores(_ hundredths: Int) -> String { String(format: "%.2f", Double(hundredths) / 100) }
    private static func watts(_ hundredths: Int) -> String { String(format: "%.2f", Double(hundredths) / 100) }

    static func duration(_ milliseconds: Int) -> String {
        if milliseconds < 1000 { return "\(milliseconds) ms" }
        if milliseconds < 60_000 { return String(format: "%.1f s", Double(milliseconds) / 1000) }
        let seconds = milliseconds / 1000
        if seconds < 3600 { return "\(seconds / 60) min \(seconds % 60) s" }
        return "\(seconds / 3600) h \(seconds % 3600 / 60) min"
    }

    private static func since(_ milliseconds: Int) -> String { duration(milliseconds) }

    private static func stamp(_ date: Date) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        return formatter.string(from: date)
    }

    private static func stamp(at milliseconds: Int, in record: StallRecord) -> String {
        stamp(record.base.addingTimeInterval(TimeInterval(milliseconds) / 1000))
    }
}
