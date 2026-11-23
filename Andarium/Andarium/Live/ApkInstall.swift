// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Lets a cancel reach a coordinated read that is waiting for iCloud.
nonisolated final class FileCoordinatorBox: @unchecked Sendable {
    private let lock = NSLock()
    private let coordinator = NSFileCoordinator()

    var current: NSFileCoordinator { coordinator }

    func cancel() {
        lock.withLock { coordinator.cancel() }
    }
}

/// Installs one APK through the package manager's `abb_exec` service: the
/// file goes over in 256 KiB reads (`FileChunkReader`, so no piece outlives
/// its iteration), never whole in memory, and the output the stream ends with
/// is `Success` or `Failure [CODE: text]`.
nonisolated enum ApkInstallOperation {
    static let chunkBytes = 256 * 1024
    /// Sending the file is eight tenths of the bar; Android's own install,
    /// whose length is not known, holds there.
    static let sendingShare = 0.8
    static let sendIdleLimit = Duration.seconds(60)
    static let commitLimit = Duration.seconds(15 * 60)
    /// Reading an archive's directory and manifests is quick; a file that
    /// takes this long, in the guest's running time, is not one.
    static let inspectLimit = Duration.seconds(5 * 60)

    /// Installs the file at `url`. What the install needs to know of the file
    /// (its size, and with it the room it takes) is read once the coordinated
    /// read has the file, since an iCloud file has no size, and perhaps no
    /// room, until it is down. `device` is "iPad", "iPhone" or "Mac" and
    /// `hostFree` the device's free space, read after the file is in hand.
    /// An .apk is streamed whole as it always was; the other names the picker
    /// offers (.apks, .xapk, .apkm) are archives of an app's parts, installed
    /// through a session (`ContainerInstallOperation`). `trail` is told what
    /// a failure or a cancel must undo in the guest.
    @discardableResult
    static func run(
        session: AdbSession, coordinator: FileCoordinatorBox, url: URL, device: String,
        bypassLowTargetSdkBlock: Bool = false, hostFree: @Sendable () -> UInt64,
        trail: ApkInstallTrail = ApkInstallTrail(), data: ApkDataStep = ApkDataStep(),
        update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws -> ApkInstallOutcome {
        var result: Result<ApkInstallOutcome, Error> = .success(.installed)
        var coordinationError: NSError?
        // The read is coordinated, which brings an iCloud file down first.
        update(BusyUpdate(detail: String(localized: "Getting the file\u{2026}")))
        coordinator.current.coordinate(readingItemAt: url, options: [], error: &coordinationError) { readable in
            result = Result {
                if InstallRefusal.isPlainApk(fileName: url.lastPathComponent) {
                    try sendPlain(
                        session: session, reader: FileChunkReader(contentsOf: readable, capacity: chunkBytes),
                        name: url.lastPathComponent, device: device,
                        bypassLowTargetSdkBlock: bypassLowTargetSdkBlock, hostFree: hostFree, update: update)
                    return .installed
                }
                return try ContainerInstallOperation.run(
                    session: session, file: readable, name: url.lastPathComponent, device: device,
                    bypassLowTargetSdkBlock: bypassLowTargetSdkBlock, hostFree: hostFree, trail: trail, data: data, update: update)
            }
        }
        if let coordinationError { throw coordinationError }
        return try result.get()
    }

    /// One APK, sent whole with the package manager's `install`, from the
    /// file `reader` has open (its size is that file's).
    static func sendPlain(
        session: AdbSession, reader: FileChunkReader, name: String, device: String, bypassLowTargetSdkBlock: Bool,
        hostFree: @Sendable () -> UInt64, update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws {
        let bytes = reader.size
        guard bytes > 0 else { throw AdbFailure(message: PackageInstallMessages.message(forCode: "FAILED_INVALID_APK")) }
        update(BusyUpdate(detail: "\(name) \u{00B7} \(Formats.size(bytes: bytes))"))
        // Nothing is streamed that Android cannot take or the device cannot hold.
        if let refusal = InstallRefusal(sizeBytes: bytes) { throw AdbFailure(message: refusal.message) }
        if let shortage = InstallRoom.deviceShortage(bytes: bytes, free: hostFree()) {
            throw AdbFailure(message: shortage.message(device: device))
        }
        let size = Int(bytes)
        let watchdog = session.watchdog
        let throttle = ProgressThrottle { sent, total in
            update(BusyUpdate(progress: Double(sent) / Double(total) * sendingShare))
        }
        try session.run { connection in
            watchdog.arm(idle: sendIdleLimit)
            // A reading that cannot be had does not stop the install: Android
            // says so itself, if it must (`PackageInstallMessages`).
            if let free = try GuestFiles.freeSpace(of: InstallRoom.guestPath, on: connection),
               let shortage = InstallRoom.guestShortage(bytes: bytes, free: free) {
                throw AdbFailure(message: shortage.message(device: device))
            }
            let stream = try connection.open(
                PackageInstallCommand.service(size: size, bypassLowTargetSdkBlock: bypassLowTargetSdkBlock),
                outputLimit: PackageInstallCommand.replyBudgetBytes)
            var sent = 0
            do {
                while sent < size {
                    guard let data = try reader.read(upTo: min(chunkBytes, size - sent)), !data.isEmpty else {
                        throw AdbError.io(EIO)
                    }
                    try stream.write(data)
                    sent += data.count
                    watchdog.touch()
                    throttle.report(UInt64(sent), bytes)
                }
            } catch AdbError.streamClosed {
                // The package manager gave up early; what it printed says why.
            }
            update(BusyUpdate(
                progress: sendingShare, detail: String(localized: "The virtual machine is installing the app\u{2026}"),
                cancellable: false))
            watchdog.arm(idle: nil, overall: commitLimit)
            let text = String(decoding: try stream.readToEnd(limit: PackageInstallCommand.replyBudgetBytes), as: UTF8.self)
            // The one refusal that is asked about, when the flag was not sent:
            // Settings decides what follows (`LegacyInstallFlow`). With the
            // flag it is an ordinary failure.
            if !bypassLowTargetSdkBlock, case let .tooOldTarget(target) = PackageInstallAnswer(output: text) {
                throw LegacyApkRefused(target: target)
            }
            if let message = PackageInstallMessages.failure(inOutput: text) {
                throw AdbFailure(message: message)
            }
        }
    }
}
