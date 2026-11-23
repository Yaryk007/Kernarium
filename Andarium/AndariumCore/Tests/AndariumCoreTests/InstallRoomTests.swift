// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct InstallRoomTests {
    @Test func theDeviceKeepsTheReserveOfEveryOtherTransfer() {
        let reserve = TransferRoom.deviceReserveBytes
        #expect(InstallRoom.deviceShortage(bytes: 100, free: 100 + reserve) == nil)
        #expect(InstallRoom.deviceShortage(bytes: 100, free: 99 + reserve) == .device(needed: 100 + reserve, free: 99 + reserve))
        #expect(InstallRoom.deviceShortage(bytes: 100, free: 50) == .device(needed: 100 + reserve, free: 50))
    }

    @Test func theMachinesDataPartitionKeepsTheImportsReserve() {
        let reserve = GuestFiles.guestReserveBytes
        #expect(InstallRoom.guestShortage(bytes: 100, free: 100 + reserve) == nil)
        #expect(InstallRoom.guestShortage(bytes: 100, free: 99 + reserve) == .virtualMachine(needed: 100 + reserve, free: 99 + reserve))
        #expect(InstallRoom.guestShortage(bytes: 3 * Bytes.gib, free: Bytes.gib) == .virtualMachine(needed: 3 * Bytes.gib + reserve, free: Bytes.gib))
        // A size past the range of the sum is still a shortage.
        #expect(InstallRoom.guestShortage(bytes: .max, free: 1) == .virtualMachine(needed: .max, free: 1))
    }

    @Test func theMachineIsAskedAboutItsDataPartition() {
        #expect(InstallRoom.guestPath == "/data")
        #expect(GuestFiles.freeSpaceCommand(for: InstallRoom.guestPath) == "df -Pk '/data'")
    }

    @Test func theWordsSayWhatIsNeededAndAvailable() {
        let device = InstallRoom.Shortage.device(needed: 5 * Bytes.gib + 256 * Bytes.mib, free: 2 * Bytes.gib)
        #expect(device.message(device: "iPad")
            == "There isn\u{2019}t enough free space on this iPad to install this app (5.25 GiB needed, 2 GiB available). Free up space in Settings \u{203A} General \u{203A} iPad Storage and try again.")
        let machine = InstallRoom.Shortage.virtualMachine(needed: 3 * Bytes.gib + 512 * Bytes.mib, free: 900 * Bytes.mib)
        #expect(machine.message(device: "iPad")
            == "The virtual machine doesn\u{2019}t have enough free space to install this app (3.5 GiB needed, 900 MiB available). Free some up in the virtual machine, or give it more storage with Resize.")
        #expect(!machine.message(device: "iPhone").contains("iPhone"))
    }
}
