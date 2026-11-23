// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore

private func extent(_ offset: Int64, _ length: Int64) -> PhysicalExtent {
    PhysicalExtent(offset: offset, length: length)
}

/// The interval arithmetic that counts what several clones hold together.
struct ExtentArithmeticTests {
    @Test func extentsAreSortedAndMergedWhereTheyTouchOrOverlap() {
        let merged = SnapshotMeasure.merged([extent(100, 10), extent(0, 10), extent(10, 5), extent(105, 20), extent(50, 0)])
        #expect(merged == [extent(0, 15), extent(100, 25)])
        #expect(SnapshotMeasure.merged([]) == [])
        // One inside another changes nothing.
        #expect(SnapshotMeasure.merged([extent(0, 100), extent(10, 5)]) == [extent(0, 100)])
    }

    @Test func theBytesOfOneSetThatAnotherDoesNotCoverAreCountedOnce() {
        let held = SnapshotMeasure.merged([extent(0, 100), extent(200, 50), extent(300, 10)])
        #expect(SnapshotMeasure.bytes(of: held, notIn: []) == 160)
        #expect(SnapshotMeasure.bytes(of: held, notIn: held) == 0)
        // Covered at the start, in the middle and over the whole of an extent.
        let others = SnapshotMeasure.merged([extent(0, 10), extent(220, 10), extent(290, 40)])
        #expect(SnapshotMeasure.bytes(of: held, notIn: others) == 90 + 40 + 0)
        // One other extent spanning several held ones.
        #expect(SnapshotMeasure.bytes(of: held, notIn: [extent(50, 200)]) == 50 + 0 + 10)
    }
}

private let blockUnit: Int64 = 16384

/// The figures against known writes on a sparse file: the copy-on-write unit
/// is the 16 KiB host page, so a rewrite costs its size rounded to whole units.
@Suite(.enabled(if: temporaryVolumeClones, "needs a volume that clones files"))
struct SnapshotMeasureTests {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-core-measure-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// A 1 GiB sparse file with 8 MiB at its start, 4 MiB at 32 MiB and its last
    /// mebibyte written: 13 MiB of data.
    private func disk(in directory: URL) throws -> URL {
        let url = directory.appendingPathComponent("a.img")
        try TestDisk.make(at: url, gib: 1, marker: 1)
        try TestDisk.write(url, at: 0, count: 8 << 20, value: 2)
        try TestDisk.write(url, at: 32 << 20, count: 4 << 20, value: 3)
        return url
    }

    private func clone(_ source: URL, as name: String) throws -> URL {
        let target = source.deletingLastPathComponent().appendingPathComponent(name)
        try CloneFile.clone(source, to: target)
        return target
    }

    /// Within a few units: the file system adds its own tables and rounds to units.
    private func near(_ value: UInt64, _ expected: Int64, units: Int64 = 8, sourceLocation: SourceLocation = #_sourceLocation) {
        #expect(abs(Int64(value) - expected) <= units * blockUnit, "\(value) against \(expected)", sourceLocation: sourceLocation)
    }

    @Test func aFreshCloneSharesEverythingAndARewriteMakesBothSidesHoldTheirOwn() throws {
        let folder = try directory()
        defer { try? FileManager.default.removeItem(at: folder) }
        let live = try disk(in: folder)
        let snapshot = try clone(live, as: "snapshot.img")
        #expect(SnapshotMeasure.privateSize(of: snapshot) != nil, "the volume answers private sizes")

        near(try #require(SnapshotMeasure.privateSize(of: snapshot)), 0)
        near(try #require(SnapshotMeasure.privateSize(of: live)), 0)
        // The guest rewrites 2 MiB of what both hold.
        try TestDisk.write(live, at: 1 << 20, count: 2 << 20, value: 9)
        near(try #require(SnapshotMeasure.privateSize(of: snapshot)), 2 << 20)
        near(try #require(SnapshotMeasure.privateSize(of: live)), 2 << 20)
        #expect(SnapshotMeasure.freed(by: [snapshot], keeping: [live]).bytes == SnapshotMeasure.privateSize(of: snapshot))
        // Data it keeps: what it would take on its own.
        near(SnapshotMeasure.allocatedBytes(of: snapshot), 13 << 20, units: 64)
    }

    @Test func aSmallRewriteCostsAWholeUnit() throws {
        let folder = try directory()
        defer { try? FileManager.default.removeItem(at: folder) }
        let live = try disk(in: folder)
        let snapshot = try clone(live, as: "snapshot.img")
        // 4 KiB written into shared data: the snapshot keeps the unit it was in.
        try TestDisk.write(live, at: 3 << 20, count: 4096, value: 7)
        near(try #require(SnapshotMeasure.privateSize(of: snapshot)), blockUnit, units: 1)
    }

    @Test func anEmptyRegionWrittenAfterwardsCostsTheSnapshotNothing() throws {
        let folder = try directory()
        defer { try? FileManager.default.removeItem(at: folder) }
        let live = try disk(in: folder)
        let snapshot = try clone(live, as: "snapshot.img")
        try TestDisk.write(live, at: 48 << 20, count: 4 << 20, value: 5)
        near(try #require(SnapshotMeasure.privateSize(of: snapshot)), 0)
    }

    @Test func theExtentMapsAgreeUntilARewriteAndThenDiffer() throws {
        let folder = try directory()
        defer { try? FileManager.default.removeItem(at: folder) }
        let live = try disk(in: folder)
        let snapshot = try clone(live, as: "snapshot.img")
        let before = try #require(SnapshotMeasure.physicalExtents(of: live), "the volume answers extent maps")
        #expect(before == SnapshotMeasure.physicalExtents(of: snapshot))
        #expect(!before.isEmpty)
        try TestDisk.write(live, at: 1 << 20, count: 2 << 20, value: 9)
        let held = SnapshotMeasure.merged(try #require(SnapshotMeasure.physicalExtents(of: snapshot)))
        let others = SnapshotMeasure.merged(try #require(SnapshotMeasure.physicalExtents(of: live)))
        near(SnapshotMeasure.bytes(of: held, notIn: others), 2 << 20)
    }

    /// Two snapshots that share what the live disk gave up are the case a sum
    /// of private sizes cannot see: the blocks are private to neither.
    @Test func blocksOnlyTwoSnapshotsShareAreCountedOnceInTheirUse() throws {
        let folder = try directory()
        defer { try? FileManager.default.removeItem(at: folder) }
        let live = try disk(in: folder)
        let first = try clone(live, as: "first.img")
        let second = try clone(live, as: "second.img")
        try TestDisk.write(live, at: 1 << 20, count: 2 << 20, value: 9)

        near(try #require(SnapshotMeasure.privateSize(of: first)), 0)
        near(try #require(SnapshotMeasure.privateSize(of: second)), 0)
        let together = SnapshotMeasure.freed(by: [first, second], keeping: [live])
        #expect(!together.isLowerBound)
        near(together.bytes, 2 << 20)

        // Where the extent map is refused the sum of private sizes stands in,
        // and says it is a lower bound.
        let refused = SnapshotMeasure.Primitives(privateSize: { SnapshotMeasure.privateSize(of: $0) }, extents: { _ in nil })
        let fallback = SnapshotMeasure.freed(by: [first, second], keeping: [live], using: refused)
        #expect(fallback.isLowerBound)
        #expect(fallback.bytes <= together.bytes)

        // Deleting one raises the other's private size to what both held.
        try FileManager.default.removeItem(at: first)
        near(try #require(SnapshotMeasure.privateSize(of: second)), 2 << 20)
    }

    @Test func aFileThatCannotBeMeasuredAtAllIsZeroAsALowerBound() {
        let nothing = SnapshotMeasure.Primitives(privateSize: { _ in nil }, extents: { _ in nil })
        let url = URL(fileURLWithPath: "/nonexistent/a")
        #expect(SnapshotMeasure.freed(by: [], keeping: [url], using: nothing) == SnapshotFigure(bytes: 0))
        #expect(SnapshotMeasure.freed(by: [url], keeping: [], using: nothing) == SnapshotFigure(bytes: 0, isLowerBound: true))
        // One file takes the extents when the private size is refused.
        let mapOnly = SnapshotMeasure.Primitives(
            privateSize: { _ in nil }, extents: { $0.lastPathComponent == "held" ? [extent(0, 100), extent(500, 50)] : [extent(0, 100)] })
        #expect(SnapshotMeasure.freed(by: [URL(fileURLWithPath: "/held")], keeping: [url], using: mapOnly) == SnapshotFigure(bytes: 50))
        #expect(SnapshotMeasure.privateSize(of: url) == nil)
        #expect(SnapshotMeasure.physicalExtents(of: url) == nil)
        #expect(SnapshotMeasure.allocatedBytes(of: url) == 0)
    }

    @Test func aFileWithNoDataHasNoExtents() throws {
        let folder = try directory()
        defer { try? FileManager.default.removeItem(at: folder) }
        let url = folder.appendingPathComponent("empty.img")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        let fd = try TestDisk.descriptor(url)
        defer { close(fd) }
        #expect(ftruncate(fd, 1 << 20) == 0)
        #expect(SnapshotMeasure.physicalExtents(of: url) == [])
    }
}
