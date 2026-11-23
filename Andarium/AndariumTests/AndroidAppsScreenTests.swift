// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// What a request to open an Android app shows on the real screens in the
/// application's window: the card on the boot screen while Android starts for
/// it, the card above Home's notice while it waits for an answer, the capsule
/// while the app is being opened, and the banner when it could not be, each
/// with the buttons it offers and what they do. A stub stands for the runtime
/// and the tool is the played one.
///
/// SwiftUI publishes its accessibility elements only while application
/// accessibility is on (the Simulator preference `ApplicationAccessibilityEnabled`);
/// a run without it finds none and the tests say so.
@Suite(.serialized)
@MainActor
final class AndroidAppsScreenTests {
    init() async { await AndroidAppsHubLock.shared.acquire() }
    deinit { Task { await AndroidAppsHubLock.shared.release() } }

    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 41_000_000_000)
    private let comet = ScriptedAndroidApps.installedApps[0]

    private struct Fixture {
        let model: AppModel
        let driver: StubDriver
        let scripted: ScriptedAndroidApps
    }

    private func rig(phase: Phase, plan: StartPlan = .normal, host override: HostDevice? = nil) -> Fixture {
        let used = override ?? host
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (9)")
        let model = AppModel(
            guest: Guest.fresh(on: used), host: used, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: plan)
        model.driver = driver
        model.plan = plan
        model.phase = phase
        model.session.storageReady = true
        let scripted = ScriptedAndroidApps()
        scripted.tool.delay = .milliseconds(1)
        scripted.readyDelay = .seconds(3600)
        model.scriptedAndroidApps = scripted
        AndroidAppsHub.shared.configure(
            directory: FileManager.default.temporaryDirectory.appendingPathComponent("andarium-apps-screen-\(UUID().uuidString)"))
        // The host the screens are about is an iPhone of iOS 27, whatever system the tests run on.
        model.playedPadAppHost = IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false)
        AndroidAppsHub.shared.attach(model)
        model.androidApps.store = AndroidAppsHub.shared.store
        model.androidApps.adoptCache()
        return Fixture(model: model, driver: driver, scripted: scripted)
    }

    // MARK: Finding what is on screen

    private func node(in root: NSObject, depth: Int = 0, where match: (NSObject) -> Bool) -> NSObject? {
        if match(root) { return root }
        guard depth < 64 else { return nil }
        var children = (root.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = root as? UIView { children += view.subviews }
        for child in children {
            if let found = node(in: child, depth: depth + 1, where: match) { return found }
        }
        return nil
    }

    private func labelled(_ prefix: String, in root: NSObject) -> NSObject? {
        node(in: root) { $0.accessibilityLabel?.hasPrefix(prefix) == true }
    }

    private func button(_ label: String, in root: NSObject) -> NSObject? {
        node(in: root) { $0.accessibilityLabel == label && $0.accessibilityTraits.contains(.button) }
    }

    private func eventually(_ condition: () -> Bool, seconds: Double = 8) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private func root(_ fixture: Fixture, lease: HostWindow.Lease) -> UIViewController {
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(fixture.model),
            model: fixture.model)
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    // MARK: Home, waiting for an answer

    @Test func anUnfinishedUpdateShowsTheWaitingCardAboveTheNoticeAndDontOpenDropsTheRequest() async throws {
        let fixture = rig(
            phase: .home, plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = root(fixture, lease: lease)
        fixture.model.requestAndroidApp(comet.component, label: "Comet Hop")
        guard await eventually({ labelled("\u{201C}Comet Hop\u{201D} is waiting", in: controller.view) != nil }) else {
            Issue.record("the card was not found: is application accessibility on in this Simulator?")
            return
        }
        let card = try #require(labelled("\u{201C}Comet Hop\u{201D} is waiting", in: controller.view))
        #expect(card.accessibilityLabel?.contains("The app opens once Android has started.") == true)
        // The notice keeps its own buttons, and the card sits above it.
        let notice = try #require(labelled("System image update didn\u{2019}t finish", in: controller.view))
        #expect(card.accessibilityFrame.maxY <= notice.accessibilityFrame.minY + 1, "the card is above the notice")
        let drop = try #require(button("Don\u{2019}t open", in: controller.view))
        #expect(drop.accessibilityActivate())
        #expect(await eventually { fixture.model.androidApps.open.pending == nil })
        #expect(await eventually { labelled("\u{201C}Comet Hop\u{201D} is waiting", in: controller.view) == nil })
        #expect(fixture.driver.begun.isEmpty)
    }

    @Test func tooLittleSpaceShowsTheWaitingCardThatNamesTheDevice() async throws {
        var tight = host
        tight.freeStorageBytes = 1_000_000
        let fixture = rig(phase: .home, host: tight)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = root(fixture, lease: lease)
        fixture.model.requestAndroidApp(comet.component, label: "Comet Hop")
        guard await eventually({ labelled("\u{201C}Comet Hop\u{201D} is waiting for space", in: controller.view) != nil }) else {
            Issue.record("the card was not found: is application accessibility on in this Simulator?")
            return
        }
        let card = try #require(labelled("\u{201C}Comet Hop\u{201D} is waiting for space", in: controller.view))
        #expect(card.accessibilityLabel?.contains("Free up space on this iPhone, then tap Start.") == true)
    }

    // MARK: Android starting

    @Test func theBootScreenShowsTheCardAndDontOpenLeavesTheStartAlone() async throws {
        let fixture = rig(phase: .home)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = root(fixture, lease: lease)
        fixture.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(fixture.model.phase == .booting(.normal))
        guard await eventually({ labelled("Opening \u{201C}Comet Hop\u{201D}", in: controller.view) != nil }) else {
            Issue.record("the card was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(labelled("It opens as soon as Android has started.", in: controller.view) != nil)
        let drop = try #require(button("Don\u{2019}t open", in: controller.view))
        #expect(drop.accessibilityActivate())
        #expect(await eventually { fixture.model.androidApps.open.pending == nil })
        #expect(fixture.model.phase == .booting(.normal), "Android keeps starting")
        #expect(await eventually { labelled("Opening \u{201C}Comet Hop\u{201D}", in: controller.view) == nil })
        // The boot ends: nothing is opened.
        fixture.model.phase = .running(.normal)
        fixture.model.finishBoot(.normal)
        try await Task.sleep(for: .milliseconds(300))
        #expect(fixture.scripted.tool.opened.isEmpty)
    }

    // MARK: Android running

    @Test func theCapsuleSaysTheAppIsBeingOpenedAndGoesWhenItIs() async throws {
        let fixture = rig(phase: .running(.normal))
        fixture.scripted.tool.delay = .milliseconds(1)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = root(fixture, lease: lease)
        fixture.model.requestAndroidApp(comet.component, label: "Comet Hop")
        guard await eventually({ labelled("Opening \u{201C}Comet Hop\u{201D}\u{2026}", in: controller.view) != nil }) else {
            Issue.record("the capsule was not found: is application accessibility on in this Simulator?")
            return
        }
        fixture.scripted.readyDelay = .milliseconds(1)
        // The wait that was started keeps its long delay; a newer request opens at once.
        fixture.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { fixture.scripted.tool.opened == [comet.component] })
        #expect(await eventually { labelled("Opening \u{201C}Comet Hop\u{201D}\u{2026}", in: controller.view) == nil })
    }

    @Test func aFailureShowsTheBannerWithTryAgainAndDismiss() async throws {
        let fixture = rig(phase: .running(.normal))
        fixture.scripted.readyDelay = .milliseconds(1)
        fixture.scripted.tool.answer(open: comet.component, .failed)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = root(fixture, lease: lease)
        fixture.model.requestAndroidApp(comet.component, label: "Comet Hop")
        guard await eventually({ labelled("\u{201C}Comet Hop\u{201D} didn\u{2019}t open", in: controller.view) != nil }) else {
            Issue.record("the banner was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(labelled("Android took too long.", in: controller.view) != nil)
        // Try Again asks again and the banner goes.
        fixture.scripted.tool.answer(open: comet.component, .opened)
        let again = try #require(button("Try again", in: controller.view))
        #expect(again.accessibilityActivate())
        #expect(await eventually { fixture.scripted.tool.opened.count == 2 })
        #expect(await eventually { labelled("\u{201C}Comet Hop\u{201D} didn\u{2019}t open", in: controller.view) == nil })
    }

    @Test func theBannerOfAnAppThatIsGoneHasNoTryAgainAndDismissClosesIt() async throws {
        let fixture = rig(phase: .running(.normal))
        fixture.scripted.readyDelay = .milliseconds(1)
        fixture.scripted.tool.remove(comet.id)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = root(fixture, lease: lease)
        fixture.model.requestAndroidApp(comet.component, label: "Comet Hop")
        guard await eventually({ labelled("\u{201C}Comet Hop\u{201D} isn\u{2019}t in Android", in: controller.view) != nil }) else {
            Issue.record("the banner was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(button("Try again", in: controller.view) == nil)
        let close = try #require(button("Dismiss", in: controller.view))
        #expect(close.accessibilityActivate())
        #expect(await eventually { fixture.model.androidApps.open.banner == nil })
    }
}
