// SPDX-License-Identifier: GPL-2.0-or-later

@testable import AndariumCore
import Foundation
import Testing

struct DisplayCutoutTests {
    @Test func onMakesRoomWithoutPaintingTheCutout() {
        let command = DisplayCutoutEmulation.command(on: true)
        #expect(command.hasPrefix(
            "cmd overlay enable-exclusive --user 0 --category com.android.internal.display.cutout.emulation.tall && "))
        #expect(command.contains(
            "cmd overlay fabricate --target android --name AndariumCutoutClear android:bool/config_fillMainBuiltInDisplayCutout 0x12 0"))
        #expect(command.hasSuffix(
            "cmd overlay set-priority --user 0 com.android.shell:AndariumCutoutClear highest"))
    }

    @Test func offTurnsBothOff() {
        let command = DisplayCutoutEmulation.command(on: false)
        #expect(command.hasPrefix("cmd overlay disable --user 0 com.android.internal.display.cutout.emulation.tall && "))
        #expect(command.contains("cmd overlay disable --user 0 com.android.shell:AndariumCutoutClear"))
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
        let old = try JSONDecoder().decode(DisplayOptions.self, from: Data(#"{"retina":true,"chosen":true}"#.utf8))
        #expect(old == DisplayOptions(chosenRetina: true))
    }
}
