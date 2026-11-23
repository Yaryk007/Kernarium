// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The root switch on the model. The guest's root state is `Guest.root`
/// (AndariumCore); its three rules (when the Magisk app is put in, what
/// becomes of it with root off, and what a failed start does) are the
/// `Root...Rule` enums there.
extension AppModel {
    /// The switch as Home shows it: on is the person's choice, and a pause that
    /// the application put on root after a failed start does not turn it off.
    var rootOn: Bool { guest?.root.on ?? false }

    /// What Home owes the person about root, if anything.
    var rootAttention: RootState.Attention? { guest?.root.attention }

    /// The Root switch. Turning root off applies at once and applies to the
    /// next start; turning it on asks first (`rootWarningShowing`), and the
    /// switch stays off until the person confirms.
    func setRoot(_ on: Bool) {
        guard guest != nil, rootAvailable else { return }
        if on {
            guard !rootOn else { return }
            rootWarningShowing = true
        } else {
            guard rootOn || rootAttention != nil else { return }
            guest?.root.setOn(false)
            rootPeriod &+= 1
            persistNow()
        }
    }

    /// The warning's Turn On.
    func confirmRoot() {
        rootWarningShowing = false
        guard guest != nil, rootAvailable else { return }
        guest?.root.setOn(true)
        rootPeriod &+= 1
        persistNow()
    }

    /// The warning's Cancel.
    func cancelRootWarning() {
        rootWarningShowing = false
    }

    // MARK: A failed or stopped start with root

    /// A start with root ended before Android booted (`RootFallbackRule`):
    /// the switch's state follows the rule's behaviour, and the state is
    /// saved as soon as a driver is there to save it. A save that fails stays
    /// owed (`rootStateUnsaved`), and no run begins before it is paid.
    func applyRootFallback() {
        guard guest != nil else { return }
        var state = guest?.root ?? RootState()
        if RootFallbackRule.failed(&state) {
            guest?.root = state
            rootPeriod &+= 1
            if driver == nil || !persistNow() { rootStateUnsaved = true }
        } else {
            guest?.root.startRooted = false
        }
    }

    /// After the driver is attached, and before a start: saves a fallback
    /// decided at launch, before there was a driver, or whose save failed.
    func saveRootStateIfPending() {
        if rootStateUnsaved, driver != nil, persistNow() { rootStateUnsaved = false }
        keepPreviousSessionIfDecided()
        keepRunRecordIfDecided()
    }

    /// Moves the previous session's record aside for the bug report once what
    /// was decided from it is saved.
    func keepPreviousSessionIfDecided() {
        guard previousSessionHeld, !rootStateUnsaved, let diagnosticsDirectory else { return }
        PreviousSession.keep(in: diagnosticsDirectory)
        previousSessionHeld = false
    }

    /// Moves the record of the run that ended aside for the bug report once
    /// the pause decided from it is saved, so that a relaunch does not read it
    /// again and pause root once more after "Try with root again".
    func keepRunRecordIfDecided() {
        guard runRecordHeld, !rootStateUnsaved else { return }
        driver?.retireRunRecord()
        runRecordHeld = false
    }

    /// The boot the run that is under way, or the last one, used: the runtime's
    /// record of it, never the request that was made.
    private var currentRootRecord: SessionRecord? { driver?.currentSessionRecord() }

    /// The run of a start with root ended: a restart loop before Android booted
    /// is a failed start, and so is a kernel's error exit the record shows; a
    /// stop by the person before it is a stopped one. Whether the boot used
    /// the Magisk ramdisk is the record's word.
    func rootRunEnded(_ end: RunEnd) {
        guard let record = currentRootRecord, record.ramdisk == .root else { return }
        let booted = bootTracker.isReady
        if RootFallbackRule.endedBeforeBoot(end, bootCompleted: booted) || RootFallbackRule.endedBeforeBoot(record) {
            // The pause is decided from this run's record, which stays until it is saved.
            runRecordHeld = true
            previousBootRamdisk = record.ramdisk
            applyRootFallback()
            keepRunRecordIfDecided()
        } else if end.stoppedByUser, !booted {
            var state = guest?.root ?? RootState()
            RootFallbackRule.stopped(&state)
            guest?.root = state
        }
    }

    /// Android booted with the Magisk ramdisk: a stopped start that waited for
    /// the person's decision is answered by it.
    func rootBootFinished(_ kind: BootKind) {
        guard kind != .recovery, guest?.root.attention == .stoppedStart, currentRootRecord?.ramdisk == .root else { return }
        guest?.root.attention = nil
    }

    /// The paused card's "Try with root again": the pause ends and Android
    /// starts with root, through the Start button's path (`start`), with its
    /// admission and its refusals. A start that cannot begin leaves the pause
    /// ended and says what Start would say.
    func tryRootAgain() {
        guard guest != nil else { return }
        guest?.root.tryAgain()
        rootPeriod &+= 1
        persistNow()
        start(.normal)
    }

    /// The cards' close button, for the two that only inform.
    func dismissRootNotice() {
        guard let attention = rootAttention, attention != .failedStart else { return }
        guest?.root.attention = nil
        persistNow()
    }

    /// The report entry for a bug report. The ramdisk of the last boot is the
    /// record's: this run's when one has begun, the previous session's before.
    func rootReportEntry() -> BugReportArchive.Entry? {
        guard rootAvailable || rootOn, let state = guest?.root else { return nil }
        let lastBoot = currentRootRecord?.ramdisk ?? previousBootRamdisk
        return RootReport.entry(state: state, release: driver?.rootRelease, lastBoot: lastBoot)
    }
}


/// The boot screen's Magisk app step.
extension AppModel {
    /// Whether the boot under way used the Magisk ramdisk and may need the
    /// app: the runtime's record of that boot says so (a restart in place may
    /// have booted recovery, and a request may never have begun), and the
    /// release holds both of root's files.
    private var bootedRooted: Bool {
        guard rootAvailable, let driver, currentRootRecord?.ramdisk == .root else { return false }
        return driver.rootRelease != nil && driver.rootApkURL != nil
    }

    /// After Android has booted and before the boot screen ends: with root,
    /// asks the guest whether the official Magisk app is there and puts it in
    /// when it is not (`RootAppStep`). `installing` is called once, when an
    /// install begins and not before, so that the screen adds its fifth step
    /// only for a boot that installs. A failure or the hard bound is told to the
    /// person in an alert and the boot goes on with root on and no app; the
    /// next start tries again. Without root the same point removes the app only
    /// where `RootOffRule` says so.
    func runRootAppStep(kind: BootKind, installing: @escaping @MainActor () -> Void) async {
        guard kind != .recovery, let driver, let shell = driver.guestShell else { return }
        guard bootedRooted, let release = driver.rootRelease, let state = guest?.root else {
            await runRootOffStep(driver: driver, shell: shell)
            return
        }
        // The step belongs to this boot and to the period of the switch it
        // began in: a Stop, a restart or a later start moves `bootRun`, a change
        // of the switch moves `rootPeriod`, and nothing it finds after either
        // reaches the screen or the state.
        let run = bootRun
        let period = rootPeriod
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: state,
            installing: { @MainActor [weak self] in
                guard let self, self.bootRun == run, self.rootPeriod == period else { return }
                self.rootAppInstalling = true
                installing()
            },
            install: { [weak self] in
                guard let self else { throw CancellationError() }
                try await self.installRootApp()
            })
        // A result that finds either moved changes nothing: a package that is
        // in the guest and not recorded is installed over at the next start.
        guard bootRun == run, rootPeriod == period, !Task.isCancelled else { return }
        switch outcome {
        case .notNeeded:
            break
        case let .installed(learned):
            // Only the app's own bookkeeping is added to the state as it is now.
            guest?.root.adoptApp(from: learned)
            persistNow()
        case let .failed(learned):
            guest?.root.adoptApp(from: learned)
            persistNow()
            rootAppInstalling = false
            alert = AppAlert(title: RootWords.appFailedTitle, message: RootWords.appFailedMessage)
        }
    }

    /// Puts the release's Magisk app in the guest over the application's adb
    /// install path, which replaces an older copy and keeps its data.
    private func installRootApp() async throws {
        guard let driver else { throw CancellationError() }
        try await driver.installRootApp()
    }

    /// A start without root, when `RootOffRule` removes the app the person
    /// no longer has root for. The default rule keeps it, and this asks nothing.
    private func runRootOffStep(driver: any RuntimeDriver, shell: any GuestShell) async {
        guard let state = guest?.root, RootOffRule.removesApp(state: state) else { return }
        guard await RootOffRule.removeAppIfAny(shell: shell, limit: .seconds(15)) else { return }
        guest?.root.appEverHandled = false
        persistNow()
    }
}
