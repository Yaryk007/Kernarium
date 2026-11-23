// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// An Android application that captures the pointer (a game, a remote desktop)
/// takes the mouse's raw motion and buttons and has the cursor hidden. The
/// host answers by asking iOS to lock the pointer, and by sending the mouse's
/// raw motion from the Game Controller framework, which still reports it under
/// the lock, in place of the position and the buttons of the system's pointer.
///
/// This holds when that is so. Capture is Android's; it counts only while the
/// guest is in front and has the mouse plugged, and goes away with either.
/// The lock is iOS's to give: it is refused in a window that is not full
/// screen. Then the raw motion goes on only while the system's pointer is
/// over the display, so a pointer that has left for another window does not
/// move the guest.
public struct PointerCapture: Equatable, Sendable {
    /// What a change did to the capture.
    public enum Change: Equatable, Sendable {
        case none
        /// Capture began: the pointer's contact and buttons end, because from
        /// here on the guest's pointer device is disabled and the mouse is the
        /// guest's pointer.
        case began
        /// Capture ended: the mouse's buttons come up and the system's pointer
        /// is the guest's again.
        case ended
    }

    private var androidCaptured = false
    private var inFront = false
    private var mousePlugged = false
    private var lockHeld = false
    private var pointerOverDisplay = false

    public init() {}

    /// The pointer is captured for the guest.
    public var isCapturing: Bool { androidCaptured && inFront && mousePlugged }

    /// iOS should be asked to lock the pointer.
    public var wantsLock: Bool { isCapturing }

    /// The mouse's raw motion, buttons and scroll go to the guest.
    public var forwardsRawMotion: Bool { isCapturing && (lockHeld || pointerOverDisplay) }

    /// Android captured or released the pointer.
    public mutating func androidCaptured(_ captured: Bool) -> Change {
        change { $0.androidCaptured = captured }
    }

    /// The guest runs and is in front, or is not.
    public mutating func inFront(_ front: Bool) -> Change {
        change { $0.inFront = front }
    }

    /// The guest's mouse was plugged or unplugged.
    public mutating func mousePlugged(_ plugged: Bool) -> Change {
        change { $0.mousePlugged = plugged }
    }

    /// The scene's pointer is locked, or is not.
    public mutating func lockHeld(_ held: Bool) {
        lockHeld = held
    }

    /// The system's pointer is over the display, or is not.
    public mutating func pointerOverDisplay(_ over: Bool) {
        pointerOverDisplay = over
    }

    /// The run is over: Android's capture went with it.
    public mutating func reset() {
        self = PointerCapture()
    }

    private mutating func change(_ update: (inout PointerCapture) -> Void) -> Change {
        let before = isCapturing
        update(&self)
        switch (before, isCapturing) {
        case (false, true): return .began
        case (true, false): return .ended
        default: return .none
        }
    }
}

/// Whether the system pointer's clicks are the guest's touches
/// (Settings' "Use the pointer as touch"), kept for the install across
/// launches. A fresh install is off: Android sees a mouse.
public struct PointerPreference {
    /// The stored value's key in the defaults database.
    public static let key = "andarium.pointerAsTouch"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// The last value kept; off until one is.
    public var isOn: Bool { defaults.bool(forKey: Self.key) }

    public func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }

    /// How a launch begins.
    public struct Launch: Equatable, Sendable {
        /// Whether the pointer is a touch from the start.
        public var isOn: Bool
        /// Whether what the person sets from here on is kept. A harness's
        /// launch decides for its run alone, so what the person chose stays.
        public var keepsChanges: Bool
    }

    /// The start of a launch. A launch that carries any launch setting is a
    /// harness's and starts off; only a launch with none restores the kept
    /// value.
    public func launch(automated: Bool) -> Launch {
        automated ? Launch(isOn: false, keepsChanges: false) : Launch(isOn: isOn, keepsChanges: true)
    }
}
