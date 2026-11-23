// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation

/// The resolutions of the guest's display a screen offers: 1.0 (one guest
/// pixel to a point), every 0.5 step that is at least a quarter below the
/// screen's native scale, then native (one guest pixel to each pixel of the
/// screen). Native 3.0 gives 1.0, 1.5, 2.0, 2.5 and 3.0; native 2.61 gives
/// 1.0, 1.5, 2.0 and 2.61, the 2.5 a step too near it being dropped; native 1.0
/// gives the one. A screen below 1.0 offers native alone, and one above it
/// always offers 1.0.
///
/// A choice is `.native` for the last of them and a factor for the rest, so
/// what is stored follows the screen the application runs on.
public struct ResolutionChoices: Equatable, Sendable {
    /// The step between the choices below native.
    public static let step = 0.5
    /// A step this close to native, or closer, is left out.
    public static let nearestToNative = 0.25
    /// The scale a person who never chose gets on a low-memory device (240
    /// dpi), or the choice nearest it.
    public static let lowMemoryDefault = 1.5
    /// The scale a person who never chose gets elsewhere (320 dpi), or the
    /// choice nearest it.
    public static let highMemoryDefault = 2.0

    /// The screen's native scale, as `UIScreen.nativeScale` says it.
    public let nativeScale: Double
    /// Every choice's factor, the least first; the last is native's.
    public let scales: [Double]

    public init(nativeScale: Double) {
        let native = nativeScale.isFinite && nativeScale > 0 ? nativeScale : 1
        self.nativeScale = native
        // A tolerance of a millionth keeps 2.0 on a 2.25 screen whichever way
        // the quarter rounds.
        let tolerance = 1e-6
        guard native > 1 + tolerance else {
            scales = [native]
            return
        }
        var scales = [1.0]
        var next = 1 + Self.step
        while next <= native - Self.nearestToNative + tolerance {
            scales.append(next)
            next += Self.step
        }
        scales.append(native)
        self.scales = scales
    }

    public var count: Int { scales.count }
    /// Whether there is anything to choose between.
    public var hasChoice: Bool { scales.count > 1 }
    public var nativeIndex: Int { scales.count - 1 }

    /// The choice at `index`: `.native` for the last, else its factor.
    public func scale(at index: Int) -> GuestDisplayScale {
        index == nativeIndex ? .native : .factor(scales[index])
    }

    /// The index of the choice nearest `scale`; the lower one where two are
    /// as near. Native is the last.
    public func index(nearest scale: GuestDisplayScale) -> Int {
        guard let wanted = scale.resolved(nativeScale: nativeScale) else { return nativeIndex }
        var best = 0
        for index in scales.indices where abs(scales[index] - wanted) < abs(scales[best] - wanted) - 1e-9 {
            best = index
        }
        return best
    }

    /// `scale` as this screen has it: itself where it is a choice, else the
    /// choice nearest it.
    public func resolve(_ scale: GuestDisplayScale) -> GuestDisplayScale {
        self.scale(at: index(nearest: scale))
    }

    /// What a choice is named for, when it is an end of the list: the end of
    /// native is "Native" and the other is "Fastest". A single choice is
    /// neither.
    public enum End: Equatable, Sendable {
        case native, fastest
    }

    public func end(at index: Int) -> End? {
        guard hasChoice else { return nil }
        if index == nativeIndex { return .native }
        return index == 0 ? .fastest : nil
    }
}

/// The size of the guest's display in points for the session that has not
/// begun, which Home states the resolution of. A session is in the same window
/// as Home, but hides the status bar, so the top of its safe area is not
/// Home's: an iPad's status bar takes 24 points that the session gives to the
/// display, and an iPhone's sensor housing keeps what it takes.
public enum SessionDisplayArea {
    /// `home` is the size Home lays a view out in, safe area excluded and
    /// the window controls' corner offset taken, as `GuestLayer` takes it;
    /// `topInset` and `bottomInset` are the safe area's, as Home sees it. A
    /// phone with a home indicator (`bottomInset` above zero) has a sensor
    /// housing, which is its top inset and stays when the bar hides; a phone
    /// with a home button has only the bar's.
    public static func size(home: CGSize, topInset: CGFloat, bottomInset: CGFloat, isPhone: Bool) -> CGSize {
        let keepsTopInset = isPhone && bottomInset > 0
        let top = max(topInset, 0)
        return CGSize(width: home.width, height: home.height + (keepsTopInset ? 0 : top))
    }
}
