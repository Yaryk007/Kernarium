// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct SegmentedDownloadTests {
    private let mirror = URL(string: "https://one.example/file")!

    @Test func thePiecesAreTheWholeFileOnce() async {
        let bytes = 3 * SegmentedDownload.pieceBytes + 5
        let pieces = SegmentedDownload.Pieces(bytes: bytes)
        var end: Int64 = 0
        while let piece = await pieces.next() {
            #expect(piece.lowerBound == end)
            end = piece.upperBound
        }
        #expect(end == bytes)
        await pieces.giveBack(bytes..<bytes)
        #expect(await pieces.next() == nil)
        await pieces.giveBack(7..<bytes)
        #expect(await pieces.next() == 7..<bytes)
    }

    @Test func aConnectionFarBehindTheFastestChangesToIt() async {
        let pieces = SegmentedDownload.Pieces(bytes: 1)
        // With nothing to compare to, only a trickle is left.
        #expect(await !pieces.shouldLeave(rate: 100_000))
        #expect(await pieces.shouldLeave(rate: 1000))
        await pieces.finished(rate: 1_000_000, from: mirror)
        #expect(await !pieces.shouldLeave(rate: 300_000))
        #expect(await pieces.shouldLeave(rate: 200_000))
        #expect(await pieces.mirror(after: URL(string: "https://two.example/file")) == mirror)
        // One that leaves the fastest is sent wherever the host likes.
        #expect(await pieces.mirror(after: mirror) == nil)
    }

    @Test func failuresWithNoByteBetweenThemEndTheDownload() async {
        let pieces = SegmentedDownload.Pieces(bytes: 1)
        for _ in 0..<30 { #expect(await pieces.mayRetry()) }
        #expect(await !pieces.mayRetry())
        _ = await pieces.add(1)
        #expect(await pieces.mayRetry())
    }
}
