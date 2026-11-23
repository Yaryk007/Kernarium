// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation

/// How many guest pixels the display draws for each point of the view that
/// shows it: a factor (1 is one guest pixel to a point), or `native`, one
/// guest pixel to each pixel of the screen the view is on. Native stands for
/// a number that differs between screens, so it is a symbol and never stored
/// or stated as the number.
public enum GuestDisplayScale: Equatable, Hashable, Sendable {
    case native
    case factor(Double)

    /// One guest pixel to a point.
    public static let points = GuestDisplayScale.factor(1)

    /// The factor on a screen whose native scale is `nativeScale`; nil where
    /// it is not a number above zero.
    public func resolved(nativeScale: Double) -> Double? {
        let value: Double
        switch self {
        case .native: value = nativeScale
        case .factor(let factor): value = factor
        }
        return value.isFinite && value > 0 ? value : nil
    }

    /// The largest factor a text is read as; no screen's native scale is near it.
    public static let largestStated = 8.0

    /// The scale a text states: "native" in any case, or a decimal number
    /// above zero and at most `largestStated`. Anything else is nil.
    public init?(text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.lowercased() == "native" {
            self = .native
        } else if let value = Double(trimmed), let scale = GuestDisplayScale(number: value) {
            self = scale
        } else {
            return nil
        }
    }

    /// The factor `number`, if it is one a text may state.
    public init?(number: Double) {
        guard number.isFinite, number > 0, number <= Self.largestStated else { return nil }
        self = .factor(number)
    }

    /// The text `init(text:)` reads back: "native", or the factor with no
    /// trailing zeros ("1", "1.5", "2.61").
    public var text: String {
        switch self {
        case .native: return "native"
        case .factor(let factor):
            var text = String(format: "%.4f", factor)
            while text.hasSuffix("0") { text.removeLast() }
            if text.hasSuffix(".") { text.removeLast() }
            return text
        }
    }
}

/// The stored form is "native" or the number.
extension GuestDisplayScale: Codable {
    public init(from decoder: any Decoder) throws {
        let value = try decoder.singleValueContainer()
        if let text = try? value.decode(String.self) {
            guard let scale = GuestDisplayScale(text: text) else {
                throw DecodingError.dataCorruptedError(
                    in: value, debugDescription: "\(text) is not a display scale")
            }
            self = scale
        } else {
            let number = try value.decode(Double.self)
            guard let scale = GuestDisplayScale(number: number) else {
                throw DecodingError.dataCorruptedError(
                    in: value, debugDescription: "\(number) is not a display scale")
            }
            self = scale
        }
    }

    public func encode(to encoder: any Encoder) throws {
        var value = encoder.singleValueContainer()
        switch self {
        case .native: try value.encode("native")
        case .factor(let factor): try value.encode(factor)
        }
    }
}

/// What the guest's display is at one scale on one view: its size in guest
/// pixels and the density Android is told, which is 160 dots an inch for
/// each point of scale, so the interface keeps the size it has at 1.0.
/// The host's layout of the view and anything that shows the resolution the
/// guest will get both come from here.
public struct GuestDisplayLayout: Equatable, Sendable {
    /// Android's density at a factor of 1: its baseline of 160 dots an inch.
    public static let baselineDotsPerInch = 160.0

    public let width: Int
    public let height: Int
    public let dotsPerInch: Int
    /// The factor the layout was made at.
    public let factor: Double

    /// The layout of a display `points` large on a screen whose native scale
    /// is `nativeScale`: the points times the factor, each rounded down. Nil
    /// where the scale or the size is not a number, or the display would not
    /// have a pixel.
    public init?(scale: GuestDisplayScale, points: CGSize, nativeScale: Double) {
        guard let factor = scale.resolved(nativeScale: nativeScale),
              points.width.isFinite, points.height.isFinite else { return nil }
        let width = (Double(points.width) * factor).rounded(.down)
        let height = (Double(points.height) * factor).rounded(.down)
        guard width >= 1, height >= 1, width < Double(Int32.max), height < Double(Int32.max) else { return nil }
        self.width = Int(width)
        self.height = Int(height)
        dotsPerInch = Self.dotsPerInch(factor: factor)
        self.factor = factor
    }

    /// The density at `factor`: 160 dots an inch to each point of scale.
    public static func dotsPerInch(factor: Double) -> Int {
        Int((baselineDotsPerInch * factor).rounded())
    }

    public var pixelSize: CGSize { CGSize(width: width, height: height) }

    /// The display's width in Android's density-independent pixels, which a
    /// change of scale leaves where it was (to a pixel of rounding).
    public var dpWidth: Double { Double(width) * Self.baselineDotsPerInch / Double(dotsPerInch) }
    public var dpHeight: Double { Double(height) * Self.baselineDotsPerInch / Double(dotsPerInch) }
}
