// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Tells the guest's thermal HAL how hot the device is, and says when Android
/// starts to lower the frame rate for it.
///
/// Settings' "When hot" (`setLimit`) decides whether Android is told: with the
/// limit off the HAL hears none at every state of iOS's, critical included, at
/// once when the limit is turned off and with the policy's current severity at
/// once when it is turned on, and the cap is never announced while it is off.
/// The policy runs either way, so `isHot` (critical, its hold included) is
/// known with the limit off too. The gate is here, where the severity is
/// sent, so that the policy stays one pure rule.
///
/// The application reads iOS's thermal state and gives each reading to
/// `observe`. A run (a normal start, from its first moment until it ends) has
/// a `ThermalPolicy` of its own, which turns the readings into the severity
/// Android is told, and a `ThermalStream`, which carries it to the HAL: the
/// stream connects at once and tries again every second, because the HAL
/// starts early in boot. Once Android has booted, five attempts in a row that
/// fail end the attempts for that boot, silently: an older image has no HAL,
/// and then nothing lowers the frame rate and nothing is announced. A
/// connection that was made and is lost is tried again the same way. When it
/// is a HAL that did connect in this boot that the attempts gave up on, the
/// give-up is not final: a new choice of the limit, or a change of the
/// severity Android is told, starts one new round of attempts. Nothing else
/// does (no timer keeps trying while nothing changes), and a HAL that never
/// connected in this boot stays given up.
///
/// While the display is the 120 Hz one and iOS is in Low Power Mode, which
/// holds the screen to 60 (`setSixtyHertzHold`), Android is told MODERATE at the
/// least: its display configuration keeps the render rate to 60 at that
/// severity. The hold is Android's own limit on what is rendered, not the
/// person's "When hot" choice, so it applies with the limit off too; it is
/// never announced.
///
/// The cap is announced once in a run (`ThermalPolicy.claimAnnouncement`), at
/// the first moment the severity is at the cap or above and the HAL is
/// connected: that is when Android's frame rate drops. A guest that restarts
/// itself reconnects without a new announcement. The severity falls on a
/// timer when a hotter state stops counting; the clock and the timer are
/// the caller's to give, so a test moves the one by hand and fires the other.
///
/// Every call returns at once. The work is in order on one queue, and the
/// policy and the stream are only touched there.
public final class ThermalSession: @unchecked Sendable {
    /// Attempts that fail in a row, after Android has booted, before the
    /// session stops trying for that boot.
    public static let attemptsAfterBoot = 5

    private let opener: any GuestStreamOpener
    private let retryDelay: DispatchTimeInterval
    private let clock: @Sendable () -> UInt64
    private let schedule: @Sendable (_ nanoseconds: UInt64, _ work: @escaping @Sendable () -> Void) -> Void
    private let hotChanged: @Sendable (Bool) -> Void
    private let capStarted: @Sendable () -> Void
    private let present: @Sendable () -> HostThermalState?
    private let queue = DispatchQueue(label: "andarium-thermal-session")

    // Touched only on `queue`.
    private var booted = false
    private var inRun = false
    private var observed = HostThermalState.nominal
    private var policy = ThermalPolicy()
    private var stream: ThermalStream?
    private var streamEpoch: UInt64 = 0
    private var connected = false
    private var failedAttempts = 0
    private var gaveUp = false
    /// The HAL connected in this boot, so that a give-up can be revived.
    private var connectedThisBoot = false
    private var timerEpoch: UInt64 = 0
    /// Whether Android is told how hot the device is (Settings' "When hot").
    private var limitsFrameRate = true
    /// Whether the render rate is held to 60 (Low Power Mode on a 120 Hz
    /// display).
    private var holdsAtSixty = false
    /// The severity the guest's HAL holds, from the last record sent in this
    /// boot (a HAL keeps what it was told when the connection is lost), so that
    /// a change of what Android is told is one record and nothing that does
    /// not change it is sent. Nil where nothing is known: before the first
    /// record of a run or of a guest's restart.
    private var lastTold: AndroidThermalSeverity?
    /// The last `hotChanged` said the device is hot.
    private var reportedHot = false

    /// `present` reads iOS's thermal state as it is now, where the caller has
    /// no reading to give before a run begins (the monitor starts after it
    /// does): every run begins from it, and never from the state an earlier
    /// run ended with. `capStarted` is called on the session's queue, once per run, when the
    /// frame rate cap starts. `hotChanged` is called on it each time the
    /// device becomes hot (critical, the hold included) or stops being, with
    /// the limit on or off, and with false when a run ends.
    public init(
        opener: any GuestStreamOpener, retryDelay: DispatchTimeInterval = .seconds(1),
        clock: @escaping @Sendable () -> UInt64 = ThermalSession.uptimeNanoseconds,
        schedule: @escaping @Sendable (UInt64, @escaping @Sendable () -> Void) -> Void = ThermalSession.dispatchAfter,
        hotChanged: @escaping @Sendable (Bool) -> Void = { _ in },
        present: @escaping @Sendable () -> HostThermalState? = { nil },
        capStarted: @escaping @Sendable () -> Void
    ) {
        self.opener = opener
        self.retryDelay = retryDelay
        self.clock = clock
        self.schedule = schedule
        self.hotChanged = hotChanged
        self.present = present
        self.capStarted = capStarted
    }

    deinit { stream?.stop() }

    public static let uptimeNanoseconds: @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }

    /// Runs `work` after `nanoseconds` on `DispatchTime`'s clock.
    public static let dispatchAfter: @Sendable (UInt64, @escaping @Sendable () -> Void) -> Void = { nanoseconds, work in
        let delay = DispatchTimeInterval.nanoseconds(Int(min(nanoseconds, UInt64(Int.max))))
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + delay, execute: work)
    }

    /// What is wanted now. `running` is a normal run of the guest (not
    /// Recovery, which has no HAL); `booted` that Android has reported boot
    /// completed in it.
    public func update(running: Bool, booted: Bool) {
        queue.async { [self] in
            let bootedNow = booted && !self.booted
            self.booted = booted
            // Boot completion is not a new boot: what the connection did
            // before it is still true (`connectedThisBoot`).
            if bootedNow { failedAttempts = 0 }
            if running {
                if !inRun { beginRun() }
                if stream == nil && !gaveUp { openStream() }
            } else if inRun {
                endRun()
            }
        }
    }

    /// Whether the render rate is held to 60: the display is the 120 Hz one and
    /// iOS is in Low Power Mode. Android is told MODERATE while it is, or the
    /// policy's severity when that is higher.
    public func setSixtyHertzHold(_ on: Bool) {
        queue.async { [self] in
            guard on != holdsAtSixty else { return }
            holdsAtSixty = on
            guard inRun else { return }
            tellIfChanged()
        }
    }

    /// Whether Android is told how hot the device is: Settings' "When hot".
    /// Off, the HAL hears none at once and at every state after; on, it hears
    /// the policy's severity at once. The first change that starts the cap
    /// while the limit is on is announced, once in a run.
    public func setLimit(_ on: Bool) {
        queue.async { [self] in
            guard on != limitsFrameRate else { return }
            limitsFrameRate = on
            guard inRun else { return }
            if !reviveAfterGiveUp() { send(told) }
            announceIfDue()
        }
    }

    /// iOS's thermal state as it is now: on a change, and on every return to
    /// the foreground, where the notification may have been missed.
    public func observe(_ state: HostThermalState) {
        queue.async { [self] in
            observed = state
            guard inRun else { return }
            apply(policy.observe(state, at: clock()))
        }
    }

    /// The guest restarted itself: what was connected to is gone, and the
    /// next boot begins the attempts again. The run, its policy and the
    /// announcement stay.
    public func restart() {
        queue.async { [self] in
            closeStream()
            lastTold = nil
            gaveUp = false
            failedAttempts = 0
            connectedThisBoot = false
        }
    }

    /// Returns when everything asked of the session so far has been done.
    public func settle() {
        queue.sync {}
    }

    /// The severity Android is told in this run: none outside a run, and none
    /// while the limit is off and the render rate is not held.
    public var severity: AndroidThermalSeverity { queue.sync { inRun ? told : .none } }

    /// Whether the policy holds the device hot (critical, its hold included),
    /// in a run, whatever Android is told.
    public var isHot: Bool { queue.sync { inRun && policy.severity >= ThermalPolicy.capSeverity } }

    /// Whether the guest's HAL is connected.
    public var isConnected: Bool { queue.sync { connected } }

    /// Whether the session stopped trying for this boot.
    public var hasGivenUp: Bool { queue.sync { gaveUp } }

    // MARK: A run

    private func beginRun() {
        inRun = true
        lastTold = nil
        policy = ThermalPolicy()
        failedAttempts = 0
        gaveUp = false
        connectedThisBoot = false
        // The run begins in the state iOS is in now. Where the caller cannot
        // say, a reading from before the run is it (a run clears what the
        // last one ended with, so that is never a stale one).
        if let now = present() { observed = now }
        policy.observe(observed, at: clock())
        scheduleExpiry()
        publishHot()
    }

    private func endRun() {
        inRun = false
        timerEpoch &+= 1
        closeStream()
        lastTold = nil
        policy = ThermalPolicy()
        // The readings stop with the run: what the next one begins in is read
        // again then.
        observed = .nominal
        connectedThisBoot = false
        publishHot()
    }

    /// What Android is told: the policy's severity, or none while the limit is
    /// off, and MODERATE at the least while the render rate is held to 60.
    private var told: AndroidThermalSeverity {
        let heat = limitsFrameRate ? policy.severity : .none
        return holdsAtSixty ? max(heat, .moderate) : heat
    }

    private func send(_ severity: AndroidThermalSeverity) {
        guard let stream else { return }
        lastTold = severity
        stream.send(severity)
    }

    /// One record when what Android is told is not what the stream last sent;
    /// a stream that gave up is revived with it.
    private func tellIfChanged() {
        guard told != lastTold else { return }
        if !reviveAfterGiveUp() { send(told) }
    }

    private func openStream() {
        streamEpoch &+= 1
        let current = streamEpoch
        let stream = ThermalStream(opener: opener, retryDelay: retryDelay) { [weak self] event in
            self?.queue.async { [weak self] in
                guard let self, streamEpoch == current else { return }
                handle(event)
            }
        }
        stream.send(told)
        lastTold = told
        self.stream = stream
        stream.start()
    }

    /// A new round of attempts, for a HAL that connected in this boot and
    /// whose attempts then gave up: the opening record carries what Android is
    /// told now. The caller says what changed, so no timer is behind this.
    /// Returns whether a round began.
    private func reviveAfterGiveUp() -> Bool {
        guard inRun, gaveUp, connectedThisBoot, stream == nil else { return false }
        gaveUp = false
        failedAttempts = 0
        openStream()
        return true
    }

    private func closeStream() {
        streamEpoch &+= 1
        stream?.stop()
        stream = nil
        connected = false
    }

    private func handle(_ event: ThermalStream.Event) {
        switch event {
        case .connected:
            connected = true
            connectedThisBoot = true
            failedAttempts = 0
            announceIfDue()
        case .attemptFailed:
            guard booted else { return }
            failedAttempts += 1
            if failedAttempts >= Self.attemptsAfterBoot {
                gaveUp = true
                closeStream()
            }
        case .lost:
            connected = false
        }
    }

    // MARK: The policy

    private func tickNow() {
        guard inRun else { return }
        apply(policy.advance(to: clock()))
    }

    private func apply(_ changed: Bool) {
        // With the limit off the HAL was told none (or MODERATE, while the
        // render rate is held) and stays as told.
        if changed { tellIfChanged() }
        scheduleExpiry()
        announceIfDue()
        publishHot()
    }

    /// The cap is announced only while Android is told of it: a limit that is
    /// off consumes nothing, so the run still announces the cap if the limit
    /// is turned on while the device is hot.
    private func announceIfDue() {
        guard limitsFrameRate, connected, policy.claimAnnouncement() else { return }
        capStarted()
    }

    private func publishHot() {
        let hot = inRun && policy.severity >= ThermalPolicy.capSeverity
        guard hot != reportedHot else { return }
        reportedHot = hot
        hotChanged(hot)
    }

    /// Looks again when a state that ended stops counting.
    private func scheduleExpiry() {
        timerEpoch &+= 1
        guard let expiry = policy.nextExpiry else { return }
        let now = clock()
        let delay = expiry > now ? expiry - now : 0
        let epoch = timerEpoch
        schedule(delay) { [weak self] in
            self?.queue.async { [weak self] in
                guard let self, timerEpoch == epoch else { return }
                tickNow()
            }
        }
    }
}
