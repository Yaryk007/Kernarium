// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// A recovery start the guest asked for itself (`adb reboot recovery`, the
/// recovery image's own menu, Android's rescue after a loop of failed boots)
/// is shown as the recovery session the launch's recovery mode makes, and the
/// session that follows the recovery's reboot is a normal one again.
@Suite(.serialized)
@MainActor
struct GuestRecoveryTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 41_000_000_000
    )

    private func model() -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (9)")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        model.driver = StubDriver(plan: .normal)
        return model
    }

    /// The console as the runtime reports it to the model.
    private func consoleShowsRecovery(_ model: AppModel) {
        model.bootTracker.noteRecovery()
        model.bootTracker.reach(.ready, atSeconds: 9)
    }

    private func consoleShowsBootCompleted(_ model: AppModel) {
        model.bootTracker.reach(.ready, atSeconds: 22)
    }

    @Test func aRecoveryTheGuestAskedForIsARecoverySession() {
        let model = model()
        model.phase = .running(.normal)
        model.restart()
        #expect(model.phase == .booting(.restart))
        consoleShowsRecovery(model)
        model.finishBoot(.restart)
        #expect(model.phase == .running(.recovery))
    }

    @Test func theRecoverysRebootReturnsToANormalSession() {
        let model = model()
        model.phase = .running(.recovery)
        model.restart()
        // The tracker belongs to the boot: the recovery before it is gone.
        #expect(!model.bootTracker.recovery)
        consoleShowsBootCompleted(model)
        model.finishBoot(.restart)
        #expect(model.phase == .running(.normal))
    }

    @Test func aRecoveryThatRebootsIntoRecoveryStaysARecoverySession() {
        let model = model()
        model.phase = .running(.recovery)
        model.restart()
        consoleShowsRecovery(model)
        model.finishBoot(.restart)
        #expect(model.phase == .running(.recovery))
    }

    @Test func aStartThatTheGuestTurnsIntoRecoveryIsRecoveryToo() {
        // Android's rescue after a loop of failed boots takes a normal start
        // to its recovery.
        let model = model()
        model.phase = .booting(.normal)
        consoleShowsRecovery(model)
        model.finishBoot(.normal)
        #expect(model.phase == .running(.recovery))
    }

    @Test func aBootThatTheConsoleDoesNotShowAsRecoveryIsNormal() {
        let model = model()
        for kind in [BootKind.normal, .restart, .upgrade, .firstStart] {
            model.phase = .booting(kind)
            model.bootTracker = BootTracker()
            consoleShowsBootCompleted(model)
            model.finishBoot(kind)
            #expect(model.phase == .running(.normal), "\(kind)")
        }
    }

    @Test func theLaunchsOwnRecoveryModeIsStillRecovery() {
        let model = model()
        model.phase = .booting(.recovery)
        consoleShowsBootCompleted(model)
        model.finishBoot(.recovery)
        #expect(model.phase == .running(.recovery))
    }

    @Test func aRecoverySessionHasWhatTheLaunchsRecoveryHas() {
        // What the session offers is read from the phase alone, so the guest's
        // own recovery has the same shape: no bug report, no adb over the
        // network and the recovery's Install APK rule.
        let model = model()
        model.phase = .booting(.restart)
        consoleShowsRecovery(model)
        model.finishBoot(.restart)
        #expect(!model.canCreateBugReport)
        #expect(!model.capabilities.offersApkInstall(in: .recovery))
    }
}
