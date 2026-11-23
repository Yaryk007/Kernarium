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
    /// Metal 4, which the A14 and later chips (and the M1 and later) have.
    public var runsGuestGraphics: Bool
    /// Whether the device can have a mouse or trackpad whose pointer iOS shows
    /// the application: an iPad or a Mac, not an iPhone.
    public var supportsPointer: Bool
    /// The kind of device it is: an iPhone, an iPad, or a Mac running the
    /// application as an iPad app.
    public var idiom: Idiom
    /// Whether the device has a Taptic Engine to play the guest's haptics.
    public var hasHaptics: Bool
    /// Whether the device can give the guest a compass: it has a magnetometer
    /// that Core Motion can give calibrated, and no permission refuses it. An
    /// iPhone or an iPad, not a Mac or the Simulator.
    public var hasCompass: Bool
    /// Whether the device has a camera the guest can be given: a built-in one,
    /// and not the iPad app on a Mac or on an Apple Vision Pro.
    public var hasCamera: Bool

    public enum Idiom: Equatable, Sendable { case phone, pad, mac }

    /// `processorCount` is the vCPU limit. Whoever builds a host says what kind
    /// of device it is and whether it has haptics, a compass and a camera: a
    /// default would let a host claim "an iPhone without a camera" by leaving
    /// the argument out, and its notes and rows would silently go missing.
    public init(
        name: String, processorCount: Int, memory: MemoryPolicy, freeStorageBytes: UInt64,
        runsGuestGraphics: Bool = true, supportsPointer: Bool = true, idiom: Idiom, hasHaptics: Bool,
        hasCompass: Bool, hasCamera: Bool
    ) {
        self.name = name
        self.processorCount = max(1, processorCount)
        self.memory = memory
        self.freeStorageBytes = freeStorageBytes
        self.runsGuestGraphics = runsGuestGraphics
        self.supportsPointer = supportsPointer
        self.idiom = idiom
        self.hasHaptics = hasHaptics
        self.hasCompass = hasCompass
        self.hasCamera = hasCamera
    }

    public func hasRoom(for bytes: UInt64) -> Bool { freeStorageBytes >= bytes }
}

/// What Home says on a device that cannot draw the guest. The requirement is
/// the chip's, so the words name the chip and leave the rest of the device out.
public enum DeviceRequirement {
    public static let title = String(localized: "Needs an A14 chip or later", bundle: .module)

    public static func explanation(deviceName: String) -> String {
        String(
            localized: "Andarium draws the virtual machine with Metal 4, which this \(deviceName) does not have, so the virtual machine cannot start here.",
            bundle: .module)
    }
}
