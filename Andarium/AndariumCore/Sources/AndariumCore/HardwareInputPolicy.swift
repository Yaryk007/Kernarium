// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// When the guest has a keyboard and a mouse, and where the system pointer's
/// clicks go.
///
/// Android shows a cursor as soon as it has a mouse and hides its on-screen
/// keyboard when it has a keyboard, so the guest's devices are plugged only
/// while iOS has the hardware, and the guest's own touch behaviour is kept
/// where it does not. The policy holds what iOS has said and what the person
/// has done, and answers what the guest should have. Time is whatever clock
/// the caller reads, in seconds.
///
/// - The keyboard is plugged while iOS reports one, and also by any key that
///   goes down, because the Simulator reports its keyboard only around a key.
///   Unplugging waits `unplugDelay` after the report goes, or after the last
///   key, so a report that flaps does not take Android's keyboard away and
///   give it back, and waits for the keys held down to come up.
/// - The mouse is plugged once iOS reports one and the person has used it: it
///   moved over the display, clicked or scrolled. The Simulator reports a
///   mouse that is not there, and a stray report must not put a cursor on the
///   guest's screen. Unplugging waits `unplugDelay` after the report goes, and
///   the next mouse has to be used again. An iPhone has no pointer to give
///   the guest, and the person's "Use the pointer as touch" keeps the mouse
///   away so that the system pointer's clicks stay touches.
public struct HardwareInputPolicy: Equatable, Sendable {
    /// How long the devices stay once iOS stops reporting their hardware.
    public static let unplugDelay: Double = 3

    /// What the guest should have now.
    public struct Plan: Equatable, Sendable {
        public var keyboardPlugged = false
        public var mousePlugged = false
        /// The system pointer's events go to the guest's mouse, even before it
        /// is plugged: the first of them is what plugs it.
        public var routesPointer = false
        /// The time at which the plan changes by itself, if nothing happens.
        public var nextChange: Double?
    }

    /// The guest's run has started, so its devices exist; the plan is empty
    /// before and after, and nothing plugged survives the run.
    public var running = false {
        didSet {
            guard !running, oldValue else { return }
            keyboardPlugged = false
            keyboardLastKeyAt = nil
            keyboardGoneAt = nil
            keysHeld = false
            mousePlugged = false
            mouseUsed = false
            mouseGoneAt = nil
        }
    }
    /// The device can have a pointer: not an iPhone.
    public var supportsMouse = true
    /// The person turned "Use the pointer as touch" on.
    public var pointerAsTouch = false

    private var keyboardReported = false
    private var keyboardPlugged = false
    private var keyboardLastKeyAt: Double?
    private var keyboardGoneAt: Double?
    private var keysHeld = false

    private var mouseReported = false
    private var mouseUsed = false
    private var mousePlugged = false
    private var mouseGoneAt: Double?

    public init() {}

    /// iOS reports a keyboard, or no longer does.
    public mutating func keyboardReported(_ connected: Bool, at now: Double) {
        if connected {
            keyboardGoneAt = nil
        } else if keyboardReported {
            keyboardGoneAt = now
        }
        keyboardReported = connected
    }

    /// A key went down. The keyboard is plugged at once.
    public mutating func keyDown(at now: Double) {
        keyboardLastKeyAt = now
        if running { keyboardPlugged = true }
    }

    /// Whether any key is held down. A held key keeps the keyboard.
    public mutating func keysHeld(_ held: Bool) {
        keysHeld = held
    }

    /// iOS reports a mouse, or no longer does.
    public mutating func mouseReported(_ connected: Bool, at now: Double) {
        if connected {
            mouseGoneAt = nil
        } else if mouseReported {
            mouseGoneAt = now
        }
        if !connected, !mousePlugged { mouseUsed = false }
        mouseReported = connected
    }

    /// The person moved the pointer over the display, clicked it or scrolled.
    /// Only a mouse the system pointer's events are routed to counts.
    public mutating func pointerUsed() {
        guard running, supportsMouse, !pointerAsTouch, mouseReported || mousePlugged else { return }
        mouseUsed = true
    }

    /// What the guest should have at `now`; time passing is part of it.
    public mutating func plan(at now: Double) -> Plan {
        var plan = Plan()
        guard running else { return plan }
        var changes: [Double] = []

        if keyboardReported {
            keyboardPlugged = true
        } else if keyboardPlugged {
            let last = max(keyboardGoneAt ?? -.infinity, keyboardLastKeyAt ?? -.infinity)
            let due = last == -.infinity ? now : last + Self.unplugDelay
            if keysHeld {
                // Its release asks again.
            } else if now >= due {
                keyboardPlugged = false
                keyboardGoneAt = nil
                keyboardLastKeyAt = nil
            } else {
                changes.append(due)
            }
        }

        let mouseAllowed = supportsMouse && !pointerAsTouch
        if !mouseAllowed {
            mousePlugged = false
        } else if mouseReported, mouseUsed {
            mousePlugged = true
        } else if mousePlugged, let goneAt = mouseGoneAt {
            let due = goneAt + Self.unplugDelay
            if now >= due {
                mousePlugged = false
                mouseUsed = false
                mouseGoneAt = nil
            } else {
                changes.append(due)
            }
        }

        plan.keyboardPlugged = keyboardPlugged
        plan.mousePlugged = mousePlugged
        plan.routesPointer = mouseAllowed && (mouseReported || mousePlugged)
        plan.nextChange = changes.min()
        return plan
    }
}
