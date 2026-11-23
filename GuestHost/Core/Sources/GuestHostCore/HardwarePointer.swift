// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation

/// The buttons of a mouse as the runtime takes them: one bit per button, in
/// the order of UIKit's button numbers (primary, secondary, then the others
/// by number), so a button mask from UIKit converts by dropping the bits the
/// guest has no button for.
public enum HardwareButtons {
    public static let primary: UInt32 = 1 << 0
    public static let secondary: UInt32 = 1 << 1
    public static let middle: UInt32 = 1 << 2
    public static let back: UInt32 = 1 << 3
    public static let forward: UInt32 = 1 << 4
    public static let all: UInt32 = 0x1F

    /// The bits of a UIKit button mask (`UIEvent.ButtonMask`) the guest's
    /// mouse has a button for.
    public static func normalized(uiKitMask: Int) -> UInt32 {
        UInt32(truncatingIfNeeded: uiKitMask) & all
    }
}

/// Where a pointer is over the guest's display, in the range the touchscreen
/// takes. The display is drawn as large as fits the view without changing its
/// proportions, so a point of the view may be beside it.
public struct GuestPointerMapping: Equatable, Sendable {
    /// The largest position on each axis of the range the touchscreen and the
    /// pointer take, which does not depend on the guest's display.
    public static let maximum: UInt32 = 32767

    public struct Position: Equatable, Sendable {
        public let x: UInt32
        public let y: UInt32
        /// The point is over the display, edges included. A point outside it
        /// has the position of the nearest point of the display.
        public let inside: Bool

        public init(x: UInt32, y: UInt32, inside: Bool) {
            self.x = x
            self.y = y
            self.inside = inside
        }
    }

    private let boundsSize: CGSize
    private let drawableSize: CGSize
    private let viewport: CGRect

    /// `boundsSize` is the view in points, `drawableSize` what it draws in and
    /// `contentSize` the guest's display, drawn at the largest size that fits
    /// the drawable, centred: the touchscreen's own geometry.
    public init?(boundsSize: CGSize, drawableSize: CGSize, contentSize: CGSize) {
        guard Self.isPositive(boundsSize), Self.isPositive(drawableSize), Self.isPositive(contentSize) else {
            return nil
        }
        let scale = min(drawableSize.width / contentSize.width, drawableSize.height / contentSize.height)
        let width = contentSize.width * scale
        let height = contentSize.height * scale
        self.boundsSize = boundsSize
        self.drawableSize = drawableSize
        viewport = CGRect(
            x: (drawableSize.width - width) / 2, y: (drawableSize.height - height) / 2,
            width: width, height: height)
    }

    private static func isPositive(_ size: CGSize) -> Bool {
        size.width.isFinite && size.height.isFinite && size.width > 0 && size.height > 0
    }

    /// The display in the view's points: the part of the view the pointer is
    /// over the guest on. The rest is the letterbox.
    public var displayFrame: CGRect {
        let across = boundsSize.width / drawableSize.width
        let down = boundsSize.height / drawableSize.height
        return CGRect(
            x: viewport.minX * across, y: viewport.minY * down,
            width: viewport.width * across, height: viewport.height * down)
    }

    /// The position of `point`, in the view's points; nil for a point that is
    /// not a number.
    public func position(of point: CGPoint) -> Position? {
        guard point.x.isFinite, point.y.isFinite else { return nil }
        let x = point.x * drawableSize.width / boundsSize.width
        let y = point.y * drawableSize.height / boundsSize.height
        guard x.isFinite, y.isFinite else { return nil }
        let inside = x >= viewport.minX && x <= viewport.maxX && y >= viewport.minY && y <= viewport.maxY
        return Position(
            x: Self.coordinate(x, from: viewport.minX, extent: viewport.width),
            y: Self.coordinate(y, from: viewport.minY, extent: viewport.height),
            inside: inside)
    }

    private static func coordinate(_ value: CGFloat, from origin: CGFloat, extent: CGFloat) -> UInt32 {
        let size = CGFloat(maximum) + 1
        let clamped = min(max(value, origin), origin + extent)
        let scaled = ((clamped - origin) * size / extent).rounded(.down)
        return UInt32(min(max(scaled, 0), CGFloat(maximum)))
    }
}

/// What the pointer reports to the guest: where it is, the buttons that are
/// down, and whether it is over the display at all.
public struct PointerReport: Equatable, Sendable {
    public let x: UInt32
    public let y: UInt32
    public let buttons: UInt32
    public let inRange: Bool

    public init(x: UInt32, y: UInt32, buttons: UInt32, inRange: Bool) {
        self.x = x
        self.y = y
        self.buttons = buttons
        self.inRange = inRange
    }
}

/// The pointer of the guest's mouse from what UIKit says of the system's
/// pointer: hovering over the view, and the touches of a pointer's buttons.
///
/// Hovering over the display is in range; leaving it, or the view, ends the
/// contact and Android's cursor stays where it was. A drag that began over the
/// display goes on past its edge with the position held at the edge, as a
/// desktop pointer does, and the button's release is reported wherever it
/// happens; only then does the contact end if the pointer is outside.
public struct GuestPointerTracker: Equatable, Sendable {
    public enum Phase: Equatable, Sendable {
        case began, moved, ended, cancelled
    }

    public private(set) var buttons: UInt32 = 0
    public private(set) var inRange = false
    private var lastReport: PointerReport?
    /// A touch that began beside the display is not the guest's, and neither
    /// is the rest of it.
    private var ignoringTouch = false

    public init() {}

    /// The pointer is over the view, at `position`, with no button down; nil
    /// when it has left the view.
    public mutating func hover(_ position: GuestPointerMapping.Position?) -> [PointerReport] {
        // UIKit sends touches while a button is down.
        guard buttons == 0, !ignoringTouch else { return [] }
        guard let position, position.inside else { return leave() }
        return emit(PointerReport(x: position.x, y: position.y, buttons: 0, inRange: true))
    }

    /// A touch of the pointer's buttons. `buttons` is the mask of the buttons
    /// down in the touch's event; a touch that ends or is cancelled has none.
    public mutating func touch(
        _ phase: Phase, at position: GuestPointerMapping.Position?, buttons mask: UInt32
    ) -> [PointerReport] {
        switch phase {
        case .began:
            guard let position, position.inside else {
                ignoringTouch = true
                return leave()
            }
            ignoringTouch = false
            buttons = mask & HardwareButtons.all
            return emit(PointerReport(x: position.x, y: position.y, buttons: buttons, inRange: true))
        case .moved:
            guard !ignoringTouch, let position else { return [] }
            buttons = mask & HardwareButtons.all
            return emit(PointerReport(x: position.x, y: position.y, buttons: buttons, inRange: true))
        case .ended:
            buttons = 0
            guard !ignoringTouch else {
                ignoringTouch = false
                return []
            }
            guard let position else { return leave() }
            // The pointer goes on hovering where the button came up, if that
            // is over the display; UIKit says so when it moves.
            return emit(PointerReport(x: position.x, y: position.y, buttons: 0, inRange: position.inside))
        case .cancelled:
            ignoringTouch = false
            return leave()
        }
    }

    /// The pointer is gone from the guest: the buttons come up and the contact
    /// ends where it was.
    public mutating func leave() -> [PointerReport] {
        buttons = 0
        guard let lastReport, lastReport.inRange || lastReport.buttons != 0 else { return [] }
        return emit(PointerReport(x: lastReport.x, y: lastReport.y, buttons: 0, inRange: false))
    }

    /// Whether the guest has a contact or a button of the pointer.
    public var isActive: Bool { inRange || buttons != 0 }

    private mutating func emit(_ report: PointerReport) -> [PointerReport] {
        inRange = report.inRange
        guard report != lastReport else { return [] }
        lastReport = report
        return [report]
    }
}

/// Turns what a scroll gesture reports, in points of translation, into the
/// wheel steps the mouse takes, 120 to a detent, keeping what does not make a
/// whole unit for the next report.
///
/// The translation is what UIKit scrolls content by, so a negative vertical
/// translation is a scroll down (the wheel turns toward the person, negative)
/// and a negative horizontal translation a scroll to the right (the horizontal
/// wheel's positive direction).
public struct ScrollAccumulator: Equatable, Sendable {
    public static let unitsPerDetent: Double = 120
    /// The most one report carries, so one wild translation cannot spin a
    /// guest list; the rest is dropped.
    public static let largestReport: Int32 = 120 * 20

    public let pointsPerDetent: Double
    private var vertical = 0.0
    private var horizontal = 0.0

    public init(pointsPerDetent: Double) {
        self.pointsPerDetent = pointsPerDetent > 0 ? pointsPerDetent : 1
    }

    public mutating func add(translationX: Double, translationY: Double) -> (wheel: Int32, horizontalWheel: Int32) {
        guard translationX.isFinite, translationY.isFinite else { return (0, 0) }
        vertical += translationY * Self.unitsPerDetent / pointsPerDetent
        horizontal += -translationX * Self.unitsPerDetent / pointsPerDetent
        return (Self.take(&vertical), Self.take(&horizontal))
    }

    /// A gesture began or ended: what was left over belongs to it.
    public mutating func reset() {
        vertical = 0
        horizontal = 0
    }

    private static func take(_ accumulated: inout Double) -> Int32 {
        let whole = accumulated.rounded(.towardZero)
        accumulated -= whole
        let limit = Double(largestReport)
        return Int32(min(max(whole, -limit), limit))
    }
}

/// The relative motion of a captured pointer in guest pixels, from the raw
/// deltas of the system's mouse, with the fraction of a pixel kept for the
/// next report.
public struct RelativeMotionAccumulator: Equatable, Sendable {
    private var x = 0.0
    private var y = 0.0

    public init() {}

    /// Game Controller reports a mouse's motion with a positive vertical delta
    /// for up; the guest's vertical axis grows downward.
    public mutating func add(gameControllerDeltaX dx: Double, deltaY dy: Double) -> (dx: Int32, dy: Int32) {
        guard dx.isFinite, dy.isFinite else { return (0, 0) }
        x += dx
        y -= dy
        return (Self.take(&x), Self.take(&y))
    }

    public mutating func reset() {
        x = 0
        y = 0
    }

    private static func take(_ accumulated: inout Double) -> Int32 {
        let whole = accumulated.rounded(.towardZero)
        accumulated -= whole
        return Int32(min(max(whole, Double(Int16.min)), Double(Int16.max)))
    }
}
