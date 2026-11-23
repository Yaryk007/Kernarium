// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Darwin
import Foundation
import Testing
@testable import Andarium

/// Where a test collects the updates an operation sends the overlay.
nonisolated final class UpdateLog: @unchecked Sendable {
    private let lock = NSLock()
    private var updates: [BusyUpdate] = []

    func add(_ update: BusyUpdate) { lock.withLock { updates.append(update) } }
    var all: [BusyUpdate] { lock.withLock { updates } }
    var progress: [Double] { all.compactMap(\.progress) }
    var details: [String] { all.compactMap(\.detail) }
}

/// A file of `bytes` with no data written, which reads as zeros and costs no
/// disk.
nonisolated func makeSparseFile(bytes: Int, name: String) throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-\(name)")
    let descriptor = open(url.path, O_CREAT | O_WRONLY | O_EXCL, 0o600)
    guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    defer { close(descriptor) }
    guard ftruncate(descriptor, off_t(bytes)) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    return url
}

/// What the process holds at the points a transfer reports progress, which
/// are inside the coordinated read. The coordinator (and a thread's own pool)
/// empties an autorelease pool only when it leaves, so a piece that outlives
/// its iteration shows here, at the peak, and nowhere after. Two figures: the
/// footprint iOS holds the application to, and the bytes of live allocations,
/// which the system's compressing and paging out of idle pages (a loaded Mac
/// does both within a second) cannot hide.
nonisolated final class FootprintPeak: @unchecked Sendable {
    struct Reading {
        var footprint: Int
        var heap: Int
    }

    private let lock = NSLock()
    private var highest = Reading(footprint: 0, heap: 0)

    /// `relieving` first gives back what the allocator holds free, so memory
    /// that a test before this one released is not counted against this one.
    static func current(relieving: Bool = false) -> Reading {
        if relieving { malloc_zone_pressure_relief(nil, 0) }
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        var heap = malloc_statistics_t()
        malloc_zone_statistics(nil, &heap)
        return Reading(footprint: status == KERN_SUCCESS ? Int(info.phys_footprint) : 0, heap: Int(heap.size_in_use))
    }

    func sample() {
        let now = Self.current()
        lock.withLock { highest = Reading(footprint: max(highest.footprint, now.footprint), heap: max(highest.heap, now.heap)) }
    }

    /// The most either figure rose above `before`.
    func growth(since before: Reading) -> (footprint: Int, heap: Int) {
        lock.withLock { (highest.footprint - before.footprint, highest.heap - before.heap) }
    }
}

/// A transfer reads its file in pieces on the thread of the adb operation,
/// which has no run loop to empty an autorelease pool until it ends. A piece
/// that outlives its iteration therefore stays in the application's
/// footprint until the whole transfer is over, and a 3 GiB app takes the
/// footprint past what iOS lets the application hold. These tests stream a
/// sparse file through the operation into an adbd that keeps nothing and
/// compare the footprint's peak during the transfer with the footprint before
/// it.
@Suite(.serialized)
struct LargeTransferFootprintTests {
    static let fileBytes = 256 << 20
    /// A few chunks in flight and some bookkeeping, far below the file.
    static let allowedGrowth = 32 << 20

    @Test func anApkIsStreamedWithoutTheFileStayingInTheFootprint() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try makeSparseFile(bytes: Self.fileBytes, name: "big.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        let log = UpdateLog()
        let footprint = FootprintPeak()
        let before = FootprintPeak.current(relieving: true)
        try await AdbOperationRunner.run(identity: nil, clock: GuestRunClock(), connector: adbd) { session in
            try ApkInstallOperation.run(
                session: session, coordinator: FileCoordinatorBox(), url: url, device: "iPad", hostFree: { .max },
                update: { update in
                    footprint.sample()
                    log.add(update)
                })
        }
        #expect(adbd.installBytes == UInt64(Self.fileBytes))
        let growth = footprint.growth(since: before)
        print("LARGE_TRANSFER_FOOTPRINT install file=\(Self.fileBytes) footprint-growth=\(growth.footprint) heap-growth=\(growth.heap) updates=\(log.progress.count)")
        #expect(growth.heap < Self.allowedGrowth, "the live allocations grew by \(growth.heap >> 20) MiB")
        #expect(growth.footprint < Self.allowedGrowth, "the footprint grew by \(growth.footprint >> 20) MiB")
    }

    /// A container of more than 4 GiB (each APK in it far below that, the
    /// directory and the last entry past what 32-bit fields hold, so that the
    /// archive is a ZIP64 one) goes into a session part by part, each read
    /// from the archive as it is sent, and the footprint does not grow with it.
    @Test func aContainerOfMoreThanFourGibIsStreamedWithoutTheFileStayingInTheFootprint() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let big: UInt64 = 5 << 29
        var base = Data(count: 3 << 20)
        for index in 0..<base.count { base[index] = UInt8(truncatingIfNeeded: index &* 7 &+ index >> 9) }
        let last = Data((0..<4096).map { UInt8(truncatingIfNeeded: $0 &* 13) })
        let toc = BundleToc(variants: [
            BundleToc.Variant(apks: [
                .master("splits/base-master.apk"), .split("splits/base-xxhdpi.apk", "config.xxhdpi"),
                .split("splits/base-xxxhdpi.apk", "config.xxxhdpi"), .split("splits/base-ldrtl.apk", "config.ldrtl"),
            ])
        ])
        let url = try ContainerFixture.write([
            ContainerEntry("toc.pb", toc.build()),
            ContainerEntry("splits/base-master.apk", base),
            ContainerEntry("splits/base-xxhdpi.apk", zeros: big),
            ContainerEntry("splits/base-xxxhdpi.apk", zeros: big),
            ContainerEntry("splits/base-ldrtl.apk", last),
        ], name: "Huge.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let size = try #require(try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? UInt64)
        #expect(size > 4 << 30)
        let log = UpdateLog()
        let footprint = FootprintPeak()
        let before = FootprintPeak.current(relieving: true)
        let outcome = try await AdbOperationRunner.run(identity: nil, clock: GuestRunClock(), connector: adbd) { session in
            try ApkInstallOperation.run(
                session: session, coordinator: FileCoordinatorBox(), url: url, device: "iPad", hostFree: { .max },
                update: { update in
                    footprint.sample()
                    log.add(update)
                })
        }
        #expect(outcome == .installed)
        let total = UInt64(base.count) + 2 * big + UInt64(last.count)
        #expect(adbd.installBytes == total)
        let session = try #require(adbd.committedSessions.first)
        #expect(session.parts.map(\.bytes) == [UInt64(base.count), big, big, UInt64(last.count)])
        #expect(session.parts[1].digest == session.parts[2].digest)
        let growth = footprint.growth(since: before)
        print("LARGE_TRANSFER_FOOTPRINT container file=\(size) footprint-growth=\(growth.footprint) heap-growth=\(growth.heap) updates=\(log.progress.count)")
        #expect(growth.heap < Self.allowedGrowth, "the live allocations grew by \(growth.heap >> 20) MiB")
        #expect(growth.footprint < Self.allowedGrowth, "the footprint grew by \(growth.footprint >> 20) MiB")
    }

    @Test func aFileImportIsStreamedWithoutTheFileStayingInTheFootprint() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try makeSparseFile(bytes: Self.fileBytes, name: "big.bin")
        defer { try? FileManager.default.removeItem(at: url) }
        let pick = FilePick(url: url, name: "big.bin", size: UInt64(Self.fileBytes))
        let folder = GuestFilePath.root + "/Download"
        let temporary = GuestFiles.temporaryPath(in: folder, run: GuestRunMark.random(), token: "00000000")
        let footprint = FootprintPeak()
        let throttle = ProgressThrottle { _, _ in footprint.sample() }
        let before = FootprintPeak.current(relieving: true)
        try await AdbOperationRunner.run(identity: nil, clock: GuestRunClock(), connector: adbd) { session in
            try FileUploadOperation.run(
                session: session, coordinator: FileCoordinatorBox(), pick: pick, path: folder + "/big.bin",
                temporary: temporary, replacing: false, hostFree: { .max }, progress: throttle.report)
        }
        #expect(adbd.syncBytes == UInt64(Self.fileBytes))
        let growth = footprint.growth(since: before)
        print("LARGE_TRANSFER_FOOTPRINT import file=\(Self.fileBytes) footprint-growth=\(growth.footprint) heap-growth=\(growth.heap)")
        #expect(growth.heap < Self.allowedGrowth, "the live allocations grew by \(growth.heap >> 20) MiB")
        #expect(growth.footprint < Self.allowedGrowth, "the footprint grew by \(growth.footprint >> 20) MiB")
    }
}
