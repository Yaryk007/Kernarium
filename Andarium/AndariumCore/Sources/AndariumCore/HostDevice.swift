// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the iPhone, iPad or Mac running Andarium offers.
public struct HostDevice: Equatable, Sendable {
    public var name: String
    /// The vCPUs a guest may have, and the count a new guest takes:
    /// `min(8, physical cores)`, every core of the device at most the count the
    /// memory budget is measured for. The application states it from the
    /// loader (`GuestProcessorLimit`), so it is also the number of rows
    /// `memory` has and of positions the processor ruler offers.
    public var processorCount: Int
    public var memory: MemoryPolicy
    public var freeStorageBytes: UInt64
    /// Whether the device's GPU can draw the guest. The guest's graphics need
    /// Metal 4, or the experimental Apple 6 family path used by A13 devices.
    public var runsGuestGraphics: Bool
    /// Whether graphics run below the renderer's qualified Metal 4 family.
    public var experimentalGuestGraphics: Bool

    /// `processorCount` is the vCPU limit.
    public init(
        name: String, processorCount: Int, memory: MemoryPolicy, freeStorageBytes: UInt64,
        runsGuestGraphics: Bool = true, experimentalGuestGraphics: Bool = false
    ) {
        self.name = name
        self.processorCount = max(1, processorCount)
        self.memory = memory
        self.freeStorageBytes = freeStorageBytes
        self.runsGuestGraphics = runsGuestGraphics
        self.experimentalGuestGraphics = experimentalGuestGraphics
    }

    public func hasRoom(for bytes: UInt64) -> Bool { freeStorageBytes >= bytes }
}

/// What Home says on a device that cannot draw the guest. The requirement is
/// the chip's, so the words name the chip and leave the rest of the device out.
public enum DeviceRequirement {
    public static let title = "Needs an A13 chip or later"

    public static func explanation(deviceName: String) -> String {
        "This \(deviceName)'s GPU is too old for Andarium's graphics renderer, so the guest cannot start here."
    }
}

/// What Home says when an A13-class GPU is allowed to try the Metal 4
/// renderer without belonging to its qualified device family.
public enum ExperimentalGraphicsWarning {
    public static let title = "Experimental graphics on this device"

    public static func explanation(deviceName: String) -> String {
        "This \(deviceName)'s GPU is older than the renderer's Metal 4 target. Android can start, but graphics may be slow, incorrect, or fail."
    }
}
