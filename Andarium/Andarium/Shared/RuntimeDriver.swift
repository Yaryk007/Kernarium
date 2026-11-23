// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What the Cancel button of a long task does. Two are equal when they are the
/// same button, so a state that only changes its words stays equal to itself.
struct BusyCancel: Equatable {
    let id = UUID()
    let action: @MainActor () -> Void

    static func == (lhs: BusyCancel, rhs: BusyCancel) -> Bool { lhs.id == rhs.id }
}

/// How an operation ended, when its overlay stays up to say so until the
/// person closes it.
struct BusyResult: Equatable {
    /// How the card presents the end: the operation did what was asked (a
    /// check), did it but not all of it (a warning, in the colour the app's
    /// cautions have), or did not do it (a failure).
    enum Style: Equatable {
        case success, warning, failure
    }

    var style: Style
    /// Closes the overlay.
    let dismiss: BusyCancel

    /// The operation did not do what was asked.
    var failed: Bool { style == .failure }

    init(style: Style, dismiss: BusyCancel) {
        self.style = style
        self.dismiss = dismiss
    }

    init(failed: Bool, dismiss: BusyCancel) {
        self.init(style: failed ? .failure : .success, dismiss: dismiss)
    }
}

/// The words an operation ends with on its card, and how the card presents them.
struct BusyWords: Equatable {
    var title: String
    var detail: String
    var style = BusyResult.Style.success
}

/// A long task the screens wait for: creating or erasing a guest, or one of
/// the guest's own operations.
struct BusyState: Equatable {
    var title: String
    var detail: String
    /// 0 to 1, or nil while the length of the task is not known.
    var progress: Double?
    /// Shows a Cancel button that does this; nil for a task that cannot stop.
    var cancel: BusyCancel?
    /// Set once the task is over: the overlay then shows how it ended and a
    /// button that closes it, in place of the progress and Cancel.
    var result: BusyResult?
}

/// What a running operation tells the overlay: any part may be left out.
nonisolated struct BusyUpdate: Sendable {
    var progress: Double?
    var detail: String?
    /// False once the task is past the point where it can stop. True again is
    /// sent only by an install that has finished installing and goes on to copy
    /// the app's data, which can be stopped.
    var cancellable: Bool?

    init(progress: Double? = nil, detail: String? = nil, cancellable: Bool? = nil) {
        self.progress = progress
        self.detail = detail
        self.cancellable = cancellable
    }
}

/// Something the person should know and acknowledge.
struct AppAlert: Identifiable, Equatable {
    let id = UUID()
    var title: String
    var message: String
}

/// Settings' Install APK asks whether to install an app Android refused for
/// the version it was built for, with the versions when Android named them.
struct LegacyInstallQuestion: Identifiable, Equatable {
    let id = UUID()
    var target: LegacyTarget
}

/// How far the system lets Andarium use the microphone.
enum MicrophoneState: Equatable {
    case notAsked, denied, granted
}

/// A question about a permission iOS has already refused, which only the
/// person can change in Settings.
enum PermissionPrompt: Identifiable, Equatable {
    case microphone
    /// The camera is refused or restricted; the words follow the access
    /// (`CameraWords.alert`).
    case camera
    /// The location is refused, restricted, off or approximate; the words
    /// follow the access (`LocationWords.alert`).
    case location

    var id: Self { self }
}

/// What the device application does behind the screens: the guest's files and
/// its run. The Simulator preview has no driver, and the model then plays the
/// same steps against mock data.
@MainActor
protocol RuntimeDriver: AnyObject, OldAppsSource, AndroidAppsSource {
    func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest
    /// Deletes the guest and everything it stored, its choices included.
    func erase(progress: @escaping @Sendable (Double) -> Void) async throws
    func save(_ guest: Guest) throws
    /// What a start would be, from the guest's files; nil when they cannot be
    /// read, which a start then reports.
    func startPlan() -> StartPlan?
    /// Whether the guest's first Android boot has not been seen to complete,
    /// which outlasts a start that ended before it: the next start is long by
    /// its nature (`SlowStart`) though its disk exists.
    func firstBootPending() -> Bool
    /// What the guest's data disk occupies now, measured from its file; the
    /// guest changes it while it runs.
    func measuredUsedBytes() -> UInt64
    /// A run is wanted. The runtime starts once the guest's display has its
    /// size, after the disk is ready.
    func begin(guest: Guest, mode: StartMode, kind: BootKind)
    /// The person stops the start under way, from the boot screen's notice
    /// for a slow start. A run that is under way has its guest asked to write
    /// its storage back (for `StopSettle.bound` at most), then is asked to
    /// stop, and ends through the runtime's result, which the model reports as
    /// the person's (`RunEnd.stoppedByUser`); a start that has not reached the
    /// runtime yet is ended here, and the model hears it as a run that ended.
    func stopGuest()
    /// Puts the data back to what the update that did not finish started
    /// from; the guest does not start. It waits for a snapshot operation under
    /// way, as a start does.
    func restoreUpdate() async throws
    /// The person resumes a guest paused for want of space. Reads the space
    /// again and wakes the guest when it is at least `RunSpace.resumeAtBytes`;
    /// returns whether it did. The guest stays paused otherwise.
    func resumeAfterSpacePause() async -> Bool
    /// The person asks to check again over a guest that could not be paused:
    /// reads the space, and returns true when there is enough for the guest
    /// to run on; otherwise pauses the guest again, and the model hears how
    /// that came out.
    func checkSpaceAfterPauseFailed() async -> Bool
    /// The person shuts down a guest paused for want of space: wakes it
    /// without watching the space again and asks Android to power off, which
    /// ends the run. Throws when Android cannot be asked; the guest runs on
    /// and the watch starts again then.
    func shutDownAfterSpacePause() async throws
    /// Says which denial of the network the notice is up for (nil: none), for
    /// the session record. The runtime keeps it; saying it again changes
    /// nothing.
    func setNetworkNotice(_ denial: NetworkDenial?)
    /// Lets the guest play its haptics on the device, or not: the Haptics
    /// switch, which applies at once.
    func setHaptics(_ on: Bool)
    /// Settings' "When hot" changed (`AppModel.heatLimitOn`): Android is told
    /// how hot the device is, or nothing, at once.
    func setHeatLimit(_ on: Bool)
    /// Settings' "Use the pointer as touch" changed: the guest has no mouse
    /// while it is on.
    func setPointerAsTouch(_ on: Bool)
    /// The scene's pointer lock was granted or released.
    func pointerLockDidChange(_ locked: Bool)
    /// The scene's geometry changed, which may include its interface
    /// orientation.
    func sceneGeometryDidChange()
    /// Asks the guest for a bug report and copies it into `BugReportStaging`.
    /// Returns the staged file; cancelling the task stops the report.
    func createBugReport(update: @escaping @Sendable (BusyUpdate) -> Void) async throws -> URL
    /// The guest kernel's counters that bear on a stall (`GuestProcFiles`), as
    /// entries for the bug report; empty when the guest cannot be read now.
    func guestStallFiles() async -> [BugReportArchive.Entry]
    /// Installs the app at `url`, which the caller can read, into the guest:
    /// an .apk, or a file that holds an app in parts (.apks, .xapk, .apkm).
    /// Its size is read once the file is in hand, since an iCloud file has
    /// none before. Cancelling the task stops it while the file is still being
    /// sent (an archive's parts go into an install session, which the stop
    /// abandons). `bypassLowTargetSdkBlock` is true only for the second
    /// attempt of Settings' Install APK, after the person chose to install an
    /// app Android refused for the version it was built for; without it that
    /// refusal throws `LegacyApkRefused`.
    func installApk(
        at url: URL, bypassLowTargetSdkBlock: Bool, update: @escaping @Sendable (BusyUpdate) -> Void
    ) async throws -> ApkInstallOutcome
    /// Whether the guest's USB debugging setting is on; nil when it cannot be
    /// read now.
    func usbDebugging() async -> Bool?
    /// Whether Android has mounted its internal storage (`InternalStorage`);
    /// nil when it cannot be read now. Android's boot ends before it does.
    func storageReady() async -> Bool?
    /// Delivers a text message to the guest as if from the network, over the
    /// service's own channel (`GuestSmsChannel`), and returns what is known of
    /// the request. The request's identifier and attempt are the message's
    /// (`GuestSms.Retries`); `GuestSms.result` says what the outcome means.
    /// Cancelling the task ends the exchange at once.
    func sendSms(_ request: GuestSmsRequest) async -> GuestSmsChannel.Outcome
    /// What the bar owes the person about a message whose result was not yet
    /// shown (`TextMessageNotice`), in this instance's directory.
    var textMessageNotice: TextMessageNotice { get }
    /// Android's screen reader in the guest; nil when the guest did not
    /// answer (a read that fails is not a state).
    func readScreenReader() async -> ScreenReaderState?
    /// Turns the screen reader on or off, keeping every other accessibility
    /// service the guest has on, and returns the state Android reports after
    /// the same command. `service` is the one to add when turning on. Throws
    /// when the command or its answer failed.
    func changeScreenReader(_ on: Bool, service: String) async throws -> ScreenReaderState
    /// Opens Android's Accessibility page on the guest's screen.
    func openAccessibilitySettings() async throws
    /// Opens the listener of ADB over the network on the device's network
    /// addresses. Throws when it cannot (`AdbError.io(EADDRINUSE)` for a
    /// port in use).
    func startAdbOverNetwork() throws
    /// Closes the listener and every computer's connection through it.
    func stopAdbOverNetwork()
    /// The device's IPv4 address on a local network, which a computer
    /// connects to; nil without one.
    func localNetworkAddress() -> String?
    /// One reading of the running guest's host-side figures, taken off the
    /// main thread; nil when no guest runs.
    func readStatistics() async -> StatsSample?
    /// How many graphics allocations the host has refused the running guest,
    /// since its display was made; nil when no guest runs. It reads one
    /// counter, never waits for the guest and may be asked while the
    /// statistics are not shown.
    func readGraphicsRefusals() async -> UInt64?
    /// The guest's graphics memory budget and who holds it: only holders
    /// quiet for `minimumIdle` and holding `minimumBytes` or more are listed.
    /// nil when no guest runs or its GPU has not started. It reads counters,
    /// never waits for the guest and may be asked while the statistics are not
    /// shown.
    func readGraphicsMemory(minimumIdle: Duration, minimumBytes: UInt64) async -> GraphicsMemoryReading?
    /// Frees graphics memory by asking Android to end the cached app that
    /// holds the most of it (`GraphicsReclaim.reclaim`), within the guest's
    /// shell; nothing is ended that the activity manager does not list under
    /// the holder's process.
    func reclaimGraphicsMemory(_ reading: GraphicsMemoryReading, using policy: GraphicsReclaim) async
        -> GraphicsReclaimResult
    /// The guest kernel's own memory figures; nil when they could not be
    /// had: the kernel does not answer the request yet, or asking could wake
    /// a guest that sleeps. The caller keeps the last figures then. It may
    /// wait up to half a second, and never on the main thread.
    func readGuestMemory() async -> GuestMemorySample?
    /// Whether a debugger that serves the JIT is attached to Andarium now.
    func jitDebuggerAttached() -> Bool
    /// Prepares the JIT's executable memory for `shape` when `wanted`, or
    /// tells an attached debugger to leave when not, and says how it came
    /// out. It takes the seconds the debugger takes and never blocks the main
    /// thread. Without a servicing debugger it asks the runtime nothing.
    func prepareJit(wanted: Bool, shape: JitShape) async -> JitAnswer
    /// The root the carried release holds (`RootRelease`), nil when it holds
    /// none: the boot screen's Magisk app step needs it.
    var rootRelease: RootRelease? { get }
    /// The release's own copy of the official Magisk app, which the step puts
    /// in the guest; nil with no root.
    var rootApkURL: URL? { get }
    /// Puts the release's own copy of the Magisk app in the guest, reading it
    /// from the file it opens without following a link (`RootRelease.openApk`).
    /// Throws when that file is not the release's regular file or the install
    /// fails. A driver with no root has none to put in.
    func installRootApp() async throws
    /// The runtime's record of the run the last `begin` asked for, as it stands:
    /// which ramdisk the boot used (`SessionRecord.ramdisk`), how far it got
    /// and how it ended. Nil until the runtime has begun that run's session:
    /// the record of an earlier run is moved aside as the run is asked for
    /// (`RunRecord`), so none is ever taken for it.
    func currentSessionRecord() -> SessionRecord?
    /// Moves the record of the run that ended aside for the bug report, once
    /// what was decided from it is saved (`RunRecord`): a launch that comes
    /// before the next run begins reads no record of a run that was settled.
    func retireRunRecord()
    /// Whether the earlier run's record kept beside the others is this
    /// process's own: a retirement moved one aside (`RunRecord`).
    var retiredAnEarlierRun: Bool { get }
    /// The guest's shell, for the short questions about the Magisk app; nil
    /// when there is none to ask.
    var guestShell: (any GuestShell)? { get }
}

extension RuntimeDriver {
    /// A driver with no image to ask lists no old app: the feature is not offered.
    func listOldApps(limit: Duration?) async -> OldAppListing { .unavailable }
    func setOldApp(_ package: String, on: Bool) async -> OldAppWrite { .failed }
    func isOldAppOpen(_ package: String) async -> Bool? { nil }
    /// A driver with no image to ask has no apps to offer and cannot open one.
    var androidAppsTool: (any AndroidAppsTool)? { nil }
    func awaitAndroidReady(homeReached: Bool) async -> AndroidAppReadiness.Outcome { .timedOut }
    /// A driver with no guest to read adds nothing.
    func guestStallFiles() async -> [BugReportArchive.Entry] { [] }
    /// A driver with no session record has nothing to tell.
    func setNetworkNotice(_ denial: NetworkDenial?) {}
    /// A driver with no thermal session tells Android nothing either way.
    func setHeatLimit(_ on: Bool) {}
    /// A driver with no release to read has no root to offer.
    var rootRelease: RootRelease? { nil }
    var rootApkURL: URL? { nil }
    func installRootApp() async throws { throw CancellationError() }
    var guestShell: (any GuestShell)? { nil }
    func currentSessionRecord() -> SessionRecord? { nil }
    func retireRunRecord() {}
    var retiredAnEarlierRun: Bool { false }
}

/// What a screen needs from its launch arguments to open a state directly.
struct ScreenHints {
    var scrollTarget: String?
    var infoPage: InfoPage?
}
