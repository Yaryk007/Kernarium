// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// What a booted guest sees of the cameras the production runtime states: the
/// vendor SKU the runtime composes from its source, the camera IDs Android
/// lists, and the camera lens cover on the side-key device, which only a
/// blocked session with cameras advertises. The runtime's source is the
/// synthetic phone (`KERNARIUM_CAMERA_SOURCE=synthetic`), so no real camera is opened, and the fake access source
/// of the Camera setting's hosted tests stands in for iOS's permission, which
/// the Simulator cannot grant. The guest boots twice, live and then blocked.
///
/// Runs alone with ANDARIUM_CAMERA_GUEST_TESTS=1,
/// ANDARIUM_GUEST_INSTANCE=camera and TEST_RUNNER_KERNARIUM_CAMERA_SOURCE=synthetic,
/// on the Simulator, an iPad or a Mac, with an isolated guest on an image that
/// has the camera HAL. The test installs nothing and deletes nothing: erase the
/// guest with an ANDARIUM_ERASE=1 launch (README).
///
/// The lens cover is read from the listing of the side-key device's
/// capabilities, which names SW_CAMERA_LENS_COVER where the runtime advertises
/// it; the switch is set for the whole boot when it is advertised
/// (`lensCoverAdvertised`). The test prints the listing and the sensor privacy
/// service's state on CAMERA_GUEST lines, for the reader of the log.
@MainActor
struct CameraGuestTests {
    private static let name = "camera"
    /// The camera IDs Android gives the synthetic phone's cameras
    /// (android/lineage-24/overlay/camera/hal/camera_ids.hpp): the zoom, the
    /// front, the wide, the ultra wide and the telephoto.
    private static let phoneCameraIDs = ["0", "1", "2", "3", "4"]
    /// The layout the runtime composes for that phone at the small tier every
    /// host gets, before a Bayer still has taught the device a lens's colour
    /// data (which adds RAW).
    private static let phoneLayout = "bf-small-flash-manual"
    /// The side keys ride on the keyboard device (darwin/device/input/virtio).
    private static let sideKeyDevice = "Kernarium Keyboard"

    /// The IDs `dumpsys media.camera` lists, in the order the service lists
    /// them (the parse the camera runner makes).
    private static func cameraIDs(_ dump: String) -> [String] {
        dump.split(separator: "\n").compactMap { line in
            let text = line.trimmingCharacters(in: .whitespaces)
            guard text.hasPrefix("Device "), let start = text.range(of: "maps to \""),
                let end = text.range(of: "\"", range: start.upperBound..<text.endIndex)
            else { return nil }
            return String(text[start.upperBound..<end.lowerBound])
        }
    }

    /// The capability listing of the side-key device, from `getevent -p -l`.
    private static func sideKeyListing(_ listing: String) -> String? {
        listing.components(separatedBy: "add device ").dropFirst().first { $0.contains("\"\(sideKeyDevice)\"") }
    }

    @Test(.timeLimit(.minutes(60)))
    func aBootedGuestSeesTheCamerasTheSessionStates() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard environment["ANDARIUM_CAMERA_GUEST_TESTS"] == "1" else { return }
        try #require(environment["ANDARIUM_GUEST_INSTANCE"] == Self.name)
        try #require(
            environment["KERNARIUM_CAMERA_SOURCE"] == "synthetic",
            "the runtime serves a synthetic camera only when the environment names it")
        for mode in [CameraMode.live, .blocked] {
            try await session(mode)
        }
    }

    private func session(_ mode: CameraMode) async throws {
        let access = FakeCamera(.authorized)
        try await withAndroidGuest(
            prepare: { model in
                // The runtime put the system's own source on the model; the
                // test plays the permission iOS would give. The microphone
                // is off, so the boot asks iOS for no second permission.
                model.setMicrophone(false)
                model.camera = CameraController(model: model, source: access)
                model.setCamera(mode == .live)
                try #require(model.cameraMode == mode, "the session is \(mode)")
            }
        ) { _, operations in
            let sku = try await operations.shell("getprop ro.boot.product.vendor.sku", limit: .seconds(30)).text
                .trimmingCharacters(in: .whitespacesAndNewlines)
            print("CAMERA_GUEST: \(mode): sku \(sku)")
            #expect(sku.hasPrefix(Self.phoneLayout), "\(mode): the vendor SKU is the phone's layout, not \(sku)")

            // The service lists its cameras once the provider has registered
            // them, a while after the boot.
            var ids: [String] = []
            let deadline = ContinuousClock.now + .seconds(120)
            while ContinuousClock.now < deadline {
                let dump = try await operations.shell("dumpsys media.camera", limit: .seconds(60)).text
                ids = Self.cameraIDs(dump)
                if ids.sorted() == Self.phoneCameraIDs { break }
                try await Task.sleep(for: .seconds(3))
            }
            print("CAMERA_GUEST: \(mode): camera IDs \(ids)")
            #expect(ids.sorted() == Self.phoneCameraIDs, "\(mode): the cameras Android lists")

            let listing = try await operations.shell("getevent -p -l", limit: .seconds(30)).text
            let sideKeys = try #require(Self.sideKeyListing(listing), "getevent lists no \(Self.sideKeyDevice): \(listing)")
            print("CAMERA_GUEST: \(mode): side keys \(sideKeys.replacingOccurrences(of: "\n", with: " / "))")
            #expect(
                sideKeys.contains("SW_CAMERA_LENS_COVER") == (mode == .blocked),
                "\(mode): only a blocked session with cameras advertises the lens cover")
            let privacy = try? await operations.shell("dumpsys sensor_privacy", limit: .seconds(30)).text
            print("CAMERA_GUEST: \(mode): sensor privacy \(privacy?.replacingOccurrences(of: "\n", with: " / ") ?? "-")")
        }
    }
}
