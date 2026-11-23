// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// What shows behind and around the guest's display in the light appearance:
/// black, wherever SwiftUI has not drawn (the area outside the safe area
/// before a layout, a rotation, a window resize). The scene's root view and
/// its window are the backing there, and follow the system, so they are white
/// in the light appearance unless the guest's display paints them black. The
/// phases that are not the guest's keep following the system.
@MainActor
@Suite(.serialized)
struct GuestBackingTests {
    private let light = UITraitCollection(userInterfaceStyle: .light)

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private func model(phase: Phase) -> AppModel {
        let host = HostDevice(
            name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        model.launchApplied = true
        model.driver = StubDriver(plan: .normal)
        model.phase = phase
        return model
    }

    /// The colour as the light appearance resolves it, in 0...255 and
    /// premultiplied by nothing: what a person sees of a backing.
    private func rgba(_ color: UIColor?) -> [Int]? {
        guard let color else { return nil }
        var red: CGFloat = 0, green: CGFloat = 0, blue: CGFloat = 0, alpha: CGFloat = 0
        guard color.resolvedColor(with: light).getRed(&red, green: &green, blue: &blue, alpha: &alpha) else { return nil }
        return [red, green, blue, alpha].map { Int(($0 * 255).rounded()) }
    }

    private func isBlack(_ color: UIColor?) -> Bool { rgba(color) == [0, 0, 0, 255] }

    /// The window as drawn, read at `points` (in points): red, green, blue and
    /// alpha of each. A backing nobody painted has no alpha.
    private func pixels(of window: UIWindow, at points: [CGPoint]) -> [[Int]] {
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = false
        let image = UIGraphicsImageRenderer(bounds: window.bounds, format: format).image { _ in
            window.drawHierarchy(in: window.bounds, afterScreenUpdates: true)
        }
        guard let cgImage = image.cgImage else { return [] }
        let width = cgImage.width, height = cgImage.height
        var bytes = [UInt8](repeating: 0, count: width * height * 4)
        let drawn = bytes.withUnsafeMutableBytes { buffer -> Bool in
            guard
                let context = CGContext(
                    data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                    space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            context.draw(cgImage, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        guard drawn else { return [] }
        return points.map { point in
            let x = min(max(Int(point.x), 0), width - 1), y = min(max(Int(point.y), 0), height - 1)
            let offset = (y * width + x) * 4
            return (0..<4).map { Int(bytes[offset + $0]) }
        }
    }

    /// The corners and the middle of the edges, 2 points inside, and the
    /// middle of the top and bottom safe-area insets where a device has them.
    private func outsideSamples(of window: UIWindow) -> [CGPoint] {
        let size = window.bounds.size
        var points = [
            CGPoint(x: 2, y: 2), CGPoint(x: size.width - 3, y: 2),
            CGPoint(x: 2, y: size.height - 3), CGPoint(x: size.width - 3, y: size.height - 3),
            CGPoint(x: size.width / 2, y: 2), CGPoint(x: size.width / 2, y: size.height - 3),
        ]
        let insets = window.safeAreaInsets
        if insets.top > 4 { points.append(CGPoint(x: size.width / 4, y: insets.top / 2)) }
        if insets.bottom > 4 { points.append(CGPoint(x: size.width / 4, y: size.height - insets.bottom / 2)) }
        return points
    }

    @Test func theGuestsPhasesPaintTheRootAndTheWindowBlackInTheLightAppearance() async throws {
        let model = model(phase: .home)
        let lease = try await HostWindow.take()
        let style = lease.window.overrideUserInterfaceStyle
        let windowBacking = lease.window.backgroundColor
        defer {
            lease.window.overrideUserInterfaceStyle = style
            lease.give()
        }
        lease.window.overrideUserInterfaceStyle = .light
        // The root has nothing of SwiftUI's in it: what shows is the backing.
        let controller = AndariumHostingController(rootView: Color.clear, model: model)
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        try #require(lease.window.traitCollection.userInterfaceStyle == .light)

        // Home follows the system: whatever it was before, and not black.
        let homeView = controller.view.backgroundColor
        #expect(!isBlack(homeView), "the root's backing on Home is \(String(describing: rgba(homeView)))")
        #expect(!isBlack(lease.window.backgroundColor))
        #expect(lease.window.backgroundColor == windowBacking)

        for phase in [Phase.running(.normal), .booting(.normal), .running(.recovery)] {
            model.phase = phase
            #expect(
                await eventually { isBlack(controller.view.backgroundColor) && isBlack(lease.window.backgroundColor) },
                "\(phase): the root's backing is \(String(describing: rgba(controller.view.backgroundColor))), the window's \(String(describing: rgba(lease.window.backgroundColor)))"
            )
            let seen = pixels(of: lease.window, at: outsideSamples(of: lease.window))
            #expect(!seen.isEmpty && seen.allSatisfy { $0 == [0, 0, 0, 255] }, "\(phase): \(seen)")
            // Another layout pass leaves both as they are: exactly the black
            // the controller compares against, so it assigns nothing.
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            #expect(controller.view.backgroundColor == UIColor.black && lease.window.backgroundColor == UIColor.black, "\(phase)")
        }

        // Out of the guest's phases the screens are the system's again, as they were.
        for phase in [Phase.failed, .home] {
            model.phase = .running(.normal)
            #expect(await eventually { isBlack(controller.view.backgroundColor) })
            model.phase = phase
            #expect(
                await eventually { !isBlack(controller.view.backgroundColor) && !isBlack(lease.window.backgroundColor) },
                "\(phase): the root's backing is \(String(describing: rgba(controller.view.backgroundColor)))")
            #expect(rgba(controller.view.backgroundColor) == rgba(homeView))
            #expect(lease.window.backgroundColor == windowBacking)
        }
    }

    @Test func aSessionThatIsAlreadyRunningWhenTheRootAppearsIsBlackAtOnce() async throws {
        let model = model(phase: .running(.normal))
        let lease = try await HostWindow.take()
        let style = lease.window.overrideUserInterfaceStyle
        defer {
            lease.window.overrideUserInterfaceStyle = style
            lease.give()
        }
        lease.window.overrideUserInterfaceStyle = .light
        let controller = AndariumHostingController(rootView: Color.clear, model: model)
        lease.window.rootViewController = controller
        #expect(
            await eventually { isBlack(controller.view.backgroundColor) && isBlack(lease.window.backgroundColor) },
            "the root's backing is \(String(describing: rgba(controller.view.backgroundColor))), the window's \(String(describing: rgba(lease.window.backgroundColor)))")
    }

    @Test func theRealSessionIsBlackAroundItsDisplayAndKeepsTheStatusBarHidden() async throws {
        let model = model(phase: .home)
        let lease = try await HostWindow.take()
        let style = lease.window.overrideUserInterfaceStyle
        defer {
            lease.window.overrideUserInterfaceStyle = style
            lease.give()
        }
        lease.window.overrideUserInterfaceStyle = .light
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        model.phase = .running(.normal)
        #expect(await eventually { isBlack(controller.view.backgroundColor) && lease.scene.statusBarManager?.isStatusBarHidden == true })
        // The session's cross-fade from Home has ended.
        try await Task.sleep(for: .seconds(1))
        let seen = pixels(of: lease.window, at: outsideSamples(of: lease.window))
        #expect(!seen.isEmpty && seen.allSatisfy { $0[3] == 255 && $0[0] < 3 && $0[1] < 3 && $0[2] < 3 }, "\(seen)")
        // The status bar is hidden, so no text of it is left on black in the
        // light appearance.
        #expect(lease.scene.statusBarManager?.isStatusBarHidden == true)
    }
}
