// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore

extension Readout {
    /// What the views draw of a readout, which is all that decides whether a
    /// new readout needs publishing (`StatsPublishGate`): every string a view
    /// derives from it, the bars as they are drawn and the sparkline's slots.
    /// The readout's raw numbers are not part of it: most are drawn only as
    /// the text above or the bar below, and the history moves on with every
    /// reading even where its bars stay put. A view that draws something new
    /// of a readout adds it here.
    struct Drawn: Equatable {
        /// A share as its bar is drawn: how far it fills in thousandths, which
        /// moves the bar by a fraction of a pixel at most, and whether it
        /// has turned orange (`StatsTint`).
        struct Bar: Equatable {
            static let steps = 1000

            var thousandths: Int
            var warns: Bool

            init(share: Double) {
                thousandths = Int((min(max(share, 0), 1) * Double(Self.steps)).rounded())
                warns = share >= StatsTint.warningShare
            }
        }

        var texts: [String]
        var memoryBar: Bar?
        var vramBar: Bar?
        var sparkline: [Int?]
    }

    /// The form the capsule and Settings' rows draw this readout in, for a
    /// guest of `memoryMiB`.
    func drawn(memoryMiB: Int) -> Drawn {
        Drawn(
            texts: [
                cpuText, fpsText, fpsNumber, memoryUsedText, memoryText(of: memoryMiB), vramUsedText, vramText,
                spoken(memoryMiB: memoryMiB, recovery: false), spoken(memoryMiB: memoryMiB, recovery: true),
            ],
            memoryBar: memoryFraction(of: memoryMiB).map(Drawn.Bar.init(share:)),
            vramBar: vramFraction.map(Drawn.Bar.init(share:)),
            sparkline: Sparkline.slotValues(history, slots: StatsMeter.historyLength)
        )
    }
}
