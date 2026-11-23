// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Plays the guest's vibrator on the device's haptics. The guest's HAL sends
/// scores over a private connection (`HapticsStream`); the session routes each
/// to a `HapticPlayer` while the application is in front, tells the HAL what
/// became of it, and lets everything go the moment it is not.
///
/// **When it connects.** The guest's HAL starts early in boot, so a run (a
/// normal start, from its first moment until it ends or the guest restarts)
/// connects at once and tries again every second. Once Android has booted, five
/// attempts in a row that fail end the attempts for that boot, silently: an
/// older image has no HAL, and a person turned no switch on, so there is nothing
/// to say. A connection that was made and is lost is tried again the same way.
/// A device without haptics never connects.
///
/// **The switch.** The person's Haptics switch (`enabled`) governs what the
/// guest plays on the device's own actuator. While it is off the session holds
/// no stream and the player no engine for the device: turning it off ends the
/// connection, stops what the guest played and drops the device's engine
/// (`HapticPlayer.forget`), and turning it on connects again at once. The HAL
/// sees no host then, as it does before the application connects, drops its
/// frames and completes its callbacks on time, so the guest keeps its vibrator
/// and nothing in Android shifts. The player is shared with the game
/// controllers' rumble, which the switch does not govern: the device is the
/// only target it lets go of.
///
/// **In front.** Only while the application is in front does anything play.
/// A frame that arrives otherwise is dropped, and answered as such. Leaving the
/// front and ending the run stop every target and the engine; losing the
/// connection stops what the guest started and nothing else, because the
/// player is shared with the game controllers' rumble (`GamepadSession`), which
/// the guest's vibrator never reaches: a frame for one of their targets is
/// answered as a failure and plays nothing. Returning to the front with a
/// connection has the engine ready before the first touch.
///
/// Every call returns at once. The work is in order on one queue, and the
/// player and the stream are only touched there.
public final class HapticsSession: @unchecked Sendable {
    /// Attempts that fail in a row, after Android has booted, before the
    /// session stops trying for that boot.
    public static let attemptsAfterBoot = 5

    private let opener: any GuestStreamOpener
    private let player: any HapticPlayer
    private let retryDelay: DispatchTimeInterval
    private let clock: @Sendable () -> UInt64
    private let queue = DispatchQueue(label: "andarium-haptics-session")

    // Touched only on `queue`.
    private var available = false
    private var enabled = true
    private var running = false
    private var booted = false
    private var foreground = false
    private var stream: HapticsStream?
    private var run: UInt64 = 0
    private var connected = false
    private var failedAttempts = 0
    private var gaveUp = false
    /// The score each target plays and when it began, to know whether a tone
    /// is still playing when an intensity arrives.
    private var playing: [HapticTarget: (score: HapticScore, began: UInt64)] = [:]

    public init(
        opener: any GuestStreamOpener, player: any HapticPlayer, retryDelay: DispatchTimeInterval = .seconds(1),
        clock: @escaping @Sendable () -> UInt64 = HapticsStream.uptimeNanoseconds
    ) {
        self.opener = opener
        self.player = player
        self.retryDelay = retryDelay
        self.clock = clock
    }

    deinit { stream?.stop() }

    /// What is wanted now. `available` is whether the device has haptics;
    /// `enabled` the person's Haptics switch; `running` a normal run of the
    /// guest; `booted` that Android has reported boot completed in it;
    /// `foreground` that the application is in front and holds the guest
    /// running.
    public func update(available: Bool, enabled: Bool = true, running: Bool, booted: Bool, foreground: Bool) {
        queue.async { [self] in
            let bootedNow = booted && !self.booted
            self.available = available
            self.enabled = enabled
            self.running = running
            self.booted = booted
            if bootedNow { failedAttempts = 0 }
            let frontChanged = foreground != self.foreground
            self.foreground = foreground
            if available && enabled && running {
                if stream == nil && !gaveUp { beginRun() }
            } else {
                if stream != nil { endRun(switchedOff: available && !enabled && running) }
                gaveUp = false
            }
            guard frontChanged else { return }
            if foreground {
                if connected { player.prepare() }
            } else {
                letGo()
            }
        }
    }

    /// The guest restarted itself: what was connected to is gone, and the
    /// next boot begins the attempts again.
    public func restart() {
        queue.async { [self] in
            if stream != nil { endRun() }
            gaveUp = false
            failedAttempts = 0
        }
    }

    /// Returns when everything asked of the session so far has been done.
    public func settle() {
        queue.sync {}
    }

    /// Whether the guest's HAL is connected.
    public var isConnected: Bool { queue.sync { connected } }

    /// Whether the session stopped trying for this boot.
    public var hasGivenUp: Bool { queue.sync { gaveUp } }

    // MARK: A run

    private func beginRun() {
        run &+= 1
        let current = run
        failedAttempts = 0
        let stream = HapticsStream(opener: opener, retryDelay: retryDelay, clock: clock) { [weak self] event in
            self?.queue.async { [weak self] in
                guard let self, run == current else { return }
                handle(event)
            }
        }
        self.stream = stream
        stream.start()
    }

    /// Ends the connection. The person's switch lets go of the device's
    /// actuator alone (`releaseDevice`); whatever else ends a run lets go of
    /// every target.
    private func endRun(switchedOff: Bool = false) {
        run &+= 1
        stream?.stop()
        stream = nil
        connected = false
        if switchedOff { releaseDevice() } else { letGo() }
    }

    /// Stops what the guest played and drops the device's engine, so nothing
    /// is held while Haptics is off. The controllers' targets are not touched.
    private func releaseDevice() {
        letGoOfTheGuestsTargets()
        player.forget(.device)
    }

    /// Stops every target and the engine, and forgets what played.
    private func letGo() {
        playing.removeAll()
        player.stopAll()
    }

    /// Stops the targets the guest played on, and nothing else: the player is
    /// shared, and a game controller's rumble is not the guest's to end.
    private func letGoOfTheGuestsTargets() {
        for target in playing.keys { player.stop(target) }
        playing.removeAll()
    }

    private func handle(_ event: HapticsStream.Event) {
        switch event {
        case .connected:
            connected = true
            failedAttempts = 0
            if foreground { player.prepare() }
        case .attemptFailed:
            guard booted else { return }
            failedAttempts += 1
            if failedAttempts >= Self.attemptsAfterBoot { giveUp() }
        case .lost:
            connected = false
            letGoOfTheGuestsTargets()
        case .frame(let frame, let receivedAt):
            handle(frame, receivedAt: receivedAt)
        }
    }

    private func giveUp() {
        gaveUp = true
        run &+= 1
        stream?.stop()
        stream = nil
        connected = false
        letGoOfTheGuestsTargets()
    }

    // MARK: Frames

    private func handle(_ frame: HapticsFrame, receivedAt: UInt64) {
        guard foreground else {
            acknowledge(frame.sequence, since: receivedAt, .notInFront, dropped: true)
            return
        }
        // The guest's vibrator has the device's actuator, and no other: the
        // targets from where the controllers begin are the gamepad session's.
        guard frame.target.rawValue < HapticTarget.firstControllerTarget else {
            acknowledge(frame.sequence, since: receivedAt, .engineFailed)
            return
        }
        switch frame {
        case .play(let score):
            switch player.play(score) {
            case .played:
                playing[score.target] = (score, clock())
                acknowledge(score.sequence, since: receivedAt, .played)
            case .failed:
                playing[score.target] = nil
                acknowledge(score.sequence, since: receivedAt, .engineFailed)
            }
        case .intensity(let target, let sequence, let intensity):
            if let current = playing[target], current.score.playsTone(atMilliseconds: millisecondsSince(current.began)),
                !player.setToneIntensity(intensity, on: target)
            {
                // Something else ended the tone.
                playing[target] = nil
            }
            acknowledge(sequence, since: receivedAt, .played)
        case .stop(let target, let sequence):
            player.stop(target)
            playing[target] = nil
            acknowledge(sequence, since: receivedAt, .played)
        }
    }

    private func millisecondsSince(_ began: UInt64) -> UInt64 {
        let now = clock()
        return now > began ? (now - began) / 1_000_000 : 0
    }

    private func acknowledge(
        _ sequence: UInt32, since receivedAt: UInt64, _ result: HapticsAcknowledgement.Result, dropped: Bool = false
    ) {
        let now = clock()
        let microseconds = dropped || now < receivedAt ? 0 : (now - receivedAt) / 1000
        stream?.send(
            HapticsAcknowledgement(
                sequence: sequence, microseconds: UInt32(clamping: microseconds), result: result))
    }
}
