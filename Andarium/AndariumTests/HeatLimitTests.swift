// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// Settings' "When hot" in the model and through the runtime's thermal path:
/// the notice's question and its answers, no notice while the limit is off,
/// and what Android is told as the choice moves. The heat is stated to the
/// thermal session (`ANDARIUM_THERMAL_STATE`), never felt.
@MainActor
struct HeatLimitFlowTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model(_ phase: Phase = .running(.normal)) -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1)
        model.phase = phase
        return model
    }

    /// Holds the notice's wait until a test lets it go.
    @MainActor
    private final class Wait {
        private(set) var waits: [Duration] = []
        private var held: CheckedContinuation<Void, Never>?

        func sleep(_ duration: Duration) async {
            waits.append(duration)
            await withTaskCancellationHandler {
                await withCheckedContinuation { held = $0 }
            } onCancel: {
                Task { @MainActor in self.release() }
            }
        }

        var isWaiting: Bool { held != nil }

        func release() {
            let continuation = held
            held = nil
            continuation?.resume()
        }
    }

    private func until(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while !condition() {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(2))
        }
        return true
    }

    // MARK: The choice

    @Test func theLimitIsOnUntilThePersonTurnsItOff() {
        let model = model()
        #expect(model.heatLimitOn)
        #expect(!model.deviceIsHot)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.setHeatLimit(false)
        model.setHeatLimit(false)
        #expect(!model.heatLimitOn)
        model.setHeatLimit(true)
        #expect(model.heatLimitOn)
        #expect(driver.heatLimits == [false, true], "the runtime was told \(driver.heatLimits)")
    }

    @Test func turningTheLimitOffTakesTheNoticeAwayAtOnceAndNoneComesWhileItIsOff() {
        let model = model()
        model.thermalCapStarted()
        #expect(model.thermalNotice.isShowing)
        model.setHeatLimit(false)
        #expect(!model.thermalNotice.isShowing)
        // The runtime's policy may still announce a cap that was on its way.
        model.thermalCapStarted()
        #expect(!model.thermalNotice.isShowing, "a notice came with the limit off")
        // Nor does one wait for the session screen.
        let booting = self.model(.booting(.normal))
        booting.setHeatLimit(false)
        booting.thermalCapStarted()
        booting.phase = .running(.normal)
        #expect(!booting.thermalNotice.isShowing)
    }

    @Test func aCapAnnouncedBeforeTheLimitWentOffNeverShowsAfterIt() {
        let booting = model(.booting(.normal))
        booting.thermalCapStarted()
        booting.setHeatLimit(false)
        booting.phase = .running(.normal)
        #expect(!booting.thermalNotice.isShowing)
    }

    @Test func turningTheLimitBackOnShowsNoNoticeByItself() {
        let model = model()
        model.setHeatLimit(false)
        model.setHeatLimit(true)
        #expect(!model.thermalNotice.isShowing)
        // The notice comes when the policy starts the cap, as ever.
        model.thermalCapStarted()
        #expect(model.thermalNotice.isShowing)
    }

    @Test func hotNowFollowsThePolicyAndEndsWithTheRun() {
        let model = model()
        model.deviceHeatChanged(hot: true)
        #expect(model.deviceIsHot)
        model.setHeatLimit(false)
        #expect(model.deviceIsHot, "the choice changed what is hot")
        model.deviceHeatChanged(hot: false)
        #expect(!model.deviceIsHot)
        model.deviceHeatChanged(hot: true)
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(!model.deviceIsHot)
    }

    // MARK: The notice's question

    @Test func theNoticeAsksAndTurnOffIsTheOneTap() {
        let model = model()
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.thermalCapStarted()
        model.askToTurnOffHeatLimit()
        #expect(model.askingToTurnOffHeatLimit)
        #expect(model.thermalNotice.isShowing, "the question took the notice away")
        #expect(!model.guestHasKeys, "the alert has the keys")
        model.confirmTurnOffHeatLimit()
        #expect(!model.askingToTurnOffHeatLimit)
        #expect(!model.heatLimitOn)
        #expect(!model.thermalNotice.isShowing)
        #expect(driver.heatLimits == [false])
        #expect(model.guestHasKeys)
    }

    @Test func cancelLeavesTheNoticeAsItWasAndTheLimitOn() async {
        let wait = Wait()
        let model = model()
        model.thermalNoticeSleep = { await wait.sleep($0) }
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.thermalCapStarted()
        #expect(await until { wait.isWaiting })
        model.askToTurnOffHeatLimit()
        // The notice's time waits while the question is up.
        #expect(await until { !wait.isWaiting })
        #expect(model.thermalNotice.isShowing)
        model.cancelTurnOffHeatLimit()
        #expect(!model.askingToTurnOffHeatLimit)
        #expect(model.thermalNotice.isShowing)
        #expect(model.heatLimitOn)
        #expect(driver.heatLimits.isEmpty, "Cancel told the runtime something")
        // The notice runs its time again from here.
        #expect(await until { wait.isWaiting })
        wait.release()
        #expect(await until { !model.thermalNotice.isShowing })
        #expect(model.heatLimitOn)
    }

    @Test func dismissOnlyDismissesAndEndsAQuestionThatWasOpen() {
        let model = model()
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.thermalCapStarted()
        model.dismissThermalNotice()
        #expect(!model.thermalNotice.isShowing)
        #expect(model.heatLimitOn)
        #expect(driver.heatLimits.isEmpty)

        model.thermalCapStarted()
        model.askToTurnOffHeatLimit()
        model.dismissThermalNotice()
        #expect(!model.askingToTurnOffHeatLimit)
        #expect(model.heatLimitOn)
    }

    @Test func thereIsNothingToAskAboutWithoutANoticeOrWithTheLimitOff() {
        let model = model()
        model.askToTurnOffHeatLimit()
        #expect(!model.askingToTurnOffHeatLimit)
        model.thermalCapStarted()
        model.setHeatLimit(false)
        model.askToTurnOffHeatLimit()
        #expect(!model.askingToTurnOffHeatLimit)
    }

    @Test func theEndOfTheRunEndsAQuestionThatWasOpen() {
        let model = model()
        model.thermalCapStarted()
        model.askToTurnOffHeatLimit()
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(!model.askingToTurnOffHeatLimit)
        #expect(!model.thermalNotice.isShowing)
        #expect(model.heatLimitOn)
    }

    // MARK: Through the runtime's thermal path

    /// What the stand-in for the guest's HAL heard, in order.
    private final class Heard: @unchecked Sendable {
        private let lock = NSLock()
        private var list: [AndroidThermalSeverity] = []
        func add(_ severity: AndroidThermalSeverity) { lock.withLock { list.append(severity) } }
        var all: [AndroidThermalSeverity] { lock.withLock { list } }
        var last: AndroidThermalSeverity? { lock.withLock { list.last } }
    }

    private func runtime(for model: AppModel, hal: ThermalProbeHal) throws -> LiveRuntime {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let release = try SystemFolder.release(in: systemDirectory)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: release.identity, releaseDirectory: systemDirectory)
        return LiveRuntime(
            model: model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")),
            thermalOpener: hal)
    }

    /// Runs `body` with iOS's state stated as `state`, as a harness launch does.
    private func stating(_ state: String, _ body: () async throws -> Void) async rethrows {
        let name = ThermalOverride.environmentName
        let before = getenv(name).map { String(cString: $0) }
        setenv(name, state, 1)
        defer {
            if let before { setenv(name, before, 1) } else { unsetenv(name) }
        }
        try await body()
    }

    @Test func theChoiceIsReadByTheThermalPathAtOnceAtCritical() async throws {
        try await stating("critical") {
            let model = model()
            let heard = Heard()
            let runtime = try runtime(for: model, hal: ThermalProbeHal { heard.add($0) })
            model.driver = runtime
            runtime.beginThermalProbe()
            #expect(await until { heard.last == .critical }, "Android heard \(heard.all)")
            #expect(await until { model.thermalNotice.isShowing })
            #expect(await until { model.deviceIsHot })

            // Off: Android hears none at once, the notice goes, the device is still held hot.
            model.setHeatLimit(false)
            #expect(await until { heard.last == AndroidThermalSeverity.none }, "Android heard \(heard.all)")
            #expect(!model.thermalNotice.isShowing)
            #expect(model.deviceIsHot, "hot now stopped following the policy with the limit off")

            // On: Android hears the current severity at once.
            model.setHeatLimit(true)
            #expect(await until { heard.last == .critical }, "Android heard \(heard.all)")
            #expect(model.deviceIsHot)
        }
    }

    @Test func aRunThatBeginsHotWithTheLimitOffTellsAndroidNothingAndShowsNoNotice() async throws {
        for (state, hot) in [("serious", false), ("critical", true)] {
            try await stating(state) {
                let model = model()
                model.heatLimitOn = false
                let heard = Heard()
                let runtime = try runtime(for: model, hal: ThermalProbeHal { heard.add($0) })
                model.driver = runtime
                runtime.beginThermalProbe()
                #expect(await until { !heard.all.isEmpty }, "\(state): Android heard nothing at all")
                if hot { #expect(await until { model.deviceIsHot }, "\(state)") }
                try? await Task.sleep(for: .milliseconds(200))
                #expect(model.deviceIsHot == hot, "\(state): hot now is \(model.deviceIsHot)")
                #expect(heard.all.allSatisfy { $0 == .none }, "\(state): Android heard \(heard.all)")
                #expect(!model.thermalNotice.isShowing, "\(state): a notice came with the limit off")
            }
        }
    }

    @Test func aRunThatBeginsAtSeriousWithTheLimitOnTellsAndroidNothingShowsNoNoticeAndIsNotHot() async throws {
        try await stating("serious") {
            let model = model()
            let heard = Heard()
            let runtime = try runtime(for: model, hal: ThermalProbeHal { heard.add($0) })
            model.driver = runtime
            runtime.beginThermalProbe()
            #expect(await until { !heard.all.isEmpty }, "Android heard nothing at all")
            try? await Task.sleep(for: .milliseconds(200))
            #expect(heard.all.allSatisfy { $0 == .none }, "Android heard \(heard.all)")
            #expect(!model.thermalNotice.isShowing, "a notice came at serious")
            #expect(!model.deviceIsHot, "serious is held hot")
        }
    }
}
