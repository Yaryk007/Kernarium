// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreHaptics
import Foundation

/// The connection to the guest's vibrator HAL: vsock 5557, with the same
/// bound on one attempt as motion's.
nonisolated struct HapticsConnector: GuestStreamOpener {
    func openStream() throws -> Int32 {
        let descriptor = kernarium_runtime_loader_connect_vsock(HapticsProtocol.port, 250)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -descriptor) ?? .EIO) }
        return descriptor
    }
}

/// Whether this device has haptics for the guest to play: a Taptic Engine.
/// Every iPhone that Andarium supports has one; an iPad, the Simulator and a
/// Mac do not, and there the guest finds no vibrator and nothing is created.
nonisolated enum DeviceHaptics {
    static var isAvailable: Bool { CHHapticEngine.capabilitiesForHardware().supportsHaptics }
}

/// Where the engine of a target comes from. The device's own is
/// `DeviceHapticEngines`; a target for another actuator (a game controller's
/// handle) is another source's to make.
nonisolated protocol HapticEngineSource: Sendable {
    func engine(for target: HapticTarget) throws -> CHHapticEngine
}

/// The device's own engine, which only target 0 plays on. It plays haptics
/// only and is not bound to the runtime's audio session, which belongs to the
/// guest's audio: an engine bound to that one would follow its category,
/// interrupt other applications' music or be interrupted with the guest's
/// sound. With auto shutdown on, the hardware idles between taps.
nonisolated struct DeviceHapticEngines: HapticEngineSource {
    func engine(for target: HapticTarget) throws -> CHHapticEngine {
        guard target == .device else { throw CHHapticError(.notSupported) }
        let engine = try CHHapticEngine()
        engine.playsHapticsOnly = true
        engine.isAutoShutdownEnabled = true
        return engine
    }
}

/// Plays the guest's scores on Core Haptics engines. One engine serves a
/// target, made when it first plays and kept until the target's actuator is
/// gone (`forget`, a game controller that disconnected); the engine is
/// started for a score and stopped when the application leaves the front, and
/// starts again by itself when the system stopped it, a second after a failure
/// at the soonest. A score makes a player for each element that has a shape: a
/// ramp belongs to its own pattern, and an element that starts later is
/// scheduled on the engine's clock. A score replaces what the target played,
/// and every element ends by itself within the duration the guest reported,
/// a tone with an explicit stop besides, so nothing plays on after a missed
/// stop.
nonisolated final class CoreHapticsPlayer: HapticPlayer, @unchecked Sendable {
    /// What the engine of one target holds. The engine's handlers run on its
    /// own queue.
    private nonisolated final class Channel: @unchecked Sendable {
        let engine: CHHapticEngine
        private let lock = NSLock()
        private var running = false
        private var failedAt: ContinuousClock.Instant?
        private var players: [any CHHapticPatternPlayer] = []
        private var tone: (any CHHapticPatternPlayer)?
        /// When the tone ends by itself, on the engine's clock.
        private var toneEnd: TimeInterval = 0

        init(engine: CHHapticEngine) {
            self.engine = engine
            // Whatever the reason, the engine is not running: the next score
            // starts it, and what played has ended, so a tone is not there to
            // change. A reset also drops the players, which the engine says
            // must be made again.
            engine.stoppedHandler = { [weak self] _ in
                self?.lock.withLock {
                    self?.running = false
                    self?.players.removeAll()
                    self?.tone = nil
                }
            }
            engine.resetHandler = { [weak self] in
                self?.lock.withLock {
                    self?.running = false
                    self?.players.removeAll()
                    self?.tone = nil
                }
            }
        }

        /// Starts the engine unless it runs. A failure is not tried again for a second.
        func ensureRunning() -> Bool {
            if lock.withLock({ running }) { return true }
            let now = ContinuousClock.now
            if let failed = lock.withLock({ failedAt }), now - failed < .seconds(1) { return false }
            do {
                try engine.start()
                lock.withLock {
                    running = true
                    failedAt = nil
                }
                return true
            } catch {
                lock.withLock { failedAt = now }
                HapticsDiagnostics.note("the haptic engine would not start: \(error)")
                return false
            }
        }

        func stopEngine() {
            markStopped()
            engine.stop { _ in }
        }

        /// The engine is taken as stopped until it is started again, which
        /// costs nothing if it runs: the system says so only for the stops it
        /// causes itself.
        func markStopped() {
            lock.withLock { running = false }
        }

        func hold(_ player: any CHHapticPatternPlayer, isTone: Bool, endingAt end: TimeInterval) {
            lock.withLock {
                players.append(player)
                if isTone {
                    tone = player
                    toneEnd = end
                }
            }
        }

        /// The tone that plays now: none once it was cancelled, the engine
        /// stopped, or its own end has passed.
        var playingTone: (any CHHapticPatternPlayer)? {
            let held = lock.withLock { (tone, toneEnd) }
            guard let player = held.0, engine.currentTime < held.1 else { return nil }
            return player
        }

        /// Cancels what the target plays.
        func cancelPlayers() {
            let held = lock.withLock { () -> [any CHHapticPatternPlayer] in
                defer {
                    players.removeAll()
                    tone = nil
                }
                return players
            }
            for player in held { try? player.cancel() }
        }
    }

    private let source: any HapticEngineSource
    private let lock = NSLock()
    private var channels: [HapticTarget: Channel] = [:]

    init(source: any HapticEngineSource = DeviceHapticEngines()) {
        self.source = source
    }

    private func channel(for target: HapticTarget) -> Channel? {
        lock.withLock {
            if let existing = channels[target] { return existing }
            let engine: CHHapticEngine
            do {
                engine = try source.engine(for: target)
            } catch {
                HapticsDiagnostics.note("no haptic engine for target \(target.rawValue): \(error)")
                return nil
            }
            let channel = Channel(engine: engine)
            channels[target] = channel
            return channel
        }
    }

    func play(_ score: HapticScore) -> HapticPlayResult {
        guard let channel = channel(for: score.target) else { return .failed }
        channel.cancelPlayers()
        let shaped = score.events.compactMap { event in HapticShapes.shape(for: event).map { (event, $0) } }
        // A score of delays and effects with no haptic has nothing to feel and
        // nothing wrong.
        guard !shaped.isEmpty else { return .played }
        guard channel.ensureRunning() else { return .failed }
        do {
            for (event, shape) in shaped {
                let player = try channel.engine.makePlayer(with: shape.makePattern())
                let start = event.startMilliseconds == 0
                    ? CHHapticTimeImmediate : channel.engine.currentTime + Double(event.startMilliseconds) / 1000
                try player.start(atTime: start)
                let end = (start == CHHapticTimeImmediate ? channel.engine.currentTime : start) + shape.end
                channel.hold(player, isTone: shape.toneIntensity != nil, endingAt: end)
                if let level = shape.toneIntensity {
                    if level < 1 { try player.sendParameters([Self.intensity(level)], atTime: start) }
                    // The event ends by itself; this ends it again, as the
                    // guest's duration says, if anything held it.
                    try? player.stop(atTime: end)
                }
            }
            return .played
        } catch {
            HapticsDiagnostics.note("a haptic score could not be played: \(error)")
            channel.cancelPlayers()
            channel.markStopped()
            return .failed
        }
    }

    func setToneIntensity(_ intensity: Float, on target: HapticTarget) -> Bool {
        guard let channel = lock.withLock({ channels[target] }), let tone = channel.playingTone else { return false }
        do {
            try tone.sendParameters([Self.intensity(intensity)], atTime: CHHapticTimeImmediate)
            return true
        } catch {
            return false
        }
    }

    func stop(_ target: HapticTarget) {
        lock.withLock { channels[target] }?.cancelPlayers()
    }

    func forget(_ target: HapticTarget) {
        guard let channel = lock.withLock({ channels.removeValue(forKey: target) }) else { return }
        channel.cancelPlayers()
        channel.stopEngine()
    }

    func stopAll() {
        let all = lock.withLock { Array(channels.values) }
        for channel in all {
            channel.cancelPlayers()
            channel.stopEngine()
        }
    }

    func prepare() {
        guard let channel = channel(for: .device) else { return }
        _ = channel.ensureRunning()
    }

    private static func intensity(_ level: Float) -> CHHapticDynamicParameter {
        CHHapticDynamicParameter(parameterID: .hapticIntensityControl, value: level, relativeTime: 0)
    }
}

/// What the haptics say about themselves, in the development builds only.
nonisolated enum HapticsDiagnostics {
    static func note(_ text: @autoclosure () -> String) {
        print("Andarium: \(text())")
    }
}
