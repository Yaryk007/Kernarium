// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The room an install needs, checked once the file is in hand and before a
/// byte of it goes to the virtual machine. The package manager stages the app
/// in the machine's data partition, which grows the data disk on the device by
/// as much, so both can refuse: the device (the rule of the other transfers,
/// `TransferRoom`) and the machine's data partition (`GuestFiles.guestHasRoom`).
public enum InstallRoom {
    /// The partition the package manager stages an install in.
    public static let guestPath = "/data"

    /// What is short, in bytes: `needed` is the app and the reserve beside it.
    public enum Shortage: Equatable, Sendable {
        case device(needed: UInt64, free: UInt64)
        case virtualMachine(needed: UInt64, free: UInt64)

        /// The words the screen shows. `device` is "iPad", "iPhone" or "Mac".
        public func message(device: String) -> String {
            switch self {
            case let .device(needed, free):
                String(
                    localized: "There isn\u{2019}t enough free space on this \(device) to install this app (\(Formats.size(bytes: needed)) needed, \(Formats.size(bytes: free)) available). Free up space in Settings \u{203A} General \u{203A} \(device) Storage and try again.",
                    bundle: .module)
            case let .virtualMachine(needed, free):
                String(
                    localized: "The virtual machine doesn\u{2019}t have enough free space to install this app (\(Formats.size(bytes: needed)) needed, \(Formats.size(bytes: free)) available). Free some up in the virtual machine, or give it more storage with Resize.",
                    bundle: .module)
            }
        }
    }

    /// Whether the device, with `free` bytes, can take an app of `bytes`.
    public static func deviceShortage(bytes: UInt64, free: UInt64) -> Shortage? {
        TransferRoom.deviceHasRoom(for: bytes, free: free)
            ? nil : .device(needed: adding(bytes, TransferRoom.deviceReserveBytes), free: free)
    }

    /// Whether the machine's data partition, with `free` bytes, can take it.
    public static func guestShortage(bytes: UInt64, free: UInt64) -> Shortage? {
        GuestFiles.guestHasRoom(for: bytes, free: free)
            ? nil : .virtualMachine(needed: adding(bytes, GuestFiles.guestReserveBytes), free: free)
    }

    private static func adding(_ bytes: UInt64, _ reserve: UInt64) -> UInt64 {
        let (sum, overflow) = bytes.addingReportingOverflow(reserve)
        return overflow ? .max : sum
    }
}
