// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

struct HardwareKeyboardStateTests {
    private let leftCommand: UInt32 = 0xE3
    private let leftShift: UInt32 = 0xE1
    private let keyA: UInt32 = 0x04

    @Test func aKeyGoesDownOnceAndUpOnce() {
        var keys = HardwareKeyboardState()
        let down = keys.press(keyA)
        #expect(down == [HardwareKeyEvent(usage: keyA, pressed: true)])
        #expect(keys.isHeld(keyA))
        // A repeat of a key already down is not a second press: Android
        // repeats keys itself.
        let repeated = keys.press(keyA)
        #expect(repeated.isEmpty)
        let up = keys.release(keyA)
        #expect(up == [HardwareKeyEvent(usage: keyA, pressed: false)])
        #expect(keys.isEmpty)
        let again = keys.release(keyA)
        #expect(again.isEmpty)
    }

    @Test func usagesThatNameNoKeyAreDropped() {
        var keys = HardwareKeyboardState()
        for usage: UInt32 in [0, 1, 2, 3, 0xE8, 0x100, 0xFFFF] {
            let events = keys.press(usage)
            #expect(events.isEmpty, "usage \(usage)")
        }
        #expect(keys.isEmpty)
        // Both ends of the range are keys: A, and the right Command key.
        let first = keys.press(0x04)
        let last = keys.press(0xE7)
        #expect(first.count == 1)
        #expect(last.count == 1)
    }

    @Test func releaseAllLiftsTheKeysNewestFirst() {
        var keys = HardwareKeyboardState()
        _ = keys.press(leftCommand)
        _ = keys.press(leftShift)
        _ = keys.press(keyA)
        let released = keys.releaseAll()
        #expect(released == [
            HardwareKeyEvent(usage: keyA, pressed: false),
            HardwareKeyEvent(usage: leftShift, pressed: false),
            HardwareKeyEvent(usage: leftCommand, pressed: false),
        ])
        #expect(keys.isEmpty)
        let none = keys.releaseAll()
        #expect(none.isEmpty)
        // Nothing is held, so a late release of one of them sends nothing.
        let late = keys.release(keyA)
        #expect(late.isEmpty)
    }

    @Test func keysThatArriveTogetherAreOrderedByTheirModifiers() {
        let together: [UInt32] = [keyA, leftCommand, 0x05, leftShift]
        #expect(HardwareKeyboardState.ordered(together, pressing: true) == [leftShift, leftCommand, keyA, 0x05])
        #expect(HardwareKeyboardState.ordered(together, pressing: false) == [keyA, 0x05, leftShift, leftCommand])
    }
}

struct CapsLockReconcilerTests {
    @Test func aCapsLockSetBeforeTheFocusIsTappedIntoAndroid() {
        var caps = CapsLockReconciler()
        let tap = caps.reconcile(alphaShift: true, at: 10)
        #expect(tap)
    }

    @Test func nothingIsTappedWhileTheTwoAgree() {
        var caps = CapsLockReconciler()
        let off = caps.reconcile(alphaShift: false, at: 10)
        #expect(!off)
        caps.androidLight(true)
        let on = caps.reconcile(alphaShift: true, at: 11)
        #expect(!on)
        caps.androidLight(false)
        let offAgain = caps.reconcile(alphaShift: false, at: 12)
        #expect(!offAgain)
    }

    @Test func aTapIsNotRepeatedWhileItsLightIsAwaited() {
        var caps = CapsLockReconciler()
        let tap = caps.reconcile(alphaShift: true, at: 10)
        #expect(tap)
        // The next keys come before the light does.
        let soon = caps.reconcile(alphaShift: true, at: 10.05)
        let later = caps.reconcile(alphaShift: true, at: 10.5)
        #expect(!soon)
        #expect(!later)
        caps.androidLight(true)
        let answered = caps.reconcile(alphaShift: true, at: 10.6)
        #expect(!answered)
    }

    @Test func aTapTheLightNeverAnswersIsRepeatedOnceAndThenLeftAlone() {
        var caps = CapsLockReconciler()
        let first = caps.reconcile(alphaShift: true, at: 10)
        let second = caps.reconcile(alphaShift: true, at: 11.5)
        #expect(first)
        #expect(second)
        // Two taps did nothing: Android is not toggled again and again.
        let third = caps.reconcile(alphaShift: true, at: 13)
        let muchLater = caps.reconcile(alphaShift: true, at: 100)
        #expect(!third)
        #expect(!muchLater)
        // Until the light says something.
        caps.androidLight(false)
        let afterLight = caps.reconcile(alphaShift: true, at: 101)
        #expect(afterLight)
    }

    @Test func theCapsLockKeyThePersonPressedIsGivenTimeToBeAnswered() {
        var caps = CapsLockReconciler()
        caps.capsLockKeyForwarded(at: 10)
        // iOS has toggled and so has Android, whose light has not come back.
        let early = caps.reconcile(alphaShift: true, at: 10.2)
        #expect(!early)
        caps.androidLight(true)
        let lit = caps.reconcile(alphaShift: true, at: 10.3)
        #expect(!lit)
        // After the interval a difference is real.
        let real = caps.reconcile(alphaShift: false, at: 11.5)
        #expect(real)
    }

    @Test func aNewlyPluggedKeyboardStartsWithTheLightOff() {
        var caps = CapsLockReconciler()
        caps.androidLight(true)
        let before = caps.reconcile(alphaShift: true, at: 1)
        #expect(!before)
        caps.keyboardPlugged()
        let after = caps.reconcile(alphaShift: true, at: 2)
        #expect(after)
    }
}
