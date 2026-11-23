// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

/// A policy for a run that has started on a device that can have a pointer.
private func running(supportsMouse: Bool = true) -> HardwareInputPolicy {
    var policy = HardwareInputPolicy()
    policy.supportsMouse = supportsMouse
    policy.running = true
    return policy
}

struct HardwareKeyboardPolicyTests {
    @Test func nothingIsPluggedBeforeTheRunOrAfter() {
        var policy = HardwareInputPolicy()
        policy.keyboardReported(true, at: 0)
        policy.mouseReported(true, at: 0)
        let before = policy.plan(at: 1)
        #expect(before == HardwareInputPolicy.Plan())
        policy.running = true
        // iOS said so before the run: the guest has the keyboard as it starts.
        let during = policy.plan(at: 2)
        #expect(during.keyboardPlugged)
        policy.running = false
        let after = policy.plan(at: 3)
        #expect(after == HardwareInputPolicy.Plan())
    }

    @Test func aReportedKeyboardIsPluggedAndStaysForAWhileAfterItGoes() {
        var policy = running()
        policy.keyboardReported(true, at: 10)
        let plugged = policy.plan(at: 10)
        #expect(plugged.keyboardPlugged)
        #expect(plugged.nextChange == nil)

        policy.keyboardReported(false, at: 20)
        let waiting = policy.plan(at: 21)
        #expect(waiting.keyboardPlugged)
        #expect(waiting.nextChange == 20 + HardwareInputPolicy.unplugDelay)
        let before = policy.plan(at: 22.9)
        #expect(before.keyboardPlugged)
        let gone = policy.plan(at: 23)
        #expect(!gone.keyboardPlugged)
        #expect(gone.nextChange == nil)
    }

    @Test func aReportThatFlapsNeverTakesTheKeyboardAway() {
        var policy = running()
        policy.keyboardReported(true, at: 0)
        _ = policy.plan(at: 0)
        for second in stride(from: 1.0, to: 20.0, by: 1.0) {
            policy.keyboardReported(false, at: second)
            let down = policy.plan(at: second + 0.1)
            policy.keyboardReported(true, at: second + 0.2)
            let up = policy.plan(at: second + 0.3)
            #expect(down.keyboardPlugged)
            #expect(up.keyboardPlugged)
        }
    }

    @Test func aKeyPlugsAKeyboardIOSDidNotReport() {
        var policy = running()
        let before = policy.plan(at: 5)
        #expect(!before.keyboardPlugged)
        policy.keyDown(at: 5)
        let plugged = policy.plan(at: 5)
        #expect(plugged.keyboardPlugged)
        #expect(plugged.nextChange == 5 + HardwareInputPolicy.unplugDelay)
        // The Simulator reports its keyboard around a key, and not between.
        policy.keyboardReported(true, at: 5.1)
        policy.keyboardReported(false, at: 5.2)
        let between = policy.plan(at: 6)
        #expect(between.keyboardPlugged)
        policy.keyDown(at: 7)
        let later = policy.plan(at: 9.9)
        #expect(later.keyboardPlugged)
        let gone = policy.plan(at: 10.1)
        #expect(!gone.keyboardPlugged)
    }

    @Test func aKeyHeldDownKeepsTheKeyboardUntilItComesUp() {
        var policy = running()
        policy.keyDown(at: 0)
        policy.keysHeld(true)
        let held = policy.plan(at: 60)
        #expect(held.keyboardPlugged)
        // Nothing to wait for by the clock: the key's release asks again.
        #expect(held.nextChange == nil)
        policy.keysHeld(false)
        let released = policy.plan(at: 61)
        #expect(!released.keyboardPlugged)
    }

    @Test func aKeyboardWorksWithoutAnyMouseSupport() {
        var policy = running(supportsMouse: false)
        policy.keyboardReported(true, at: 0)
        let plan = policy.plan(at: 0)
        #expect(plan.keyboardPlugged)
        #expect(!plan.mousePlugged)
    }
}

struct HardwareMousePolicyTests {
    @Test func theSimulatorsPhantomMouseNeverPlugs() {
        var policy = running()
        // Reported at launch, with nothing attached, and a hover that begins
        // where the view is without the pointer having moved.
        policy.mouseReported(true, at: 0)
        for second in 1...30 {
            let plan = policy.plan(at: Double(second))
            #expect(!plan.mousePlugged)
        }
        let routed = policy.plan(at: 31)
        // The pointer's events are the guest's even so: the first one
        // plugs it.
        #expect(routed.routesPointer)
    }

    @Test func aMouseThatMovedIsPluggedAndLeavesAfterItGoes() {
        var policy = running()
        policy.mouseReported(true, at: 0)
        policy.pointerUsed()
        let plugged = policy.plan(at: 1)
        #expect(plugged.mousePlugged)
        #expect(plugged.routesPointer)

        policy.mouseReported(false, at: 10)
        let waiting = policy.plan(at: 11)
        #expect(waiting.mousePlugged)
        #expect(waiting.routesPointer)
        #expect(waiting.nextChange == 10 + HardwareInputPolicy.unplugDelay)
        let gone = policy.plan(at: 13)
        #expect(!gone.mousePlugged)
        #expect(!gone.routesPointer)

        // The next mouse has to be used before the guest sees it.
        policy.mouseReported(true, at: 20)
        let again = policy.plan(at: 20)
        #expect(!again.mousePlugged)
        policy.pointerUsed()
        let used = policy.plan(at: 21)
        #expect(used.mousePlugged)
    }

    @Test func aMouseThatFlapsStaysPlugged() {
        var policy = running()
        policy.mouseReported(true, at: 0)
        policy.pointerUsed()
        _ = policy.plan(at: 0)
        for second in stride(from: 1.0, to: 12.0, by: 1.0) {
            policy.mouseReported(false, at: second)
            let down = policy.plan(at: second + 0.1)
            policy.mouseReported(true, at: second + 0.2)
            let up = policy.plan(at: second + 0.3)
            #expect(down.mousePlugged)
            #expect(up.mousePlugged)
        }
    }

    @Test func useWithoutAReportedMouseCountsForNothing() {
        var policy = running()
        policy.pointerUsed()
        policy.mouseReported(true, at: 1)
        let plan = policy.plan(at: 1)
        #expect(!plan.mousePlugged)
    }

    @Test func nothingPlugsOnAnIPhone() {
        var policy = running(supportsMouse: false)
        // The iPhone Simulator reports the same phantom mouse.
        policy.mouseReported(true, at: 0)
        policy.pointerUsed()
        let plan = policy.plan(at: 1)
        #expect(!plan.mousePlugged)
        #expect(!plan.routesPointer)
    }

    @Test func thePointerAsTouchKeepsTheMouseAwayAndTheClicksTouches() {
        var policy = running()
        policy.mouseReported(true, at: 0)
        policy.pointerUsed()
        let plugged = policy.plan(at: 1)
        #expect(plugged.mousePlugged)

        policy.pointerAsTouch = true
        let touch = policy.plan(at: 2)
        #expect(!touch.mousePlugged)
        #expect(!touch.routesPointer)
        // Use that arrives meanwhile counts for nothing.
        policy.pointerUsed()

        policy.pointerAsTouch = false
        let back = policy.plan(at: 3)
        #expect(back.mousePlugged)
        #expect(back.routesPointer)
    }

    @Test func aMouseAndAKeyboardAreIndependent() {
        var policy = running()
        policy.mouseReported(true, at: 0)
        policy.pointerUsed()
        policy.keyDown(at: 0)
        let first = policy.plan(at: 0)
        #expect(first.keyboardPlugged)
        #expect(first.mousePlugged)
        policy.mouseReported(false, at: 1)
        let both = policy.plan(at: 2)
        #expect(both.keyboardPlugged)
        #expect(both.mousePlugged)
        // The earlier deadline is the one the caller waits for.
        #expect(both.nextChange == 3)
        let keyboardGone = policy.plan(at: 3.5)
        #expect(!keyboardGone.keyboardPlugged)
        #expect(keyboardGone.mousePlugged)
        #expect(keyboardGone.nextChange == 4)
        let bothGone = policy.plan(at: 4.5)
        #expect(!bothGone.mousePlugged)
    }
}

struct PointerCaptureTests {
    private func front(_ capture: inout PointerCapture) {
        _ = capture.inFront(true)
        _ = capture.mousePlugged(true)
    }

    @Test func androidsCaptureBeginsAndEndsOnlyWhenTheGuestCanHaveIt() {
        var capture = PointerCapture()
        let early = capture.androidCaptured(true)
        #expect(early == .none)
        #expect(!capture.isCapturing)
        _ = capture.inFront(true)
        let begun = capture.mousePlugged(true)
        #expect(begun == .began)
        #expect(capture.isCapturing)
        #expect(capture.wantsLock)
        let ended = capture.androidCaptured(false)
        #expect(ended == .ended)
        #expect(!capture.wantsLock)
    }

    @Test func leavingTheFrontEndsTheCaptureAndTheLock() {
        var capture = PointerCapture()
        front(&capture)
        _ = capture.androidCaptured(true)
        capture.lockHeld(true)
        let left = capture.inFront(false)
        #expect(left == .ended)
        #expect(!capture.wantsLock)
        // Returning finds Android still capturing, and asks again.
        let back = capture.inFront(true)
        #expect(back == .began)
        #expect(capture.wantsLock)
    }

    @Test func anUnpluggedMouseCapturesNothing() {
        var capture = PointerCapture()
        front(&capture)
        _ = capture.androidCaptured(true)
        let unplugged = capture.mousePlugged(false)
        #expect(unplugged == .ended)
        #expect(!capture.isCapturing)
    }

    @Test func rawMotionFlowsUnderTheLock() {
        var capture = PointerCapture()
        front(&capture)
        _ = capture.androidCaptured(true)
        #expect(!capture.forwardsRawMotion)
        capture.lockHeld(true)
        #expect(capture.forwardsRawMotion)
        capture.lockHeld(false)
        #expect(!capture.forwardsRawMotion)
    }

    @Test func aRefusedLockStillGivesRawMotionOverTheDisplay() {
        var capture = PointerCapture()
        front(&capture)
        _ = capture.androidCaptured(true)
        // iOS refused: a window in Stage Manager. The pointer is elsewhere.
        #expect(!capture.forwardsRawMotion)
        capture.pointerOverDisplay(true)
        #expect(capture.forwardsRawMotion)
        capture.pointerOverDisplay(false)
        #expect(!capture.forwardsRawMotion)
    }

    @Test func aRunThatEndsForgetsTheCapture() {
        var capture = PointerCapture()
        front(&capture)
        _ = capture.androidCaptured(true)
        capture.reset()
        #expect(!capture.isCapturing)
        let next = capture.inFront(true)
        let plugged = capture.mousePlugged(true)
        #expect(next == .none)
        #expect(plugged == .none)
    }
}

struct PointerPreferenceTests {
    private func withDefaults<Result>(_ body: (UserDefaults) -> Result) -> Result {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-pointer-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let name = directory.appendingPathComponent("defaults").path
        let defaults = UserDefaults(suiteName: name)!
        defer {
            defaults.removePersistentDomain(forName: name)
            try? FileManager.default.removeItem(at: directory)
        }
        return body(defaults)
    }

    @Test func aFreshInstallLetsAndroidSeeAMouse() {
        withDefaults { defaults in
            let preference = PointerPreference(defaults: defaults)
            #expect(!preference.isOn)
            let start = preference.launch(automated: false)
            #expect(!start.isOn)
            #expect(start.keepsChanges)
        }
    }

    @Test func theChoiceIsKeptAndAHarnessLaunchLeavesItAlone() {
        withDefaults { defaults in
            let preference = PointerPreference(defaults: defaults)
            preference.keep(true)
            #expect(PointerPreference(defaults: defaults).isOn)
            let person = preference.launch(automated: false)
            #expect(person.isOn && person.keepsChanges)
            // A launch with settings runs with the default and keeps nothing.
            let harness = preference.launch(automated: true)
            #expect(!harness.isOn && !harness.keepsChanges)
            #expect(preference.isOn)
        }
    }
}
