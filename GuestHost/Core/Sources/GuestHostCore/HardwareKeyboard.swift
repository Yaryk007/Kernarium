// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// One key of a hardware keyboard going down or up, by its position: the
/// usage on the HID Keyboard/Keypad page (0x04 is A, 0xE3 the left Command
/// key), which the runtime maps to the key a USB keyboard with that usage
/// sends. Android's own key layout turns the position into a character.
public struct HardwareKeyEvent: Equatable, Sendable {
    public let usage: UInt32
    public let pressed: Bool

    public init(usage: UInt32, pressed: Bool) {
        self.usage = usage
        self.pressed = pressed
    }
}

/// The keys the guest has been told are down, so that none is left down when
/// the focus moves on. A key goes down once and up once: Android repeats keys
/// itself, so a second press of a key already down, which UIKit may deliver as
/// its own repeat, sends nothing.
public struct HardwareKeyboardState: Equatable, Sendable {
    /// The usages the keyboard can report: the keys from A through the right
    /// Command key. Rollover and error codes below them name no key.
    public static let usages: ClosedRange<UInt32> = 0x04...0xE7
    /// The modifier keys (Control, Shift, Option and Command on each side).
    public static let modifierUsages: ClosedRange<UInt32> = 0xE0...0xE7
    /// The Caps Lock key.
    public static let capsLockUsage: UInt32 = 0x39

    /// The keys that are down, in the order they went down.
    public private(set) var held: [UInt32] = []

    public init() {}

    public var isEmpty: Bool { held.isEmpty }

    public func isHeld(_ usage: UInt32) -> Bool { held.contains(usage) }

    /// Puts a key down. No event for a usage that names no key or for a key
    /// that is already down.
    public mutating func press(_ usage: UInt32) -> [HardwareKeyEvent] {
        guard Self.usages.contains(usage), !held.contains(usage) else { return [] }
        held.append(usage)
        return [HardwareKeyEvent(usage: usage, pressed: true)]
    }

    /// Lifts a key. No event for a key that is not down: it was released with
    /// the rest, or went down before this keyboard had the focus.
    public mutating func release(_ usage: UInt32) -> [HardwareKeyEvent] {
        guard let index = held.firstIndex(of: usage) else { return [] }
        held.remove(at: index)
        return [HardwareKeyEvent(usage: usage, pressed: false)]
    }

    /// Lifts every key that is down, the last one first, so a modifier that
    /// went down before the keys it modifies comes up after them.
    public mutating func releaseAll() -> [HardwareKeyEvent] {
        let events = held.reversed().map { HardwareKeyEvent(usage: $0, pressed: false) }
        held.removeAll()
        return events
    }

    /// The order in which a set of presses that arrive together goes to the
    /// guest: modifiers first when they go down, last when they come up, and
    /// otherwise by usage so the order does not depend on a set's.
    public static func ordered(_ usages: [UInt32], pressing: Bool) -> [UInt32] {
        usages.sorted { lhs, rhs in
            let lhsModifier = modifierUsages.contains(lhs)
            let rhsModifier = modifierUsages.contains(rhs)
            if lhsModifier != rhsModifier { return lhsModifier == pressing }
            return lhs < rhs
        }
    }
}

/// Keeps Android's Caps Lock in step with iOS's.
///
/// Android owns its Caps Lock: the key toggles it and Android lights the
/// keyboard's Caps Lock light, which the runtime reports. iOS keeps its own,
/// which the person may have set before the guest had the focus. At each key
/// that goes down the two are compared, and when they differ the Caps Lock key
/// is tapped once. The light arrives later than the tap, so a tap is not
/// repeated while its answer is awaited, and a light that never answers is not
/// chased: Android would be toggled again and again.
public struct CapsLockReconciler: Equatable, Sendable {
    /// How long a tap, or a Caps Lock key the person pressed, is given to be
    /// answered by the light before the two are compared again.
    public static let settleInterval: Double = 1
    /// Taps in a row that the light did not answer before the comparison
    /// stops, until the light changes.
    public static let unansweredLimit = 2

    private struct Tap: Equatable, Sendable {
        let target: Bool
        let at: Double
    }

    private var lit = false
    private var tapped: Tap?
    private var settledUntil: Double?
    private var unanswered = 0

    public init() {}

    /// Android's light, as the runtime last reported it.
    public mutating func androidLight(_ lit: Bool) {
        self.lit = lit
        unanswered = 0
        if tapped?.target == lit { tapped = nil }
    }

    /// The keyboard was plugged anew: Android starts a keyboard with its Caps
    /// Lock off.
    public mutating func keyboardPlugged() {
        self = CapsLockReconciler()
    }

    /// The person's own Caps Lock key went to the guest at `now`. iOS and
    /// Android both toggle, and the light follows, so the two are not compared
    /// until it has had time to.
    public mutating func capsLockKeyForwarded(at now: Double) {
        settledUntil = now + Self.settleInterval
        tapped = nil
    }

    /// Whether the Caps Lock key must be tapped before the key that is going
    /// down at `now`, given iOS's Caps Lock state.
    public mutating func reconcile(alphaShift: Bool, at now: Double) -> Bool {
        if let settledUntil, now < settledUntil { return false }
        guard alphaShift != lit else {
            tapped = nil
            return false
        }
        if let tapped, tapped.target == alphaShift, now < tapped.at + Self.settleInterval { return false }
        guard unanswered < Self.unansweredLimit else { return false }
        tapped = Tap(target: alphaShift, at: now)
        unanswered += 1
        return true
    }
}
