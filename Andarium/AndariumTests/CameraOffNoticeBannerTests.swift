// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The notice over the running guest that Camera is off, drawn in a window as
/// the session draws it. SwiftUI publishes its accessibility elements only
/// while application accessibility is on (the Simulator preference
/// `ApplicationAccessibilityEnabled`, as `Kernarium/scripts/
/// run-ios-simulator-tests.sh` sets it); a run without it finds none and the
/// tests fail, for they check nothing then.
@MainActor
struct CameraOffNoticeBannerTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    /// A running guest with Camera off on a device that has a camera, whose guest
    /// has just opened one.
    private func model() -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1)
        model.cameraAccess = .notDetermined
        model.phase = .running(.normal)
        model.cameraCaptureChanged(.blockedInUse)
        return model
    }

    private func find(in node: NSObject, depth: Int = 0, _ match: (NSObject) -> Bool) -> NSObject? {
        if match(node) { return node }
        guard depth < 64 else { return nil }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children {
            if let found = find(in: child, depth: depth + 1, match) { return found }
        }
        return nil
    }

    private struct Parts {
        var title: NSObject
        var detail: NSObject
        var dismiss: NSObject
    }

    /// The notice's words and its button in `controller`, waiting for SwiftUI to publish them.
    private func parts(in controller: UIViewController) async throws -> Parts? {
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            let title = find(in: controller.view) { $0.accessibilityLabel == CameraWords.offTitle }
            let detail = find(in: controller.view) { $0.accessibilityLabel == CameraWords.offDetail }
            let dismiss = find(in: controller.view) {
                $0.accessibilityLabel == NetworkWords.dismissLabel && $0.accessibilityTraits.contains(.button)
            }
            if let title, let detail, let dismiss { return Parts(title: title, detail: detail, dismiss: dismiss) }
            try await Task.sleep(for: .milliseconds(50))
        }
        return nil
    }

    private func hosted(_ model: AppModel, width: CGFloat, in lease: HostWindow.Lease) -> UIViewController {
        let controller = UIHostingController(
            rootView: SessionNoticesOverlay().environment(model).dynamicTypeSize(.large).frame(width: width))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    @Test func theNoticeSaysTheCameraIsOffAndHowToTurnItOnAndItsDismissReachesTheModel() async throws {
        let model = model()
        #expect(model.cameraNoticeKind == .cameraOff)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, in: lease)
        guard let found = try await parts(in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(found.title.accessibilityLabel == "Camera is off")
        #expect(found.detail.accessibilityLabel == "Shut down the virtual machine, then turn on Camera in Capabilities.")
        #expect(found.dismiss.accessibilityActivate())
        #expect(model.cameraNoticeKind == nil, "dismissing it puts it away")
        #expect(model.cameraCapture == .blockedInUse, "the guest still has the camera open")
        // The dismissal holds for the run: the guest closing the camera and opening it again does not bring it back.
        model.cameraCaptureChanged(.idle)
        model.cameraCaptureChanged(.blockedInUse)
        #expect(model.cameraNoticeKind == nil)
        #expect(await titleGone(in: controller), "the notice is off the screen")
    }

    /// An undismissed notice stays on the screen for the stay after the
    /// guest's close and goes when it ends, off the screen and not only out of
    /// the model.
    @Test func theNoticeLeavesTheScreenWhenItsStayAfterTheGuestClosesTheCameraEnds() async throws {
        let model = model()
        let pace = CameraNoticePace()
        pace.attach(to: model)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, in: lease)
        let shown = try #require(await parts(in: controller), "The notice was not found: is application accessibility on in this Simulator?")
        #expect(shown.title.accessibilityLabel == "Camera is off")
        model.cameraCaptureChanged(.idle)
        #expect(await CameraNoticePace.until { pace.isWaiting })
        try await Task.sleep(for: .milliseconds(300))
        controller.view.layoutIfNeeded()
        #expect(find(in: controller.view) { $0.accessibilityLabel == CameraWords.offTitle } != nil, "still up during the stay")
        pace.release()
        #expect(await titleGone(in: controller), "gone when the stay ends")
        #expect(model.cameraNoticeKind == nil)
    }

    /// Waits for the notice's title to leave the hosted screen, for at most five seconds.
    private func titleGone(in controller: UIViewController) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            if find(in: controller.view, { $0.accessibilityLabel == CameraWords.offTitle }) == nil { return true }
            try? await Task.sleep(for: .milliseconds(50))
        }
        return false
    }

    /// The test that reads an empty screen as success proves nothing when
    /// accessibility is off, so the same hosted notice is found first, with a
    /// device that has a camera, and then the device loses it.
    @Test func theNoticeShowsNothingForADeviceWithoutACamera() async throws {
        let model = model()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, in: lease)
        _ = try #require(await parts(in: controller), "The notice was not found: is application accessibility on in this Simulator?")
        model.cameraAccess = .unavailable
        #expect(await titleGone(in: controller), "the notice goes with the camera")
        #expect(model.cameraNoticeKind == nil)
    }

    /// Nothing of the notice leaves a narrow screen's width.
    @Test func theNoticeFitsANarrowScreen() async throws {
        let model = model()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let width: CGFloat = 320
        let controller = hosted(model, width: width, in: lease)
        guard let found = try await parts(in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        let title = found.title.accessibilityFrame
        let detail = found.detail.accessibilityFrame
        let dismiss = found.dismiss.accessibilityFrame
        // The column the notice is laid out in, in the window's coordinates: the hosted view is
        // centred in the window, so the frames sit inside it and not only no wider than it.
        let column = controller.view.convert(controller.view.bounds, to: nil)
            .insetBy(dx: (controller.view.bounds.width - width) / 2, dy: 0)
        for frame in [title, detail, dismiss] {
            #expect(frame.width > 0 && frame.width <= width, "inside the width: \(frame)")
            #expect(
                frame.minX >= column.minX - 0.5 && frame.maxX <= column.maxX + 0.5,
                "inside the column \(column): \(frame)")
        }
        #expect(detail.minY >= title.maxY - 1, "the detail is under the title")
        let union = title.union(detail).union(dismiss)
        #expect(union.width <= width, "the notice is no wider than the screen: \(union)")
    }
}
