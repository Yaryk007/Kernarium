// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// A storage growth the person chose applies as the start that carries it is
/// handed to the runtime, which grows the data disk in every kind of start.
@MainActor
struct StorageGrowthFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18",
        architecture: "arm64, 4 KiB pages", appVersion: "1"
    )

    /// Home with a guest whose Resize to 24 GiB is pending.
    private func growing(plan: StartPlan = .normal) -> (AppModel, StubDriver) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        let driver = StubDriver(plan: plan)
        model.driver = driver
        model.refreshPlan()
        model.guest?.setCapacity(24)
        return (model, driver)
    }

    @Test func aNormalStartHandsOverAndSavesTheGrownCapacity() {
        let (model, driver) = growing()
        model.start(.normal)
        #expect(model.phase == .booting(.normal))
        #expect(driver.begunGuests.map(\.capacityGiB) == [24])
        #expect(driver.begunGuests.map(\.pendingCapacityGiB) == [nil])
        // The runtime grows the disk as it starts: the record is saved first.
        #expect(driver.savedWhenBegun.map { $0?.capacityGiB } == [24])
        #expect(driver.savedWhenBegun.map { $0?.pendingCapacityGiB } == [nil])
        #expect(model.lifecycle.grewFromGiB == 8)
        model.finishBoot(.normal)
        #expect(model.guest?.capacityGiB == 24)
        #expect(model.guest?.pendingCapacityGiB == nil)
        #expect(model.lifecycle.grewFromGiB == nil)
    }

    @Test func aRecoveryStartHandsOverAndSavesTheGrownCapacity() {
        let (model, driver) = growing()
        model.start(.recovery)
        #expect(model.phase == .booting(.recovery))
        #expect(driver.begunGuests.map(\.capacityGiB) == [24])
        #expect(driver.savedWhenBegun.map { $0?.capacityGiB } == [24])
        model.finishBoot(.recovery)
        #expect(model.guest?.capacityGiB == 24)
        #expect(model.guest?.pendingCapacityGiB == nil)
    }

    /// The defect: a start that was killed or crashed before it finished
    /// booting, after the runtime had grown the disk, and then the old size
    /// chosen again in Resize.
    @Test func choosingTheOldSizeAfterAStartThatNeverFinishedBootingCancelsNothing() {
        let (model, driver) = growing()
        model.start(.normal)
        // Powered off from outside, before the guest reached Android.
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 5)
        #expect(model.phase == .home)
        #expect(model.guest?.capacityGiB == 24)
        #expect(model.guest?.pendingCapacityGiB == nil)
        #expect(driver.savedGuest?.capacityGiB == 24)

        model.guest?.setCapacity(8, maximum: model.releaseMaximumGiB)
        #expect(model.guest?.capacityGiB == 24)
        #expect(model.guest?.pendingCapacityGiB == nil)
        model.start(.normal)
        // The next start asks for the disk's own size, not a smaller one.
        #expect(driver.begunGuests.map(\.effectiveCapacityGiB) == [24, 24])
    }

    @Test func aRefusedStartLeavesTheGrowthPendingAndUnsaved() {
        let (model, driver) = growing()
        model.host.freeStorageBytes = 1_000_000_000
        model.start(.normal)
        #expect(model.phase == .home)
        #expect(model.guest?.capacityGiB == 8)
        #expect(model.guest?.pendingCapacityGiB == 24)
        #expect(driver.begun.isEmpty)
    }

    @Test func theBootScreenStillSaysTheStorageIsGrowing() {
        let (model, _) = growing()
        model.start(.normal)
        let guest = model.guest ?? Guest.fresh(on: host)
        for kind in [BootKind.normal, .recovery, .upgrade] {
            let steps = BootView.steps(kind, guest: guest, system: system, grewFromGiB: model.lifecycle.grewFromGiB)
            let storage = steps.first { $0.title == "Storage" }
            #expect(storage?.detail == "Growing from 8 GiB to 24 GiB")
        }
        // Without a growth the step states what the storage holds.
        let plain = BootView.steps(.recovery, guest: guest, system: system)
        #expect(plain.first { $0.title == "Storage" }?.detail.hasSuffix("of 24 GiB") == true)
    }
}
