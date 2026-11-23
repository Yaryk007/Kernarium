// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The session's Settings starts with what a new person came for: Install APK
/// and the file browser, in that order, above the statistics, Performance, the
/// developer options and the diagnostics. Each keeps its own condition; what is not
/// offered leaves the view starting with what comes next.
@MainActor
@Suite(.serialized)
struct SettingsOrderTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    /// The rows that stand for each section, as the view labels them, in the
    /// order Settings draws the sections.
    private enum Row: String, CaseIterable {
        case install = "Install APK"
        case files = "Internal storage"
        case stats = "CPU"
        case performance = "When hot"
        case developer = "ADB over network"
        case diagnostics = "Create bug report"
    }

    private func model(
        capabilities: Capabilities = .device, browser: Bool = true, mode: StartMode = .normal
    ) -> AppModel {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: capabilities, createRequirementBytes: 1
        )
        model.driver = StubDriver(plan: .normal)
        model.refreshPlan()
        if browser {
            model.fileBrowser = FileBrowserModel(backend: FileBrowserModelTests.ScriptedBackend(), center: NotificationCenter())
        }
        model.start(mode)
        model.finishBoot(mode == .recovery ? .recovery : .normal)
        return model
    }

    /// An accessibility element as Settings draws it.
    private typealias Element = (label: String, value: String?, frame: CGRect, button: Bool)

    private static func elements(in root: UIView) -> [Element] {
        var found: [Element] = []
        var seen = Set<ObjectIdentifier>()
        func walk(_ node: NSObject, depth: Int) {
            guard depth < 64, seen.insert(ObjectIdentifier(node)).inserted else { return }
            if let label = node.accessibilityLabel, !label.isEmpty {
                found.append((label, node.accessibilityValue, node.accessibilityFrame, node.accessibilityTraits.contains(.button)))
            }
            var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
            if let view = node as? UIView { children += view.subviews }
            for child in children { walk(child, depth: depth + 1) }
        }
        walk(root, depth: 0)
        return found
    }

    /// What Settings draws once it has settled, in a view tall enough to draw
    /// every section: each accessibility element with its label, frame and
    /// whether it is a button, top to bottom.
    private func settled(of model: AppModel) async throws -> [Element] {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let settings = UIHostingController(rootView: SessionSettingsSheet(large: true).environment(model))
        container.addChild(settings)
        settings.view.frame = CGRect(x: 0, y: 0, width: 390, height: 2600)
        container.view.addSubview(settings.view)
        settings.didMove(toParent: container)
        defer {
            settings.willMove(toParent: nil)
            settings.view.removeFromSuperview()
            settings.removeFromParent()
        }
        var found: [Element] = []
        var steady = 0
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline, steady < 4 {
            try await Task.sleep(for: .milliseconds(250))
            settings.view.setNeedsLayout()
            settings.view.layoutIfNeeded()
            let elements = Self.elements(in: settings.view).sorted { $0.frame.minY < $1.frame.minY }
            // The view has settled when it draws the same elements four times running.
            steady = !elements.isEmpty && elements.map(\.label) == found.map(\.label) ? steady + 1 : 0
            found = elements
        }
        return found
    }

    /// The rows Settings draws, top to bottom.
    private func rows(of model: AppModel) async throws -> [Row] {
        var found: [Row] = []
        for element in try await settled(of: model) {
            guard let row = Row.allCases.first(where: { element.label.contains($0.rawValue) }), !found.contains(row)
            else { continue }
            found.append(row)
        }
        return found
    }

    @Test func installApkAndTheFileBrowserComeFirstInThatOrder() async throws {
        let order = try await rows(of: model())
        #expect(order == [.install, .files, .stats, .performance, .developer, .diagnostics], "Settings draws \(order)")
    }

    @Test func theViewStartsWithWhatIsOfferedWhenASectionIsNot() async throws {
        var noInstall = Capabilities.device
        noInstall.apkInstall = false
        #expect(try await rows(of: model(capabilities: noInstall)) == [.files, .stats, .performance, .developer, .diagnostics])
        var noFiles = Capabilities.device
        noFiles.guestFiles = false
        #expect(try await rows(of: model(capabilities: noFiles)) == [.install, .stats, .performance, .developer, .diagnostics])
        // No browser available: the file browser's section is not drawn.
        #expect(try await rows(of: model(browser: false)) == [.install, .stats, .performance, .developer, .diagnostics])
        var neither = Capabilities.device
        neither.apkInstall = false
        neither.guestFiles = false
        #expect(try await rows(of: model(capabilities: neither)) == [.stats, .performance, .developer, .diagnostics])
    }

    /// Recovery runs no adb: neither section, nor the developer options and
    /// the diagnostics, are offered, as before.
    @Test func recoveryOffersNoneOfThem() async throws {
        let order = try await rows(of: model(mode: .recovery))
        #expect(order == [.stats], "Settings draws \(order)")
    }

    // MARK: Performance

    private let cools = "While this device is hot, games run at a lower frame rate so it can cool down."
    private let warning =
        "This device gets hotter and the battery drains faster. iOS may dim the screen, slow everything down or close Andarium."
    private let hotLimited = "This device is hot. The frame rate is limited until it cools."
    private let hotUnlimited = "This device is hot. The frame rate is not limited."

    /// The footnote of Performance in its four states, in the English the
    /// suite runs in: what the limit does while it is on, the warning while it
    /// is off, and while the device is held hot (the heat is stated to the
    /// model, no device is heated) what is happening to the frame rate.
    @Test func theFootnoteSaysWhatTheLimitDoesAndWarnsWhileItIsOff() async throws {
        let on = model()
        let limited = try await settled(of: on).map(\.label)
        #expect(limited.contains(cools), "limit on: \(limited)")
        #expect(!limited.contains(warning))

        let off = model()
        off.setHeatLimit(false)
        let unlimited = try await settled(of: off).map(\.label)
        #expect(unlimited.contains(warning), "limit off: \(unlimited)")
        #expect(!unlimited.contains(cools))
    }

    @Test func whileTheDeviceIsHeldHotTheFootnoteSaysWhatTheFrameRateIs() async throws {
        let on = model()
        on.deviceHeatChanged(hot: true)
        let limited = try await settled(of: on).map(\.label)
        #expect(limited.contains(hotLimited), "hot, limit on: \(limited)")
        #expect(!limited.contains(cools) && !limited.contains(warning))

        let off = model()
        off.setHeatLimit(false)
        off.deviceHeatChanged(hot: true)
        let unlimited = try await settled(of: off).map(\.label)
        #expect(unlimited.contains(hotUnlimited), "hot, limit off: \(unlimited)")
        #expect(unlimited.contains(warning), "the warning stays while the limit is off and the device is hot")
        #expect(!unlimited.contains(hotLimited))
    }

    /// "120 Hz" is on Home, in Performance (`HomePerformanceTests`), so that
    /// the guest's configuration is in one place: Settings, which a run opens,
    /// has no row of it on any screen, and its footnote is gone with it.
    @Test func settingsHasNoHundredAndTwentyHertzRowOnAnyScreen() async throws {
        for maximum in [60, 120] {
            let model = model()
            model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: maximum)
            let labels = try await settled(of: model).map(\.label)
            #expect(labels.contains("When hot"), "Performance is still there at \(maximum) Hz: \(labels)")
            #expect(
                !labels.contains { $0.hasPrefix("120 Hz") || $0.contains("Smoother motion") },
                "Settings still has the 120 Hz row at \(maximum) Hz: \(labels)")
        }
    }

    @Test func theRowReadsItsNameAndItsValueAndFollowsTheChoice() async throws {
        let model = model()
        let on = try await settled(of: model)
        let row = try #require(on.first { $0.label == "When hot" }, "no When hot row")
        #expect(row.value == "Limit frame rate", "the row says \(row.value ?? "nothing")")
        model.setHeatLimit(false)
        let off = try await settled(of: model)
        let after = try #require(off.first { $0.label == "When hot" })
        #expect(after.value == "Full frame rate", "the row says \(after.value ?? "nothing")")
    }
}
