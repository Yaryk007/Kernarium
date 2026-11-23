// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// iOS's microphone permission as a test plays it: the access the system
/// reports, and the question it asks the person, answered when the test says.
@MainActor
final class FakeMicrophone: MicrophoneAccessSource {
    var state: MicrophoneState
    private(set) var asked = 0
    private var pending: [@MainActor @Sendable (MicrophoneState) -> Void] = []

    init(_ state: MicrophoneState = .notAsked) { self.state = state }

    func request(_ answered: @escaping @MainActor @Sendable (MicrophoneState) -> Void) {
        asked += 1
        pending.append(answered)
    }

    /// The person answers the question iOS is showing.
    func answer(_ state: MicrophoneState) {
        self.state = state
        pending.removeFirst()(state)
    }
}

/// What the guest was told about its microphone, in order.
@MainActor
final class GuestMicrophone {
    private(set) var told: [Bool] = []
    func tell(_ on: Bool) { told.append(on) }
}

/// Microphone asks iOS when it is turned on, shows itself off and unusable
/// when iOS refuses or the person takes the access away, and gives the guest
/// no input then.
@MainActor
struct CapabilitiesFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private struct Setup {
        let model: AppModel
        let driver: StubDriver
        let system: FakeMicrophone
        let guest: GuestMicrophone
    }

    /// A model with the device's wiring: a driver that keeps the guest, and
    /// the microphone controller on a fake permission in `access`.
    private func setup(_ access: MicrophoneState = .notAsked, controller: Bool = true) -> Setup {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        // A test never opens the system's Settings.
        model.openSettings = {}
        let fake = FakeMicrophone(access)
        let guest = GuestMicrophone()
        if controller {
            model.microphone = MicrophoneController(model: model, source: fake) { guest.tell($0) }
        }
        return Setup(model: model, driver: driver, system: fake, guest: guest)
    }

    // MARK: Not asked yet -> allowed or refused

    @Test func turningItOnAsksIOSAtOnceAndTheGuestWaitsForTheAnswer() {
        let s = setup()
        #expect(s.model.microphoneState == .notAsked)
        s.model.setMicrophone(true)
        #expect(s.system.asked == 1)
        #expect(s.model.guest?.microphone == true)
        #expect(s.model.microphoneShownOn)
        // The guest hears nothing until the person has answered.
        #expect(s.guest.told == [false])
        s.system.answer(.granted)
        #expect(s.model.microphoneState == .granted)
        #expect(s.model.microphoneShownOn)
        #expect(s.model.permissionPrompt == nil)
        #expect(s.guest.told == [false, true])
        #expect(s.driver.savedGuest?.microphone == true)
    }

    @Test func aRefusalTakesTheChoiceBackAndTheGuestStaysDeaf() {
        let s = setup()
        s.model.setMicrophone(true)
        s.system.answer(.denied)
        #expect(s.model.microphoneState == .denied)
        #expect(s.model.guest?.microphone == false)
        #expect(!s.model.microphoneShownOn)
        #expect(s.driver.savedGuest?.microphone == false)
        #expect(s.guest.told == [false, false])
    }

    @Test func iOSIsAskedOnlyOnce() {
        let s = setup()
        s.model.setMicrophone(true)
        s.system.answer(.granted)
        s.model.setMicrophone(false)
        s.model.setMicrophone(true)
        #expect(s.system.asked == 1)
        #expect(s.guest.told == [false, true, false, true])
    }

    // MARK: Refused -> Settings

    @Test func afterARefusalTheSwitchExplainsAndOffersSettings() {
        let s = setup(.denied)
        var opened = 0
        s.model.openSettings = { opened += 1 }
        s.model.setMicrophone(true)
        // Nothing is asked and nothing is turned on: the person is told.
        #expect(s.model.permissionPrompt == .microphone)
        #expect(s.model.guest?.microphone == false)
        #expect(s.system.asked == 0)
        #expect(s.guest.told.isEmpty)
        #expect(opened == 0)
        s.model.openMicrophoneSettings()
        #expect(opened == 1)
        #expect(s.model.microphoneAwaitsSettings)
    }

    @Test func allowingItInSettingsTurnsTheSwitchTheyTappedOnWhenAndariumIsActive() {
        let s = setup(.denied)
        s.model.setMicrophone(true)
        s.model.openMicrophoneSettings()
        // Nothing changes while the person is in Settings...
        #expect(!s.model.microphoneShownOn)
        // ...and back in Andarium with access, the switch is on and the guest hears.
        s.system.state = .granted
        s.model.microphone?.refresh()
        #expect(s.model.microphoneState == .granted)
        #expect(s.model.microphoneShownOn)
        #expect(s.guest.told.last == true)
        #expect(!s.model.microphoneAwaitsSettings)
    }

    @Test func comingBackWithoutAccessLeavesItOffAndForgetsTheRequest() {
        let s = setup(.denied)
        s.model.setMicrophone(true)
        s.model.openMicrophoneSettings()
        s.model.microphone?.refresh()
        #expect(!s.model.microphoneShownOn)
        #expect(!s.model.microphoneAwaitsSettings)
        // Allowing it later, unprompted, does not turn it on by itself.
        s.system.state = .granted
        s.model.microphone?.refresh()
        #expect(!s.model.microphoneShownOn)
        #expect(s.guest.told.allSatisfy { !$0 })
    }

    // MARK: Allowed -> taken away

    @Test func accessTakenAwayWhileInTheBackgroundShowsItDisabledOnActivation() {
        let s = setup(.granted)
        s.model.setMicrophone(true)
        #expect(s.model.microphoneShownOn)
        #expect(s.guest.told == [true])
        // Settings > Andarium > Microphone is switched off; Andarium becomes active.
        s.system.state = .denied
        s.model.microphone?.refresh()
        #expect(s.model.microphoneState == .denied)
        #expect(!s.model.microphoneShownOn)
        #expect(s.model.guest?.microphone == false)
        #expect(s.driver.savedGuest?.microphone == false)
        #expect(s.guest.told == [true, false])
    }

    @Test func accessGivenBackLaterDoesNotTurnTheSwitchBackOn() {
        let s = setup(.granted)
        s.model.setMicrophone(true)
        s.system.state = .denied
        s.model.microphone?.refresh()
        s.system.state = .granted
        s.model.microphone?.refresh()
        #expect(s.model.microphoneState == .granted)
        #expect(!s.model.microphoneShownOn)
        #expect(s.guest.told.last == false)
    }

    // MARK: The guest's input

    @Test func theGuestGetsNoInputWhileTheSwitchIsDisabled() {
        let s = setup(.denied)
        s.model.setMicrophone(true)
        s.model.microphone?.guestStarting()
        #expect(s.guest.told == [false])
        #expect(s.guest.told.allSatisfy { !$0 })
    }

    @Test func aStartingGuestFollowsTheSwitchAsItShows() {
        let s = setup(.granted)
        s.model.microphone?.guestStarting()
        #expect(s.guest.told == [false])
        s.model.setMicrophone(true)
        s.model.microphone?.guestStarting()
        #expect(s.guest.told == [false, true, true])
    }

    @Test func turningItOffTellsTheGuest() {
        let s = setup(.granted)
        s.model.setMicrophone(true)
        s.model.setMicrophone(false)
        #expect(!s.model.microphoneShownOn)
        #expect(s.guest.told == [true, false])
        #expect(s.driver.savedGuest?.microphone == false)
    }

    @Test func theControllerReadsTheAccessWhenItStarts() {
        #expect(setup(.denied).model.microphoneState == .denied)
        #expect(setup(.granted).model.microphoneState == .granted)
        #expect(setup(.notAsked).model.microphoneState == .notAsked)
    }

    // MARK: The order of the rows

    /// Location sits above Microphone, Camera follows Microphone and Haptics
    /// comes last; the section draws its rows in this order, which VoiceOver
    /// reads.
    @Test func capabilitiesAreListedLocationMicrophoneCameraHaptics() {
        #expect(CapabilitiesSection.Row.allCases == [.location, .microphone, .camera, .haptics])
        #expect(CapabilitiesSection.rows(hapticsAvailable: true) == [.location, .microphone, .camera, .haptics])
    }

    /// An iPad, a Mac and the Simulator have no Taptic Engine: nothing to
    /// switch, so no row (and no disabled one).
    @Test func hapticsHasNoRowWithoutADeviceThatHasThem() {
        #expect(CapabilitiesSection.rows(hapticsAvailable: false) == [.location, .microphone, .camera])
        #expect(!setup().model.hapticsAvailable)
    }

    // MARK: The preview

    @Test func thePreviewWithoutAControllerTakesAccessAsGiven() {
        let s = setup(controller: false)
        s.model.setMicrophone(true)
        #expect(s.model.microphoneState == .granted)
        #expect(s.model.microphoneShownOn)
    }

    @Test func hapticsAreOnByDefaultAndAskForNothing() {
        let s = setup()
        s.model.hapticsAvailable = true
        #expect(s.model.guest?.haptics == true)
        #expect(s.model.hapticsOn)
        #expect(s.model.permissionPrompt == nil)
        #expect(s.system.asked == 0)
        s.model.setHaptics(false)
        #expect(s.model.guest?.haptics == false)
        #expect(!s.model.hapticsOn)
        // Kept as chosen, and the driver is told at once.
        #expect(s.driver.savedGuest?.haptics == false)
        #expect(!s.driver.haptics)
        s.model.setHaptics(true)
        #expect(s.model.hapticsOn)
        #expect(s.driver.savedGuest?.haptics == true)
        #expect(s.driver.haptics)
        #expect(s.system.asked == 0)
        // The other switches were never touched.
        #expect(s.model.guest?.location == false && s.model.guest?.microphone == false)
    }

    /// A harness launch that silences haptics runs as if the switch were off:
    /// the device plays nothing, and the person's choice, as shown and as
    /// stored, is not touched, whether the switch is turned meanwhile or not.
    @Test func aLaunchThatSilencesHapticsPlaysNoneAndKeepsTheChoice() {
        let s = setup()
        s.model.hapticsAvailable = true
        #expect(s.model.hapticsPlays)
        s.model.hapticsSilencedByLaunch = true
        #expect(!s.model.hapticsPlays)
        #expect(s.model.hapticsOn)
        #expect(s.model.guest?.haptics == true)
        // Turning the switch on changes nothing for this launch ...
        s.model.setHaptics(true)
        #expect(!s.model.hapticsPlays)
        #expect(s.driver.savedGuest?.haptics == true)
        // ... and off is kept as the person chose it.
        s.model.setHaptics(false)
        #expect(!s.model.hapticsOn && !s.model.hapticsPlays)
        #expect(s.driver.savedGuest?.haptics == false)
        // The guest still has its vibrator: the boot request is the device's.
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true) == UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS))
        s.model.hapticsSilencedByLaunch = false
        s.model.setHaptics(true)
        #expect(s.model.hapticsPlays)
    }

    /// The application's own touch feedback (the side keys' click, the rulers'
    /// tick) is not the guest's vibrator's, so the Haptics switch does not
    /// govern it; a launch that silences haptics does, because the taps and
    /// drags of an automated run reach it.
    @Test func aLaunchThatSilencesHapticsSilencesTheInterfaceFeedbackToo() {
        let s = setup()
        #expect(s.model.interfaceFeedbackPlays)
        #expect(InterfaceFeedback.playing(.selection, in: s.model) == .selection)
        s.model.setHaptics(false)
        #expect(!s.model.hapticsOn)
        #expect(s.model.interfaceFeedbackPlays)
        #expect(InterfaceFeedback.playing(.impact(weight: .light), in: s.model) == .impact(weight: .light))
        s.model.hapticsSilencedByLaunch = true
        #expect(!s.model.interfaceFeedbackPlays)
        #expect(InterfaceFeedback.playing(.selection, in: s.model) == nil)
        #expect(InterfaceFeedback.playing(.impact(weight: .light), in: s.model) == nil)
        // The person's choice is still the person's.
        s.model.setHaptics(true)
        #expect(s.model.hapticsOn && s.driver.savedGuest?.haptics == true)
        // A view with no model (a preview) plays as it always did.
        #expect(InterfaceFeedback.playing(.selection, in: nil) == .selection)
    }

    @Test func aGuestStoredBeforeTheSwitchReadsAsOn() throws {
        let old = Data(#"{"cores":2,"memoryMiB":1024,"capacityGiB":8,"microphone":false,"includeInBackups":false}"#.utf8)
        let guest = try JSONDecoder().decode(Guest.self, from: old)
        let model = AppModel(
            guest: guest, host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1)
        #expect(model.hapticsOn)
    }

    @Test func theHapticsSwitchNeedsNoDeviceToBeKept() {
        // The model keeps the choice whatever the device has; the row is what is absent there.
        let s = setup()
        #expect(!s.model.hapticsAvailable)
        s.model.setHaptics(false)
        #expect(s.model.guest?.haptics == false)
        #expect(s.driver.savedGuest?.haptics == false)
    }

    @Test func hapticsSymbolExistsOnTheOSFloor() {
        #expect(UIImage(systemName: HapticsRow.symbol) != nil)
    }
}
