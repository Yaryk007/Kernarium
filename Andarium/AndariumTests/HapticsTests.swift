// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import CoreHaptics
import Foundation
import Testing
@testable import Andarium

/// What the application does about haptics where the hardware may be missing.
/// A Simulator, an iPad and a Mac have no Taptic Engine: the boot request
/// states no haptics, no engine is made, and nothing fails. The player's
/// scores are played on a real iPhone, which only its owner can feel.
struct HapticsHostTests {
    @Test func theBootRequestStatesHapticsOnlyForADeviceThatHasThem() {
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false) == 0)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true) == UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS))
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true) == 1)
        // Whatever this device is, the request follows what Core Haptics reports.
        let features = LiveRuntime.hostFeatures(hapticsAvailable: DeviceHaptics.isAvailable)
        #expect((features != 0) == CHHapticEngine.capabilitiesForHardware().supportsHaptics)
    }

    #if targetEnvironment(simulator)
    @Test func theSimulatorHasNoHapticsAndMakesNoEngine() {
        #expect(!DeviceHaptics.isAvailable)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: DeviceHaptics.isAvailable) == 0)
        #expect(throws: (any Error).self) { try DeviceHapticEngines().engine(for: .device) }
    }
    #endif

    @Test func onlyTheDevicesOwnActuatorHasAnEngine() {
        #expect(throws: (any Error).self) { try DeviceHapticEngines().engine(for: HapticTarget(rawValue: 1)) }
    }

    /// An engine source that never has an engine, as on a device without haptics.
    private nonisolated struct NoEngines: HapticEngineSource {
        func engine(for target: HapticTarget) throws -> CHHapticEngine { throw CHHapticError(.notSupported) }
    }

    @Test func aPlayerWithoutAnEngineFailsQuietly() {
        let player = CoreHapticsPlayer(source: NoEngines())
        let score = HapticScore(sequence: 1, events: [.primitive(.click, durationMilliseconds: 12)])
        #expect(player.play(score) == .failed)
        // No engine means no tone, so a session that asks plays one again.
        #expect(!player.setToneIntensity(0.5, on: .device))
        player.stop(.device)
        player.prepare()
        player.stopAll()
        #expect(player.play(score) == .failed)
    }
}
