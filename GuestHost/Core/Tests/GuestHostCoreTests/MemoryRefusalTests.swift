// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

struct MemoryRefusalTests {
    @Test func aSizeAboveTheCeilingNamesTheMostTheHostGives() {
        #expect(
            MemoryRefusal.reason(memoryMiB: 5120, ceilingMiB: 4608)
                == "5 GiB of memory is more than this device can give the virtual machine (at most 4.5 GiB)")
        #expect(
            MemoryRefusal.reason(memoryMiB: 9216, ceilingMiB: 9152)
                == "9 GiB of memory is more than this device can give the virtual machine (at most 8.94 GiB)")
    }

    @Test func aSizeUnderTheCeilingThatIsNotOnTheStepsIsNotOne() {
        #expect(
            MemoryRefusal.reason(memoryMiB: 300, ceilingMiB: 4096)
                == "300 MiB of memory is not a size the virtual machine can start with")
    }

    @Test func aHostWithNoRoomForTheSmallestSizeSaysSoNotAFigureOfZero() {
        #expect(
            MemoryRefusal.reason(memoryMiB: 512, ceilingMiB: 0)
                == "this device has no room for the virtual machine\u{2019}s memory")
    }

    @Test func theScreenSaysVirtualMachineNeverGuestOrVM() {
        for (memory, ceiling) in [(5120, 4608), (300, 4096), (1 << 20, 9152), (512, 0)] {
            let text = MemoryRefusal.reason(memoryMiB: memory, ceilingMiB: ceiling)
            #expect(text.contains("virtual machine"))
            #expect(!text.localizedCaseInsensitiveContains("guest"))
            #expect(!text.contains("VM"))
        }
    }
}
