// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How far a boot has got, in the order a boot passes through it.
public enum BootMilestone: Int, Comparable, Sendable {
    /// The runtime has been asked to run the guest.
    case starting = 0
    /// The kernel is running.
    case kernelRunning
    /// Android's init has taken over.
    case androidStarting
    /// Android's graphics service has started, so the display is about to
    /// have content.
    case screenStarting
    /// Android has finished booting, or Recovery is up: the guest has
    /// something to show.
    case ready

    public static func < (lhs: Self, rhs: Self) -> Bool { lhs.rawValue < rhs.rawValue }
}

/// Finds boot milestones in the guest's console output.
///
/// This reads the console because the runtime reports no boot events yet;
/// when it does, the events replace this scanner and nothing else changes.
/// The console arrives in chunks that split lines anywhere, so the scanner
/// keeps the unfinished line.
public struct ConsoleScanner: Sendable {
    /// A line longer than this is dropped: no marker is that long.
    static let maximumLine = 4096

    private var pending = ""
    private var discarding = false
    /// How many boots Android has reported finished: init's
    /// `sys.boot_completed=1` or, a moment before it, apexd's report that the
    /// system booted (the runtime's console signal accepts both, so a restart
    /// asked for between them still ends a boot that completed). A boot that
    /// says both counts once. Recovery never does, so only a count past zero
    /// says Android itself booted.
    public private(set) var bootCompletedCount = 0
    /// The boot under way has counted its completion, and apexd's was the line
    /// that did (init's then confirms it and counts nothing more).
    private var completedByApex = false
    private var completionCounted = false
    /// How many times the recovery image has started its service. A start the
    /// person asked for in the launch knows it is a recovery boot; one the
    /// guest asks for itself (`adb reboot recovery`, the menu of the recovery
    /// image's own) is known only by this line.
    public private(set) var recoveryStartedCount = 0

    public init() {}

    /// The milestones the text completes, in order of appearance.
    public mutating func feed(_ text: String) -> [BootMilestone] {
        var found: [BootMilestone] = []
        // Scalars, not characters: "\r\n" is one character.
        for scalar in text.unicodeScalars {
            if scalar == "\n" || scalar == "\r" {
                if !discarding {
                    if let milestone = Self.milestone(in: pending) { found.append(milestone) }
                    noteCompletion(in: pending)
                    if pending.contains(Self.recoveryLine) { recoveryStartedCount += 1 }
                }
                pending.removeAll(keepingCapacity: true)
                discarding = false
            } else if !discarding {
                pending.unicodeScalars.append(scalar)
                if pending.utf8.count > Self.maximumLine {
                    pending.removeAll(keepingCapacity: true)
                    discarding = true
                }
            }
        }
        return found
    }

    public mutating func feed(_ data: Data) -> [BootMilestone] {
        feed(String(decoding: data, as: UTF8.self))
    }

    /// The guest restarted itself in place: what follows is another boot's.
    /// The line the old boot left unfinished and what it counted of its own
    /// completion are dropped, so a banner the console did not print does not
    /// leave the next boot's completion looking like the old one's. The counts
    /// only grow, as a reader that compares them expects.
    public mutating func beginBoot() {
        pending.removeAll(keepingCapacity: true)
        discarding = false
        completedByApex = false
        completionCounted = false
    }

    /// Counts the completion a line reports. The kernel's banner is a new
    /// boot, which has counted nothing yet.
    private mutating func noteCompletion(in line: String) {
        if line.hasPrefix("Linux version ") || line.contains("] Linux version ") {
            completedByApex = false
            completionCounted = false
        } else if line.contains(Self.bootCompletedLine) {
            if completedByApex {
                completedByApex = false
            } else {
                bootCompletedCount += 1
            }
            completionCounted = true
        } else if line.contains(Self.apexCompletedLine), !completionCounted {
            bootCompletedCount += 1
            completionCounted = true
            completedByApex = true
        }
    }

    static let bootCompletedLine = "processing action (sys.boot_completed=1)"
    static let apexCompletedLine = "apexd: markBootCompleted() received by ApexService"
    static let recoveryLine = "init: starting service 'recovery'"

    static func milestone(in line: String) -> BootMilestone? {
        if line.contains(bootCompletedLine) || line.contains(apexCompletedLine) { return .ready }
        if line.contains(recoveryLine) { return .ready }
        if line.contains("init: starting service 'surfaceflinger'") { return .screenStarting }
        if line.contains("init: init first stage started!") { return .androidStarting }
        if line.hasPrefix("Linux version ") || line.contains("] Linux version ") { return .kernelRunning }
        return nil
    }
}

/// The boot's progress, monotonic: a milestone never moves it back.
public struct BootTracker: Equatable, Sendable {
    public private(set) var milestone: BootMilestone = .starting
    public private(set) var reached: [BootMilestone: Int] = [.starting: 0]

    public init() {}

    /// Records `milestone` reached `seconds` into the boot. Returns whether
    /// it advanced.
    @discardableResult
    public mutating func reach(_ milestone: BootMilestone, atSeconds seconds: Int) -> Bool {
        guard milestone > self.milestone else { return false }
        self.milestone = milestone
        reached[milestone] = seconds
        return true
    }

    /// Whether the boot is the recovery image's, which the guest chose to
    /// boot (a start in the launch's recovery mode is a recovery boot from
    /// the first).
    public private(set) var recovery = false

    /// The console showed the recovery image start.
    public mutating func noteRecovery() { recovery = true }

    /// The boot screen has done its work: there is something of the guest's
    /// to show.
    public var isReady: Bool { milestone == .ready }

    /// A restarted guest boots again from the start.
    public mutating func restart() {
        self = BootTracker()
    }
}

/// Which of the boot screen's steps is under way, as the boot reaches its
/// milestones. Every boot has four: System image, Linux kernel, Storage and
/// the last (Startup, or Recovery), which stays under way from Android's init
/// until Android reports `sys.boot_completed` (or Recovery is up). `count` is
/// past the last. Preparing the guest's disk is the first step: it writes the
/// system the application carries.
///
/// A start with root that has to put the Magisk app in the guest has a fifth,
/// `rootAppStep`, which comes after Startup and is added only when Android's
/// boot is done and the guest says the app is to be installed; the four steps'
/// `count` is the same for every other start.
public enum BootSteps {
    public static let count = 4
    /// The index of the Magisk app step, and the number of steps of a start
    /// that has it.
    public static let rootAppStep = 4
    public static let countWithRootApp = 5

    /// The step's name in English, for the text a report carries (the screen
    /// has its own, in the person's language).
    public static func reportName(ofStep step: Int, recovery: Bool) -> String {
        switch step {
        case 0: "System image"
        case 1: "Linux kernel"
        case 2: "Storage"
        case rootAppStep: "Magisk app"
        default: recovery ? "Recovery" : "Startup"
        }
    }

    public static func current(preparingDisk: Bool, milestone: BootMilestone) -> Int {
        if preparingDisk { return 0 }
        switch milestone {
        case .starting: return 1
        case .kernelRunning: return 2
        case .androidStarting, .screenStarting: return 3
        case .ready: return count
        }
    }
}
