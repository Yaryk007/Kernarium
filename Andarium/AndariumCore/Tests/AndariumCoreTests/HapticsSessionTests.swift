// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore

/// The haptics session against a fake guest on the other end of real
/// sockets and a fake player: where frames go, what is answered, what happens
/// when the application leaves the front, and when the attempts to connect
/// stop.
struct HapticsSessionTests {
    // MARK: Fakes

    /// The guest's end of the connection: it accepts while `accepting` and
    /// keeps the peer of every connection it made.
    private final class FakeGuest: GuestStreamOpener, @unchecked Sendable {
        private let lock = NSLock()
        private var attemptCount = 0
        private var peerList: [Int32] = []
        private var acceptingNow = true
        private var refusalsLeft = 0

        var accepting: Bool {
            get { lock.withLock { acceptingNow } }
            set { lock.withLock { acceptingNow = newValue } }
        }
        var attempts: Int { lock.withLock { attemptCount } }
        var connections: Int { lock.withLock { peerList.count } }
        var latest: Int32? { lock.withLock { peerList.last.flatMap { $0 >= 0 ? $0 : nil } } }

        /// Refuses the next `count` attempts and then accepts again. A count,
        /// not a switch that the test throws: how soon the session tries again
        /// decides nothing about which attempts are refused.
        func refuse(next count: Int) { lock.withLock { refusalsLeft = count } }

        deinit { for peer in peerList where peer >= 0 { close(peer) } }

        /// The guest's end of the latest connection goes away: closed here and
        /// forgotten, so that it is closed once and not again by the fake.
        func dropLatest() {
            lock.withLock {
                guard let peer = peerList.last, peer >= 0 else { return }
                peerList[peerList.count - 1] = -1
                close(peer)
            }
        }

        func openStream() throws -> Int32 {
            try lock.withLock {
                attemptCount += 1
                if refusalsLeft > 0 {
                    refusalsLeft -= 1
                    throw POSIXError(.ECONNREFUSED)
                }
                guard acceptingNow else { throw POSIXError(.ECONNREFUSED) }
                var sockets: [Int32] = [-1, -1]
                guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
                peerList.append(sockets[1])
                return sockets[0]
            }
        }

        func send(_ bytes: [UInt8]) {
            guard let peer = latest else { return }
            _ = bytes.withUnsafeBytes { Darwin.send(peer, $0.baseAddress, $0.count, 0) }
        }

        func read(count: Int, timeout: TimeInterval = 3) -> [UInt8] {
            guard let peer = latest else { return [] }
            var result: [UInt8] = []
            let deadline = Date().addingTimeInterval(timeout)
            while result.count < count, Date() < deadline {
                var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
                guard poll(&descriptor, 1, 10) > 0 else { continue }
                var bytes = [UInt8](repeating: 0, count: count - result.count)
                let size = recv(peer, &bytes, bytes.count, MSG_DONTWAIT)
                guard size > 0 else { break }
                result.append(contentsOf: bytes.prefix(size))
            }
            return result
        }

        /// Whether the host closed the latest connection.
        func closedByHost(timeout: TimeInterval = 3) -> Bool {
            guard let peer = latest else { return false }
            var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, Int32(timeout * 1000)) > 0 else { return false }
            var byte: UInt8 = 0
            return recv(peer, &byte, 1, MSG_DONTWAIT) == 0
        }
    }

    private final class TestClock: @unchecked Sendable {
        private let lock = NSLock()
        private var nanoseconds: UInt64 = 1_000_000_000
        func read() -> UInt64 { lock.withLock { nanoseconds } }
        func advance(milliseconds: UInt64 = 0, microseconds: UInt64 = 0) {
            lock.withLock { nanoseconds += milliseconds * 1_000_000 + microseconds * 1000 }
        }
    }

    private final class FakePlayer: HapticPlayer, @unchecked Sendable {
        enum Call: Equatable {
            case play(HapticScore)
            case intensity(Float, HapticTarget)
            case stop(HapticTarget)
            case stopAll
            case prepare
            case forget(HapticTarget)
        }

        private let lock = NSLock()
        private var recorded: [Call] = []
        var result = HapticPlayResult.played
        /// Whether a tone is there for an intensity to change.
        var toneAlive: Bool {
            get { lock.withLock { alive } }
            set { lock.withLock { alive = newValue } }
        }
        private var alive = true
        /// What a play takes, on the test's clock.
        let clock: TestClock
        var playMicroseconds: UInt64 = 300

        init(clock: TestClock) { self.clock = clock }

        var calls: [Call] { lock.withLock { recorded } }
        private func record(_ call: Call) { lock.withLock { recorded.append(call) } }

        func play(_ score: HapticScore) -> HapticPlayResult {
            record(.play(score))
            clock.advance(microseconds: playMicroseconds)
            return result
        }
        func setToneIntensity(_ intensity: Float, on target: HapticTarget) -> Bool {
            record(.intensity(intensity, target))
            return toneAlive
        }
        func stop(_ target: HapticTarget) { record(.stop(target)) }
        func stopAll() { record(.stopAll) }
        func prepare() { record(.prepare) }
        func forget(_ target: HapticTarget) { record(.forget(target)) }
    }

    private struct Rig {
        var session: HapticsSession
        var guest: FakeGuest
        var player: FakePlayer
        var clock: TestClock
    }

    private func rig(retryDelay: DispatchTimeInterval = .milliseconds(10)) -> Rig {
        let guest = FakeGuest()
        let clock = TestClock()
        let player = FakePlayer(clock: clock)
        let session = HapticsSession(opener: guest, player: player, retryDelay: retryDelay) { clock.read() }
        return Rig(session: session, guest: guest, player: player, clock: clock)
    }

    /// Polls for `condition` for at most `timeout` seconds.
    private func eventually(_ timeout: TimeInterval = 3, _ condition: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() { return true }
            usleep(2000)
        }
        return condition()
    }

    /// A rig whose guest is connected and whose application is in front.
    private func connected(booted: Bool = true) throws -> Rig {
        let rig = rig()
        rig.session.update(available: true, running: true, booted: booted, foreground: true)
        #expect(eventually { rig.session.isConnected })
        // The engine is made ready as soon as there is a guest to play for.
        #expect(eventually { rig.player.calls == [.prepare] })
        return rig
    }

    private func frame(_ name: String) throws -> [UInt8] { try HapticsFixture.bytes(name) }

    private func acknowledgement(_ rig: Rig, count: Int = 17) -> [UInt8] { rig.guest.read(count: count) }

    // MARK: Playing

    @Test func aScoreIsPlayedAndAcknowledgedWithTheTimeItTook() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-primitive-click"))
        #expect(acknowledgement(rig) == (HapticsAcknowledgement(sequence: 7, microseconds: 300, result: .played).encoded.map { $0 }))
        #expect(rig.player.calls == [.prepare, .play(HapticScore(sequence: 7, events: [
            .primitive(.click, scale: 1, durationMilliseconds: 12),
        ]))])
    }

    @Test func scoresArePlayedInTheOrderTheyCame() throws {
        let rig = try connected()
        for name in ["play-primitive-click", "play-composition", "play-tone"] {
            rig.guest.send(try frame(name))
            _ = acknowledgement(rig)
        }
        let sequences = rig.player.calls.compactMap { call -> UInt32? in
            if case .play(let score) = call { score.sequence } else { nil }
        }
        #expect(sequences == [7, 4, 1])
    }

    @Test func aStopStopsTheTargetAndEndsTheToneItPlayed() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-tone"))
        _ = acknowledgement(rig)
        rig.guest.send(try frame("stop"))
        #expect(acknowledgement(rig) == Array(HapticsAcknowledgement(sequence: 8, microseconds: 0, result: .played).encoded))
        rig.guest.send(try frame("intensity-half"))
        _ = acknowledgement(rig)
        let calls = rig.player.calls
        #expect(calls.contains(.stop(.device)))
        // A tone that was stopped has no intensity to change.
        #expect(!calls.contains(.intensity(0.5, .device)))
    }

    @Test func anIntensityChangesTheToneOnlyWhileItPlays() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-tone"))
        _ = acknowledgement(rig)
        rig.clock.advance(milliseconds: 100)
        rig.guest.send(try frame("intensity-half"))
        #expect(acknowledgement(rig).count == 17)
        #expect(rig.player.calls.last == .intensity(0.5, .device))

        // The tone lasted 500 ms.
        rig.clock.advance(milliseconds: 500)
        rig.guest.send(try frame("intensity-half"))
        #expect(acknowledgement(rig).count == 17)
        let intensities = rig.player.calls.filter { if case .intensity = $0 { true } else { false } }
        #expect(intensities.count == 1)
    }

    @Test func anIntensityForAPrimitiveChangesNothing() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-primitive-click"))
        _ = acknowledgement(rig)
        rig.guest.send(try frame("intensity-half"))
        _ = acknowledgement(rig)
        #expect(!rig.player.calls.contains(.intensity(0.5, .device)))
    }

    @Test func anEngineThatCannotPlayIsAnsweredAsFailed() throws {
        let rig = try connected()
        rig.player.result = .failed
        rig.guest.send(try frame("play-tone"))
        #expect(acknowledgement(rig) == Array(HapticsAcknowledgement(sequence: 1, microseconds: 300, result: .engineFailed).encoded))
        // What failed is not a tone that can be changed.
        rig.guest.send(try frame("intensity-half"))
        _ = acknowledgement(rig)
        #expect(!rig.player.calls.contains(.intensity(0.5, .device)))
    }

    // MARK: In front

    @Test func nothingPlaysOutOfFront() throws {
        let rig = try connected()
        rig.session.update(available: true, running: true, booted: true, foreground: false)
        rig.session.settle()
        #expect(rig.player.calls == [.prepare, .stopAll])
        rig.guest.send(try frame("play-primitive-click"))
        #expect(acknowledgement(rig) == Array(HapticsAcknowledgement(sequence: 7, microseconds: 0, result: .notInFront).encoded))
        rig.guest.send(try frame("stop"))
        #expect(acknowledgement(rig) == Array(HapticsAcknowledgement(sequence: 8, microseconds: 0, result: .notInFront).encoded))
        #expect(rig.player.calls == [.prepare, .stopAll])
    }

    @Test func returningToTheFrontMakesTheEngineReadyAndPlaysAgain() throws {
        let rig = try connected()
        rig.session.update(available: true, running: true, booted: true, foreground: false)
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        rig.session.settle()
        #expect(rig.player.calls == [.prepare, .stopAll, .prepare])
        rig.guest.send(try frame("play-primitive-click"))
        #expect(acknowledgement(rig).last == 0)
        #expect(rig.player.calls.count == 4)
    }

    @Test func aTonePlayingWhenTheApplicationLeavesIsStoppedAndForgotten() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-tone"))
        _ = acknowledgement(rig)
        rig.session.update(available: true, running: true, booted: true, foreground: false)
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        rig.session.settle()
        // It is back in front inside the tone's 500 ms, and the tone is not
        // taken up again: an intensity has nothing to change.
        rig.guest.send(try frame("intensity-half"))
        _ = acknowledgement(rig)
        #expect(!rig.player.calls.contains(.intensity(0.5, .device)))
        #expect(rig.player.calls.contains(.stopAll))
    }

    // MARK: The run

    @Test func theEndOfTheRunClosesTheStreamAndStopsEverything() throws {
        let rig = try connected()
        rig.session.update(available: true, running: false, booted: true, foreground: true)
        #expect(rig.guest.closedByHost())
        rig.session.settle()
        #expect(rig.player.calls.last == .stopAll)
        #expect(!rig.session.isConnected)
        let attempts = rig.guest.attempts
        usleep(100_000)
        #expect(rig.guest.attempts == attempts)
    }

    @Test func aGuestRestartEndsTheRunAndTheNextOneConnectsAgain() throws {
        let rig = try connected()
        rig.session.restart()
        #expect(rig.guest.closedByHost())
        rig.session.update(available: true, running: true, booted: false, foreground: true)
        #expect(eventually { rig.guest.connections == 2 && rig.session.isConnected })
        rig.guest.send(try frame("play-primitive-click"))
        #expect(acknowledgement(rig).count == 17)
    }

    @Test func aDeviceWithoutHapticsNeverConnects() throws {
        let rig = rig()
        rig.session.update(available: false, running: true, booted: true, foreground: true)
        rig.session.settle()
        usleep(100_000)
        #expect(rig.guest.attempts == 0)
        #expect(rig.player.calls.isEmpty)
        #expect(!rig.session.isConnected)
    }

    @Test func aLostConnectionStopsWhatTheGuestPlayedAndIsMadeAgain() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-tone"))
        _ = acknowledgement(rig)
        rig.guest.dropLatest()
        #expect(eventually { rig.guest.connections == 2 && rig.session.isConnected })
        #expect(rig.player.calls.contains(.stop(.device)))
        // The player is shared with the game controllers: their rumble is not the guest's to stop.
        #expect(!rig.player.calls.contains(.stopAll))
        // The new connection is a working one, and the engine is made ready for it.
        #expect(eventually { rig.player.calls.filter { $0 == .prepare }.count == 2 })
        rig.guest.send(try frame("play-primitive-click"))
        #expect(acknowledgement(rig).count == 17)
    }

    @Test func aFrameThatBreaksTheFormatClosesTheConnection() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-primitive-click") + frame("reject-event-kind"))
        _ = acknowledgement(rig)
        // The frame before the bad one was played; the connection then went.
        #expect(eventually { rig.guest.connections == 2 && rig.session.isConnected })
        let plays = rig.player.calls.filter { if case .play = $0 { true } else { false } }
        #expect(plays.count == 1)
        #expect(rig.player.calls.contains(.stop(.device)))
        #expect(!rig.player.calls.contains(.stopAll))
    }

    @Test func givingUpStopsWhatTheGuestPlayedAndNothingElse() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-tone"))
        _ = acknowledgement(rig)
        rig.guest.accepting = false
        rig.guest.dropLatest()
        #expect(eventually { rig.session.hasGivenUp })
        rig.session.settle()
        #expect(rig.player.calls.contains(.stop(.device)))
        #expect(!rig.player.calls.contains(.stopAll))
    }

    @Test func aToneThatSomethingElseEndedIsForgottenAndItsIntensityIsNotSentAgain() throws {
        let rig = try connected()
        rig.guest.send(try frame("play-tone"))
        _ = acknowledgement(rig)
        rig.clock.advance(milliseconds: 100)
        // The system stopped the engine: the player says there is no tone.
        rig.player.toneAlive = false
        rig.guest.send(try frame("intensity-half"))
        #expect(acknowledgement(rig).count == 17)
        rig.guest.send(try frame("intensity-half"))
        #expect(acknowledgement(rig).count == 17)
        let intensities = rig.player.calls.filter { if case .intensity = $0 { true } else { false } }
        #expect(intensities.count == 1)
    }

    // MARK: The person's switch

    /// A player that keeps what each target plays, as the application's does:
    /// a stop ends the target, `stopAll` every target, and `forget` the target
    /// and the engine it had. A game controller's rumble plays on it too.
    private final class ActuatorPlayer: HapticPlayer, @unchecked Sendable {
        private let lock = NSLock()
        private var activeTargets: Set<HapticTarget> = []
        private var engineTargets: Set<HapticTarget> = []
        private var log: [String] = []

        /// What plays now.
        var active: Set<HapticTarget> { lock.withLock { activeTargets } }
        /// The targets that have an engine.
        var engines: Set<HapticTarget> { lock.withLock { engineTargets } }
        var calls: [String] { lock.withLock { log } }

        func play(_ score: HapticScore) -> HapticPlayResult {
            lock.withLock {
                log.append("play \(score.target.rawValue)")
                engineTargets.insert(score.target)
                activeTargets.insert(score.target)
            }
            return .played
        }
        func setToneIntensity(_ intensity: Float, on target: HapticTarget) -> Bool { lock.withLock { activeTargets.contains(target) } }
        func stop(_ target: HapticTarget) {
            lock.withLock {
                log.append("stop \(target.rawValue)")
                activeTargets.remove(target)
            }
        }
        func stopAll() {
            lock.withLock {
                log.append("stopAll")
                activeTargets.removeAll()
            }
        }
        func prepare() {
            lock.withLock {
                log.append("prepare")
                engineTargets.insert(.device)
            }
        }
        func forget(_ target: HapticTarget) {
            lock.withLock {
                log.append("forget \(target.rawValue)")
                activeTargets.remove(target)
                engineTargets.remove(target)
            }
        }
    }

    private func switchRig() -> (session: HapticsSession, guest: FakeGuest, player: ActuatorPlayer, clock: TestClock) {
        let guest = FakeGuest()
        let clock = TestClock()
        let player = ActuatorPlayer()
        let session = HapticsSession(opener: guest, player: player, retryDelay: .milliseconds(10)) { clock.read() }
        return (session, guest, player, clock)
    }

    @Test func turningHapticsOffEndsTheConnectionAndLetsGoOfTheDeviceAndNothingElse() throws {
        let rig = switchRig()
        let controller = HapticTarget.controller(slot: 0, locality: .standard)
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        #expect(eventually { rig.session.isConnected })
        #expect(eventually { rig.player.engines == [.device] })
        // The guest plays a tone on the device, and a game controller rumbles on the same player.
        rig.guest.send(try frame("play-tone"))
        #expect(rig.guest.read(count: 17).count == 17)
        _ = rig.player.play(HapticScore(target: controller, sequence: 1, events: [.primitive(.click, durationMilliseconds: 12)]))
        #expect(rig.player.active == [.device, controller])

        rig.session.update(available: true, enabled: false, running: true, booted: true, foreground: true)
        rig.session.settle()
        #expect(rig.guest.closedByHost())
        #expect(!rig.session.isConnected)
        // The device plays nothing and keeps no engine; the controller plays on, with its engine.
        #expect(rig.player.active == [controller])
        #expect(rig.player.engines == [controller])
        #expect(!rig.player.calls.contains("stopAll"))
        #expect(rig.player.calls.contains("forget 0"))
        // Nothing was asked of the controller's target.
        #expect(!rig.player.calls.contains { $0 == "stop \(controller.rawValue)" || $0 == "forget \(controller.rawValue)" })
    }

    @Test func whileHapticsIsOffNothingConnectsAndNoEngineIsMade() throws {
        let rig = switchRig()
        rig.session.update(available: true, enabled: false, running: true, booted: false, foreground: true)
        rig.session.update(available: true, enabled: false, running: true, booted: true, foreground: true)
        rig.session.update(available: true, enabled: false, running: true, booted: true, foreground: false)
        rig.session.update(available: true, enabled: false, running: true, booted: true, foreground: true)
        rig.session.settle()
        usleep(100_000)
        #expect(rig.guest.attempts == 0)
        #expect(!rig.session.isConnected && !rig.session.hasGivenUp)
        #expect(rig.player.engines.isEmpty && rig.player.active.isEmpty)
        #expect(!rig.player.calls.contains("prepare"))
    }

    @Test func aFrameThatComesAfterTheSwitchTurnedOffPlaysNothing() throws {
        let rig = switchRig()
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        #expect(eventually { rig.session.isConnected })
        let peer = try #require(rig.guest.latest)
        // The host may have closed its end by the time the frame goes: no signal for that.
        var noSignal: Int32 = 1
        setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, socklen_t(MemoryLayout<Int32>.size))
        // The guest's frame comes after the person turned Haptics off: whether it reaches
        // the host or not, the device plays nothing and holds no engine afterwards.
        let bytes = try frame("play-primitive-click")
        rig.session.update(available: true, enabled: false, running: true, booted: true, foreground: true)
        _ = bytes.withUnsafeBytes { Darwin.send(peer, $0.baseAddress, $0.count, 0) }
        rig.session.settle()
        usleep(100_000)
        #expect(!rig.player.calls.contains("play 0"))
        #expect(rig.player.engines.isEmpty && rig.player.active.isEmpty)
    }

    @Test func turningHapticsOnAgainConnectsAtOnceAndPlays() throws {
        let rig = switchRig()
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        #expect(eventually { rig.session.isConnected })
        rig.session.update(available: true, enabled: false, running: true, booted: true, foreground: true)
        rig.session.settle()
        #expect(rig.guest.closedByHost())
        rig.session.update(available: true, enabled: true, running: true, booted: true, foreground: true)
        #expect(eventually { rig.guest.connections == 2 && rig.session.isConnected })
        // The engine is made ready for it, and the guest's score plays.
        #expect(eventually { rig.player.engines == [.device] })
        rig.guest.send(try frame("play-primitive-click"))
        #expect(rig.guest.read(count: 17).count == 17)
        #expect(rig.player.active == [.device])
    }

    @Test func theSwitchDoesNotCountAsAFailedAttemptAndTheNextBootBeginsAgain() throws {
        let rig = switchRig()
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        #expect(eventually { rig.session.isConnected })
        rig.session.update(available: true, enabled: false, running: true, booted: true, foreground: true)
        rig.session.settle()
        #expect(!rig.session.hasGivenUp)
        // A run that ends while it is off leaves nothing to end, and the next run connects.
        rig.session.update(available: true, enabled: false, running: false, booted: false, foreground: true)
        rig.session.update(available: true, enabled: true, running: true, booted: false, foreground: true)
        #expect(eventually { rig.session.isConnected })
    }

    @Test func aRunThatEndsWhileHapticsIsOnStillLetsGoOfEveryTarget() throws {
        let rig = switchRig()
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        #expect(eventually { rig.session.isConnected })
        rig.session.update(available: true, enabled: true, running: false, booted: true, foreground: true)
        rig.session.settle()
        #expect(rig.player.calls.last == "stopAll")
    }

    // MARK: Controllers are not the guest's

    @Test func aFrameForAControllerIsAnsweredAsFailedAndReachesNoPlayer() throws {
        let rig = try connected()
        for (name, sequence, target) in [("play-tone", UInt32(1), 16), ("intensity-half", 6, 21), ("stop", 8, 47)] {
            var bytes = try frame(name)
            bytes[HapticsProtocol.headerSize] = UInt8(target)
            rig.guest.send(bytes)
            #expect(
                acknowledgement(rig)
                    == Array(HapticsAcknowledgement(sequence: sequence, microseconds: 0, result: .engineFailed).encoded))
        }
        rig.session.settle()
        // Only the engine's preparation was asked of the player.
        #expect(rig.player.calls == [.prepare])
        // The device's own actuator still plays.
        rig.guest.send(try frame("play-tone"))
        #expect(acknowledgement(rig).last == 0)
        #expect(rig.player.calls.count == 2)
    }

    // MARK: Attempts

    @Test func theAttemptsContinueUntilAndroidHasBooted() throws {
        let rig = rig()
        rig.guest.accepting = false
        rig.session.update(available: true, running: true, booted: false, foreground: true)
        #expect(eventually { rig.guest.attempts >= 8 })
        #expect(!rig.session.hasGivenUp)
        // The HAL starts late: the next attempt gets it.
        rig.guest.accepting = true
        #expect(eventually { rig.session.isConnected })
    }

    @Test func fiveFailedAttemptsAfterTheBootEndThemForThatBoot() throws {
        let rig = rig(retryDelay: .milliseconds(100))
        rig.guest.accepting = false
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        #expect(eventually { rig.session.hasGivenUp })
        #expect(rig.guest.attempts == HapticsSession.attemptsAfterBoot)
        usleep(300_000)
        #expect(rig.guest.attempts == HapticsSession.attemptsAfterBoot)
        // Nothing else asks again for this boot...
        rig.session.update(available: true, running: true, booted: true, foreground: false)
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        rig.session.settle()
        usleep(300_000)
        #expect(rig.guest.attempts == HapticsSession.attemptsAfterBoot)
        // ... and the next boot begins again.
        rig.session.restart()
        rig.guest.accepting = true
        rig.session.update(available: true, running: true, booted: false, foreground: true)
        #expect(eventually { rig.session.isConnected })
        #expect(!rig.session.hasGivenUp)
    }

    @Test func theFailuresBeforeTheBootDoNotCount() throws {
        let rig = rig(retryDelay: .milliseconds(20))
        rig.guest.accepting = false
        rig.session.update(available: true, running: true, booted: false, foreground: true)
        #expect(eventually { rig.guest.attempts >= 12 })
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        // The count begins at the boot: five more, not the twelve before.
        let atBoot = rig.guest.attempts
        #expect(eventually { rig.session.hasGivenUp })
        #expect(rig.guest.attempts >= atBoot + 4)
        #expect(rig.guest.attempts <= atBoot + 8)
    }

    @Test func aSuccessfulAttemptStartsTheCountOver() throws {
        let rig = rig()
        rig.session.update(available: true, running: true, booted: true, foreground: true)
        #expect(eventually { rig.session.isConnected })
        // The HAL goes away and comes back after four failures, twice: eight
        // in all, more than the five that end the attempts, so the first
        // success started the count over.
        for connections in 2...3 {
            rig.guest.refuse(next: HapticsSession.attemptsAfterBoot - 1)
            rig.guest.dropLatest()
            #expect(eventually { rig.session.isConnected && rig.guest.connections == connections })
            #expect(!rig.session.hasGivenUp)
        }
    }
}
