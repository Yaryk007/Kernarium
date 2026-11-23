// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

// The Camera setting's saved choice, the session's mode, and the two
// statements the screens make about it (the notice and the rotation).
struct CameraTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    @Test func theChoiceIsOffForANewGuestAndForOneStoredBefore() throws {
        #expect(!Guest.fresh(on: host).camera)
        let before = Data(#"{"cores":2,"memoryMiB":1024,"capacityGiB":8,"microphone":true,"includeInBackups":false}"#.utf8)
        #expect(try JSONDecoder().decode(Guest.self, from: before).camera == false)
    }

    @Test func theChoiceIsKeptAsChosenThroughSaveAndErase() throws {
        var guest = Guest.fresh(on: host)
        guest.camera = true
        let saved = try JSONEncoder().encode(guest)
        #expect(try JSONDecoder().decode(Guest.self, from: saved).camera)
        #expect(try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest.erased)).camera)
        guest.camera = false
        #expect(try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest)).camera == false)
    }

    @Test func theChoiceDoesNotDisturbTheOthersAStoredGuestHas() throws {
        var guest = Guest.fresh(on: host)
        guest.microphone = true
        guest.location = true
        guest.haptics = false
        guest.camera = true
        let decoded = try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest))
        #expect(decoded.microphone && decoded.location && !decoded.haptics && decoded.camera)
    }

    @Test func aSessionIsLiveOnlyWhenTheSwitchIsOnAndIOSAllowsCapture() {
        let access: [CameraAccess] = [.unavailable, .notDetermined, .restricted, .denied, .authorized]
        for choice in [false, true] {
            for state in access {
                let expected: CameraMode = choice && state == .authorized ? .live : .blocked
                #expect(CameraSession.mode(choice: choice, access: state) == expected)
            }
        }
    }

    @Test func accessSaysWhatTheRowCanOffer() {
        let all: [CameraAccess] = [.unavailable, .notDetermined, .restricted, .denied, .authorized]
        #expect(all.filter(\.refuses) == [.restricted, .denied])
        #expect(all.filter(\.offered) == [.notDetermined, .restricted, .denied, .authorized])
    }

    @Test func onlyOtherAppsSharingTheScreenAndABlockedOpenRaiseANotice() {
        let all: [CameraCapture] = [
            .idle, .streaming, .interrupted, .interruptedByMultitasking, .interruptedBySystemPressure, .blockedInUse,
        ]
        #expect(all.compactMap { CameraNotice.raised(by: $0) } == [.pausedForOtherApps, .cameraOff])
        #expect(CameraNotice.raised(by: .interruptedByMultitasking) == .pausedForOtherApps)
        #expect(CameraNotice.raised(by: .blockedInUse) == .cameraOff)
    }

    private let t0 = ContinuousClock.Instant.now

    private func at(_ seconds: Int) -> ContinuousClock.Instant { t0 + .seconds(seconds) }

    @Test func theCameraOffNoticeFollowsTheOpen() {
        var state = CameraNoticeState()
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.blockedInUse, at: at(0))
        #expect(state.showing(canTurnOn: true) == .cameraOff)
        #expect(!state.lingers)
        // The runtime says nothing more for the same open, and a repeat changes nothing.
        let shown = state
        state.captureChanged(.blockedInUse, at: at(5))
        #expect(state == shown)
    }

    @Test func theCameraOffNoticeStaysTwoSecondsAfterTheGuestClosesItsCameraAndThenGoes() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse, at: at(10))
        state.captureChanged(.idle, at: at(20))
        #expect(state.lingers)
        #expect(state.showing(canTurnOn: true) == .cameraOff)
        state.expire(at: at(21))
        #expect(state.showing(canTurnOn: true) == .cameraOff, "one second after the close it is still up")
        state.expire(at: at(22))
        #expect(state.showing(canTurnOn: true) == nil && !state.lingers, "two seconds after, it goes")
        #expect(CameraNoticeState.lingerFor == .seconds(2))
    }

    @Test func aCameraThatOpensAgainWithinTheTwoSecondsIsTheSameShowing() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse, at: at(0))
        state.captureChanged(.idle, at: at(10))
        state.captureChanged(.blockedInUse, at: at(11))
        #expect(state.showing(canTurnOn: true) == .cameraOff && !state.lingers, "nothing is left to expire")
        state.expire(at: at(60))
        #expect(state.showing(canTurnOn: true) == .cameraOff, "the open that came back is still open")
    }

    @Test func anOpenAfterTheNoticeWentShowsItAgain() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse, at: at(0))
        state.captureChanged(.idle, at: at(10))
        state.expire(at: at(13))
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.blockedInUse, at: at(70))
        #expect(state.showing(canTurnOn: true) == .cameraOff)
    }

    @Test func aReportThatComesAfterTheTwoSecondsEndsTheStayItself() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse, at: at(0))
        state.captureChanged(.idle, at: at(10))
        // The report is late and nothing expired the notice: it is a new open, not the same one.
        state.captureChanged(.blockedInUse, at: at(13))
        #expect(state.showing(canTurnOn: true) == .cameraOff && !state.lingers)
        state.captureChanged(.idle, at: at(20))
        state.captureChanged(.idle, at: at(30))
        #expect(state.showing(canTurnOn: true) == nil, "a repeated close at 30 s finds the stay over")
    }

    @Test func aDismissedCameraOffNoticeStaysAwayForTheRestOfTheRun() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse, at: at(0))
        state.dismiss()
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.blockedInUse, at: at(1))
        #expect(state.showing(canTurnOn: true) == nil, "the same open does not raise it again")
        state.captureChanged(.idle, at: at(2))
        state.captureChanged(.blockedInUse, at: at(3))
        #expect(state.showing(canTurnOn: true) == nil, "a reopen within a few seconds does not")
        state.captureChanged(.idle, at: at(4))
        state.expire(at: at(30))
        state.captureChanged(.blockedInUse, at: at(90))
        #expect(state.showing(canTurnOn: true) == nil, "nor does an open a minute later")
        state.captureChanged(.interruptedByMultitasking, at: at(100))
        state.captureChanged(.idle, at: at(101))
        state.captureChanged(.blockedInUse, at: at(102))
        #expect(state.showing(canTurnOn: true) == nil, "nor does the other notice coming and going")
    }

    @Test func aNoticeDismissedWhileItStaysAfterTheCloseIsDismissed() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse, at: at(0))
        state.captureChanged(.idle, at: at(10))
        state.dismiss()
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.blockedInUse, at: at(11))
        #expect(state.showing(canTurnOn: true) == nil)
    }

    @Test func dismissingWithNothingUpChangesNothingForTheNextOpen() {
        var state = CameraNoticeState()
        state.dismiss()
        state.captureChanged(.blockedInUse, at: at(0))
        #expect(state.showing(canTurnOn: true) == .cameraOff)
        state.captureChanged(.idle, at: at(5))
        state.expire(at: at(8))
        state.dismiss()
        state.captureChanged(.blockedInUse, at: at(9))
        #expect(state.showing(canTurnOn: true) == .cameraOff)
    }

    @Test func theEndOfARunForgetsADismissalAndWhatTheGuestHadOpen() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse, at: at(0))
        state.dismiss()
        state.captureChanged(.idle, at: at(1))
        state.runEnded()
        #expect(state == CameraNoticeState() && state.capture == .idle && !state.lingers)
        state.captureChanged(.blockedInUse, at: at(2))
        #expect(state.showing(canTurnOn: true) == .cameraOff, "the next run shows it again")
        state.runEnded()
        #expect(state.showing(canTurnOn: true) == nil)
    }

    @Test func theCameraOffNoticeWaitsForARowThatCanBeTurnedOn() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse, at: at(0))
        #expect(state.showing(canTurnOn: false) == nil, "a device with no camera to share, or a switch already on, has nothing to tell")
        #expect(state.showing(canTurnOn: true) == .cameraOff)
        state.captureChanged(.idle, at: at(1))
        #expect(state.showing(canTurnOn: false) == nil && state.showing(canTurnOn: true) == .cameraOff)
        // The other notice does not depend on the row.
        state.captureChanged(.interruptedByMultitasking, at: at(2))
        #expect(state.showing(canTurnOn: false) == .pausedForOtherApps)
    }

    @Test func aDismissedNoticeOfOneKindDoesNotHideTheOtherKind() {
        var state = CameraNoticeState()
        state.captureChanged(.interruptedByMultitasking, at: at(0))
        state.dismiss()
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.blockedInUse, at: at(1))
        #expect(state.showing(canTurnOn: true) == .cameraOff, "dismissing the paused notice does not hide Camera off")
        state.dismiss()
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.streaming, at: at(3))
        state.captureChanged(.interruptedByMultitasking, at: at(4))
        #expect(state.showing(canTurnOn: true) == .pausedForOtherApps, "dismissing Camera off does not hide the paused notice")
        state.captureChanged(.streaming, at: at(5))
        state.captureChanged(.blockedInUse, at: at(6))
        #expect(state.showing(canTurnOn: true) == nil, "and the paused notice coming and going does not bring Camera off back")
    }

    @Test func aDismissedPausedNoticeStaysGoneUntilTheNextInterruption() {
        var state = CameraNoticeState()
        state.captureChanged(.interruptedByMultitasking, at: at(0))
        state.dismiss()
        state.captureChanged(.interruptedByMultitasking, at: at(1))
        #expect(state.showing(canTurnOn: true) == nil, "the same interruption does not raise it again")
        state.captureChanged(.streaming, at: at(2))
        state.captureChanged(.interruptedByMultitasking, at: at(3))
        #expect(state.showing(canTurnOn: true) == .pausedForOtherApps, "a new one does")
    }

    @Test func anInterruptionOtherThanSharingTheScreenKeepsTheStateButRaisesNothing() {
        var state = CameraNoticeState()
        for (second, capture) in [CameraCapture.streaming, .interrupted, .interruptedBySystemPressure].enumerated() {
            state.captureChanged(capture, at: at(second))
            #expect(state.capture == capture && state.showing(canTurnOn: true) == nil && !state.lingers)
        }
    }

    @Test func theRowOffersACameraByTheLayoutsRuleAndTheIPadAppOnAMacOrAVisionProNone() {
        func offered(front: Bool, mac: Bool = false, vision: Bool = false) -> Bool {
            CameraAvailability.isAvailable(hasFrontCamera: front, isiOSAppOnMac: mac, isiOSAppOnVision: vision)
        }
        // Elsewhere: a built-in front camera, the rule the guest's layout follows.
        #expect(offered(front: true))
        #expect(!offered(front: false))
        // The iPad app on a Mac is offered none, whatever the Mac reports. The
        // decision is one constant.
        #expect(!CameraAvailability.offeredToIOSAppOnMac)
        #expect(!offered(front: true, mac: true))
        #expect(!offered(front: false, mac: true))
        // The iPad app on a Vision Pro is offered none either, by a constant of its own.
        #expect(!CameraAvailability.offeredToIOSAppOnVision)
        #expect(!offered(front: true, vision: true))
        #expect(!offered(front: false, vision: true))
        #expect(!offered(front: true, mac: true, vision: true))
        // An unavailable camera is not offered, and the person has no question to answer.
        #expect(!CameraAccess.unavailable.offered && !CameraAccess.unavailable.refuses)
    }

    @Test func theIPadAppOnAMacOrAVisionProLooksForNoCamera() {
        var looks = 0
        func front(_ found: Bool) -> Bool {
            looks += 1
            return found
        }
        func available(mac: Bool, vision: Bool) -> Bool {
            CameraAvailability.isAvailable(hasFrontCamera: front(true), isiOSAppOnMac: mac, isiOSAppOnVision: vision)
        }
        // The calls are made outside `#expect`, which evaluates the arguments it quotes.
        let onMac = available(mac: true, vision: false)
        let onVision = available(mac: false, vision: true)
        #expect(!onMac && !onVision)
        #expect(looks == 0, "a host that is offered no camera has none discovered")
        let elsewhere = available(mac: false, vision: false)
        #expect(elsewhere)
        #expect(looks == 1, "any other host looks once")
    }

    @Test func theGuestIsToldTheDeviceBodysTurnOnlyWhereTheHostOwnsTheRotation() {
        let axes: [MotionSample.Axes] = [.portrait, .landscapeLeft, .portraitUpsideDown, .landscapeRight]
        for interface in axes {
            #expect(CameraRotation.degrees(policy: .guest, interface: interface) == 0)
        }
        // The clockwise turn of the device from portrait: with the home button
        // on the left the device has turned a quarter clockwise.
        #expect(axes.map { CameraRotation.degrees(policy: .host, interface: $0) } == [0, 90, 180, 270])
        // The two landscape orientations are opposite turns.
        let left = CameraRotation.degrees(policy: .host, interface: .landscapeLeft)
        let right = CameraRotation.degrees(policy: .host, interface: .landscapeRight)
        #expect((left + right) % 360 == 0)
        #expect(axes.allSatisfy { CameraRotation.degrees(policy: .host, interface: $0) % 90 == 0 })
    }
}
