// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

private let ipad = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
)
private let fullIpad = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 1_000_000_000
)

struct GuestTests {
    @Test func aFreshGuestTakesTheHostsChoices() {
        let guest = Guest.fresh(on: ipad)
        #expect(guest.cores == 6)
        // The safe limit is 4096 MiB here: nothing to round down or clamp.
        #expect(guest.memoryMiB == 4096)
        #expect(guest.capacityGiB == 8)
        #expect(!guest.includeInBackups)
        #expect(!guest.microphone)
    }

    @Test func storageOnlyGrowsAndTheGrowthWaitsForTheNextStart() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        #expect(guest.capacityGiB == 8)
        #expect(guest.pendingCapacityGiB == 24)
        #expect(guest.effectiveCapacityGiB == 24)

        guest.setCapacity(4)
        #expect(guest.pendingCapacityGiB == nil)
        #expect(guest.effectiveCapacityGiB == 8)

        guest.setCapacity(500)
        #expect(guest.pendingCapacityGiB == 128)
        #expect(!guest.canGrow)
    }

    @Test func theCarriedSystemsLargestDiskLimitsGrowth() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(500, maximum: 64)
        #expect(guest.pendingCapacityGiB == 64)
    }

    @Test func theRulersOfferEveryFourGiBFromFourToOneHundredTwentyEight() {
        let stops = Limits.capacityStopsGiB()
        #expect(stops.first == 4 && stops.last == 128 && stops.count == 32)
        #expect(zip(stops, stops.dropFirst()).allSatisfy { $1 - $0 == Limits.capacityStepGiB })
        #expect(stops.contains(Limits.defaultCapacityGiB))
    }

    @Test func aSystemWhoseDiskStopsLowerCutsTheRulerThere() {
        #expect(Limits.capacityStopsGiB(releaseMaximumGiB: 64).last == 64)
        // Between steps, the last step below it.
        #expect(Limits.capacityStopsGiB(releaseMaximumGiB: 70).last == 68)
        #expect(Limits.capacityStopsGiB(releaseMaximumGiB: 500).last == 128)
        #expect(Limits.capacityStopsGiB(releaseMaximumGiB: 2) == [4])
    }

    @Test func choosingTheCurrentCapacityCancelsAPendingGrowth() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(16)
        guest.setCapacity(8)
        #expect(guest.pendingCapacityGiB == nil)
    }

    @Test func aStartAppliesAPendingGrowthAndSaysWhatItGrewFrom() {
        var guest = Guest.fresh(on: ipad)
        #expect(guest.applyPendingGrowth() == nil)
        #expect(guest.capacityGiB == 8)
        guest.setCapacity(24)
        #expect(guest.applyPendingGrowth() == 8)
        #expect(guest.capacityGiB == 24)
        #expect(guest.pendingCapacityGiB == nil)
        #expect(guest.effectiveCapacityGiB == 24)
        // Nothing is left to apply, or to cancel.
        #expect(guest.applyPendingGrowth() == nil)
        guest.setCapacity(8)
        #expect(guest.capacityGiB == 24 && guest.pendingCapacityGiB == nil)
    }

    @Test func erasedKeepsTheChoicesAndTheCapacityTheNextStartRunsWith() {
        var guest = Guest.fresh(on: ipad)
        guest.microphone = true
        guest.includeInBackups = true
        guest.memoryMiB = 2048
        guest.setCapacity(24)
        guest.applyPendingGrowth()
        guest.usedBytes = 5_000_000_000
        guest.setCapacity(32)

        let erased = guest.erased
        #expect(erased.capacityGiB == 32)
        #expect(erased.pendingCapacityGiB == nil)
        #expect(erased.usedBytes == 0)
        #expect(erased.microphone)
        #expect(erased.includeInBackups)
        #expect(erased.memoryMiB == 2048)
    }

    @Test func hapticsAreOnForANewGuestAndForOneStoredBefore() throws {
        #expect(Guest.fresh(on: ipad).haptics)
        // A record from before the switch has no such key: the guest behaves as it did.
        let before = Data(#"{"cores":2,"memoryMiB":1024,"capacityGiB":8,"microphone":true,"includeInBackups":false}"#.utf8)
        #expect(try JSONDecoder().decode(Guest.self, from: before).haptics)
        #expect(Guest(cores: 2, memoryMiB: 1024, capacityGiB: 8).haptics)
    }

    @Test func theHapticsChoiceIsKeptAsChosenThroughSaveAndErase() throws {
        var guest = Guest.fresh(on: ipad)
        guest.haptics = false
        let saved = try JSONEncoder().encode(guest)
        #expect(try JSONDecoder().decode(Guest.self, from: saved).haptics == false)
        #expect(try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest.erased)).haptics == false)
        #expect(guest.erased.haptics == false)
        guest.haptics = true
        #expect(try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest)).haptics)
        // It is a choice of its own: the other switches are as they were.
        let off = Data(#"{"cores":2,"memoryMiB":1024,"capacityGiB":8,"microphone":true,"includeInBackups":false,"haptics":false}"#.utf8)
        let decoded = try JSONDecoder().decode(Guest.self, from: off)
        #expect(!decoded.haptics && decoded.microphone && !decoded.location)
    }

    @Test func onlyTheChoicesAreStored() throws {
        var guest = Guest.fresh(on: ipad)
        guest.usedBytes = 123
        let data = try JSONEncoder().encode(guest)
        let text = String(decoding: data, as: UTF8.self)
        #expect(!text.contains("usedBytes"))

        let decoded = try JSONDecoder().decode(Guest.self, from: data)
        #expect(decoded.usedBytes == 0)
        #expect(decoded.cores == guest.cores)
    }
}

struct LifecycleTests {
    @Test func noGuestCannotStart() {
        var life = Lifecycle(guest: nil)
        #expect(life.start(.normal, host: ipad) == .noGuest)
        #expect(life.phase == .home)
    }

    /// Metal 4 starts with the A14: an earlier chip cannot draw the guest, so
    /// nothing starts, with or without a guest, and the words name the chip.
    @Test func aDeviceThatCannotDrawTheGuestNeverStarts() {
        var older = ipad
        older.runsGuestGraphics = false
        var life = Lifecycle(guest: Guest.fresh(on: older))
        #expect(life.start(.normal, host: older) == .needsNewerChip)
        #expect(life.start(.recovery, host: older, plan: .firstStart) == .needsNewerChip)
        #expect(life.phase == .home)
        #expect(Lifecycle(guest: nil).startVerdict(.normal, host: older) == .needsNewerChip)
        #expect(DeviceRequirement.title.contains("A14"))
        #expect(DeviceRequirement.explanation(deviceName: "iPhone").contains("this iPhone"))
        #expect(ipad.runsGuestGraphics)
    }

    @Test func aStartNeedsRoomForAndroidsOwnWrites() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        let verdict = life.start(.normal, host: fullIpad)
        #expect(verdict == .lowSpace(neededBytes: Limits.startReserveBytes, freeBytes: 1_000_000_000))
        #expect(life.phase == .home)
    }

    @Test func aFirstStartBootsThenRuns() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        #expect(life.start(.normal, host: ipad, plan: .firstStart) == .allowed(.firstStart))
        #expect(life.phase == .booting(.firstStart))
        life.finishBoot(.firstStart)
        #expect(life.phase == .running(.normal))
    }

    /// The runtime grows the data disk at every start, Recovery's included, so
    /// the growth is the guest's from the moment the start is handed over.
    @Test func aRecoveryStartAppliesAPendingGrowth() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        var life = Lifecycle(guest: guest)
        #expect(life.start(.recovery, host: ipad) == .allowed(.recovery))
        #expect(life.guest?.capacityGiB == 24)
        #expect(life.guest?.pendingCapacityGiB == nil)
        life.finishBoot(.recovery)
        #expect(life.phase == .running(.recovery))
        #expect(life.guest?.capacityGiB == 24)
        #expect(life.guest?.pendingCapacityGiB == nil)
    }

    /// A start that is stopped, crashes or is killed before it finishes
    /// booting has still had its disk grown; choosing the old size afterwards
    /// must not leave a pending growth to cancel.
    @Test func aStartThatNeverFinishesBootingStillAppliesAPendingGrowth() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        var life = Lifecycle(guest: guest)
        #expect(life.start(.normal, host: ipad) == .allowed(.normal))
        life.fail()
        #expect(life.guest?.capacityGiB == 24)
        #expect(life.guest?.pendingCapacityGiB == nil)

        var ended = Lifecycle(guest: guest)
        _ = ended.start(.normal, host: ipad)
        ended.powerOff()
        #expect(ended.guest?.capacityGiB == 24)
        #expect(ended.guest?.pendingCapacityGiB == nil)
        // The Resize sheet's old size is below the disk now: nothing to cancel,
        // and the next start asks for the disk's own size.
        ended.guest?.setCapacity(8)
        #expect(ended.guest?.pendingCapacityGiB == nil)
        #expect(ended.guest?.capacityGiB == 24)
        #expect(ended.guest?.effectiveCapacityGiB == 24)
    }

    @Test func aNormalBootAppliesAPendingGrowthAsBefore() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        var life = Lifecycle(guest: guest)
        #expect(life.start(.normal, host: ipad) == .allowed(.normal))
        life.finishBoot(.normal)
        #expect(life.phase == .running(.normal))
        #expect(life.guest?.capacityGiB == 24)
        #expect(life.guest?.pendingCapacityGiB == nil)
    }

    /// The boot screen says what the start grew the storage from while it
    /// boots, in whatever kind of boot the growth came with.
    @Test func theBootSaysWhatTheStartGrewTheStorageFromUntilItEnds() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        var life = Lifecycle(guest: guest)
        #expect(life.start(.recovery, host: ipad) == .allowed(.recovery))
        #expect(life.grewFromGiB == 8)
        life.finishBoot(.recovery)
        #expect(life.grewFromGiB == nil)

        for end in [{ (life: inout Lifecycle) in life.powerOff() }, { $0.fail() }, { $0.restart() }] {
            var grown = Lifecycle(guest: guest)
            _ = grown.start(.normal, host: ipad)
            #expect(grown.grewFromGiB == 8)
            end(&grown)
            #expect(grown.grewFromGiB == nil)
        }

        // A start that grows nothing says nothing, and the next one is its own.
        var plain = Lifecycle(guest: Guest.fresh(on: ipad))
        _ = plain.start(.normal, host: ipad)
        #expect(plain.grewFromGiB == nil)
    }

    /// A start the host refuses never reaches the runtime: the growth waits.
    @Test func aRefusedStartLeavesAPendingGrowthPending() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        var life = Lifecycle(guest: guest)
        #expect(life.start(.normal, host: fullIpad) != .allowed(.normal))
        #expect(life.guest?.capacityGiB == 8)
        #expect(life.guest?.pendingCapacityGiB == 24)
    }

    @Test func eraseLeavesNoGuestAndNoNotice() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        life.notice = .closedInBackground
        life.erase()
        #expect(life.guest == nil)
        #expect(life.notice == nil)
        #expect(life.phase == .home)
        #expect(life.startVerdict(.normal, host: ipad) == .noGuest)
        life.create(Guest.fresh(on: ipad))
        #expect(life.guest != nil)
    }

    @Test func aFactoryResetFromRecoveryKeepsTheGuestAndItsChoices() {
        var guest = Guest.fresh(on: ipad)
        guest.memoryMiB = 2048
        guest.usedBytes = 4_000_000_000
        var life = Lifecycle(guest: guest)
        life.factoryReset()
        #expect(life.notice == nil)
        #expect(life.guest?.memoryMiB == 2048)
        #expect(life.guest?.usedBytes == 0)
    }

    @Test func theRunEndsInOneOfThreeWays() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        _ = life.start(.normal, host: ipad)
        life.finishBoot(.normal)
        life.restart()
        #expect(life.phase == .booting(.restart))
        life.powerOff()
        #expect(life.phase == .home)
        life.fail()
        #expect(life.phase == .failed)
    }

    @Test func aRunEndFollowsTheRuntimeResult() {
        #expect(RunEnd(result: 0).phase == .home)
        #expect(RunEnd(result: -5).phase == .failed)
        #expect(RunEnd(result: 0, restartedInALoop: true).phase == .failed)
        // Whose choice the end was does not change where it leads.
        #expect(RunEnd(result: -5, stoppedByUser: true).phase == .failed)
        #expect(RunEnd(result: 0, stoppedByUser: true).phase == .home)
    }

    @Test func theFailureReportSaysTheStopWasTheirs() {
        let system = SystemInfo(
            version: "17", release: "20261031-los24-r4",
            kernel: "Linux 6.18.54", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (1)"
        )
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8)
        let report = RunEnd(result: -5, stoppedByUser: true)
            .report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: 197)
        #expect(report.hasSuffix("Virtual machine ended: it was stopped from the boot screen after 3:17"))
    }

    @Test func theFailureReportNamesTheMemoryTheDeviceWouldNotGive() {
        let system = SystemInfo(
            version: "17", release: "20261031-los24-r4",
            kernel: "Linux 6.18.54", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (1)"
        )
        let guest = Guest(cores: 2, memoryMiB: 2048, capacityGiB: 8)
        let refused = RunEnd(result: -101, memoryRefusedMiB: 2048)
        // The run never began, so it did not run for any time.
        #expect(refused.report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: nil) == """
            Andarium 1.0 (1) · 20261031-los24-r4
            Linux 6.18.54 · 2 vCPUs · 2 GiB
            Virtual machine ended: it could not start (this device could not give it 2 GiB of memory)
            """)
        // The report of the run as the application makes it measures the time
        // from the call that began it, which the refusal ends within seconds:
        // the line still says the run never ran for any time.
        #expect(
            refused.report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: 3)
                == refused.report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: nil))
        // It is a failure that leads to the failure screen, and a clean
        // power-off with the same size in it is not one.
        #expect(refused.phase == .failed)
        #expect(!refused.poweredOff)
        // The report of a launch's own start failure keeps its own words.
        let launch = RunEnd(result: -1, startFailure: "2 GiB of memory is more than this device can give", memoryRefusedMiB: 2048)
        #expect(
            launch.report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: nil)
                .hasSuffix("it could not start (2 GiB of memory is more than this device can give)"))
        // Without the cause a result of -2, the library's own out-of-memory
        // status, is only a runtime result: a failed allocation of its own
        // returns it too.
        #expect(
            RunEnd(result: -2).report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: nil)
                .hasSuffix("Virtual machine ended: runtime result -2"))
    }

    @Test func lowerMemoryIsAdvisedOnlyWhereASmallerSizeIsOffered() {
        let refused = RunEnd(result: -101, memoryRefusedMiB: 2048)
        #expect(refused.canLowerMemory(leastOfferedMiB: 1536))
        // The size the ruler starts at cannot be lowered, and neither can one
        // below it (a device whose ruler holds a single size).
        #expect(!refused.canLowerMemory(leastOfferedMiB: 2048))
        #expect(!refused.canLowerMemory(leastOfferedMiB: 2560))
        // An end that names no refused memory has nothing to lower.
        #expect(!RunEnd(result: -2).canLowerMemory(leastOfferedMiB: 1536))
        #expect(!RunEnd(result: -5, stoppedByUser: true).canLowerMemory(leastOfferedMiB: 1536))
    }

    /// The start of a process with no room for any virtual machine ends before
    /// the runtime with the loader's own reason, and no size is one to lower.
    @Test func aProcessWithNoRoomEndsWithTheReasonAndNoAdviceToLowerTheMemory() {
        let system = SystemInfo(
            version: "17", release: "20261031-los24-r4",
            kernel: "Linux 6.18.54", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (1)"
        )
        let guest = Guest(cores: 2, memoryMiB: 0, capacityGiB: 8)
        let end = RunEnd(
            result: -1, startFailure: MemoryRefusal.reason(memoryMiB: 0, ceilingMiB: 0), memoryRefusedMiB: 0)
        #expect(end.phase == .failed)
        #expect(!end.canLowerMemory(leastOfferedMiB: 0))
        #expect(
            end.report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: nil)
                .hasSuffix("Virtual machine ended: it could not start (this device has no room for the virtual machine\u{2019}s memory)"))
    }

    @Test func cleanPowerOffAllowsAnotherStartAndFailureRequiresRelaunch() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        #expect(life.start(.normal, host: ipad) == .allowed(.normal))
        life.finishBoot(.normal)
        life.powerOff()
        #expect(life.phase == .home)
        #expect(life.start(.recovery, host: ipad) == .allowed(.recovery))
        life.fail()
        #expect(life.start(.normal, host: ipad) == .relaunchRequired)
        #expect(life.phase == .failed)
    }

    @Test func theFailureReportSaysVCPUForASingleProcessor() {
        let system = SystemInfo(
            version: "17", release: "20261031-los24-r4",
            kernel: "Linux 6.18.54", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (1)"
        )
        let guest = Guest(cores: 1, memoryMiB: 1536, capacityGiB: 8)
        let report = RunEnd(result: -5).report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: 74)
        #expect(report.contains("Linux 6.18.54 · 1 vCPU · 1.5 GiB\n"))
    }

    @Test func theFailureReportNamesTheBuildAndTheEnd() {
        let system = SystemInfo(
            version: "17", release: "20261031-los24-r4",
            kernel: "Linux 6.18.54", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (1)"
        )
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8)
        let report = RunEnd(result: -5).report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: 74)
        #expect(report == """
            Andarium 1.0 (1) · 20261031-los24-r4
            Linux 6.18.54 · 2 vCPUs · 1.5 GiB
            Virtual machine ended: runtime result -5 after 1:14
            """)
    }
}
