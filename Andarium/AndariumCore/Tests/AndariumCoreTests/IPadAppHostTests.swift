// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import AndariumCore

/// The iPad app on a Mac or on an Apple Vision Pro is a host two features leave
/// out. The system tells the Vision Pro from iOS 26.1; below it only the
/// hardware does, and a visionOS 26.0 that answers nothing must not read as an
/// iPad.
struct IPadAppHostTests {
    @Test func theSystemsAnswerIsTrustedWhereItGivesOne() {
        #expect(IPadAppHost(isiOSAppOnMac: false, visionAnswer: true, machine: "iPad16,3").isiOSAppOnVision)
        #expect(!IPadAppHost(isiOSAppOnMac: false, visionAnswer: false, machine: "iPad16,3").isiOSAppOnVision)
        #expect(!IPadAppHost(isiOSAppOnMac: false, visionAnswer: false, machine: "RealityDevice14,1").isiOSAppOnVision)
    }

    @Test func aSystemThatAnswersNothingIsToldByTheHardwareName() {
        func vision(_ machine: String) -> Bool {
            IPadAppHost(isiOSAppOnMac: false, visionAnswer: nil, machine: machine).isiOSAppOnVision
        }
        #expect(vision("RealityDevice14,1"), "an Apple Vision Pro on visionOS 26.0")
        #expect(!vision("iPad16,3"))
        #expect(!vision("iPhone16,2"))
        // The Simulator's own name, and a host with no name.
        #expect(!vision("arm64"))
        #expect(!vision(""))
    }

    @Test func aHomeScreenBelongsToAnIPhoneOrAnIPadOnly() {
        #expect(IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false).hasHomeScreen)
        #expect(!IPadAppHost(isiOSAppOnMac: true, isiOSAppOnVision: false).hasHomeScreen)
        #expect(!IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: true).hasHomeScreen)
        #expect(!IPadAppHost(isiOSAppOnMac: false, visionAnswer: nil, machine: "RealityDevice14,1").hasHomeScreen)
    }

    @Test func addToHomeScreenNeedsAHomeScreenAndTheTilesInShortcuts() {
        #expect(IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false).offersAddToHomeScreen)
        // An iOS before 27: a Home Screen, and no tile in Shortcuts to send the person to.
        #expect(!IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false, hasAppTilesInShortcuts: false).offersAddToHomeScreen)
        #expect(!IPadAppHost(isiOSAppOnMac: true, isiOSAppOnVision: false).offersAddToHomeScreen)
        #expect(!IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: true).offersAddToHomeScreen)
        #expect(!IPadAppHost(isiOSAppOnMac: false, visionAnswer: nil, machine: "RealityDevice14,1").offersAddToHomeScreen)
    }

    @Test func aVisionProOn26OffersNoCameraAndLooksForNone() {
        var looks = 0
        let host = IPadAppHost(isiOSAppOnMac: false, visionAnswer: nil, machine: "RealityDevice14,1")
        let offered = CameraAvailability.isAvailable(
            hasFrontCamera: { looks += 1; return true }(),
            isiOSAppOnMac: host.isiOSAppOnMac, isiOSAppOnVision: host.isiOSAppOnVision)
        #expect(!offered)
        #expect(looks == 0, "front-camera discovery is not reached")
    }
}
