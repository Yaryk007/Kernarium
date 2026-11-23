// SPDX-License-Identifier: GPL-2.0-or-later

import Testing
@testable import AndariumCore

struct RulerThinningTests {
    /// The memory ruler: a long tick every gibibyte (16 positions of 64 MiB)
    /// and a short one every 256 MiB, labelled with one or two digits.
    private func memory(stops: Int, width: Double, labelWidth: Double = 15) -> RulerThinning {
        RulerThinning(
            positionWidth: width / Double(stops - 1), longEvery: 16, shortEvery: 4, labelWidth: labelWidth
        )
    }

    @Test func aRulerWithRoomIsDrawnWhole() {
        // The ruler of a 4 GiB device on a phone and on an iPad: 41 positions.
        for width in [316.0, 700.0] {
            let thinning = memory(stops: 41, width: width, labelWidth: 8)
            #expect(thinning.longEvery == 1 && thinning.labelEvery == 1 && thinning.shortEvery == 4)
            #expect((1...4).allSatisfy { thinning.labels($0) && thinning.drawsLong($0) })
        }
        // And one that reaches 9 GiB: 120 positions.
        let wide = memory(stops: 120, width: 316, labelWidth: 8)
        #expect(wide.longEvery == 1 && wide.labelEvery == 1 && wide.shortEvery == 4)
        // The processor and storage rulers: a tick at each of a few positions.
        let storage = RulerThinning(positionWidth: 22, longEvery: 4, shortEvery: 1, labelWidth: 22)
        #expect(storage.longEvery == 1 && storage.labelEvery == 1 && storage.shortEvery == 1)
        let processor = RulerThinning(positionWidth: 90, longEvery: 1, shortEvery: 1, labelWidth: 8)
        #expect(processor.longEvery == 1 && processor.labelEvery == 1 && processor.shortEvery == 1)
    }

    @Test func aCrowdedRulerKeepsRoundLabelsThatDoNotTouch() {
        // 64 GiB: 1001 positions across 316 points (a phone) and 700 (an iPad).
        let phone = memory(stops: 1001, width: 316)
        #expect(phone.labelEvery == 5)
        #expect(phone.shortEvery == nil)
        let pad = memory(stops: 1001, width: 700)
        #expect(pad.labelEvery == 5)
        #expect(pad.shortEvery == 8, "a short tick every 512 MiB has room on an iPad")
        for (thinning, width) in [(phone, 316.0), (pad, 700.0)] {
            let gap = Double(16 * thinning.labelEvery) * width / 1000
            #expect(gap >= 15 + RulerThinning.labelGap, "labels are \(gap) points apart")
            // The labelled numbers are multiples of the interval.
            let labelled = (2...64).filter { thinning.labels($0) }
            #expect(labelled.allSatisfy { $0 % thinning.labelEvery == 0 })
            #expect(labelled.count == 64 / thinning.labelEvery)
            // A labelled tick is always drawn.
            #expect(labelled.allSatisfy { thinning.drawsLong($0) })
        }
        // A narrower ruler never labels more.
        #expect(phone.labelEvery >= pad.labelEvery)
        let narrow = memory(stops: 1001, width: 200)
        #expect(narrow.labelEvery == 10 && narrow.labelEvery >= phone.labelEvery)
    }

    @Test func theLabelsGetSparserTheMoreThereAre() {
        var previous = 1
        for gib in [4, 9, 16, 32, 64, 128, 256, 512, 1024] {
            let stops = (gib * 1024 - 1536) / 64 + 1
            let thinning = memory(stops: stops, width: 316)
            #expect(thinning.labelEvery >= previous, "\(gib) GiB")
            previous = thinning.labelEvery
            // Every label has its room whatever the range.
            let gap = Double(16 * thinning.labelEvery) * 316 / Double(stops - 1)
            #expect(gap >= 15 + RulerThinning.labelGap, "\(gib) GiB: \(gap) points")
            #expect(thinning.labelEvery.isNice)
        }
        // A 1 TiB ruler draws long ticks that are not all of the gibibytes.
        let huge = memory(stops: 16_000, width: 316)
        #expect(huge.longEvery > 1)
        #expect(Double(16 * huge.longEvery) * 316 / 16_000 >= RulerThinning.tickGap)
    }

    @Test func shortTicksStayOnTheLongOnesLatticeOrGo() {
        // Room for every other short tick but not every one.
        let half = RulerThinning(positionWidth: 0.55, longEvery: 16, shortEvery: 4, labelWidth: 15)
        #expect(half.shortEvery == 8)
        // No width and no positions are drawn as the fewest.
        let none = RulerThinning(positionWidth: 0, longEvery: 0, shortEvery: 0, labelWidth: 15)
        #expect(none.shortEvery == nil)
        #expect(none.labelEvery > 1 && none.longEvery > 1)
    }

    /// VoiceOver's increment and decrement: one position on a ruler drawn
    /// whole, the next labelled gibibyte on a thinned one.
    @Test func anAccessibilityStepFollowsTheThinnedLabels() {
        // The memory ruler from 1.5 GiB: position 8 is 2 GiB, every 16 more is one.
        let stops = (1536...65536).filter { $0 % 64 == 0 }
        func gib(_ index: Int) -> Int { stops[index] / 1024 }
        func isLongTick(_ index: Int) -> Bool { stops[index] % 1024 == 0 }
        let thinned = memory(stops: stops.count, width: 316)
        func step(_ index: Int, forward: Bool, floor: Int = 0) -> Int {
            thinned.adjusted(from: index, forward: forward, floor: floor, count: stops.count) {
                isLongTick($0) && thinned.labels(gib($0))
            }
        }
        // Up from the bottom: 5, 10 ... 60 GiB, then the end of the range.
        var index = 0
        var visited: [Int] = []
        while index < stops.count - 1 {
            index = step(index, forward: true)
            visited.append(gib(index))
        }
        #expect(visited == Array(stride(from: 5, through: 60, by: 5)) + [64])
        // And down again, 60 ... 5 GiB, then the bottom.
        visited = []
        while index > 0 {
            index = step(index, forward: false)
            visited.append(gib(index))
        }
        #expect(visited == Array(stride(from: 60, through: 5, by: -5)).map { $0 } + [1])
        // From between two labelled sizes, to the next one in the direction.
        let between = stops.firstIndex(of: 24 * 1024 + 512)!
        #expect(gib(step(between, forward: true)) == 25)
        #expect(gib(step(between, forward: false)) == 20)
        // A floor stops the decrement; the ends stay where they are.
        let floor = stops.firstIndex(of: 30 * 1024)!
        #expect(step(floor + 3, forward: false, floor: floor) == floor)
        #expect(step(stops.count - 1, forward: true) == stops.count - 1)
        #expect(step(0, forward: false) == 0)
        // A ruler drawn whole moves one position, as every ruler did.
        let whole = memory(stops: 41, width: 316, labelWidth: 8)
        #expect(whole.adjusted(from: 10, forward: true, floor: 0, count: 41) { _ in false } == 11)
        #expect(whole.adjusted(from: 10, forward: false, floor: 4, count: 41) { _ in false } == 9)
        #expect(whole.adjusted(from: 4, forward: false, floor: 4, count: 41) { _ in false } == 4)
        #expect(whole.adjusted(from: 40, forward: true, floor: 0, count: 41) { _ in false } == 40)
    }

    @Test func theIntervalsAreOneTwoFiveAndTheirTens() {
        #expect([0.0, 1, 1.1, 2, 2.1, 5, 5.1, 10, 11, 21, 51, 101, 1234].map(RulerThinning.nice(atLeast:))
            == [1, 1, 2, 2, 5, 5, 10, 10, 20, 50, 100, 200, 2000])
    }
}

private extension Int {
    /// One of 1, 2, 5 and their tens.
    var isNice: Bool {
        var value = self
        while value >= 10, value % 10 == 0 { value /= 10 }
        return [1, 2, 5].contains(value)
    }
}
