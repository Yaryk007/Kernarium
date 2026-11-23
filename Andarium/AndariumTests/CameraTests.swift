// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import AVFoundation
import Foundation
import os
import Testing
@testable import Andarium

/// iOS's camera permission as a test plays it: the access the system reports,
/// and the question it asks the person, answered when the test says.
@MainActor
final class FakeCamera: CameraAccessSource {
    var access: CameraAccess
    private(set) var asked = 0
    private var pending: [@MainActor @Sendable (CameraAccess) -> Void] = []

    init(_ access: CameraAccess = .notDetermined) { self.access = access }

    func request(_ answered: @escaping @MainActor @Sendable (CameraAccess) -> Void) {
        asked += 1
        pending.append(answered)
    }

    /// The person answers the question iOS is showing.
    func answer(_ access: CameraAccess) {
        self.access = access
        pending.removeFirst()(access)
    }
}

/// The system's source with the question counted and never put: a test that
/// plays another platform must not raise iOS's own prompt on the host it runs on.
@MainActor
final class AskCounting: CameraAccessSource {
    private let source: SystemCameraSource
    private(set) var asked = 0

    init(_ source: SystemCameraSource) { self.source = source }

    var access: CameraAccess { source.access }

    func request(_ answered: @escaping @MainActor @Sendable (CameraAccess) -> Void) { asked += 1 }
}

/// Holds the Camera-is-off notice's stay after the guest closes its camera until
/// a test ends it, and gives the model the clock a test moves by hand.
@MainActor
final class CameraNoticePace {
    private(set) var waits: [Duration] = []
    private var held: CheckedContinuation<Void, Never>?
    var now = ContinuousClock.now

    func attach(to model: AppModel) {
        model.cameraNoticeSleep = { [self] duration in await sleep(duration) }
        model.cameraNoticeClock = { [self] in now }
    }

    private func sleep(_ duration: Duration) async {
        waits.append(duration)
        await withTaskCancellationHandler {
            await withCheckedContinuation { held = $0 }
        } onCancel: {
            Task { @MainActor in self.letGo() }
        }
    }

    var isWaiting: Bool { held != nil }

    func advance(_ seconds: Int) { now = now.advanced(by: .seconds(seconds)) }

    /// The stay ends `seconds` later by the model's clock.
    func release(after seconds: Int = 2) {
        advance(seconds)
        letGo()
    }

    private func letGo() {
        let continuation = held
        held = nil
        continuation?.resume()
    }

    /// Lets the model's task run until `condition` holds, for at most five seconds.
    static func until(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while !condition() {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(1))
        }
        return true
    }
}

/// Camera asks iOS when it is turned on, shows itself off and unusable when iOS
/// refuses or the device has no camera, is read once when the guest starts
/// (live only while it is on and allowed), and keeps the person's choice
/// across launches. Nothing here opens a camera.
@MainActor
struct CameraFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private struct Setup {
        let model: AppModel
        let driver: StubDriver
        let system: FakeCamera
    }

    private func setup(_ access: CameraAccess = .notDetermined, controller: Bool = true, chosen: Bool = false) -> Setup {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        var guest = Guest.fresh(on: host)
        guest.camera = chosen
        let model = AppModel(guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        // A test never opens the system's Settings.
        model.openSettings = {}
        let fake = FakeCamera(access)
        if controller { model.camera = CameraController(model: model, source: fake) }
        return Setup(model: model, driver: driver, system: fake)
    }

    // MARK: The switch

    @Test func itIsOffForANewGuestAndTheSessionIsBlocked() {
        let s = setup(.authorized)
        #expect(s.model.guest?.camera == false)
        #expect(!s.model.cameraShownOn)
        #expect(s.model.cameraMode == .blocked)
        #expect(s.system.asked == 0)
    }

    @Test func turningItOnAsksIOSAtOnceAndTheSessionIsLiveOnceItAllows() {
        let s = setup()
        s.model.setCamera(true)
        #expect(s.system.asked == 1)
        #expect(s.model.guest?.camera == true)
        #expect(s.model.cameraShownOn)
        // Until the person has answered the session is still blocked.
        #expect(s.model.cameraMode == .blocked)
        s.system.answer(.authorized)
        #expect(s.model.cameraAccess == .authorized)
        #expect(s.model.cameraShownOn && s.model.cameraMode == .live)
        #expect(s.model.permissionPrompt == nil)
        #expect(s.driver.savedGuest?.camera == true)
    }

    @Test func aRefusalTakesTheChoiceBackAndTheSessionStaysBlocked() {
        let s = setup()
        s.model.setCamera(true)
        s.system.answer(.denied)
        #expect(s.model.cameraAccess == .denied)
        #expect(s.model.guest?.camera == false)
        #expect(!s.model.cameraShownOn && s.model.cameraMode == .blocked)
        #expect(s.driver.savedGuest?.camera == false)
    }

    @Test func iOSIsAskedOnlyOnce() {
        let s = setup()
        s.model.setCamera(true)
        s.system.answer(.authorized)
        s.model.setCamera(false)
        s.model.setCamera(true)
        #expect(s.system.asked == 1)
        #expect(s.model.cameraMode == .live)
        s.model.setCamera(false)
        #expect(s.model.cameraMode == .blocked && s.driver.savedGuest?.camera == false)
    }

    @Test func aPreviewWithoutAControllerTakesTheAccessAsGiven() {
        let s = setup(controller: false)
        // The preview states a device with a camera iOS has not been asked about.
        s.model.cameraAccess = .notDetermined
        s.model.setCamera(true)
        #expect(s.model.cameraShownOn && s.model.cameraAccess == .authorized)
    }

    // MARK: The memory a live session holds

    /// What a planner is asked and answers, in order.
    private final class Planner: Sendable {
        private let state = OSAllocatedUnfairLock(initialState: (asked: 0, answers: [Int]()))
        let exact: Bool

        init(_ answers: [Int], exact: Bool = true) {
            state.withLock { $0.answers = answers }
            self.exact = exact
        }

        var asked: Int { state.withLock { $0.asked } }

        var plan: @Sendable (MemoryPolicy) -> CameraReservePlan {
            { policy in
                let reserve = self.state.withLock { state -> Int in
                    state.asked += 1
                    return state.answers[min(state.asked, state.answers.count) - 1]
                }
                return CameraReservePlan(policy: policy.withCameraReserve { _ in reserve }, exact: self.exact)
            }
        }
    }

    /// An iPad mini 6's budget (534 MiB) with the policy's own reserve the ceiling (197 MiB).
    private let tightPolicy = MemoryPolicy(
        processorCount: 1, bound: { _ in 4096 }, defaultMemory: { _ in 2048 },
        graphicsMemory: { _, _ in 534 }, cameraReserve: { _ in 197 })

    private func pool(_ model: AppModel) -> Int? {
        model.host.memory.graphicsMemoryMiB(forMemory: 2048, vCPUs: 1, cameraLive: model.cameraMode == .live)
    }

    @Test func theRuntimeIsAskedForTheReserveWhenTheChoiceTurnsOnAndThePoolFollows() async {
        let s = setup(.authorized)
        s.model.host.memory = tightPolicy
        let planner = Planner([171])
        s.model.cameraReservePlanner = planner.plan
        #expect(pool(s.model) == 534, "off, the budget, and the runtime is not asked")
        await s.model.refreshCameraReserve().value
        #expect(planner.asked == 0)
        s.model.setCamera(true)
        await s.model.cameraReserveTask?.value
        #expect(planner.asked == 1)
        #expect(s.model.cameraMode == .live)
        #expect(pool(s.model) == 363, "534 less the layout's 171, not the ceiling's 197")
        // Off again: the budget is the figure, whatever the policy holds.
        s.model.setCamera(false)
        #expect(pool(s.model) == 534)
    }

    @Test func theFigureAPlannerAnswersIsTheOneShownWhetherOrNotItIsExact() async {
        // The answer differs from the policy's own reserve (197), so the pool can only be 329 if the plan was applied.
        let s = setup(.authorized)
        s.model.host.memory = tightPolicy
        let planner = Planner([205], exact: false)
        s.model.cameraReservePlanner = planner.plan
        s.model.setCamera(true)
        await s.model.cameraReserveTask?.value
        #expect(planner.asked == 1)
        #expect(pool(s.model) == 329, "534 less the 205 the planner answered, not the policy's own 197")
    }

    @Test func aHostThatHoldsNothingIsShownTheWholeBudgetWhileCameraIsOn() async {
        // No camera the image can describe: the reserve is 0 and exact, and an on switch costs nothing.
        let s = setup(.authorized)
        s.model.host.memory = tightPolicy
        s.model.cameraReservePlanner = Planner([0]).plan
        s.model.setCamera(true)
        await s.model.cameraReserveTask?.value
        #expect(pool(s.model) == 534)
    }

    @Test func theChoiceMadeBeforeIOSAnswersIsPlannedAtOnceAndTheAnswerChangesWhatTheSheetShows() async {
        let s = setup()
        s.model.host.memory = tightPolicy
        let planner = Planner([124])
        s.model.cameraReservePlanner = planner.plan
        s.model.setCamera(true)
        await s.model.cameraReserveTask?.value
        let askedBeforeTheAnswer = planner.asked
        #expect(askedBeforeTheAnswer >= 1, "asked at once, before the person has answered")
        // Not allowed yet: the session is blocked and holds nothing, so the sheet shows the budget.
        #expect(s.model.cameraMode == .blocked && pool(s.model) == 534)
        s.system.answer(.authorized)
        await s.model.cameraReserveTask?.value
        #expect(planner.asked > askedBeforeTheAnswer, "the answer asks again")
        #expect(s.model.cameraMode == .live && pool(s.model) == 410, "534 less 124")
        // A refusal takes the choice and the session with it: nothing is held, nothing is asked.
        let refused = setup()
        refused.model.host.memory = tightPolicy
        let refusedPlanner = Planner([124])
        refused.model.cameraReservePlanner = refusedPlanner.plan
        refused.model.setCamera(true)
        refused.system.answer(.denied)
        await refused.model.cameraReserveTask?.value
        #expect(refused.model.cameraMode == .blocked && pool(refused.model) == 534)
        #expect(refusedPlanner.asked == 0, "the choice was taken back before the question was put")
    }

    @Test func aDeniedDeviceAndOneWithoutACameraAskTheRuntimeNothing() async {
        for access in [CameraAccess.denied, .restricted, .unavailable] {
            let s = setup(access)
            s.model.host.memory = tightPolicy
            let planner = Planner([171])
            s.model.cameraReservePlanner = planner.plan
            s.model.setCamera(true)
            await s.model.cameraReserveTask?.value
            #expect(planner.asked == 0, "\(access)")
            #expect(s.model.cameraMode == .blocked && pool(s.model) == 534)
        }
    }

    @Test func theReserveIsReadAgainWhenTheAccessChangesAndAfterARun() async {
        let s = setup(.authorized, chosen: true)
        s.model.host.memory = tightPolicy
        // A run taught the store that no lens takes a RAW still: 99, where a layout that may is 171.
        let planner = Planner([171, 99, 171])
        s.model.cameraReservePlanner = planner.plan
        s.model.refreshPlan()
        await s.model.cameraReserveTask?.value
        #expect(planner.asked == 1, "the plan is computed at launch")
        #expect(pool(s.model) == 534 - 171)
        s.model.cameraAccessChanged(.authorized)
        await s.model.cameraReserveTask?.value
        #expect(planner.asked == 2, "the access changed")
        #expect(pool(s.model) == 534 - 99, "what the run learned")
        s.model.refreshPlan()
        await s.model.cameraReserveTask?.value
        #expect(planner.asked == 3, "a run ended")
        #expect(pool(s.model) == 534 - 171)
    }

    @Test func questionsAreAnsweredInOrderAndTheLastWins() async {
        let s = setup(.authorized, chosen: true)
        s.model.host.memory = tightPolicy
        let planner = Planner([99, 124, 171])
        s.model.cameraReservePlanner = planner.plan
        s.model.refreshCameraReserve()
        s.model.refreshCameraReserve()
        await s.model.refreshCameraReserve().value
        #expect(planner.asked == 3)
        #expect(pool(s.model) == 534 - 171)
    }

    // MARK: Refused, restricted, absent

    @Test func afterARefusalTheSwitchExplainsAndOffersSettings() {
        let s = setup(.denied)
        var opened = 0
        s.model.openSettings = { opened += 1 }
        s.model.setCamera(true)
        #expect(s.model.permissionPrompt == .camera)
        #expect(s.model.guest?.camera == false)
        #expect(s.system.asked == 0)
        #expect(opened == 0)
        s.model.openCameraSettings()
        #expect(opened == 1 && s.model.cameraAwaitsSettings)
    }

    @Test func allowingItInSettingsTurnsTheSwitchTheyTappedOnWhenAndariumIsActive() {
        let s = setup(.denied)
        s.model.setCamera(true)
        s.model.openCameraSettings()
        #expect(!s.model.cameraShownOn)
        s.system.access = .authorized
        s.model.camera?.refresh()
        #expect(s.model.cameraShownOn && s.model.cameraMode == .live)
        #expect(s.driver.savedGuest?.camera == true)
        #expect(!s.model.cameraAwaitsSettings)
    }

    @Test func accessAnotherAppSettingTookAwayTurnsTheSwitchOffAndBlocksTheSession() {
        let s = setup(.authorized, chosen: true)
        #expect(s.model.cameraMode == .live)
        s.system.access = .denied
        s.model.camera?.refresh()
        #expect(!s.model.cameraShownOn && s.model.guest?.camera == false)
        #expect(s.model.cameraMode == .blocked)
    }

    @Test func aRestrictedCameraIsExplainedAndNeverOffersSettings() {
        let s = setup(.restricted)
        s.model.setCamera(true)
        #expect(s.model.permissionPrompt == .camera)
        let alert = CameraWords.alert(for: .restricted)
        #expect(alert?.offersSettings == false)
        #expect(CameraWords.alert(for: .denied)?.offersSettings == true)
        #expect(CameraWords.alert(for: .authorized) == nil && CameraWords.alert(for: .notDetermined) == nil)
        #expect(CameraWords.detail(for: .restricted) == "Restricted on this device")
        #expect(CameraWords.detail(for: .denied) == "Not allowed in Settings")
        #expect(CameraWords.detail(for: .authorized) == nil && CameraWords.detail(for: .unavailable) == nil)
    }

    @Test func aDeviceWithoutACameraCannotTurnItOnAndKeepsAChoiceMadeBefore() {
        let s = setup(.unavailable, chosen: true)
        // The device says it has none: the row shows the switch off and
        // disabled and the session is blocked, but the choice is not taken back.
        s.model.camera?.refresh()
        #expect(s.model.guest?.camera == true)
        #expect(!s.model.cameraShownOn && !s.model.cameraAccess.offered)
        #expect(s.model.cameraMode == .blocked)
        #expect(s.driver.savedGuest == nil, "nothing was written for it")
        s.model.setCamera(true)
        #expect(!s.model.cameraShownOn)
        #expect(s.model.permissionPrompt == nil && s.system.asked == 0)
        #expect(s.model.cameraMode == .blocked)
        // The camera comes back (a Continuity camera in reach, the guest moved
        // to another device): the switch is on again as the person left it.
        s.system.access = .authorized
        s.model.camera?.refresh()
        #expect(s.model.cameraShownOn && s.model.cameraMode == .live)
    }

    @Test func aDeviceWithoutACameraTurnedOffStaysOff() {
        let s = setup(.unavailable, chosen: false)
        s.model.camera?.refresh()
        #expect(s.model.guest?.camera == false && !s.model.cameraShownOn)
        #expect(s.model.cameraMode == .blocked)
    }

    // MARK: Kept across launches

    @Test func theChoiceAndTheSessionModeSurviveARelaunch() throws {
        let first = setup(.notDetermined)
        first.model.setCamera(true)
        first.system.answer(.authorized)
        let stored = try #require(first.driver.savedGuest)
        let data = try JSONEncoder().encode(stored)

        // The next launch loads the guest and reads the access iOS keeps.
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1")
        let guest = try JSONDecoder().decode(Guest.self, from: data)
        let model = AppModel(guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        model.camera = CameraController(model: model, source: FakeCamera(.authorized))
        #expect(model.cameraShownOn && model.cameraMode == .live)

        // iOS no longer allows it at that launch: off, and blocked.
        let revoked = AppModel(guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let denied = FakeCamera(.authorized)
        revoked.camera = CameraController(model: revoked, source: denied)
        denied.access = .denied
        revoked.camera?.refresh()
        #expect(!revoked.cameraShownOn && revoked.cameraMode == .blocked && revoked.guest?.camera == false)
    }

    // MARK: The notice for other apps sharing the screen

    @Test func theNoticeFollowsTheMultitaskingInterruptionAloneWhileCameraIsOn() {
        let s = setup(.authorized, chosen: true)
        #expect(!s.model.cameraNoticeShowing)
        for capture in [CameraCapture.streaming, .interrupted, .interruptedBySystemPressure] {
            s.model.cameraCaptureChanged(capture)
            #expect(!s.model.cameraNoticeShowing)
        }
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        #expect(s.model.cameraNoticeShowing)
        s.model.cameraCaptureChanged(.streaming)
        #expect(!s.model.cameraNoticeShowing)
    }

    @Test func aDismissedNoticeStaysGoneUntilTheNextInterruption() {
        let s = setup(.authorized, chosen: true)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        s.model.dismissCameraNotice()
        #expect(!s.model.cameraNoticeShowing)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        #expect(!s.model.cameraNoticeShowing, "the same interruption does not raise it again")
        s.model.cameraCaptureChanged(.streaming)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        #expect(s.model.cameraNoticeShowing, "a new interruption does")
    }

    @Test func theEndOfARunClearsTheNotice() {
        let s = setup(.authorized, chosen: true)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        s.model.runEnded(RunEnd(result: 0), uptimeSeconds: 10)
        #expect(!s.model.cameraNoticeShowing && s.model.cameraCapture == .idle)
    }

    // MARK: The notice that Camera is off

    @Test func aBlockedGuestOpeningACameraRaisesTheCameraOffNotice() {
        let s = setup(.notDetermined)
        #expect(s.model.cameraMode == .blocked && s.model.cameraNoticeKind == nil)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(s.model.cameraNoticeKind == .cameraOff && s.model.cameraNoticeShowing)
    }

    @Test func theCameraOffNoticeStaysTwoSecondsAfterTheGuestClosesItsCameraAndThenGoes() async {
        let s = setup(.notDetermined)
        let pace = CameraNoticePace()
        pace.attach(to: s.model)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(!pace.isWaiting, "an open camera has no stay to wait out")
        s.model.cameraCaptureChanged(.idle)
        #expect(await CameraNoticePace.until { pace.isWaiting })
        #expect(pace.waits == [.seconds(2)])
        #expect(s.model.cameraNoticeKind == .cameraOff, "the close does not take it away at once")
        pace.release()
        #expect(await CameraNoticePace.until { s.model.cameraNoticeKind == nil })
        #expect(!pace.isWaiting)
        // A later open raises it again.
        pace.advance(60)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(s.model.cameraNoticeKind == .cameraOff)
    }

    @Test func aCameraThatOpensAgainWithinTheStayIsTheSameShowing() async {
        let s = setup(.notDetermined)
        let pace = CameraNoticePace()
        pace.attach(to: s.model)
        s.model.cameraCaptureChanged(.blockedInUse)
        s.model.cameraCaptureChanged(.idle)
        #expect(await CameraNoticePace.until { pace.isWaiting })
        pace.advance(1)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(await CameraNoticePace.until { !pace.isWaiting }, "the reopen ends the wait")
        #expect(s.model.cameraNoticeKind == .cameraOff)
        pace.advance(60)
        #expect(s.model.cameraNoticeKind == .cameraOff, "the camera that came back is open")
    }

    @Test func aDismissedCameraOffNoticeStaysAwayForTheRestOfTheRun() {
        let s = setup(.authorized)
        let pace = CameraNoticePace()
        pace.attach(to: s.model)
        s.model.cameraCaptureChanged(.blockedInUse)
        s.model.dismissCameraNotice()
        #expect(s.model.cameraNoticeKind == nil)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(s.model.cameraNoticeKind == nil, "the same open does not raise it again")
        s.model.cameraCaptureChanged(.idle)
        pace.advance(1)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(s.model.cameraNoticeKind == nil, "a reopen a second later does not")
        s.model.cameraCaptureChanged(.idle)
        pace.release(after: 60)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(s.model.cameraNoticeKind == nil, "an open a minute later does not")
    }

    @Test func dismissingOneKindOfNoticeDoesNotHideTheOther() {
        let s = setup(.authorized)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        s.model.dismissCameraNotice()
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(s.model.cameraNoticeKind == .cameraOff)
        s.model.dismissCameraNotice()
        s.model.cameraCaptureChanged(.streaming)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        #expect(s.model.cameraNoticeKind == .pausedForOtherApps)
    }

    @Test func theEndOfARunClearsTheCameraOffNoticeItsDismissalAndItsStay() async {
        let s = setup(.authorized)
        let pace = CameraNoticePace()
        pace.attach(to: s.model)
        s.model.cameraCaptureChanged(.blockedInUse)
        s.model.dismissCameraNotice()
        s.model.cameraCaptureChanged(.idle)
        #expect(await CameraNoticePace.until { pace.isWaiting })
        s.model.runEnded(RunEnd(result: 0), uptimeSeconds: 10)
        #expect(await CameraNoticePace.until { !pace.isWaiting }, "the end of the run ends the wait")
        #expect(s.model.cameraNoticeKind == nil && s.model.cameraCapture == .idle)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(s.model.cameraNoticeKind == .cameraOff, "the next run's first open raises it, dismissed before or not")
        // A notice that was up when the run ended does not stay for the next one.
        s.model.runEnded(RunEnd(result: 0), uptimeSeconds: 10)
        #expect(s.model.cameraNoticeKind == nil)
    }

    @Test func theCameraOffNoticeIsRaisedWhereTheRowCanBeTurnedOnAndNowhereElse() {
        // Camera off while iOS has not been asked, or has refused: the row can be tried.
        for access in [CameraAccess.notDetermined, .authorized, .denied, .restricted] {
            let s = setup(access)
            s.model.cameraCaptureChanged(.blockedInUse)
            #expect(s.model.cameraNoticeKind == .cameraOff, "access \(access)")
        }
        // A device with no camera to share has no row to turn on, and the guest has no camera to open.
        let none = setup(.unavailable)
        none.model.cameraCaptureChanged(.blockedInUse)
        #expect(none.model.cameraNoticeKind == nil)
        // With the switch on the notice would contradict the row.
        let on = setup(.authorized, chosen: true)
        on.model.cameraCaptureChanged(.blockedInUse)
        #expect(on.model.cameraNoticeKind == nil)
    }

    @Test func theNoticeIsAboutTheNextStartBecauseTheRowOnlyChangesOnHome() {
        let s = setup(.authorized)
        s.model.cameraCaptureChanged(.blockedInUse)
        #expect(s.model.cameraNoticeKind == .cameraOff)
        // Turning the row on, as Home does between runs, is what the notice
        // asks for: the session it applies to is the next one.
        s.model.setCamera(true)
        #expect(s.model.cameraMode == .live)
        #expect(s.model.cameraNoticeKind == nil)
    }

    @Test func theCameraOffWordsSayWhatToDo() {
        #expect(CameraWords.noticeTitle(for: .cameraOff) == "Camera is off")
        #expect(CameraWords.noticeDetail(for: .cameraOff)
            == "Shut down the virtual machine, then turn on Camera in Capabilities.")
        #expect(CameraWords.noticeTitle(for: .pausedForOtherApps) == CameraWords.pausedTitle)
        #expect(CameraWords.noticeDetail(for: .pausedForOtherApps) == nil)
        // The row the words name is on Home, under the section they name.
        #expect(CameraWords.title == "Camera")
    }

    // MARK: The runtime's words

    @Test func theRuntimeIsToldTheModeInItsOwnWords() {
        #expect(LiveRuntime.cameraRuntimeMode(.blocked) == UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED))
        #expect(LiveRuntime.cameraRuntimeMode(.live) == UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE))
        #expect(LiveRuntime.cameraRuntimeMode(.blocked) != LiveRuntime.cameraRuntimeMode(.live))
    }

    @Test func theCameraModeAStartStatesIsWhatIOSAllowsThen() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let release = try SystemFolder.release(in: systemDirectory)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: release.identity, releaseDirectory: systemDirectory
        )
        let s = setup(.authorized, chosen: true)
        let runtime = LiveRuntime(
            model: s.model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        // The runtime put the system's own source on the model; the test puts the fake back.
        s.model.camera = CameraController(model: s.model, source: s.system)
        #expect(runtime.cameraBootMode() == UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE))
        // iOS takes the access away after Home read it: the boot says blocked.
        s.system.access = .denied
        #expect(runtime.cameraBootMode() == UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED))
        #expect(s.model.guest?.camera == false)
        // Off, or no camera at all: blocked.
        let off = setup(.authorized, chosen: false)
        let offRuntime = LiveRuntime(
            model: off.model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        off.model.camera = CameraController(model: off.model, source: off.system)
        #expect(offRuntime.cameraBootMode() == UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED))
        let none = setup(.unavailable, chosen: true)
        let noneRuntime = LiveRuntime(
            model: none.model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        none.model.camera = CameraController(model: none.model, source: none.system)
        #expect(noneRuntime.cameraBootMode() == UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED))
    }

    @Test func theLoaderStatesTheCamerasCeilingThatNoSystemCameraExceeds() {
        // The most any use of a system camera holds, which the application plans with: every host is given the
        // small tier for now, so it is the same at any budget: 196.31 MiB, the worst case of the capture pools
        // (six held buffers of the aligned 4:3 frame a 1080p stream is delivered, for each of two cameras), the
        // staging, the transformer's scratch and the stills the device admits at once, a 12 MP JPEG (with the system
        // camera's working storage) with a RAW.
        for budget: UInt32 in [384, 534, 1249, 2048, 4096, 9152] {
            #expect(kernarium_runtime_loader_camera_reserve_mib(budget) == 197, "budget \(budget) MiB")
        }
        // The model's policy takes that reserve out of the budget the runtime
        // computes: an iPad mini 6 (B 534) is left 337 MiB, a 4 GiB iPhone
        // (B 1249) 1052 MiB, and the figure is the budget's while Camera is off.
        for (budget, pool) in [(534, 337), (1249, 1052)] {
            let policy = MemoryPolicy(
                processorCount: 1, bound: { _ in 4096 }, defaultMemory: { _ in 2048 },
                graphicsMemory: { _, _ in budget },
                cameraReserve: { Int(kernarium_runtime_loader_camera_reserve_mib(UInt32(clamping: $0))) })
            #expect(policy.graphicsMemoryMiB(forMemory: 2048, vCPUs: 1, cameraLive: true) == pool)
            #expect(policy.graphicsMemoryMiB(forMemory: 2048, vCPUs: 1) == budget)
        }
    }

    @Test func theRuntimePlansTheReserveOfThisHostsCameras() {
        // An iPad mini 6's B, with the policy's own figure the ceiling.
        let policy = MemoryPolicy(
            processorCount: 1, bound: { _ in 4096 }, defaultMemory: { _ in 2048 },
            graphicsMemory: { _, _ in 534 },
            cameraReserve: { Int(kernarium_runtime_loader_camera_reserve_mib(UInt32(clamping: $0))) })
        #expect(policy.graphicsMemoryMiB(forMemory: 2048, vCPUs: 1, cameraLive: true) == 337)
        // The runtime serves the synthetic phone (a development knob): one
        // stream, a RAW still and the working storage its source states.
        let name = "KERNARIUM_CAMERA_SOURCE"
        let before = getenv(name).map { String(cString: $0) }
        setenv(name, "synthetic:phone", 1)
        defer {
            if let before { setenv(name, before, 1) } else { unsetenv(name) }
            var again: UInt32 = 0
            _ = kernarium_runtime_loader_plan_camera_reserve_mib(534, nil, 1, &again)
        }
        let plan = LiveRuntime.cameraReservePlan(for: policy, calibration: nil)
        #expect(plan.exact, "the runtime discovers the cameras and answers its own figure")
        #expect(plan.policy.graphicsMemoryMiB(forMemory: 2048, vCPUs: 1, cameraLive: true) == 534 - 281)
        #expect(plan.policy.graphicsMemoryMiB(forMemory: 2048, vCPUs: 1) == 534, "off, the budget")
        // The loader's own question agrees, and a calibration folder that does not exist teaches nothing.
        var exact: UInt32 = 0
        #expect(kernarium_runtime_loader_plan_camera_reserve_mib(534, "/nonexistent/Camera", 1, &exact) == 281)
        #expect(exact == 1)
        // A host with no camera the image can describe holds nothing, and says so as exactly.
        setenv(name, "no-such-source", 1)
        #expect(kernarium_runtime_loader_plan_camera_reserve_mib(534, nil, 1, &exact) == 0)
        #expect(exact == 1)
    }

    @Test func theCalibrationFolderIsMadeAndKeptOutOfBackups() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let folder = root.appendingPathComponent("Camera", isDirectory: true)
        let prepared = LiveRuntime.prepareCalibrationDirectory(folder)
        #expect(prepared == folder && prepared.path == folder.path, "the boot reads the folder the planner asks about")
        var isDirectory: ObjCBool = false
        #expect(FileManager.default.fileExists(atPath: folder.path, isDirectory: &isDirectory) && isDirectory.boolValue)
        let values = try folder.resourceValues(forKeys: [.isExcludedFromBackupKey])
        #expect(values.isExcludedFromBackup == true)
        // A folder that is already there is kept and flagged all the same.
        _ = LiveRuntime.prepareCalibrationDirectory(folder)
        #expect(try folder.resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup == true)
    }

    @Test func theRuntimesStatesAreReadAsTheyAreSaid() {
        let said: [(Int, CameraCapture)] = [
            (KERNARIUM_RUNTIME_CAMERA_STATE_IDLE, .idle),
            (KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING, .streaming),
            (KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED, .interrupted),
            (KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING, .interruptedByMultitasking),
            (KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_SYSTEM_PRESSURE, .interruptedBySystemPressure),
            (KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE, .blockedInUse),
        ]
        for (state, capture) in said {
            #expect(LiveRuntime.cameraCapture(runtimeState: Int32(state)) == capture)
        }
        // A state this application does not know is an interruption without the notice.
        let unknown = LiveRuntime.cameraCapture(runtimeState: 99)
        #expect(unknown == .interrupted && CameraNotice.raised(by: unknown) == nil)
    }

    // MARK: The notes under Capabilities

    @Test func onlyADeviceWithoutACameraHasANoteUnderCapabilities() {
        // Photos taken sideways need no note: the guest always has the device's motion sensors where it has them.
        #expect(CameraWords.notes(access: .unavailable) == [CameraWords.noCamera])
        for access in [CameraAccess.notDetermined, .authorized, .denied, .restricted] {
            #expect(CameraWords.notes(access: access).isEmpty)
        }
    }

    @Test func theDeviceOffersACameraByTheLayoutsRuleAndTheIPadAppOnAMacOrAVisionProNone() {
        // The system's own answer for this process is a front camera, a Mac and a Vision Pro.
        let device = SystemCameraSource.hasCamera
        let system = IPadAppHost.system
        let mac = system.isiOSAppOnMac
        let vision = system.isiOSAppOnVision
        #expect(device == CameraAvailability.isAvailable(
            hasFrontCamera: SystemCameraSource.hasFrontCamera, isiOSAppOnMac: mac, isiOSAppOnVision: vision))
        if mac || vision { #expect(!device) }
        #expect(SystemCameraSource().access == (device ? SystemCameraSource.access(of: AVCaptureDevice.authorizationStatus(for: .video)) : .unavailable))
        // The preview's system mode asks the Simulator's own permission whatever it has.
        #expect(SystemCameraSource(requiresCamera: false).access != .unavailable)
    }

    @Test func theIPadAppOnAVisionProIsOfferedNoCameraAndNothingIsDiscoveredForIt() {
        for (host, mac, vision) in [("Mac", true, false), ("Vision Pro", false, true)] {
            var looks = 0
            let platform = CameraPlatform(
                isiOSAppOnMac: mac, isiOSAppOnVision: vision, hasFrontCamera: { looks += 1; return true })
            #expect(SystemCameraSource(platform: platform).access == .unavailable, "\(host)")
            #expect(!platform.hasCamera, "\(host)")
            #expect(looks == 0, "\(host): no camera is looked for")
        }
        // Any other host with a front camera is offered it, after one look.
        var looks = 0
        let elsewhere = CameraPlatform(
            isiOSAppOnMac: false, isiOSAppOnVision: false, hasFrontCamera: { looks += 1; return true })
        #expect(elsewhere.hasCamera && looks == 1)
        #expect(SystemCameraSource(platform: elsewhere).access != .unavailable)
        let without = CameraPlatform(isiOSAppOnMac: false, isiOSAppOnVision: false, hasFrontCamera: { false })
        #expect(SystemCameraSource(platform: without).access == .unavailable)
    }

    @Test func theIPadAppOnAVisionProOn26IsOfferedNoCameraAndNothingIsDiscoveredForIt() {
        // visionOS 26.0 does not say it runs the iPad app; the hardware's name does.
        let host = IPadAppHost(isiOSAppOnMac: false, visionAnswer: nil, machine: "RealityDevice14,1")
        var looks = 0
        let platform = CameraPlatform(host: host, hasFrontCamera: { looks += 1; return true })
        #expect(SystemCameraSource(platform: platform).access == .unavailable)
        #expect(!platform.hasCamera)
        #expect(looks == 0, "no camera is looked for")
    }

    /// The row on a Vision Pro is the Mac's: off and disabled with the note, the
    /// choice not turned on, no question put to iOS and no notice about turning it on.
    @Test func theRowOfAnIPadAppOnAVisionProIsTheMacsAndAsksNothing() {
        let note = [CameraWords.noCamera]
        var rows: [String: [String]] = [:]
        for (host, mac, vision) in [("Mac", true, false), ("Vision Pro", false, true)] {
            let s = setup(controller: false)
            let source = AskCounting(SystemCameraSource(platform: CameraPlatform(
                isiOSAppOnMac: mac, isiOSAppOnVision: vision, hasFrontCamera: { true })))
            s.model.camera = CameraController(model: s.model, source: source)
            #expect(s.model.cameraAccess == .unavailable && !s.model.cameraAccess.offered, "\(host)")
            #expect(CameraWords.notes(access: s.model.cameraAccess) == note, "\(host)")
            // The choice is still off: a row that could be turned on would have the guest's open of a camera say so.
            s.model.cameraCaptureChanged(.blockedInUse)
            #expect(s.model.cameraNoticeKind == nil, "\(host): no row to turn on, so no notice")
            s.model.setCamera(true)
            s.model.camera?.refresh()
            #expect(s.model.permissionPrompt == nil && source.asked == 0, "\(host): nothing is asked")
            #expect(s.model.guest?.camera == false && !s.model.cameraShownOn && s.model.cameraMode == .blocked, "\(host)")
            rows[host] = CameraWords.notes(access: s.model.cameraAccess)
        }
        #expect(rows["Vision Pro"] == rows["Mac"])
    }

    @Test func theWordsAreThoseOfTheDesign() {
        #expect(CameraWords.title == "Camera")
        #expect(CameraWords.noCamera == "This device has no camera to share.")
        #expect(CameraWords.pausedTitle == "Camera paused while other apps are on screen")
    }
}

/// The Capabilities rows: Camera follows Microphone.
@MainActor
struct CameraRowTests {
    @Test func cameraFollowsMicrophone() {
        let rows = CapabilitiesSection.rows(hapticsAvailable: true)
        #expect(rows == [.location, .microphone, .camera, .haptics])
        #expect(CapabilitiesSection.rows(hapticsAvailable: false) == [.location, .microphone, .camera])
    }
}
