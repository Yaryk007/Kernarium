// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

/// The gamepad session against a fake relay on the other end of real sockets,
/// a fake haptic player and fake indicators: how a controller becomes a
/// device, what is sent and when, what happens when the application leaves the
/// front, what the guest's drivers ask for, and when the attempts to connect
/// stop. No real controller, Bluetooth or guest is involved.
struct GamepadSessionTests {
    // MARK: Rig

    private final class Log: @unchecked Sendable {
        private let lock = NSLock()
        private var list: [String] = []
        var lines: [String] { lock.withLock { list } }
        func add(_ line: String) { lock.withLock { list.append(line) } }
    }

    private struct Rig {
        var session: GamepadSession
        var guest: GamepadFakeGuest
        var player: GamepadFakePlayer
        var indicators: GamepadFakeIndicators
        var clock: GamepadTestClock
        var scheduler: GamepadTestScheduler
        var wakes: GamepadCounter
        var log: Log
    }

    private func rig(retryDelay: DispatchTimeInterval = .milliseconds(10)) -> Rig {
        let guest = GamepadFakeGuest()
        let player = GamepadFakePlayer()
        let indicators = GamepadFakeIndicators()
        let clock = GamepadTestClock()
        let scheduler = GamepadTestScheduler()
        let wakes = GamepadCounter()
        let log = Log()
        let session = GamepadSession(
            opener: guest, player: player, indicators: indicators, retryDelay: retryDelay, clock: { clock.read() },
            schedule: scheduler.schedule, wake: { wakes.bump() }, log: { log.add($0) })
        return Rig(
            session: session, guest: guest, player: player, indicators: indicators, clock: clock, scheduler: scheduler,
            wakes: wakes, log: log)
    }

    /// A rig whose run has begun and whose relay has said hello.
    private func greeted(booted: Bool = true, foreground: Bool = true, slotCount: UInt8 = 8) -> Rig {
        let rig = rig()
        rig.session.update(running: true, booted: booted, foreground: foreground)
        #expect(gamepadEventually { rig.guest.connections == 1 })
        rig.guest.send(.hello(version: 1, slotCount: slotCount))
        #expect(gamepadEventually { rig.session.isConnected })
        return rig
    }

    private func dualSense(_ id: Int, haptics: GamepadHaptics = .handles) -> GamepadController {
        GamepadController(id: id, family: .dualSense, name: "DualSense", haptics: haptics)
    }

    /// The first report a fresh encoder of the controller's family writes for `state`.
    private func firstReport(_ controller: GamepadController, _ state: GamepadState) -> [UInt8] {
        var encoder = GamepadProfiles.profile(family: controller.family, name: controller.name, elements: controller.elements)
            .makeEncoder()
        return encoder.reports(for: state, after: nil)[0]
    }

    private func createFrame(_ controller: GamepadController, slot: Int) -> GamepadHostFrame {
        .create(
            GamepadProfiles.profile(family: controller.family, name: controller.name, elements: controller.elements)
                .create(slot: slot))
    }

    /// Makes `controller` in the guest the way the relay would: its device is
    /// created and started, and the state it is given is read (with the one the
    /// settle delay sends again, when `settle`).
    @discardableResult
    private func ready(
        _ rig: Rig, _ controller: GamepadController, state: GamepadState = .neutral, slot: Int = 0, settle: Bool = true
    ) -> GamepadHostFrame? {
        rig.session.connect(controller, state: state)
        #expect(rig.guest.nextFrame() == createFrame(controller, slot: slot))
        rig.guest.send(.created(slot: slot, result: 0))
        rig.guest.send(.event(slot: slot, event: .start))
        let first = rig.guest.nextFrame()
        #expect(first != nil)
        if settle {
            #expect(gamepadEventually { rig.scheduler.delays.contains(GamepadSession.settleDelay) })
            rig.scheduler.fire()
            #expect(rig.guest.nextFrame() != nil)
        }
        return first
    }

    /// Whether the session has sent nothing. The answer does not wait for a
    /// time to pass: after the relay's hello it is a round trip (`barrier`), and
    /// before it nothing can be in flight, so what the session wrote is
    /// already in the socket once it has done what it was asked.
    private func noFrame(_ rig: Rig) -> Bool {
        guard rig.session.isConnected else {
            rig.session.settle()
            return rig.guest.nextFrame(timeout: 0.02) == nil
        }
        return barrier(rig)
    }

    /// The frames the host sent before it answered a marker, or nil if it never
    /// did. The relay's frames are handled in order, after everything the
    /// session was asked before, and the session answers a SET_REPORT of a slot
    /// nobody holds: when that answer comes, everything sent before it has.
    private func framesUntilBarrier(_ rig: Rig, id: UInt32 = 0xbeef) -> [GamepadHostFrame]? {
        rig.guest.send(.setReport(slot: 7, reportType: GamepadReportType.feature, number: 0, id: id, data: []))
        var frames: [GamepadHostFrame] = []
        while let frame = rig.guest.nextFrame() {
            if frame == .setReportReply(slot: 7, id: id, error: 0) { return frames }
            frames.append(frame)
        }
        return nil
    }

    /// Returns once the session has handled every frame the guest sent before
    /// this call. Answers false if the host sent anything else before it.
    private func barrier(_ rig: Rig, id: UInt32 = 0xbeef) -> Bool {
        framesUntilBarrier(rig, id: id)?.isEmpty == true
    }

    /// An output report of the DualSense's driver.
    private func dualSenseOutput(flag0: UInt8 = 0, flag1: UInt8 = 0, right: UInt8 = 0, left: UInt8 = 0, leds: UInt8 = 0) -> [UInt8] {
        var report = [UInt8](repeating: 0, count: 63)
        report[0] = 2
        report[1] = flag0
        report[2] = flag1
        report[3] = right
        report[4] = left
        report[44] = leds
        (report[45], report[46], report[47]) = (1, 2, 3)
        return report
    }

    private func rumble(strong: UInt8, weak: UInt8, slot: Int = 0) -> GamepadGuestFrame {
        .output(slot: slot, reportType: GamepadReportType.output, data: dualSenseOutput(flag0: 0x03, right: weak, left: strong))
    }

    // MARK: Making devices

    @Test func aControllerThatConnectsAfterTheHelloIsMadeAtOnce() {
        let rig = greeted()
        let pad = dualSense(7)
        rig.session.connect(pad, state: .neutral)
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
        #expect(rig.session.slot(ofController: 7) == 0)
        #expect(rig.session.controller(inSlot: 0) == 7)
    }

    @Test func aControllerThatWasConnectedBeforeTheRunIsMadeAtTheHello() {
        let rig = rig()
        let pad = dualSense(3)
        rig.session.connect(pad, state: .neutral)
        rig.session.update(running: true, booted: false, foreground: true)
        #expect(gamepadEventually { rig.guest.connections == 1 })
        // Nothing is made before the relay says hello.
        #expect(noFrame(rig))
        rig.guest.send(.hello(version: 1, slotCount: 8))
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
    }

    @Test func controllersTakeTheLowestFreeSlotAndAKnownOneChangesNothing() {
        let rig = greeted()
        let a = GamepadController(id: 1, family: .generic, name: "A")
        let b = GamepadController(id: 2, family: .xbox)
        let c = dualSense(3)
        rig.session.connect(a, state: .neutral)
        rig.session.connect(b, state: .neutral)
        rig.session.connect(b, state: .neutral)
        rig.session.connect(c, state: .neutral)
        #expect(rig.guest.nextFrame() == createFrame(a, slot: 0))
        #expect(rig.guest.nextFrame() == createFrame(b, slot: 1))
        #expect(rig.guest.nextFrame() == createFrame(c, slot: 2))
        #expect(noFrame(rig))
        rig.session.disconnect(id: 2)
        #expect(rig.guest.nextFrame() == .destroy(slot: 1))
        let d = dualSense(4)
        rig.session.connect(d, state: .neutral)
        #expect(rig.guest.nextFrame() == createFrame(d, slot: 1))
    }

    @Test func aNinthControllerHasNoSlotAndIsSaidSo() {
        let rig = greeted()
        for id in 1...9 { rig.session.connect(GamepadController(id: id, family: .generic, name: "P\(id)"), state: .neutral) }
        // The marker is a request of slot 7's device, which the session answers once its CREATE has been.
        rig.guest.send(.created(slot: 7, result: 0))
        let frames = framesUntilBarrier(rig) ?? []
        #expect(frames.count == 8)
        rig.session.settle()
        #expect(rig.session.slot(ofController: 9) == nil)
        #expect(rig.log.lines.contains { $0.contains("no free slot") })
    }

    @Test func aRelayWithFewerSlotsMakesOnlyThoseButHoldsTheRest() {
        let rig = greeted(slotCount: 2)
        let pads = (1...3).map { GamepadController(id: $0, family: .generic, name: "P\($0)") }
        for pad in pads { rig.session.connect(pad, state: .neutral) }
        #expect(framesUntilBarrier(rig) == [createFrame(pads[0], slot: 0), createFrame(pads[1], slot: 1)])
        #expect(rig.session.slot(ofController: 3) == 2)
    }

    @Test func theDeviceGetsItsStateOnceTheGuestHasCreatedAndStartedIt() {
        let rig = greeted()
        let pad = dualSense(1)
        let state = GamepadState(buttons: .south, leftStick: GamepadStick(x: 1, y: 0))
        rig.session.connect(pad, state: state)
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
        // Created is not enough: a driver that has not started the device would refuse a report.
        rig.guest.send(.created(slot: 0, result: 0))
        #expect(noFrame(rig))
        rig.guest.send(.event(slot: 0, event: .start))
        #expect(rig.guest.nextFrame() == .input(slot: 0, report: firstReport(pad, state)))
    }

    @Test func aStartThatComesBeforeTheCreatedAnswerIsOfADeviceThatIsGone() {
        let rig = greeted()
        let pad = dualSense(1)
        rig.session.connect(pad, state: .neutral)
        _ = rig.guest.nextFrame()
        // The relay answers a CREATE before anything of its device.
        rig.guest.send(.event(slot: 0, event: .start))
        rig.guest.send(.created(slot: 0, result: 0))
        #expect(barrier(rig))
        rig.session.submit(GamepadState(buttons: .south), for: 1)
        #expect(noFrame(rig))
        rig.guest.send(.event(slot: 0, event: .start))
        #expect(rig.guest.nextFrame() == .input(slot: 0, report: firstReport(pad, GamepadState(buttons: .south))))
    }

    // MARK: A slot that is used again

    /// Controller 1 holds slot 0 with its CREATE unanswered and leaves, and
    /// controller 2 takes the slot: the relay's answers to the first CREATE
    /// come before the second's.
    private func reusedSlot() -> (rig: Rig, second: GamepadController) {
        let rig = greeted()
        let first = dualSense(1)
        let second = dualSense(2)
        rig.session.connect(first, state: .neutral)
        #expect(rig.guest.nextFrame() == createFrame(first, slot: 0))
        rig.session.disconnect(id: 1)
        #expect(rig.guest.nextFrame() == .destroy(slot: 0))
        rig.session.connect(second, state: .neutral)
        #expect(rig.guest.nextFrame() == createFrame(second, slot: 0))
        return (rig, second)
    }

    @Test func aSlotUsedAgainTakesNothingOfTheFirstControllersAnswers() {
        let (rig, second) = reusedSlot()
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.event(slot: 0, event: .start))
        rig.guest.send(rumble(strong: 255, weak: 255))
        #expect(barrier(rig))
        // The second controller is not ready, and does not rumble at the first's level.
        rig.session.submit(GamepadState(buttons: .south), for: 2)
        #expect(noFrame(rig))
        #expect(!rig.player.calls.contains { if case .play = $0 { true } else { false } })
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.event(slot: 0, event: .start))
        #expect(rig.guest.nextFrame() == .input(slot: 0, report: firstReport(second, GamepadState(buttons: .south))))
    }

    @Test func aSlotUsedAgainIsNotAskedForTheFirstControllersReports() {
        let (rig, _) = reusedSlot()
        // The first device's driver was still asking when it went.
        rig.guest.send(.getReport(slot: 0, reportType: GamepadReportType.feature, number: 0x09, id: 1))
        rig.guest.send(.setReport(slot: 0, reportType: GamepadReportType.feature, number: 8, id: 2, data: [8, 1]))
        #expect(barrier(rig))
        // Still the first device's: the second's CREATE is unanswered.
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.getReport(slot: 0, reportType: GamepadReportType.feature, number: 0x09, id: 3))
        rig.guest.send(.setReport(slot: 0, reportType: GamepadReportType.feature, number: 8, id: 4, data: [8, 1]))
        #expect(barrier(rig))
        // The second device's own requests are answered, in order.
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.getReport(slot: 0, reportType: GamepadReportType.feature, number: 0x09, id: 5))
        rig.guest.send(.setReport(slot: 0, reportType: GamepadReportType.feature, number: 8, id: 6, data: [8, 1]))
        let profile = DualSenseProfile(edge: false)
        #expect(
            rig.guest.nextFrame()
                == .getReportReply(slot: 0, id: 5, error: 0, data: profile.featureReport(number: 0x09, slot: 0)!))
        #expect(rig.guest.nextFrame() == .setReportReply(slot: 0, id: 6, error: 0))
    }

    @Test func aFailedCreateOfTheFirstControllerDoesNotFailTheSecond() {
        let (rig, second) = reusedSlot()
        rig.guest.send(.created(slot: 0, result: -22))
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.event(slot: 0, event: .start))
        #expect(rig.guest.nextFrame() == .input(slot: 0, report: firstReport(second, .neutral)))
        #expect(!rig.log.lines.contains { $0.contains("could not make") })
    }

    @Test func aFailedCreateOfTheSecondControllerStillFailsIt() {
        let (rig, _) = reusedSlot()
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.created(slot: 0, result: -22))
        rig.guest.send(.event(slot: 0, event: .start))
        rig.session.submit(GamepadState(buttons: .south), for: 2)
        #expect(noFrame(rig))
        #expect(rig.log.lines.contains { $0.contains("could not make") })
    }

    @Test func aStateGivenBeforeTheDeviceIsReadyIsHeldAndTheNewestIsSent() {
        let rig = greeted()
        let pad = dualSense(1)
        rig.session.connect(pad, state: .neutral)
        _ = rig.guest.nextFrame()
        rig.session.submit(GamepadState(buttons: .east), for: 1)
        rig.session.submit(GamepadState(buttons: .north), for: 1)
        #expect(rig.wakes.count == 0)
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.event(slot: 0, event: .start))
        #expect(rig.guest.nextFrame() == .input(slot: 0, report: firstReport(pad, GamepadState(buttons: .north))))
    }

    @Test func theStateIsSentOnceMoreAfterTheDeviceHasHadTimeToSettle() {
        let rig = greeted()
        let pad = dualSense(1)
        let held = GamepadState(buttons: .south)
        let first = ready(rig, pad, state: held, settle: false)
        guard case .input(_, let report)? = first else { Issue.record("no first report"); return }
        #expect(gamepadEventually { rig.scheduler.delays == [GamepadSession.settleDelay] })
        rig.scheduler.fire()
        // The same state, with the encoder's sequence counting on.
        guard case .input(_, let again)? = rig.guest.nextFrame() else { Issue.record("no second report"); return }
        #expect(again[8...10] == report[8...10])
        #expect(again[7] == report[7] &+ 1)
    }

    @Test func aDeviceThatWasRemadeIsNotGivenTheEarlierSettleResend() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad, settle: false)
        rig.session.disconnect(id: 1)
        #expect(rig.guest.nextFrame() == .destroy(slot: 0))
        rig.session.connect(pad, state: .neutral)
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
        rig.scheduler.fire()
        #expect(noFrame(rig))
    }

    @Test func aDeviceTheGuestCouldNotMakeGetsNoReports() {
        let rig = greeted()
        let pad = dualSense(1)
        rig.session.connect(pad, state: .neutral)
        _ = rig.guest.nextFrame()
        rig.guest.send(.created(slot: 0, result: -22))
        rig.guest.send(.event(slot: 0, event: .start))
        rig.session.submit(GamepadState(buttons: .south), for: 1)
        #expect(noFrame(rig))
        #expect(rig.log.lines.contains { $0.contains("could not make") })
    }

    @Test func aDeviceTheDriverLetGoOfGetsNoMoreReports() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.guest.send(.event(slot: 0, event: .stop))
        #expect(barrier(rig))
        rig.session.submit(GamepadState(buttons: .south), for: 1)
        #expect(noFrame(rig))
    }

    // MARK: Sending states

    @Test func aButtonGoesAtOnceAndWakesTheGuestOnlyWhenItGoesDown() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.session.submit(GamepadState(buttons: .south), for: 1)
        guard case .input(0, let report)? = rig.guest.nextFrame() else { Issue.record("no report"); return }
        #expect(report[8] & 0x20 != 0)
        #expect(rig.wakes.count == 1)
        // The same state again says nothing; a release is a report but no wake.
        rig.session.submit(GamepadState(buttons: .south), for: 1)
        #expect(noFrame(rig))
        rig.session.submit(.neutral, for: 1)
        #expect(rig.guest.nextFrame() != nil)
        #expect(rig.wakes.count == 1)
        // Another button while one is held is a button going down.
        rig.session.submit(GamepadState(buttons: .east), for: 1)
        _ = rig.guest.nextFrame()
        rig.session.submit(GamepadState(buttons: [.east, .north]), for: 1)
        _ = rig.guest.nextFrame()
        #expect(rig.wakes.count == 3)
    }

    @Test func everyButtonChangeIsItsOwnReportAndNoTapIsJoinedWithAnother() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        // A tap and a release, one after the other with no time between: two reports each.
        for state in [GamepadState(buttons: .south), .neutral, GamepadState(buttons: .south), .neutral] {
            rig.session.submit(state, for: 1)
        }
        let frames = framesUntilBarrier(rig) ?? []
        #expect(frames.count == 4)
        let pressed = frames.compactMap { frame -> Bool? in
            if case .input(_, let report) = frame { report[8] & 0x20 != 0 } else { nil }
        }
        #expect(pressed == [true, false, true, false])
    }

    @Test func analogChangesAreCoalescedToTheIntervalAndTheNewestIsSent() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        // The clock has not moved since the last report: no report, one timer, however many states.
        for x: Float in [0.1, 0.2, 0.3] { rig.session.submit(GamepadState(leftStick: GamepadStick(x: x)), for: 1) }
        #expect(noFrame(rig))
        rig.session.settle()
        #expect(rig.scheduler.delays == [GamepadSession.analogInterval])
        rig.scheduler.fire()
        guard case .input(_, let report)? = rig.guest.nextFrame() else { Issue.record("no report"); return }
        #expect(report[1] == PlayStationReports.axis(0.3))
        #expect(noFrame(rig))
        // After the interval a change goes at once.
        rig.clock.advance(milliseconds: 10)
        rig.session.submit(GamepadState(leftStick: GamepadStick(x: 0.5)), for: 1)
        guard case .input(_, let next)? = rig.guest.nextFrame() else { Issue.record("no report"); return }
        #expect(next[1] == PlayStationReports.axis(0.5))
    }

    @Test func aButtonDuringACoalescedAnalogWaitSendsTheNewestAnalogValuesToo() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.session.submit(GamepadState(leftStick: GamepadStick(x: 0.4)), for: 1)
        rig.session.submit(GamepadState(buttons: .south, leftStick: GamepadStick(x: 0.6)), for: 1)
        guard case .input(_, let report)? = rig.guest.nextFrame() else { Issue.record("no report"); return }
        #expect(report[8] & 0x20 != 0)
        #expect(report[1] == PlayStationReports.axis(0.6))
        // The timer that was waiting finds nothing left to send.
        rig.scheduler.fire()
        #expect(noFrame(rig))
    }

    @Test func aBatteryChangeIsAnAnalogChange() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.clock.advance(milliseconds: 20)
        rig.session.updateBattery(GamepadBattery(level: 0.5, state: .discharging), for: 1)
        guard case .input(_, let report)? = rig.guest.nextFrame() else { Issue.record("no report"); return }
        #expect(report[53] == 5)
        rig.session.updateBattery(GamepadBattery(level: 0.5, state: .discharging), for: 1)
        #expect(noFrame(rig))
    }

    @Test func aStateOfTheInputsKeepsTheBatteryTheApplicationReported() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.clock.advance(milliseconds: 20)
        rig.session.updateBattery(GamepadBattery(level: 0.3, state: .discharging), for: 1)
        _ = rig.guest.nextFrame()
        rig.clock.advance(milliseconds: 20)
        // A state built from the inputs alone carries the default battery.
        rig.session.submit(GamepadState(buttons: .south), for: 1)
        guard case .input(_, let report)? = rig.guest.nextFrame() else { Issue.record("no report"); return }
        #expect(report[53] == 3)
    }

    @Test func aStateForAControllerThatIsNotKnownIsIgnored() {
        let rig = greeted()
        rig.session.submit(GamepadState(buttons: .south), for: 99)
        rig.session.updateBattery(.none, for: 99)
        #expect(noFrame(rig))
        #expect(rig.wakes.count == 0)
    }

    // MARK: In front

    @Test func leavingTheFrontSendsEachDeviceANeutralReportAndStopsItsRumble() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.session.submit(GamepadState(buttons: .south, leftTrigger: 1), for: 1)
        _ = rig.guest.nextFrame()
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(gamepadEventually { !rig.player.calls.isEmpty })
        rig.session.update(running: true, booted: true, foreground: false)
        guard case .input(0, let report)? = rig.guest.nextFrame() else { Issue.record("no report"); return }
        #expect(Array(report[8...10]) == [8, 0, 0])
        #expect(report[5] == 0)
        // The neutral report is written before the rumble is stopped, in the same step of the session.
        rig.session.settle()
        #expect(rig.player.calls.contains(.stop(.controller(slot: 0, locality: .leftHandle))))
        // The guest's vibrator is not this session's to stop.
        #expect(!rig.player.calls.contains(.stopAll))
    }

    @Test func aButtonHeldOnADeviceStillBeingMadeIsLetGoOfWhenTheApplicationLeaves() {
        let rig = greeted()
        let pad = dualSense(1)
        rig.session.connect(pad, state: GamepadState(buttons: .south))
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
        rig.session.update(running: true, booted: true, foreground: false)
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.event(slot: 0, event: .start))
        // The device is ready while the application is away: it is told nothing is pressed.
        guard case .input(0, let report)? = rig.guest.nextFrame() else { Issue.record("no report"); return }
        #expect(report[8] & 0x20 == 0)
        #expect(rig.wakes.count == 0)
    }

    @Test func aControllerThatConnectsWhileTheApplicationIsAwayHoldsNothing() {
        let rig = greeted(foreground: false)
        let pad = dualSense(1)
        rig.session.connect(pad, state: GamepadState(buttons: .south))
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.event(slot: 0, event: .start))
        #expect(rig.guest.nextFrame() == .input(slot: 0, report: firstReport(pad, .neutral)))
    }

    /// A plain gamepad whose reports carry no counter, so that they can be
    /// compared byte for byte.
    private func plainPad(_ id: Int) -> GamepadController {
        GamepadController(id: id, family: .generic, name: "Pad", elements: .extended)
    }

    /// Something of every kind the guest is told: buttons, a trigger and a stick.
    private let heldState = GamepadState(
        buttons: [.south, .rightShoulder], leftStick: GamepadStick(x: 0.5, y: -0.25), rightTrigger: 0.8)

    @Test func nothingIsSentWhileNotInFrontAndOnlyTheLetGoStateIsSentOnReturn() {
        let rig = greeted()
        let pad = plainPad(1)
        let released = firstReport(pad, .neutral)
        #expect(ready(rig, pad, state: heldState) == .input(slot: 0, report: firstReport(pad, heldState)))
        #expect(firstReport(pad, heldState) != released)
        rig.session.update(running: true, booted: true, foreground: false)
        #expect(framesUntilBarrier(rig) == [.input(slot: 0, report: released)])
        // What a controller says while the application is away is not taken.
        rig.session.submit(heldState, for: 1)
        rig.session.submit(GamepadState(buttons: .east), for: 1)
        #expect(noFrame(rig))
        #expect(rig.wakes.count == 0)
        // The return sends the let go state once, whatever was submitted meanwhile.
        rig.session.update(running: true, booted: true, foreground: true)
        #expect(framesUntilBarrier(rig, id: 0xbee0) == [.input(slot: 0, report: released)])
        rig.session.update(running: true, booted: true, foreground: true)
        #expect(noFrame(rig))
        // Only what the controller reports after the return is its state.
        rig.session.submit(heldState, for: 1)
        #expect(framesUntilBarrier(rig, id: 0xbee1) == [.input(slot: 0, report: firstReport(pad, heldState))])
    }

    @Test func aButtonHeldWhenTheRelayWasGivenUpOnIsLetGoOfWhenTheApplicationLeaves() {
        let rig = rig()
        let pad = plainPad(1)
        let released = firstReport(pad, .neutral)
        rig.guest.accepting = false
        rig.session.update(running: true, booted: true, foreground: true)
        rig.session.connect(pad, state: heldState)
        #expect(gamepadEventually { rig.session.hasGivenUp })
        // No relay to tell: the controller is let go of all the same, and the button
        // is let go of on the controller while the application is away.
        rig.session.update(running: true, booted: true, foreground: false)
        rig.session.settle()
        // The guest restarts while the application is away, and the relay is reached again.
        rig.session.restart()
        rig.guest.accepting = true
        rig.session.update(running: true, booted: false, foreground: false)
        #expect(gamepadEventually { rig.guest.connections == 1 })
        rig.guest.send(.hello(version: 1, slotCount: 8))
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.event(slot: 0, event: .start))
        // The device is made with nothing held, and so is the return.
        #expect(rig.guest.nextFrame() == .input(slot: 0, report: released))
        rig.session.update(running: true, booted: false, foreground: true)
        #expect(framesUntilBarrier(rig) == [.input(slot: 0, report: released)])
    }

    @Test func aRumbleReportWhileNotInFrontPlaysNothing() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.session.update(running: true, booted: true, foreground: false)
        _ = rig.guest.nextFrame()
        rig.player.clear()
        rig.guest.send(rumble(strong: 255, weak: 255))
        #expect(barrier(rig))
        #expect(rig.player.calls.isEmpty)
    }

    // MARK: Rumble

    private func tone(_ slot: Int, _ locality: HapticTarget.ControllerLocality, sequence: UInt32, intensity: Float, sharpness: Float) -> GamepadFakePlayer.Call {
        .play(
            HapticScore(
                target: .controller(slot: slot, locality: locality), sequence: sequence,
                events: [.tone(durationMilliseconds: 65_535, intensity: intensity, sharpness: sharpness)]))
    }

    private func waitForCalls(_ rig: Rig, _ count: Int) -> Bool { gamepadEventually { rig.player.calls.count >= count } }

    @Test func aRumbleReportPlaysTheStrongMotorOnTheLeftHandleAndTheWeakOneOnTheRight() {
        let rig = greeted()
        ready(rig, dualSense(1))
        rig.guest.send(rumble(strong: 255, weak: 51))
        #expect(waitForCalls(rig, 2))
        #expect(
            rig.player.calls == [
                tone(0, .leftHandle, sequence: 1, intensity: 1, sharpness: 0.2),
                tone(0, .rightHandle, sequence: 2, intensity: 51.0 / 255, sharpness: 0.8),
            ])
    }

    @Test func aTonePlayingIsChangedInPlaceAndStoppedAtZero() {
        let rig = greeted()
        ready(rig, dualSense(1))
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(waitForCalls(rig, 1))
        rig.guest.send(rumble(strong: 102, weak: 0))
        #expect(waitForCalls(rig, 2))
        rig.guest.send(rumble(strong: 102, weak: 0))
        rig.guest.send(rumble(strong: 0, weak: 0))
        #expect(waitForCalls(rig, 3))
        let left = HapticTarget.controller(slot: 0, locality: .leftHandle)
        #expect(
            rig.player.calls == [
                tone(0, .leftHandle, sequence: 1, intensity: 1, sharpness: 0.2),
                .intensity(102.0 / 255, left),
                .stop(left),
            ])
        // Nothing playing: a zero asks for nothing.
        rig.guest.send(rumble(strong: 0, weak: 0))
        #expect(barrier(rig))
        #expect(rig.player.calls.count == 3)
    }

    @Test func aToneThatSomethingElseEndedPlaysAgainWhenItsLevelChanges() {
        let rig = greeted()
        ready(rig, dualSense(1))
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(waitForCalls(rig, 1))
        // The system stopped the engine, or a stop of every target came: no tone is there.
        rig.player.toneAlive = false
        rig.guest.send(rumble(strong: 102, weak: 0))
        #expect(waitForCalls(rig, 3))
        let left = HapticTarget.controller(slot: 0, locality: .leftHandle)
        #expect(
            rig.player.calls == [
                tone(0, .leftHandle, sequence: 1, intensity: 1, sharpness: 0.2),
                .intensity(102.0 / 255, left),
                tone(0, .leftHandle, sequence: 2, intensity: 102.0 / 255, sharpness: 0.2),
            ])
        // The new tone is the one that is changed in place from now on.
        rig.player.toneAlive = true
        rig.guest.send(rumble(strong: 51, weak: 0))
        #expect(waitForCalls(rig, 4))
        #expect(rig.player.calls[3] == .intensity(51.0 / 255, left))
        #expect(rig.player.calls.count == 4)
    }

    @Test func aToneIsStartedAgainWhenItsLongestTimeHasPassed() {
        let rig = greeted()
        ready(rig, dualSense(1))
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(waitForCalls(rig, 1))
        // The player cuts a tone at 30 seconds, and the next report restarts it.
        rig.clock.advance(milliseconds: UInt64(HapticShapes.maximumToneMilliseconds) + 1)
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(waitForCalls(rig, 2))
        #expect(rig.player.calls[1] == tone(0, .leftHandle, sequence: 2, intensity: 1, sharpness: 0.2))
    }

    @Test func aControllerWithOneEngineGetsOneToneAtTheStrongerLevel() {
        let rig = greeted()
        ready(rig, dualSense(1, haptics: .single))
        rig.guest.send(rumble(strong: 255, weak: 255))
        #expect(waitForCalls(rig, 1))
        guard case .play(let score) = rig.player.calls[0] else { Issue.record("no play"); return }
        #expect(score.target == .controller(slot: 0, locality: .standard))
        #expect(rig.player.calls.count == 1)
    }

    @Test func aControllerWithNoMotorsDropsTheGuestsRumble() {
        let rig = greeted()
        ready(rig, dualSense(1, haptics: .none))
        rig.guest.send(rumble(strong: 255, weak: 255))
        #expect(barrier(rig))
        #expect(rig.player.calls.isEmpty)
    }

    @Test func aToneThePlayerCouldNotStartIsNotRemembered() {
        let rig = greeted()
        rig.player.result = .failed
        ready(rig, dualSense(1))
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(waitForCalls(rig, 1))
        // Not playing, so the same report asks the player again.
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(waitForCalls(rig, 2))
    }

    @Test func aControllerThatDisconnectsHasItsEnginesForgotten() {
        let rig = greeted()
        ready(rig, dualSense(1))
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(waitForCalls(rig, 1))
        rig.session.disconnect(id: 1)
        #expect(rig.guest.nextFrame() == .destroy(slot: 0))
        #expect(gamepadEventually { rig.player.calls.count >= 4 })
        for locality in HapticTarget.ControllerLocality.allCases {
            #expect(rig.player.calls.contains(.forget(.controller(slot: 0, locality: locality))))
        }
    }

    // MARK: Light and player

    @Test func theLightbarAndThePlayerLedsAreAppliedToTheController() {
        let rig = greeted()
        ready(rig, dualSense(4))
        rig.guest.send(.output(slot: 0, reportType: GamepadReportType.output, data: dualSenseOutput(flag1: 0x04 | 0x10, leds: 0b01010)))
        #expect(gamepadEventually { rig.indicators.calls.count == 2 })
        #expect(rig.indicators.calls == [.light(controller: 4, red: 1, green: 2, blue: 3), .player(controller: 4, index: 1)])
    }

    @Test func theLightAndPlayerAreAppliedAgainWhenTheApplicationReturnsToTheFront() {
        let rig = greeted()
        ready(rig, dualSense(4))
        rig.guest.send(.output(slot: 0, reportType: GamepadReportType.output, data: dualSenseOutput(flag1: 0x04 | 0x10, leds: 0b00100)))
        #expect(gamepadEventually { rig.indicators.calls.count == 2 })
        rig.session.update(running: true, booted: true, foreground: false)
        _ = rig.guest.nextFrame()
        rig.indicators.clear()
        rig.session.update(running: true, booted: true, foreground: true)
        _ = rig.guest.nextFrame()
        #expect(rig.indicators.calls == [.light(controller: 4, red: 1, green: 2, blue: 3), .player(controller: 4, index: 0)])
    }

    @Test func aReportThatIsNotTheDriversOutputReportAsksNothing() {
        let rig = greeted()
        ready(rig, dualSense(4))
        // A feature report type, and an output report for a slot with no controller.
        rig.guest.send(.output(slot: 0, reportType: GamepadReportType.feature, data: dualSenseOutput(flag1: 0x14, leds: 1)))
        rig.guest.send(.output(slot: 3, reportType: GamepadReportType.output, data: dualSenseOutput(flag1: 0x14, leds: 1)))
        rig.guest.send(rumble(strong: 255, weak: 255, slot: 5))
        #expect(barrier(rig))
        #expect(rig.indicators.calls.isEmpty)
        #expect(rig.player.calls.isEmpty)
    }

    // MARK: Feature and set reports

    @Test func aFeatureReportTheDriverAsksForIsAnsweredWithTheFamilysReport() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        let profile = DualSenseProfile(edge: false)
        rig.guest.send(.getReport(slot: 0, reportType: GamepadReportType.feature, number: 0x09, id: 5))
        #expect(
            rig.guest.nextFrame()
                == .getReportReply(slot: 0, id: 5, error: 0, data: profile.featureReport(number: 0x09, slot: 0)!))
        rig.guest.send(.getReport(slot: 0, reportType: GamepadReportType.feature, number: 0x05, id: 6))
        guard case .getReportReply(0, 6, 0, let calibration)? = rig.guest.nextFrame() else { Issue.record("no reply"); return }
        #expect(calibration.count == 41)
    }

    @Test func aReportTheDeviceDoesNotHaveIsAnsweredWithAnError() {
        let rig = greeted()
        ready(rig, dualSense(1))
        rig.guest.send(.getReport(slot: 0, reportType: GamepadReportType.feature, number: 0x77, id: 8))
        #expect(rig.guest.nextFrame() == .getReportReply(slot: 0, id: 8, error: GamepadErrno.invalid, data: []))
        // An input report is not asked for as a feature, and a slot with no controller has no device.
        rig.guest.send(.getReport(slot: 0, reportType: GamepadReportType.input, number: 0x09, id: 9))
        #expect(rig.guest.nextFrame() == .getReportReply(slot: 0, id: 9, error: GamepadErrno.invalid, data: []))
        rig.guest.send(.getReport(slot: 4, reportType: GamepadReportType.feature, number: 0x09, id: 10))
        #expect(rig.guest.nextFrame() == .getReportReply(slot: 4, id: 10, error: GamepadErrno.invalid, data: []))
    }

    @Test func aSetReportIsAcknowledged() {
        let rig = greeted()
        ready(rig, dualSense(1))
        rig.guest.send(.setReport(slot: 0, reportType: GamepadReportType.feature, number: 8, id: 33, data: [8, 1]))
        #expect(rig.guest.nextFrame() == .setReportReply(slot: 0, id: 33, error: 0))
    }

    // MARK: The relay

    @Test func aRelayThatIsLostIsConnectedToAgainAndTheDevicesAreMadeAgain() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.guest.send(rumble(strong: 255, weak: 0))
        #expect(waitForCalls(rig, 1))
        rig.guest.dropLatest()
        #expect(gamepadEventually { !rig.session.isConnected })
        // What the guest played is stopped, since its device went with the connection.
        #expect(gamepadEventually { rig.player.calls.contains(.stop(.controller(slot: 0, locality: .leftHandle))) })
        #expect(gamepadEventually { rig.guest.connections == 2 })
        rig.guest.send(.hello(version: 1, slotCount: 8))
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
        #expect(rig.session.slot(ofController: 1) == 0)
    }

    @Test func aRestartedDeviceStartsItsSequenceAgain() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.guest.dropLatest()
        #expect(gamepadEventually { rig.guest.connections == 2 })
        rig.guest.send(.hello(version: 1, slotCount: 8))
        _ = rig.guest.nextFrame()
        rig.guest.send(.created(slot: 0, result: 0))
        rig.guest.send(.event(slot: 0, event: .start))
        #expect(rig.guest.nextFrame() == .input(slot: 0, report: firstReport(pad, .neutral)))
    }

    @Test func aHelloTwiceBreaksTheProtocolAndTheConnectionIsMadeAgain() {
        let rig = greeted()
        rig.guest.send(.hello(version: 1, slotCount: 8))
        #expect(gamepadEventually { rig.guest.connections == 2 })
    }

    @Test func aFrameBeforeTheHelloBreaksTheProtocol() {
        let rig = rig()
        rig.session.update(running: true, booted: false, foreground: true)
        #expect(gamepadEventually { rig.guest.connections == 1 })
        rig.guest.send(.created(slot: 0, result: 0))
        #expect(gamepadEventually { rig.guest.connections == 2 })
    }

    @Test func aRelayOfAnotherVersionEndsTheAttempts() {
        let rig = rig()
        rig.session.update(running: true, booted: true, foreground: true)
        #expect(gamepadEventually { rig.guest.connections == 1 })
        rig.guest.send(.hello(version: 2, slotCount: 8))
        #expect(gamepadEventually { rig.session.hasGivenUp })
        #expect(rig.log.lines.contains { $0.contains("version 2") })
        #expect(rig.guest.closedByHost())
    }

    @Test func aRelayAnswersAreIgnoredForASlotThatIsNotMade() {
        let rig = greeted()
        rig.guest.send(.created(slot: 2, result: 0))
        rig.guest.send(.event(slot: 2, event: .start))
        rig.guest.send(.event(slot: 2, event: .open))
        #expect(barrier(rig))
        #expect(rig.session.isConnected)
    }

    // MARK: Connecting and giving up

    @Test func aSessionThatIsNotRunningNeverConnects() {
        let rig = rig()
        rig.session.connect(dualSense(1), state: .neutral)
        rig.session.update(running: false, booted: false, foreground: true)
        usleep(100_000)
        #expect(rig.guest.attempts == 0)
    }

    @Test func theEndOfTheRunClosesTheConnectionAndForgetsTheDevices() {
        let rig = greeted()
        let pad = dualSense(1)
        ready(rig, pad)
        rig.session.update(running: false, booted: true, foreground: true)
        #expect(rig.guest.closedByHost())
        #expect(!rig.session.isConnected)
        // The controller stays: the next run's hello makes it again.
        rig.session.update(running: true, booted: false, foreground: true)
        #expect(gamepadEventually { rig.guest.connections == 2 })
        rig.guest.send(.hello(version: 1, slotCount: 8))
        #expect(rig.guest.nextFrame() == createFrame(pad, slot: 0))
    }

    @Test func attemptsBeforeBootNeverEndAndAfterBootFiveFailuresEndThem() {
        let rig = rig()
        rig.guest.accepting = false
        rig.session.update(running: true, booted: false, foreground: true)
        #expect(gamepadEventually { rig.guest.attempts >= 8 })
        #expect(!rig.session.hasGivenUp)
        rig.session.update(running: true, booted: true, foreground: true)
        #expect(gamepadEventually { rig.session.hasGivenUp })
        let attempts = rig.guest.attempts
        usleep(100_000)
        #expect(rig.guest.attempts == attempts)
    }

    @Test func aRunThatEndsAndBeginsAgainTriesAgainAfterGivingUp() {
        let rig = rig()
        rig.guest.accepting = false
        rig.session.update(running: true, booted: true, foreground: true)
        #expect(gamepadEventually { rig.session.hasGivenUp })
        // Still running: no more attempts, though the application says the same again.
        rig.session.update(running: true, booted: true, foreground: true)
        let attempts = rig.guest.attempts
        usleep(60_000)
        #expect(rig.guest.attempts == attempts)
        rig.session.update(running: false, booted: false, foreground: true)
        #expect(gamepadEventually { !rig.session.hasGivenUp })
        rig.guest.accepting = true
        rig.session.update(running: true, booted: false, foreground: true)
        #expect(gamepadEventually { rig.guest.connections == 1 })
    }

    @Test func aGuestThatRestartedBeginsTheAttemptsAgain() {
        let rig = rig()
        rig.guest.accepting = false
        rig.session.update(running: true, booted: true, foreground: true)
        #expect(gamepadEventually { rig.session.hasGivenUp })
        rig.session.restart()
        #expect(gamepadEventually { !rig.session.hasGivenUp })
        // The run is still wanted: the next update begins it.
        rig.guest.accepting = true
        rig.session.update(running: true, booted: false, foreground: true)
        #expect(gamepadEventually { rig.guest.connections == 1 })
    }
}
