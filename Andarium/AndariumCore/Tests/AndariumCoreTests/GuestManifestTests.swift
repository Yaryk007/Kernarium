// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing

struct GuestManifestTests {
    private let r28 = "20261115-los24-r28"

    private func manifest(
        family: String = "lineage-24-mainline-6.18-4k", release: String = "20261115-los24-r28", abi: Int = 26,
        page: Int = 4096, kernel: String = "6.18.60-g51ccd1d883a7", major: Int = 17, base: String? = nil
    ) throws -> Data {
        var root: [String: Any] = [
            "product_family": family, "release_id": release,
            "kernel": ["abi_version": abi, "page_size": page, "release": kernel] as [String: Any],
            "android": ["major": major, "abi_list": ["arm64-v8a", "armeabi-v7a", "armeabi"]] as [String: Any],
        ]
        root["base_release_id"] = base
        return try JSONSerialization.data(withJSONObject: root)
    }

    /// The values r28's own manifest has.
    @Test func theOfficialReleaseFits() throws {
        #expect(GuestManifest.fits(try manifest(), as: .official(release: r28)))
        #expect(GuestManifest.fits(try manifest(family: "lineage-24"), as: .official(release: r28)))
    }

    @Test func anotherReleaseOrFormatDoesNot() throws {
        for other in [
            try manifest(release: "20261121-los24-r29"), try manifest(family: "lineage-240"),
            try manifest(family: "lineage-24-gapps"), try manifest(abi: 29), try manifest(page: 16384),
            try manifest(kernel: "6.12.1"), try manifest(major: 16), Data("{}".utf8),
        ] {
            #expect(!GuestManifest.fits(other, as: .official(release: r28)))
        }
    }

    @Test func gappsNamesTheReleaseItWasMadeFrom() throws {
        let gapps = try manifest(family: "lineage-24-gapps", release: "r28-gapps", base: r28)
        #expect(GuestManifest.fits(gapps, as: .gapps(base: r28)))
        #expect(!GuestManifest.fits(gapps, as: .official(release: r28)))
        #expect(!GuestManifest.fits(try manifest(family: "lineage-24-gapps", release: "x"), as: .gapps(base: r28)))
    }

    @Test func aCustomGuestNeedsItsFamilyAndInterface() throws {
        let skin = try manifest(family: "one-ui", release: "x", abi: 29)
        #expect(GuestManifest.fits(skin, as: .other(family: "one-ui")))
        #expect(GuestManifest.fits(skin, as: .other(family: nil)))
        #expect(!GuestManifest.fits(skin, as: .other(family: "hyperos")))
        #expect(!GuestManifest.fits(try manifest(), as: .other(family: nil)))
    }
}
