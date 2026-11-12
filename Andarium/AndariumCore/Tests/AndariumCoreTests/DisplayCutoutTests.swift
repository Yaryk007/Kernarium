// SPDX-License-Identifier: GPL-2.0-or-later

@testable import AndariumCore
import Foundation
import Testing

struct DisplayCutoutTests {
    @Test func onEnablesTheTallCutoutAlone() {
        #expect(DisplayCutoutEmulation.command(on: true)
            == "cmd overlay enable-exclusive --user 0 --category com.android.internal.display.cutout.emulation.tall")
    }

    @Test func offDisablesOnlyTheTallCutout() {
        #expect(DisplayCutoutEmulation.command(on: false)
            == "cmd overlay disable --user 0 com.android.internal.display.cutout.emulation.tall")
    }

    @Test func onlyAZeroExitSucceeds() {
        #expect(DisplayCutoutEmulation.succeeded(ShellResult(exitCode: 0)))
        #expect(!DisplayCutoutEmulation.succeeded(ShellResult(exitCode: 1)))
        #expect(!DisplayCutoutEmulation.succeeded(ShellResult()))
    }

    @Test func theOptionsKeepTheNewSettings() throws {
        let options = DisplayOptions(chosenRetina: nil, nativeResolution: true, displayCutout: true)
        let decoded = try JSONDecoder().decode(DisplayOptions.self, from: JSONEncoder().encode(options))
        #expect(decoded == options)
        // A form from before the settings has neither.
        let old = try JSONDecoder().decode(DisplayOptions.self, from: Data(#"{"retina":true,"chosen":true}"#.utf8))
        #expect(old == DisplayOptions(chosenRetina: true))
    }
}
