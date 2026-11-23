// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// A file that has no bytes until a coordinated read of it begins, as an
/// iCloud file has none until it is down: the presenter fills the file when
/// the reader asks it to relinquish the item.
nonisolated final class FillingPresenter: NSObject, NSFilePresenter, @unchecked Sendable {
    let presentedItemURL: URL?
    let presentedItemOperationQueue = OperationQueue()
    private let bytes: Int

    init(url: URL, bytes: Int) {
        presentedItemURL = url
        self.bytes = bytes
        presentedItemOperationQueue.maxConcurrentOperationCount = 1
        super.init()
    }

    func relinquishPresentedItem(toReader reader: @escaping ((() -> Void)?) -> Void) {
        if let path = presentedItemURL?.path { truncate(path, off_t(bytes)) }
        reader(nil)
    }
}

/// What an install checks, with the file in hand and before a byte of it goes
/// to the virtual machine, against an adbd that records what it was asked.
@Suite(.serialized)
struct ApkInstallRoomTests {
    private func install(
        _ url: URL, on adbd: DiscardingAdbd, hostFree: UInt64 = .max, log: UpdateLog = UpdateLog()
    ) async throws {
        try await AdbOperationRunner.run(identity: nil, clock: GuestRunClock(), connector: adbd) { session in
            try ApkInstallOperation.run(
                session: session, coordinator: FileCoordinatorBox(), url: url, device: "iPad",
                hostFree: { hostFree }, update: log.add)
        }
    }

    private func failure(of work: () async throws -> Void) async -> String? {
        do {
            try await work()
            return nil
        } catch {
            return (error as? AdbFailure)?.message ?? "\(error)"
        }
    }

    @Test func anApkOfFourGibibytesIsRefusedBeforeAnythingConnects() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try makeSparseFile(bytes: 4 << 30, name: "huge.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        let message = await failure { try await install(url, on: adbd) }
        #expect(message == InstallRefusal.tooLarge.message)
        #expect(adbd.connectionCount == 0 && adbd.installBytes == 0)
    }

    @Test func aDeviceWithoutRoomForTheAppAndItsReserveIsToldBeforeAnythingConnects() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try makeSparseFile(bytes: 1 << 20, name: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        let free = Bytes.gib
        let message = await failure { try await install(url, on: adbd, hostFree: free) }
        #expect(message == InstallRoom.Shortage.device(needed: (1 << 20) + TransferRoom.deviceReserveBytes, free: free)
            .message(device: "iPad"))
        #expect(adbd.connectionCount == 0)
    }

    @Test func aMachineWithoutRoomForTheAppAndItsReserveIsToldBeforeTheAppIsSent() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.guestFreeKiB = 400 << 10
        let url = try makeSparseFile(bytes: 1 << 20, name: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        let message = await failure { try await install(url, on: adbd) }
        #expect(message == InstallRoom.Shortage
            .virtualMachine(needed: (1 << 20) + GuestFiles.guestReserveBytes, free: 400 << 20).message(device: "iPad"))
        #expect(message?.contains("Resize") == true)
        #expect(adbd.shellCommands == ["df -Pk '/data'"])
        #expect(adbd.servicesOpened.allSatisfy { !$0.hasPrefix("abb_exec") } && adbd.installBytes == 0)
    }

    @Test func anAppThatFitsIsSentAfterTheMachinesRoomIsAsked() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try makeSparseFile(bytes: 3 << 20, name: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        let log = UpdateLog()
        try await install(url, on: adbd, log: log)
        #expect(adbd.shellCommands == ["df -Pk '/data'"])
        #expect(adbd.servicesOpened.count == 2 && adbd.servicesOpened[1].hasPrefix("abb_exec:package\0install"))
        #expect(adbd.installBytes == 3 << 20)
        #expect(log.details.contains { $0.hasSuffix("a.apk \u{00B7} 3 MiB") })
    }

    @Test func aStorageExceptionOfCreateSessionIsTheStorageMessage() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.failsAtOnce = true
        adbd.installOutput = """
            Exception occurred while executing 'install':
            android.os.ParcelableException: java.io.IOException: Requested internal only, but not enough space
            \tat android.util.ExceptionUtils.wrap(ExceptionUtils.java:34)

            """
        let url = try makeSparseFile(bytes: 3 << 20, name: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        let message = await failure { try await install(url, on: adbd) }
        #expect(message == PackageInstallMessages.message(forCode: "FAILED_INSUFFICIENT_STORAGE"))
    }

    @Test func anEmptyFileIsNotAnApp() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try makeSparseFile(bytes: 0, name: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        let message = await failure { try await install(url, on: adbd) }
        #expect(message == PackageInstallMessages.message(forCode: "FAILED_INVALID_APK"))
        #expect(adbd.connectionCount == 0)
    }

    @Test func theSizeIsReadOnceTheCoordinatedReadHasTheFile() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        // The file says 0 bytes until the read begins, and the size the picker
        // read first is cached on the URL.
        let url = try makeSparseFile(bytes: 0, name: "cloud.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try url.resourceValues(forKeys: [.fileSizeKey]).fileSize == 0)
        let presenter = FillingPresenter(url: url, bytes: 5 << 20)
        NSFileCoordinator.addFilePresenter(presenter)
        defer { NSFileCoordinator.removeFilePresenter(presenter) }
        let log = UpdateLog()
        try await install(url, on: adbd, log: log)
        #expect(adbd.installBytes == 5 << 20)
        #expect(log.details.contains { $0.hasSuffix("cloud.apk \u{00B7} 5 MiB") })
    }
}

/// The same for a file the browser imports, whose size the picker may not
/// have known.
@Suite(.serialized)
struct FileImportRoomTests {
    private let folder = GuestFilePath.root + "/Download"

    private func upload(
        _ url: URL, declaring declared: UInt64, on adbd: DiscardingAdbd, hostFree: UInt64 = .max
    ) async throws {
        let pick = FilePick(url: url, name: "x.bin", size: declared)
        let temporary = GuestFiles.temporaryPath(in: folder, run: GuestRunMark.random(), token: "00000000")
        try await AdbOperationRunner.run(identity: nil, clock: GuestRunClock(), connector: adbd) { session in
            try FileUploadOperation.run(
                session: session, coordinator: FileCoordinatorBox(), pick: pick, path: folder + "/x.bin",
                temporary: temporary, replacing: false, hostFree: { hostFree }, progress: { _, _ in })
        }
    }

    @Test func theSizeComesFromTheFileNotFromThePick() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try makeSparseFile(bytes: 0, name: "cloud.bin")
        defer { try? FileManager.default.removeItem(at: url) }
        let presenter = FillingPresenter(url: url, bytes: 6 << 20)
        NSFileCoordinator.addFilePresenter(presenter)
        defer { NSFileCoordinator.removeFilePresenter(presenter) }
        let seen = ProgressLog()
        let pick = FilePick(url: url, name: "x.bin", size: 0)
        let temporary = GuestFiles.temporaryPath(in: folder, run: GuestRunMark.random(), token: "00000000")
        try await AdbOperationRunner.run(identity: nil, clock: GuestRunClock(), connector: adbd) { session in
            try FileUploadOperation.run(
                session: session, coordinator: FileCoordinatorBox(), pick: pick, path: folder + "/x.bin",
                temporary: temporary, replacing: false, hostFree: { .max }, progress: seen.add)
        }
        #expect(adbd.syncBytes == 6 << 20)
        // Every report carries the size the file has once it is here.
        #expect(!seen.all.isEmpty && seen.all.allSatisfy { $0[1] == 6 << 20 })
    }

    @Test func aFileLargerThanThePickSaidIsCheckedAgainstTheRoomItNeeds() async throws {
        let url = try makeSparseFile(bytes: 2 << 20, name: "big.bin")
        defer { try? FileManager.default.removeItem(at: url) }
        // The device.
        let device = DiscardingAdbd()
        defer { device.shutdown() }
        await #expect(throws: GuestFilesError.deviceFull(needed: 2 << 20, free: Bytes.gib)) {
            try await upload(url, declaring: 0, on: device, hostFree: Bytes.gib)
        }
        #expect(device.syncBytes == 0)
        // The machine.
        let machine = DiscardingAdbd()
        defer { machine.shutdown() }
        machine.guestFreeKiB = 1 << 10
        await #expect(throws: GuestFilesError.guestFull(needed: 2 << 20, free: 1 << 20)) {
            try await upload(url, declaring: 0, on: machine)
        }
        #expect(machine.syncBytes == 0 && machine.shellCommands == ["df -Pk '\(folder)'"])
        // Room for it: it goes.
        let roomy = DiscardingAdbd()
        defer { roomy.shutdown() }
        try await upload(url, declaring: 0, on: roomy)
        #expect(roomy.syncBytes == 2 << 20)
    }

    @Test func aFileThePickDescribedFullyIsNotCheckedAgain() async throws {
        let url = try makeSparseFile(bytes: 2 << 20, name: "big.bin")
        defer { try? FileManager.default.removeItem(at: url) }
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        // The import's own preparation has checked the room for the size it was told.
        adbd.guestFreeKiB = 1 << 10
        try await upload(url, declaring: 2 << 20, on: adbd, hostFree: Bytes.gib)
        #expect(adbd.syncBytes == 2 << 20 && !adbd.shellCommands.contains { $0.hasPrefix("df") })
    }
}

nonisolated final class ProgressLog: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [[UInt64]] = []
    func add(_ moved: UInt64, _ total: UInt64) { lock.withLock { items.append([moved, total]) } }
    var all: [[UInt64]] { lock.withLock { items } }
}

struct ProgressThrottleTests {
    @Test func theFirstReportAndTheLastAreDeliveredAndNothingBetweenWithinAnInterval() {
        let seen = ProgressLog()
        let throttle = ProgressThrottle(interval: .seconds(3600), deliver: seen.add)
        for moved in 1...1000 { throttle.report(UInt64(moved), 1000) }
        #expect(seen.all == [[1, 1000], [1000, 1000]])
    }

    @Test func aReportIsDeliveredInEachIntervalAndNoneIsLost() async throws {
        let seen = ProgressLog()
        let throttle = ProgressThrottle(interval: .milliseconds(50), deliver: seen.add)
        for moved in 1...20 {
            throttle.report(UInt64(moved), 40)
            try await Task.sleep(for: .milliseconds(20))
        }
        throttle.report(40, 40)
        let all = seen.all
        #expect(all.count >= 4 && all.count < 21)
        #expect(all.map { $0[0] } == all.map { $0[0] }.sorted())
        #expect(all.last == [40, 40])
    }
}
