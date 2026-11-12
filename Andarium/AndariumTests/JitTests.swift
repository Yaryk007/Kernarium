// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// A defaults database of its own for one test, removed after it.
private func withDefaults<Result>(_ body: (UserDefaults) throws -> Result) rethrows -> Result {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-jit-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let name = directory.appendingPathComponent("defaults").path
    let defaults = UserDefaults(suiteName: name)!
    defer {
        defaults.removePersistentDomain(forName: name)
        try? FileManager.default.removeItem(at: directory)
    }
    return try body(defaults)
}

@MainActor
struct JitPreferenceTests {
    @Test func aFreshInstallIsOff() {
        withDefaults { defaults in
            let preference = JitPreference(defaults: defaults)
            #expect(!preference.isOn)
            let start = preference.launch(stated: nil, hasOtherSettings: false)
            #expect(!start.isOn)
            #expect(start.keepsChanges)
        }
    }

    @Test func theLastValueIsKeptAcrossLaunches() {
        withDefaults { defaults in
            JitPreference(defaults: defaults).keep(true)
            #expect(JitPreference(defaults: defaults).isOn)
            let on = JitPreference(defaults: defaults).launch(stated: nil, hasOtherSettings: false)
            #expect(on.isOn && on.keepsChanges)
            JitPreference(defaults: defaults).keep(false)
            #expect(!JitPreference(defaults: defaults).launch(stated: nil, hasOtherSettings: false).isOn)
        }
    }

    @Test func aLaunchThatStatesTheValueForcesItAndKeepsNothing() {
        withDefaults { defaults in
            let preference = JitPreference(defaults: defaults)
            preference.keep(true)
            let off = preference.launch(stated: false, hasOtherSettings: true)
            #expect(!off.isOn && !off.keepsChanges)
            preference.keep(false)
            let on = preference.launch(stated: true, hasOtherSettings: false)
            #expect(on.isOn && !on.keepsChanges)
            #expect(!preference.isOn)
        }
    }

    @Test func aLaunchWithOtherSettingsStartsOffWhateverWasKept() {
        withDefaults { defaults in
            let preference = JitPreference(defaults: defaults)
            preference.keep(true)
            let start = preference.launch(stated: nil, hasOtherSettings: true)
            #expect(!start.isOn && !start.keepsChanges)
            #expect(preference.isOn)
        }
    }

    @Test func theEnvironmentStatesTheValueOrNothing() {
        #expect(JitPreference.stated(in: ["ANDARIUM_JIT": "1"]) == true)
        #expect(JitPreference.stated(in: ["ANDARIUM_JIT": "0"]) == false)
        #expect(JitPreference.stated(in: ["ANDARIUM_JIT": "yes"]) == nil)
        #expect(JitPreference.stated(in: [:]) == nil)
    }

    @Test func itIsAValueOfItsOwn() {
        #expect(JitPreference.key != StatsPreference.key)
        #expect(JitPreference.key != DisplayOptions.storageKey)
    }
}

@MainActor
struct JitAnswerTests {
    @Test func theRuntimeValuesMapToTheirAnswers() {
        #expect(JitAnswer(runtimeValue: 0) == .ready)
        #expect(JitAnswer(runtimeValue: 1) == .unsupported)
        #expect(JitAnswer(runtimeValue: 2) == .noDebugger)
        #expect(JitAnswer(runtimeValue: 3) == .failed)
        #expect(JitAnswer(runtimeValue: 4) == .otherSettings)
        #expect(JitAnswer(runtimeValue: 5) == .disabled)
        // An answer a later runtime adds is a failure, never a ready JIT.
        #expect(JitAnswer(runtimeValue: 6) == .failed)
        #expect(JitAnswer(runtimeValue: -1) == .failed)
    }

    @Test func onlyAFallBackHasASentence() {
        #expect(JitAnswer.ready.fallbackSentence == nil)
        #expect(JitAnswer.disabled.fallbackSentence == nil)
        #expect(JitAnswer.noDebugger.fallbackSentence == "Started without JIT: StikDebug is not connected.")
        for answer in [JitAnswer.failed, .otherSettings, .unsupported] {
            let sentence = answer.fallbackSentence
            #expect(sentence?.hasPrefix("Started without JIT: ") == true)
        }
    }
}

@MainActor
struct JitStatusTests {
    private let guest = JitShape(memoryMiB: 4096, processorCount: 4)

    @Test func withoutADebuggerItSaysHowToGetOne() {
        let status = JitStatus(debuggerAttached: false, guest: guest)
        #expect(!status.usable)
        #expect(status.footer == "To use JIT, open Andarium from StikDebug.")
    }

    @Test func withADebuggerItIsReady() {
        let status = JitStatus(debuggerAttached: true, guest: guest)
        #expect(status.usable)
        #expect(status.footer == "Ready. Applies to the next start.")
    }

    @Test func preparedMemoryIsReadyAfterTheDebuggerLeft() {
        let status = JitStatus(debuggerAttached: false, prepared: guest, guest: guest)
        #expect(status.usable)
        #expect(status.footer == JitStatus.ready)
    }

    @Test func aGuestChangedAfterPreparingNeedsANewProcess() {
        let other = JitShape(memoryMiB: 2048, processorCount: 4)
        for attached in [false, true] {
            let status = JitStatus(debuggerAttached: attached, prepared: guest, guest: other)
            #expect(!status.usable)
            #expect(status.footer == JitStatus.changed)
        }
        let cores = JitShape(memoryMiB: 4096, processorCount: 6)
        #expect(JitStatus(debuggerAttached: false, prepared: guest, guest: cores).footer == JitStatus.changed)
    }

    @Test func aStartThatFellBackSaysSoUntilTheNextStart() {
        var status = JitStatus(debuggerAttached: false, guest: guest, lastStart: .noDebugger)
        #expect(status.footer == "Started without JIT: StikDebug is not connected.")
        status.lastStart = nil
        #expect(status.footer == JitStatus.needsStikDebug)
        // An answer that is no fall-back leaves the state's own words.
        status.lastStart = .ready
        #expect(status.footer == JitStatus.needsStikDebug)
    }
}

@MainActor
struct JitStatusLaunchTests {
    private let guest = JitShape(memoryMiB: 4096, processorCount: 4)

    @Test func anAttemptAtLaunchThatFailedIsSpentWhateverTheDebuggerDoes() {
        for answer in [JitAnswer.noDebugger, .failed, .unsupported, .otherSettings] {
            let status = JitStatus(debuggerAttached: true, guest: guest, launch: answer)
            #expect(!status.usable)
            #expect(status.footer == JitStatus.needsStikDebug)
        }
    }

    @Test func aStartsOwnSentenceComesBeforeIt() {
        let status = JitStatus(debuggerAttached: false, guest: guest, lastStart: .failed, launch: .failed)
        #expect(status.footer == JitAnswer.failed.fallbackSentence)
    }

    @Test func anAttemptThatLeftTheMemoryPreparedIsNotAFailure() {
        let status = JitStatus(debuggerAttached: false, prepared: guest, guest: guest, launch: nil)
        #expect(status.footer == JitStatus.ready)
    }
}

@MainActor
struct JitLaunchRequestTests {
    private let guest = JitShape(memoryMiB: 4096, processorCount: 4)
    private let standard = JitShape(memoryMiB: 1536, processorCount: 6)

    @Test func withTheRowOnTheMemoryIsPreparedForTheGuest() {
        let request = JitLaunchRequest.atLaunch(
            wanted: true, debuggerAttached: true, guest: guest, standard: standard)
        #expect(request == .prepare(guest))
    }

    @Test func withTheRowOffTheDebuggerIsToldToGo() {
        let request = JitLaunchRequest.atLaunch(
            wanted: false, debuggerAttached: true, guest: guest, standard: standard)
        #expect(request == .detach)
    }

    @Test func withoutADebuggerNothingIsAsked() {
        for wanted in [true, false] {
            let request = JitLaunchRequest.atLaunch(
                wanted: wanted, debuggerAttached: false, guest: guest, standard: standard)
            #expect(request == nil)
        }
    }

    @Test func withoutAGuestTheMemoryIsPreparedForTheOneCreateOffers() {
        let request = JitLaunchRequest.atLaunch(
            wanted: true, debuggerAttached: true, guest: nil, standard: standard)
        #expect(request == .prepare(standard))
    }
}

/// What the application asks of an attached debugger when it opens, and what a
/// start then does about it, against a stub that stands for the runtime.
@MainActor
struct JitAtLaunchTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private var standard: JitShape {
        let fresh = Guest.fresh(on: host)
        return JitShape(memoryMiB: fresh.memoryMiB, processorCount: fresh.cores)
    }

    /// A device model with a debugger attached, as opened from StikDebug.
    private func model(guest: Bool = true, jitOn: Bool, attached: Bool = true) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages",
            appVersion: "1")
        let model = AppModel(
            guest: guest ? Guest.fresh(on: host) : nil, host: host, system: system, capabilities: .device,
            createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        driver.debuggerAttached = attached
        model.driver = driver
        model.jitAvailable = true
        model.jitOn = jitOn
        return (model, driver)
    }

    @Test func withTheRowOnTheMemoryIsPreparedAtOnceForTheGuest() async {
        let (model, driver) = model(jitOn: true)
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        #expect(driver.jitAsked.count == 1)
        #expect(driver.jitAsked.first?.wanted == true)
        #expect(driver.jitAsked.first?.shape == standard)
        #expect(model.jitPrepared == standard)
        // The debugger has left; the memory stays prepared for this guest.
        #expect(model.jitStatus.usable)
        #expect(model.jitStatus.footer == JitStatus.ready)
    }

    @Test func withTheRowOffTheDebuggerIsToldToGoAndTheFooterSaysHowToGetItBack() async {
        let (model, driver) = model(jitOn: false)
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        #expect(driver.jitAsked.map(\.wanted) == [false])
        #expect(model.jitPrepared == nil)
        #expect(!driver.debuggerAttached)
        // Turning the JIT on later in this process cannot have a debugger back.
        model.setJit(true)
        #expect(!model.jitStatus.usable)
        #expect(model.jitStatus.footer == "To use JIT, open Andarium from StikDebug.")
    }

    @Test func aStartAfterTheDebuggerWasToldToGoFallsBackWithTheNoDebuggerSentence() async {
        let (model, driver) = model(jitOn: false)
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        model.setJit(true)
        driver.jitAnswer = .noDebugger
        let onJit = await model.startOnJit(standard)
        #expect(!onJit)
        #expect(model.jitLastStart == .noDebugger)
        #expect(model.jitStatus.footer == "Started without JIT: StikDebug is not connected.")
    }

    @Test func withoutAGuestTheMemoryIsPreparedForTheShapeCreateOffers() async {
        let (model, driver) = model(guest: false, jitOn: true)
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        #expect(driver.jitAsked.first?.shape == standard)
        #expect(model.jitPrepared == standard)
        // A guest made as offered finds it ready; another needs a new process.
        model.guest = Guest.fresh(on: host)
        #expect(model.jitStatus.footer == JitStatus.ready)
        model.guest?.memoryMiB -= 512
        #expect(model.jitStatus.footer == JitStatus.changed)
    }

    @Test func aGuestTheLaunchIsAboutToMakeIsTheOneThatIsPreparedFor() async {
        let (model, driver) = model(jitOn: true)
        let other = JitShape(memoryMiB: 1024, processorCount: 2)
        model.prepareJitAtLaunch(standard: other, replacingGuest: true)
        await model.waitForJitLaunch()
        #expect(driver.jitAsked.first?.shape == other)
    }

    @Test func withoutADebuggerNothingIsAskedAtLaunch() async {
        for on in [true, false] {
            let (model, driver) = model(jitOn: on, attached: false)
            model.prepareJitAtLaunch(standard: standard)
            await model.waitForJitLaunch()
            #expect(driver.jitAsked.isEmpty)
            #expect(model.jitPrepared == nil)
            #expect(model.jitStatus.footer == JitStatus.needsStikDebug)
        }
    }

    @Test func aBuildWithoutTheJitAsksNothing() async {
        let (model, driver) = model(jitOn: true)
        model.jitAvailable = false
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        #expect(driver.jitAsked.isEmpty)
    }

    @Test func theLaunchAsksOnce() async {
        let (model, driver) = model(jitOn: true)
        model.prepareJitAtLaunch(standard: standard)
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        #expect(driver.jitAsked.count == 1)
    }

    @Test func aDebuggerThatLeftBeforeItServedLeavesTheFooterAskingToOpenFromStikDebug() async {
        let (model, driver) = model(jitOn: true)
        driver.jitAnswer = .noDebugger
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        #expect(model.jitPrepared == nil)
        #expect(model.jitLaunch == .noDebugger)
        // Even if a debugger still reads as attached, the one attempt is spent.
        driver.debuggerAttached = true
        model.refreshJit()
        #expect(!model.jitStatus.usable)
        #expect(model.jitStatus.footer == JitStatus.needsStikDebug)
    }

    @Test func aStartUsesTheMemoryPreparedAtLaunchWithoutAskingAgain() async {
        let (model, driver) = model(jitOn: true)
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        let onJit = await model.startOnJit(standard)
        #expect(onJit)
        #expect(driver.jitAsked.count == 1)
        #expect(model.jitLastStart == nil)
        #expect(!model.preparingJit)
    }

    @Test func aStartThatComesWhileTheAttemptRunsWaitsForItAndSaysSo() async {
        let (model, driver) = model(jitOn: true)
        driver.holdsJit = true
        model.prepareJitAtLaunch(standard: standard)
        await Task.yield()
        #expect(driver.jitAsked.count == 1)

        let start = Task { await model.startOnJit(standard) }
        await Task.yield()
        #expect(model.preparingJit)
        driver.releaseJit()
        let onJit = await start.value
        #expect(onJit)
        // Only the attempt at launch reached the debugger.
        #expect(driver.jitAsked.count == 1)
        #expect(!model.preparingJit)
    }

    @Test func aGuestChangedAfterTheAttemptKeepsTodaysRuleAndBootsOnGadgets() async {
        let (model, driver) = model(jitOn: true)
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        model.guest?.memoryMiB -= 512
        #expect(model.jitStatus.footer == JitStatus.changed)

        let changed = JitShape(
            memoryMiB: standard.memoryMiB - 512, processorCount: standard.processorCount)
        driver.jitAnswer = .otherSettings
        let onJit = await model.startOnJit(changed)
        #expect(!onJit)
        #expect(driver.jitAsked.count == 2)
        #expect(model.jitLastStart == .otherSettings)
        #expect(model.jitPrepared == standard)
    }

    @Test func aStartWithTheRowOffBootsOnGadgetsWithoutASentence() async {
        let (model, driver) = model(jitOn: true)
        model.prepareJitAtLaunch(standard: standard)
        await model.waitForJitLaunch()
        model.setJit(false)
        let onJit = await model.startOnJit(standard)
        #expect(!onJit)
        #expect(model.jitLastStart == nil)
        #expect(driver.jitAsked.last?.wanted == false)
    }
}
