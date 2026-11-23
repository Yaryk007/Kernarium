// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How much of a ruler is drawn once it has more positions than room: the
/// memory ruler of a Mac with 64 GiB has about a thousand, each a fraction of
/// a point wide. A ruler with room for everything draws everything (the
/// result is 1 and 1 and the short ticks as asked), and one without keeps the
/// long ticks and labels whose whole numbers are multiples of 1, 2, 5, 10, 20,
/// 50 and so on, the fewest that fit, so a label is always a round number and
/// never touches the next.
public struct RulerThinning: Equatable, Sendable {
    /// The least space, in points, between two drawn ticks.
    public static let tickGap = 4.0
    /// The least space, in points, between two labels.
    public static let labelGap = 10.0

    /// Every long tick whose number is a multiple of this is drawn; 1 is all.
    public let longEvery: Int
    /// Every long tick whose number is a multiple of this is labelled.
    public let labelEvery: Int
    /// Every this many positions a short tick is drawn from the ruler's lattice
    /// of them; nil where none fits between two long ticks.
    public let shortEvery: Int?

    /// - Parameters:
    ///   - positionWidth: the points between two positions.
    ///   - longEvery: the positions between two long ticks.
    ///   - shortEvery: the positions between two short ticks, as drawn when
    ///     there is room.
    ///   - labelWidth: the points one label takes.
    public init(positionWidth: Double, longEvery: Int, shortEvery: Int, labelWidth: Double) {
        // A ruler of no width draws the fewest the steps below allow.
        let long = Double(max(longEvery, 1)) * max(positionWidth, 1e-6)
        self.longEvery = Self.nice(atLeast: Self.tickGap / long)
        self.labelEvery = Self.nice(atLeast: (labelWidth + Self.labelGap) / long)
        self.shortEvery = Self.shorts(
            positionWidth: positionWidth, longEvery: max(longEvery, 1), shortEvery: max(shortEvery, 1)
        )
    }

    /// Whether the long tick of this whole number is labelled.
    public func labels(_ number: Int) -> Bool {
        number % labelEvery == 0
    }

    /// Whether the long tick of this whole number is drawn: a labelled one
    /// always is.
    public func drawsLong(_ number: Int) -> Bool {
        number % longEvery == 0 || labels(number)
    }

    /// Where an accessibility increment (`forward`) or decrement of a ruler
    /// at `index` goes, among its `count` positions and no lower than `floor`.
    /// One position where the ruler is drawn whole. Where its labels are
    /// thinned it is the next labelled long tick in that direction
    /// (`isLabelled` says which positions are), so a thousand positions are
    /// not a thousand swipes, and the end of the ruler when there is none.
    public func adjusted(
        from index: Int, forward: Bool, floor: Int, count: Int, isLabelled: (Int) -> Bool
    ) -> Int {
        let last = count - 1
        let neighbour = forward ? min(last, index + 1) : max(floor, index - 1)
        guard labelEvery > 1 else { return neighbour }
        var position = neighbour
        while forward ? position < last : position > floor {
            if isLabelled(position) { return position }
            position += forward ? 1 : -1
        }
        return forward ? last : floor
    }

    /// The least of 1, 2, 5, 10, 20, 50, 100 ... that is at least `value`.
    static func nice(atLeast value: Double) -> Int {
        var scale = 1
        while true {
            for factor in [1, 2, 5] where Double(scale * factor) >= value {
                return scale * factor
            }
            scale *= 10
        }
    }

    /// The spacing of the short ticks: as asked where they have room;
    /// otherwise the smallest multiple of it that divides the long ticks'
    /// and has room; nil where none does.
    private static func shorts(positionWidth: Double, longEvery: Int, shortEvery: Int) -> Int? {
        func fits(_ every: Int) -> Bool { Double(every) * positionWidth >= tickGap }
        if fits(shortEvery) { return shortEvery }
        for every in stride(from: shortEvery * 2, to: longEvery, by: shortEvery)
        where longEvery % every == 0 && fits(every) {
            return every
        }
        return nil
    }
}
