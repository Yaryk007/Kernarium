// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Opens connections to the guest's adbd through the runtime: a connected
/// stream socket to the guest's vsock port, which only this process holds.
nonisolated struct VsockConnector: AdbConnector, GuestStreamOpener {
    static let adbPort: UInt32 = 5555
    /// How long one attempt waits for the guest to answer; the session asks
    /// again until its connect limit.
    static let attemptTimeoutMilliseconds: UInt32 = 3000
    /// How long a computer's connection waits for the guest: it asks again
    /// itself when this one fails.
    static let networkAttemptTimeoutMilliseconds: UInt32 = 5000

    /// The runtime's connect, which takes the port and the timeout in
    /// milliseconds; a test names a recording one.
    var connectVsock: @Sendable (UInt32, UInt32) -> Int32 = { port, timeout in
        kernarium_runtime_loader_connect_vsock(port, timeout)
    }

    init() {}

    init(connect: @escaping @Sendable (UInt32, UInt32) -> Int32) {
        connectVsock = connect
    }

    func connect() throws -> any AdbTransport {
        DescriptorTransport(descriptor: try open(timeout: Self.attemptTimeoutMilliseconds))
    }

    /// A stream for a computer on the network; the bridge owns it.
    func openStream() throws -> Int32 {
        try open(timeout: Self.networkAttemptTimeoutMilliseconds)
    }

    private func open(timeout: UInt32) throws -> Int32 {
        let result = connectVsock(Self.adbPort, timeout)
        if result >= 0 { return result }
        switch -result {
        case ECONNREFUSED: throw AdbError.refused
        case ETIMEDOUT: throw AdbError.notAnswering
        case ENODEV, ECONNRESET: throw AdbError.guestGone
        default: throw AdbError.io(-result)
        }
    }
}

/// Runs an adb operation on a thread of its own, behind `async`: the I/O
/// blocks, and a task cancelling ends the connection, which wakes the thread.
nonisolated enum AdbOperationRunner {
    static func run<Output: Sendable>(
        identity: AdbKeyStore.Identity?, clock: GuestRunClock, cancellable: Bool = true,
        connector: any AdbConnector = VsockConnector(), connectLimit: Duration = AdbSession.defaultConnectLimit,
        onCancel: @escaping @Sendable () -> Void = {},
        _ body: @escaping @Sendable (AdbSession) throws -> Output
    ) async throws -> Output {
        let session = AdbSession(
            connector: connector, signer: identity?.signer, clock: clock, connectLimit: connectLimit)
        let operation: () async throws -> Output = {
            try await withCheckedThrowingContinuation { continuation in
                let thread = Thread {
                    defer { session.watchdog.stop() }
                    do {
                        continuation.resume(returning: try body(session))
                    } catch {
                        continuation.resume(throwing: error)
                    }
                }
                thread.name = "andarium-adb"
                thread.qualityOfService = .userInitiated
                thread.start()
            }
        }
        guard cancellable else { return try await operation() }
        return try await withTaskCancellationHandler(operation: operation) {
            session.cancel()
            onCancel()
        }
    }

    /// An operation's error in the form the model shows: a cancel is a
    /// `CancellationError`, an adb failure has its words, and anything else
    /// (no room for the report) says what it says.
    static func presented(_ error: Error, activity: AdbActivity) -> Error {
        if error is CancellationError { return error }
        if let cocoa = error as? CocoaError, cocoa.code == .userCancelled { return CancellationError() }
        guard let adb = error as? AdbError else { return error }
        return adb == .cancelled ? CancellationError() : AdbFailure(adb, activity: activity)
    }
}

/// Runs the short commands the graphics reclaim sends, each on a connection
/// of its own.
nonisolated struct AdbGuestShell: GuestShell {
    var operations: AdbGuestOperations

    func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
        let result = try await operations.shell(command, limit: limit)
        return GuestShellOutput(text: result.text, exitCode: result.exitCode)
    }
}

/// The image's `kernarium_apps` tool prints binary pictures: its answers are
/// read as the bytes the shell sent.
extension AdbGuestShell: GuestBinaryShell {
    func runBinary(_ command: String, limit: Duration) async throws -> ShellResult {
        try await operations.shell(command, limit: limit)
    }
}

/// What a bug report has left in the guest, shared between the operation's
/// thread and the cleanup that follows a cancel.
nonisolated final class BugReportTrail: @unchecked Sendable {
    private let lock = NSLock()
    private var remote: String?

    /// The report's path in the guest, once Android has said it.
    var remotePath: String? {
        get { lock.withLock { remote } }
        set { lock.withLock { remote = newValue } }
    }
}

nonisolated enum BugReportOperation {
    /// Android's own collecting is nine tenths of the bar, the copy the rest.
    static let generatingShare = 0.9
    /// Silence from `bugreportz` for this long, in the guest's running time,
    /// ends the report; so does the whole taking this long.
    static let idleLimit = Duration.seconds(5 * 60)
    static let overallLimit = Duration.seconds(45 * 60)
    static let pullIdleLimit = Duration.seconds(60)
    static let cleanupLimit = Duration.seconds(15)

    /// Makes the report and returns the staged file. The staged partial file
    /// goes on any failure; the guest's copy goes after a good pull.
    static func run(
        session: AdbSession, device: String, trail: BugReportTrail,
        update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws -> URL {
        let watchdog = session.watchdog
        var sink: StagedFileSink?
        do {
            return try session.run { connection in
                watchdog.arm(idle: idleLimit, overall: overallLimit)
                let path = try BugReport.generate(
                    on: connection,
                    progress: { update(BusyUpdate(progress: $0 * generatingShare)) },
                    alive: { watchdog.touch() }
                )
                trail.remotePath = path
                update(BusyUpdate(progress: generatingShare, detail: String(localized: "Copying the report\u{2026}")))
                watchdog.arm(idle: pullIdleLimit)
                var hooks = BugReport.Hooks()
                hooks.alive = { watchdog.touch() }
                hooks.stage = { remote, size in
                    let made = try BugReportStaging.makeSink(remotePath: remote, size: size, device: device)
                    sink = made
                    return made
                }
                hooks.pulling = pullingProgress(update: update).report
                let pulled = try BugReport.pull(on: connection, remotePath: path, hooks: hooks)
                sink?.close()
                // The guest's copy and any an earlier run left: best effort.
                watchdog.arm(idle: cleanupLimit)
                _ = try? connection.runShell(BugReport.cleanupCommand(afterPulling: pulled.remotePath))
                trail.remotePath = nil
                guard let url = sink?.url else { throw AdbError.protocolViolation("no report was staged") }
                return url
            }
        } catch {
            sink?.close()
            if let url = sink?.url { try? FileManager.default.removeItem(at: url) }
            throw error
        }
    }

    /// The pull's share of the bar, passed on no more often than the screen
    /// needs it (`ProgressThrottle`): the pull reports every 64 KiB.
    static func pullingProgress(update: @escaping @Sendable (BusyUpdate) -> Void) -> ProgressThrottle {
        ProgressThrottle { received, total in
            let fraction = total == 0 ? 1 : Double(received) / Double(total)
            update(BusyUpdate(progress: generatingShare + (1 - generatingShare) * fraction))
        }
    }

    /// Whether the guest's dump may still run when the operation ended with
    /// `error`: it does when only the stream ended (a cancel, a limit, the
    /// service restarting), and a second report would be refused meanwhile.
    static func dumpMayGoOn(after error: Error) -> Bool {
        if error is CancellationError { return true }
        switch error as? AdbError {
        case .cancelled, .timedOut, .guestGone, .streamClosed: return true
        default: return false
        }
    }

    /// After a cancel or a failure: ends a dump still running and removes
    /// what it left, on a connection of its own, within a bound. The report's
    /// stream is already closed, which is the fallback if this cannot run.
    static func stopInGuest(session: AdbSession, trail: BugReportTrail) {
        _ = try? session.run { connection in
            session.watchdog.arm(idle: cleanupLimit, overall: cleanupLimit)
            if let path = trail.remotePath {
                _ = try? connection.runShell(BugReport.cleanupCommand(afterPulling: path))
            } else {
                _ = try? connection.runShell(BugReport.stopCommand)
            }
        }
    }
}

/// The operations the screens ask of Android over adb, with the key this boot
/// carries and the clock of the guest's running time. The application builds
/// one for each call; the hosted tests build their own and drive a real guest
/// through the same code.
nonisolated struct AdbGuestOperations: Sendable {
    var identity: AdbKeyStore.Identity?
    var clock: GuestRunClock
    /// "iPad", "iPhone" or "Mac", for the words of a failure.
    var device: String
    var connector: any AdbConnector = VsockConnector()
    /// The device's free space now, which an install or an import reads after
    /// the file is in hand to see that the device has room for what it grows
    /// the guest's data disk by (`TransferRoom`).
    var hostFree: @Sendable () -> UInt64 = { HostStorage.freeBytes() }
    /// The guest run these operations belong to. Its mark is in the name of
    /// every partial file an import of this run writes, which is how the sweep
    /// of the guest's storage leaves them alone (`sweepPartialFiles`). A caller
    /// that is not the run itself gives the run's mark (`LiveRuntime.runMark`)
    /// or one of its own that no sweep of the run's guest could take for it.
    var run: GuestRunMark

    /// Asks Android for a bug report and stages it for the share sheet.
    /// Cancelling the task stops the dump in the guest and removes what it
    /// left, then throws `CancellationError`.
    func createBugReport(update: @escaping @Sendable (BusyUpdate) -> Void) async throws -> URL {
        guard let identity else { throw AdbFailure(.keyRejected, activity: .bugReport) }
        let trail = BugReportTrail()
        let device = device
        do {
            let file = try await AdbOperationRunner.run(identity: identity, clock: clock, connector: connector) { session in
                try BugReportOperation.run(session: session, device: device, trail: trail, update: update)
            }
            if Task.isCancelled {
                try? FileManager.default.removeItem(at: file)
                throw CancellationError()
            }
            return file
        } catch {
            let shown = AdbOperationRunner.presented(error, activity: .bugReport)
            // The guest's dump goes on by itself when only the stream ended.
            if BugReportOperation.dumpMayGoOn(after: error) {
                await stopBugReport(identity: identity, trail: trail)
            }
            throw shown
        }
    }

    /// The guest kernel's counters that bear on a stall (`GuestProcFiles`),
    /// read within `GuestProcFiles.timeBound`, connecting included. The entry
    /// says why when they could not be read.
    func guestProcFiles() async -> [BugReportArchive.Entry] {
        guard let identity else { return [GuestProcFiles.entry(failedBecause: "no adb key is held")] }
        do {
            let result = try await AdbOperationRunner.run(
                identity: identity, clock: clock, connector: connector, connectLimit: GuestProcFiles.connectLimit
            ) { session in
                try session.run { connection in
                    session.watchdog.arm(idle: GuestProcFiles.commandLimit, overall: GuestProcFiles.commandLimit)
                    return try connection.runShell(GuestProcFiles.command)
                }
            }
            return [GuestProcFiles.entry(output: result.text)]
        } catch {
            return [GuestProcFiles.entry(failedBecause: "adb said \(error)")]
        }
    }

    /// Stops a dump the guest may still run and removes what it left, on a
    /// connection of its own that a cancelled task cannot end.
    private func stopBugReport(identity: AdbKeyStore.Identity, trail: BugReportTrail) async {
        _ = try? await AdbOperationRunner.run(
            identity: identity, clock: clock, cancellable: false, connector: connector
        ) { session in
            BugReportOperation.stopInGuest(session: session, trail: trail)
        }
    }

    /// Whether the guest's USB debugging setting is on; nil when it cannot be
    /// read now (the guest is starting, or stopped). Each call opens a
    /// connection, which wakes a guest that suspended itself.
    func usbDebugging() async -> Bool? {
        guard let result = try? await shell(UsbDebugging.command, limit: .seconds(15)) else { return nil }
        return UsbDebugging.parse(result)
    }

    /// Whether the guest's internal storage is mounted and made (`InternalStorage`);
    /// nil when it cannot be read now.
    func storageReady() async -> Bool? {
        guard let result = try? await shell(InternalStorage.command, limit: .seconds(15)) else { return nil }
        return InternalStorage.parse(result)
    }

    /// Runs a command in the guest's shell and returns what it printed.
    func shell(_ command: String, limit: Duration = .seconds(60)) async throws -> ShellResult {
        guard let identity else { throw AdbFailure(.keyRejected, activity: .bugReport) }
        return try await AdbOperationRunner.run(identity: identity, clock: clock, connector: connector) { session in
            try session.run { connection in
                session.watchdog.arm(idle: limit, overall: limit)
                return try connection.runShell(command)
            }
        }
    }

    /// Installs the app at `url` (readable by the caller): an .apk, or a
    /// file that holds an app in parts (.apks, .xapk, .apkm). Cancellable
    /// while the file is being sent; a stop or a failure before the commit
    /// abandons the install session the parts were written into. The file's
    /// size is read when it is in hand.
    /// `bypassLowTargetSdkBlock` sends the package manager's flag that lifts its
    /// refusal of an app built for a much older Android; only Settings' Install
    /// APK sets it, once, after the person's answer (`LegacyInstallFlow`). An
    /// app refused for that reason without it throws `LegacyApkRefused`.
    ///
    /// An .xapk's OBB files are copied after the commit. The app is installed
    /// by then, so whatever stops that copy (the Cancel button, a failure, the
    /// connection) ends the call with `installedWithoutData` and not with an
    /// error; the partial file it was writing is removed. `partials` is told of
    /// the copy as of an import (`PartialSweepLedger`).
    @discardableResult
    func installApk(
        at url: URL, bypassLowTargetSdkBlock: Bool = false, partials: PartialSweepLedger? = nil,
        update: @escaping @Sendable (BusyUpdate) -> Void
    ) async throws -> ApkInstallOutcome {
        guard let identity else { throw AdbFailure(.keyRejected, activity: .installApp) }
        let coordinator = FileCoordinatorBox()
        let device = device
        let hostFree = hostFree
        let trail = ApkInstallTrail()
        let step = ApkDataStep(run: run, partials: partials)
        let outcome: ApkInstallOutcome
        do {
            outcome = try await AdbOperationRunner.run(
                identity: identity, clock: clock, connector: connector, onCancel: { coordinator.cancel() }
            ) { session in
                try ApkInstallOperation.run(
                    session: session, coordinator: coordinator, url: url, device: device,
                    bypassLowTargetSdkBlock: bypassLowTargetSdkBlock, hostFree: hostFree, trail: trail, data: step,
                    update: update)
            }
        } catch {
            await finishInstall(identity: identity, trail: trail, step: step)
            // Once the commit has answered the app is installed; what ended
            // the run after that ended the copying of its data.
            if trail.committed { return .installedWithoutData }
            throw AdbOperationRunner.presented(error, activity: .installApp)
        }
        await finishInstall(identity: identity, trail: trail, step: step)
        return outcome
    }

    /// Installs one APK from the file `open` opens, on the thread of the
    /// operation, so that the file that is checked is the file that is sent.
    /// For the release's own Magisk app (`RootRelease.openApk`); cancellable
    /// while it is sent, and nothing of it needs undoing in the guest.
    func installReleaseApk(
        name: String, open: @escaping @Sendable () throws -> FileChunkReader,
        update: @escaping @Sendable (BusyUpdate) -> Void
    ) async throws {
        guard let identity else { throw AdbFailure(.keyRejected, activity: .installApp) }
        let device = device
        let hostFree = hostFree
        do {
            try await AdbOperationRunner.run(identity: identity, clock: clock, connector: connector) { session in
                try ApkInstallOperation.sendPlain(
                    session: session, reader: open(), name: name, device: device, bypassLowTargetSdkBlock: false,
                    hostFree: hostFree, update: update)
            }
        } catch {
            throw AdbOperationRunner.presented(error, activity: .installApp)
        }
    }

    /// Undoes what an install left in the guest, on connections of their own
    /// that a cancelled task cannot end: the session that was not committed,
    /// and the partial file of a data copy that did not finish. The ledger is
    /// told the copy ended when its file is in place or known to be gone.
    private func finishInstall(identity: AdbKeyStore.Identity, trail: ApkInstallTrail, step: ApkDataStep) async {
        await abandonInstallSession(identity: identity, trail: trail)
        if let temporary = trail.partialFile {
            let removed = try? await AdbOperationRunner.run(
                identity: identity, clock: clock, cancellable: false, connector: connector
            ) { session in
                try session.run { connection in
                    session.watchdog.arm(idle: .seconds(15), overall: .seconds(30))
                    return try connection.runShell(GuestFiles.removeCommand(for: temporary))
                }
            }
            if removed?.exitCode == 0 { trail.partialFile = nil }
        }
        if trail.importOpen {
            trail.importOpen = false
            step.partials?.importEnded(mark: step.run, clean: trail.partialFile == nil)
        }
    }

    /// Deletes the install session a failed or stopped install left open in
    /// the guest, on a connection of its own that a cancelled task cannot
    /// end. A session that was committed is gone already and is not touched.
    private func abandonInstallSession(identity: AdbKeyStore.Identity, trail: ApkInstallTrail) async {
        guard let id = trail.openSession else { return }
        trail.openSession = nil
        _ = try? await AdbOperationRunner.run(
            identity: identity, clock: clock, cancellable: false, connector: connector
        ) { session in
            try session.run { connection in
                session.watchdog.arm(idle: .seconds(15), overall: .seconds(30))
                let stream = try connection.open(PackageInstallCommand.abandonSession(id))
                _ = try stream.readToEnd(limit: PackageInstallCommand.replyBudgetBytes)
            }
        }
    }
}
