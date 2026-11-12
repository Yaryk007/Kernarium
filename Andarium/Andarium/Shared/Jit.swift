// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

#if !APP_STORE

// The JIT's policy and words. They live in the application target, not in
// AndariumCore, so that an App Store build, which has no JIT, carries none of
// it: the package is built without the `APP_STORE` condition.

/// Whether the guest starts on the JIT engine (Home's JIT row), kept for the
/// install across launches. A fresh install is off.
///
/// The JIT needs a debugger the person attached to Andarium (StikDebug), so
/// the value only says what the person wants; `JitStatus` says whether the
/// next start can have it. An App Store build has no JIT and reads no value.
struct JitPreference {
    /// The stored value's key in the defaults database.
    static let key = "andarium.jit"

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// The last value kept; off until one is.
    var isOn: Bool { defaults.bool(forKey: Self.key) }

    func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }

    /// How a launch begins.
    struct Launch: Equatable, Sendable {
        /// Whether the JIT is wanted from the start.
        var isOn: Bool
        /// Whether what the person sets from here on is kept. A launch that
        /// carries a launch setting (automation) is the harness's, so the
        /// value kept from the person's own choice is left as it is.
        var keepsChanges: Bool
    }

    /// The start of a launch. One that states the value (`stated`) starts as
    /// stated; any other launch that carries a launch setting
    /// (`hasOtherSettings`) starts off, so a measured run runs on the engine
    /// its harness chose and never on the one the person's last choice left;
    /// only a launch with no setting restores the kept value.
    func launch(stated: Bool?, hasOtherSettings: Bool) -> Launch {
        if let stated { return Launch(isOn: stated, keepsChanges: false) }
        if hasOtherSettings { return Launch(isOn: false, keepsChanges: false) }
        return Launch(isOn: isOn, keepsChanges: true)
    }
}

/// The guest memory and vCPU count the JIT's executable memory is prepared
/// for. The runtime prepares it once per process, for the guest at the time
/// (when Andarium opens, or at the first start if it did not), so a guest
/// changed after that waits for the next process.
nonisolated struct JitShape: Equatable, Sendable {
    var memoryMiB: Int
    var processorCount: Int

    init(memoryMiB: Int, processorCount: Int) {
        self.memoryMiB = memoryMiB
        self.processorCount = processorCount
    }
}

/// What Andarium asks of a debugger that is attached when it opens. StikDebug
/// spins while it waits and the system ends it within a minute, so the
/// request is made at once and not when the person gets to Start.
enum JitLaunchRequest: Equatable, Sendable {
    /// Prepare the executable memory for this guest.
    case prepare(JitShape)
    /// The JIT is off: tell the debugger to go, so the guest never runs
    /// under it.
    case detach

    /// What the launch asks, or nil when there is no debugger to ask.
    /// `guest` is the guest the next start boots; with none yet, the memory
    /// is prepared for `standard`, the guest Create offers, so that a guest
    /// made as offered finds it ready and any other waits for the next
    /// process.
    static func atLaunch(
        wanted: Bool, debuggerAttached: Bool, guest: JitShape?, standard: JitShape
    ) -> JitLaunchRequest? {
        guard debuggerAttached else { return nil }
        return wanted ? .prepare(guest ?? standard) : .detach
    }
}

/// What the runtime answered when asked to prepare the JIT
/// (`kernarium_runtime_prepare_jit`).
nonisolated enum JitAnswer: Equatable, Sendable {
    /// The executable memory is prepared; the start runs on the JIT.
    case ready
    /// This build cannot make executable memory.
    case unsupported
    /// No debugger is attached to Andarium.
    case noDebugger
    /// The debugger did not prepare the memory, or the memory did not run code.
    case failed
    /// The memory is prepared for another guest memory or vCPU count.
    case otherSettings
    /// The JIT was not wanted; an attached debugger was told to leave.
    case disabled

    /// The answer for a `KERNARIUM_RUNTIME_JIT_` value; one this version does
    /// not know is a failure.
    init(runtimeValue: Int32) {
        switch runtimeValue {
        case 0: self = .ready
        case 1: self = .unsupported
        case 2: self = .noDebugger
        case 4: self = .otherSettings
        case 5: self = .disabled
        default: self = .failed
        }
    }

    /// The sentence the start shows when it ran without the JIT it was
    /// asked for; nil for an answer that is no such fall-back.
    var fallbackSentence: String? {
        switch self {
        case .ready, .disabled: nil
        case .noDebugger: "Started without JIT: StikDebug is not connected."
        case .failed: "Started without JIT: StikDebug could not prepare it."
        case .otherSettings:
            "Started without JIT: the CPU or memory changed after StikDebug prepared it. Open Andarium from StikDebug again."
        case .unsupported: "Started without JIT: this copy of Andarium cannot use it."
        }
    }
}

/// What Home says under the JIT row.
struct JitStatus: Equatable, Sendable {
    /// A debugger is attached to Andarium now.
    var debuggerAttached: Bool
    /// What an earlier start in this process prepared the memory for.
    var prepared: JitShape?
    /// The guest as it is now.
    var guest: JitShape?
    /// How the last start of this process answered, when it ran without the
    /// JIT it was asked for. A new start clears it.
    var lastStart: JitAnswer?
    /// How the attempt made when Andarium opened ended, when it left the
    /// memory unprepared. The process makes one attempt.
    var launch: JitAnswer?

    init(
        debuggerAttached: Bool, prepared: JitShape? = nil, guest: JitShape? = nil, lastStart: JitAnswer? = nil,
        launch: JitAnswer? = nil
    ) {
        self.debuggerAttached = debuggerAttached
        self.prepared = prepared
        self.guest = guest
        self.lastStart = lastStart
        self.launch = launch
    }

    /// The next start can use the JIT when it is on: the memory is prepared
    /// for this guest, or a debugger is here to prepare it. A debugger that
    /// failed its one attempt is no use.
    var usable: Bool {
        if let prepared { return prepared == guest }
        if launch?.fallbackSentence != nil { return false }
        return debuggerAttached
    }

    static let ready = "Ready. Applies to the next start."
    static let needsStikDebug = "To use JIT, open Andarium from StikDebug."
    static let changed =
        "The CPU or memory changed after JIT was prepared. Open Andarium from StikDebug again to use it."
    /// "Preparing JIT" on the boot screen while the debugger works.
    static let preparing = "Preparing JIT"

    var footer: String {
        if let sentence = lastStart?.fallbackSentence { return sentence }
        if prepared != nil, prepared != guest { return Self.changed }
        return usable ? Self.ready : Self.needsStikDebug
    }
}

#endif
