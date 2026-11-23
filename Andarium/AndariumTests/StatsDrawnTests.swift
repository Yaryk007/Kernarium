// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Testing
@testable import Andarium

/// What a readout draws, which decides whether a reading reaches the views:
/// whatever the capsule or Settings' rows would write or draw differently
/// differs, and nothing else does.
struct StatsDrawnTests {
    private let mebibyte = Limits.bytesPerMiB

    /// A guest that has been idle for a while: figures for everything, and a
    /// full sparkline.
    private func steady() -> Readout {
        var readout = Readout()
        readout.cpu = 25
        readout.fps = 60
        readout.memoryUsedBytes = 818 * mebibyte
        readout.vramUsedBytes = 412 * mebibyte
        readout.vramBudgetBytes = 534 * mebibyte
        readout.history = Array(repeating: 25, count: StatsMeter.historyLength)
        return readout
    }

    private func drawn(_ readout: Readout, memoryMiB: Int = 2048) -> Readout.Drawn {
        readout.drawn(memoryMiB: memoryMiB)
    }

    @Test func aReadoutDrawsAsItDidWhenNothingIsDifferent() {
        #expect(drawn(steady()) == drawn(steady()))
        #expect(drawn(Readout()) == drawn(Readout()))
        #expect(drawn(Readout()) != drawn(steady()))
    }

    @Test func aHistoryThatMovedOnOverTheSameValuesDrawsTheSame() {
        var before = steady()
        before.history = [25, 25, 25, 25, 25, 25, 25, 25, 25, 25]
        var after = steady()
        after.history = Array((before.history + [25]).suffix(StatsMeter.historyLength))
        #expect(drawn(before) == drawn(after))
    }

    @Test func steadyReadingsOfAMeterDrawTheSameOnceTheHistoryIsFull() {
        var meter = StatsMeter()
        var forms: [Readout.Drawn] = []
        for step in 0...(StatsMeter.historyLength + 4) {
            // Two vCPUs, a quarter of them busy and 60 frames a second, with
            // the time the host charged and the instant it read the clock
            // wandering a little from second to second, as they do.
            let charge = UInt64(step % 3) * 1_000_000
            let jitter = UInt64(step % 2) * 100_000
            meter.record(
                StatsSample(
                    bootIndex: 0, processorCount: 2, timestampNanoseconds: UInt64(step) * 1_000_000_000 + jitter,
                    processorTimeNanoseconds: UInt64(step) * 500_000_000 + charge,
                    presentedFrames: UInt64(step) * 60, ramBytes: 2048 * mebibyte,
                    ramHeldBytes: UInt64(1800 + step) * mebibyte))
            forms.append(drawn(Readout(meter)))
        }
        let full = forms[StatsMeter.historyLength...]
        #expect(full.allSatisfy { $0 == full.first })
        #expect(forms[StatsMeter.historyLength - 1] != full.first, "the history was still filling a second before")
    }

    @Test func everyFigureTheViewsWriteIsPartOfTheForm() {
        let base = drawn(steady())
        var cpu = steady()
        cpu.cpu = 26
        var fps = steady()
        fps.fps = 59
        var memory = steady()
        memory.memoryUsedBytes = 819 * mebibyte
        var vram = steady()
        vram.vramUsedBytes = 413 * mebibyte
        var budget = steady()
        budget.vramBudgetBytes = 535 * mebibyte
        var history = steady()
        history.history[0] = 24
        var unknown = steady()
        unknown.cpu = nil
        for (name, changed) in [
            ("cpu", cpu), ("frame rate", fps), ("memory", memory), ("graphics memory", vram),
            ("graphics budget", budget), ("sparkline", history), ("an unknown figure", unknown),
        ] {
            #expect(drawn(changed) != base, "\(name)")
        }
    }

    @Test func aMemoryThatDrawsTheSameFigureAndBarDrawsTheSame() {
        var more = steady()
        more.memoryUsedBytes = 818 * mebibyte + (100 << 10)
        #expect(more.memoryUsedText == steady().memoryUsedText)
        #expect(drawn(more) == drawn(steady()))
    }

    @Test func aBarThatMovedWhileItsFigureDidNotDrawsDifferently() {
        // 1.248 and 1.255 GiB are both written 1.25 GiB, and a bar of a 4 GiB
        // guest is two thousandths longer.
        var low = steady()
        low.memoryUsedBytes = 1278 * mebibyte
        var high = steady()
        high.memoryUsedBytes = 1285 * mebibyte
        #expect(low.memoryUsedText == high.memoryUsedText)
        #expect(drawn(low, memoryMiB: 4096) != drawn(high, memoryMiB: 4096))
    }

    @Test func aBarThatTurnsOrangeDrawsDifferentlyWhateverItsLength() {
        // Just under and just over nine tenths of 2 GiB: the same figure and
        // the same thousandth, the other colour.
        var under = steady()
        under.memoryUsedBytes = 1843 * mebibyte
        var over = steady()
        over.memoryUsedBytes = 1844 * mebibyte
        #expect(under.memoryUsedText == over.memoryUsedText)
        #expect(drawn(under).memoryBar?.thousandths == drawn(over).memoryBar?.thousandths)
        #expect(drawn(under).memoryBar?.warns == false)
        #expect(drawn(over).memoryBar?.warns == true)
        #expect(drawn(under) != drawn(over))
        // The same goes for the graphics memory against its budget.
        var graphics = steady()
        graphics.vramBudgetBytes = 1000 * mebibyte
        graphics.vramUsedBytes = 899 * mebibyte + (900 << 10)
        var warned = graphics
        warned.vramUsedBytes = 900 * mebibyte
        #expect(drawn(graphics).vramBar?.warns == false)
        #expect(drawn(warned).vramBar?.warns == true)
    }

    @Test func theSizeOfTheGuestIsPartOfTheForm() {
        #expect(drawn(steady(), memoryMiB: 2048) != drawn(steady(), memoryMiB: 4096))
    }

    @Test func theBarsFollowTheShareToAThousandth() {
        #expect(Readout.Drawn.Bar(share: 0).thousandths == 0)
        #expect(Readout.Drawn.Bar(share: 0.4994).thousandths == 499)
        #expect(Readout.Drawn.Bar(share: 0.4996).thousandths == 500)
        #expect(Readout.Drawn.Bar(share: 1).thousandths == 1000)
        #expect(Readout.Drawn.Bar(share: 1.5).thousandths == 1000)
        #expect(!Readout.Drawn.Bar(share: StatsTint.warningShare - 0.0001).warns)
        #expect(Readout.Drawn.Bar(share: StatsTint.warningShare).warns)
    }
}
