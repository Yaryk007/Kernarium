// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct PhoneHardwareTests {
    @Test(arguments: [
        ("iPhone10,3", PhoneHardware.Housing.wideNotch), ("iPhone13,4", .wideNotch),
        ("iPhone14,5", .notch), ("iPhone17,5", .notch), ("iPhone18,5", .notch),
        ("iPhone15,2", .island), ("iPhone18,4", .island),
        ("iPhone19,2", .smallIsland), ("iPhone19,7", .smallIsland),
    ])
    func eachGenerationHasItsHousing(machine: String, housing: PhoneHardware.Housing) {
        #expect(PhoneHardware(machine: machine)?.housing == housing)
    }

    @Test func aPhoneWithAHomeButtonHasNoHousing() throws {
        #expect(try #require(PhoneHardware(machine: "iPhone14,6")).housing == nil)
    }

    @Test(arguments: ["", "arm64", "iPad16,3", "iPhone99,1"])
    func anythingElseIsNoKnownPhone(machine: String) {
        #expect(PhoneHardware(machine: machine) == nil)
    }

    @Test func thePropertiesNameThePhoneAndHideThePaintedCutoutWithTheNotchSpace() throws {
        let phone = try #require(PhoneHardware(machine: "iPhone18,1"))
        let plain = phone.properties(notchSpace: false)
        #expect(plain == """
            ro.product.manufacturer=Apple
            ro.product.model=iPhone 17 Pro
            ro.soc.manufacturer=Apple
            ro.soc.model=A19 Pro

            """)
        #expect(phone.properties(notchSpace: true) == plain + "debug.disable_screen_decorations=true\n")
        #expect(phone.deviceNameCommand.contains("settings put global device_name 'iPhone 17 Pro'"))
    }

    @Test func theNotchSpaceIsOffUntilItIsKeptOn() throws {
        let name = "andarium-notch-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: name))
        defer { defaults.removePersistentDomain(forName: name) }
        let preference = NotchSpacePreference(defaults: defaults)
        #expect(!preference.isOn)
        preference.keep(true)
        #expect(preference.isOn)
    }

    @Test func theClockMovesFurtherInTheMoreRoomThereIs() {
        func padding(_ housing: PhoneHardware.Housing) -> Int {
            DisplayCutoutEmulation(housing: housing, height: 59).padding
        }
        #expect(padding(.wideNotch) < padding(.notch))
        #expect(padding(.notch) == padding(.island))
        #expect(padding(.island) < padding(.smallIsland))
    }

    @Test func rootMakesOverlaysForThisPhone() {
        let command = DisplayCutoutEmulation(housing: .island, height: 62).rootCommand
        #expect(command.hasPrefix("su -c '"))
        #expect(command.contains("0x03 \"M -63,0 L -63,62 L 63,62 L 63,0 Z @dp\""))
        #expect(command.contains("android:dimen/rounded_corner_content_padding 0x05 0x1801 "))
        #expect(command.contains("settings put global andarium_notch 1-126-62-24'"))
        #expect(command.count { $0 == "'" } == 2)
    }

    @Test func aStartWithoutRootUsesThemOrTheStockCutout() {
        let cutout = DisplayCutoutEmulation(housing: .notch, height: 47)
        #expect(cutout.enableCommand.contains("= '1-162-47-24' ]"))
        #expect(cutout.enableCommand.hasSuffix("--category \(DisplayCutoutEmulation.stockOverlay)"))
        #expect(!cutout.enableCommand.contains("su -c"))
        #expect(DisplayCutoutEmulation.disableCommand.contains("com.android.shell:AndariumNotchPadding"))
        #expect(DisplayCutoutEmulation(housing: .notch, height: 0).height == 24)
    }
}
