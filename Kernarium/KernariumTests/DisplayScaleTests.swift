// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GuestHostCore
import Testing
@testable import Kernarium

/// Kernarium's one display switch is two of the scales the shared display
/// takes: Retina mode is the screen's own pixels, and its absence is one guest
/// pixel to a point.
@MainActor
struct DisplayScaleTests {
    private func launcher(retinaMode: Bool) -> EmulatorLauncher {
        EmulatorLauncher(configuration: EmulatorConfiguration(
            vCPUCount: 2, guestMemoryMiB: 1536, overlaySizeGiB: 8, androidDiskSizeGiB: 8,
            rootfsFilename: "x.erofs", retinaMode: retinaMode, autoResizeGuest: true, androidBootMode: .automatic
        ))
    }

    @Test func retinaModeIsNativeAndTheOtherIsPoints() {
        #expect(launcher(retinaMode: true).displayScale == .native)
        #expect(launcher(retinaMode: false).displayScale == .points)
    }

    @Test func theSwitchKeepsItsLayout() throws {
        let points = CGSize(width: 390, height: 700)
        let native = try #require(GuestDisplayLayout(
            scale: launcher(retinaMode: true).displayScale, points: points, nativeScale: 3))
        #expect((native.width, native.height, native.dotsPerInch) == (1170, 2100, 480))
        let off = try #require(GuestDisplayLayout(
            scale: launcher(retinaMode: false).displayScale, points: points, nativeScale: 3))
        #expect((off.width, off.height, off.dotsPerInch) == (390, 700, 160))
    }
}
