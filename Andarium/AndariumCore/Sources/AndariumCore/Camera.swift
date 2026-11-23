// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How far the system lets Andarium use this device's cameras, in the terms the
/// Camera row and the start need. The application reads it from iOS; reading it
/// never shows a prompt.
public enum CameraAccess: Equatable, Sendable {
    /// The device has no camera to share (the Simulator, a Mac without one).
    case unavailable
    /// iOS has not asked the person yet.
    case notDetermined
    /// Something other than the person forbids it (Screen Time, a profile).
    case restricted
    /// The person refused it, or took it away in Settings.
    case denied
    /// Andarium may capture.
    case authorized

    /// Nothing can be captured and the person has no question left to answer.
    public var refuses: Bool { self == .restricted || self == .denied }

    /// The person may turn Camera on: there is a camera, and iOS has not
    /// refused it.
    public var offered: Bool { self != .unavailable }
}

/// Whether the device has a camera to share. The guest is given cameras when
/// the host has a built-in front camera (the layout's rule), so the Camera row
/// offers the same. The iPad app running on a Mac is offered none: the system
/// reports the Mac's cameras (and a Continuity iPhone's) without a position, the
/// layout does not count them, and a hidden window would keep one open. The iPad
/// app running on an Apple Vision Pro is offered none either.
public enum CameraAvailability {
    /// Whether the iPad app on a Mac is offered the Mac's camera. This is the
    /// one place to turn it on once the Mac's cameras are checked.
    public static let offeredToIOSAppOnMac = false
    /// Whether the iPad app on a Vision Pro is offered a camera: the Mac's rule,
    /// and the one place to turn it on.
    public static let offeredToIOSAppOnVision = false

    /// A host that is offered no camera looks for none: `hasFrontCamera` is
    /// asked only for any other.
    public static func isAvailable(
        hasFrontCamera: @autoclosure () -> Bool, isiOSAppOnMac: Bool, isiOSAppOnVision: Bool
    ) -> Bool {
        if isiOSAppOnMac, !offeredToIOSAppOnMac { return false }
        if isiOSAppOnVision, !offeredToIOSAppOnVision { return false }
        return hasFrontCamera()
    }
}

/// What the runtime planned for the cameras a host presents: the memory policy
/// with the memory a live camera session holds (`MemoryPolicy.withCameraReserve`),
/// and whether every figure is the one the layout the runtime would run gives.
/// It is not where the runtime could not discover the cameras and answered the
/// tier's ceiling (which bounds the system cameras), as the policy's own figure
/// is before the runtime is asked.
public struct CameraReservePlan: Equatable, Sendable {
    public var policy: MemoryPolicy
    public var exact: Bool

    public init(policy: MemoryPolicy, exact: Bool) {
        self.policy = policy
        self.exact = exact
    }
}

/// What a session's cameras do, fixed when the guest starts: the guest lists
/// the same cameras either way.
public enum CameraMode: Equatable, Sendable {
    /// Every frame is black, and the host opens no capture session, so iOS's
    /// camera indicator cannot light.
    case blocked
    /// A guest app that opens a camera sees the host's.
    case live
}

/// The session's camera mode: live only while Camera is on and iOS lets
/// Andarium capture at the moment the guest starts. Off, access that iOS
/// refuses or has not been asked for, and a device without cameras are all
/// blocked.
public enum CameraSession {
    public static func mode(choice: Bool, access: CameraAccess) -> CameraMode {
        choice && access == .authorized ? .live : .blocked
    }
}

/// What the host is doing for the guest's cameras, as the runtime reports it.
public enum CameraCapture: Equatable, Sendable {
    /// No guest app has a camera open.
    case idle
    case streaming
    /// The capture cannot run for a reason that ends by itself: another app or
    /// a call has the camera.
    case interrupted
    /// The same, because other apps share the screen (iPad Split View, Slide
    /// Over).
    case interruptedByMultitasking
    /// The same, because the device is hot.
    case interruptedBySystemPressure
    /// The session is blocked and a guest app has a camera open: it gets black
    /// frames and nothing is captured. The runtime says it once for each open.
    case blockedInUse
}

/// A notice the session shows about the camera, over the guest.
public enum CameraNotice: Hashable, Sendable {
    /// An iPad shows it while other apps share the screen: the camera pauses
    /// then, and the guest gets black frames until they no longer do.
    case pausedForOtherApps
    /// A guest app opened a camera while Camera is off: it got black frames.
    /// The row decides at the start of the run and nothing changes it until
    /// the guest powers off, so the notice says to shut the guest down and
    /// turn it on.
    case cameraOff

    /// What the host's state raises, before the screens decide whether the
    /// notice can say anything true.
    public static func raised(by capture: CameraCapture) -> CameraNotice? {
        switch capture {
        case .interruptedByMultitasking: .pausedForOtherApps
        case .blockedInUse: .cameraOff
        case .idle, .streaming, .interrupted, .interruptedBySystemPressure: nil
        }
    }
}

/// The session's camera notice: what the host reports, and whether the person
/// has put the notice away.
///
/// A dismissed Camera-is-off notice stays away for the rest of the run, since
/// nothing can turn the row on before the next start, and each kind is put
/// away on its own. A dismissed paused notice stays away until its
/// interruption ends.
///
/// The guest closing its last camera does not take the Camera-is-off notice
/// away at once: it stays for `lingerFor`, so a camera that is closed and
/// opened again (a rotation, an app that retries) is one showing and not a
/// notice that leaves and comes back. After that a later open shows it again.
/// The moments come from the caller, so a test sets its own.
public struct CameraNoticeState: Equatable, Sendable {
    /// How long the Camera-is-off notice stays after the guest closed every
    /// camera.
    public static let lingerFor = Duration.seconds(2)

    public private(set) var capture = CameraCapture.idle
    private var dismissed: Set<CameraNotice> = []
    /// When the guest closed the last camera it had open while Camera was off,
    /// until `lingerFor` has passed or a camera opens again.
    private var closedAt: ContinuousClock.Instant?

    public init() {}

    /// The runtime reports what the host is doing for the guest's cameras.
    public mutating func captureChanged(_ capture: CameraCapture, at now: ContinuousClock.Instant) {
        expire(at: now)
        guard capture != self.capture else { return }
        let before = CameraNotice.raised(by: self.capture)
        let after = CameraNotice.raised(by: capture)
        self.capture = capture
        if after != .pausedForOtherApps { dismissed.remove(.pausedForOtherApps) }
        if after == .cameraOff {
            closedAt = nil
        } else if before == .cameraOff {
            closedAt = now
        }
    }

    /// Ends the linger of a notice whose camera closed `lingerFor` ago.
    public mutating func expire(at now: ContinuousClock.Instant) {
        guard let closedAt, closedAt.duration(to: now) >= Self.lingerFor else { return }
        self.closedAt = nil
    }

    /// The Camera-is-off notice is up only because the guest closed its camera
    /// a moment ago: it goes when `expire(at:)` finds `lingerFor` has passed.
    public var lingers: Bool { closedAt != nil }

    /// The person puts the notice that is up away.
    public mutating func dismiss() {
        if let notice = displayed { dismissed.insert(notice) }
    }

    /// The run ended: the next one starts without a dismissal.
    public mutating func runEnded() {
        self = CameraNoticeState()
    }

    private var displayed: CameraNotice? {
        CameraNotice.raised(by: capture) ?? (closedAt == nil ? nil : .cameraOff)
    }

    /// The notice to show now. The Camera-is-off notice tells the person to
    /// turn the row on, so it waits for a row that can be turned on: `canTurnOn`
    /// is false for a device with no camera to share and for a switch that is
    /// already on.
    public func showing(canTurnOn: Bool) -> CameraNotice? {
        guard let notice = displayed, !dismissed.contains(notice) else { return nil }
        if notice == .cameraOff, !canTurnOn { return nil }
        return notice
    }
}

/// The turn of the device body the guest is told, for its cameras. Where the
/// host owns the guest's rotation (an iPad window) the guest stays upright in
/// the window while the device, which carries every built-in sensor, turns under
/// it. The runtime derives each camera's SENSOR_ORIENTATION from this turn when
/// the guest starts (a back camera adds it, a front camera subtracts it) and
/// turns the picture by the change since, so the guest's photos and previews
/// come out upright. Where the guest owns the rotation it compensates itself
/// and the turn is zero.
public enum CameraRotation {
    /// The device body's clockwise turn from portrait, as its user sees it:
    /// 0, 90, 180 or 270.
    public static func degrees(policy: GuestRotationPolicy, interface: MotionSample.Axes) -> UInt32 {
        guard policy == .host else { return 0 }
        switch interface {
        case .portrait: return 0
        // The window's home button is on the left: the device is turned
        // clockwise a quarter.
        case .landscapeLeft: return 90
        case .portraitUpsideDown: return 180
        case .landscapeRight: return 270
        }
    }
}
