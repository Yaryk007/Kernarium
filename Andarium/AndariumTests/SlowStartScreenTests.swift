// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The boot screen's notice for a slow start, in the application: when the
/// model puts it up (by a clock the test moves, so nothing waits three
/// minutes), what Stop does, that Stop is the screen's one control, and that
/// the notice goes with the boot.
@Suite(.serialized)
@MainActor
struct SlowStartScreenTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 41_000_000_000
    )
    private let t0: TimeInterval = 1_000

    /// A clock the test moves.
    private final class Clock {
        var now: TimeInterval
        init(_ now: TimeInterval) { self.now = now }
        func advance(_ seconds: TimeInterval) { now += seconds }
    }

    private struct Fixture {
        let model: AppModel
        let driver: StubDriver
        let clock: Clock
    }

    private func fixture(driver attached: Bool = true) -> Fixture {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (9)")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        if attached { model.driver = driver }
        let clock = Clock(t0)
        model.clock = { clock.now }
        return Fixture(model: model, driver: driver, clock: clock)
    }

    /// A start under way, as `start` leaves it.
    private func begun(_ fixture: Fixture, kind: BootKind = .normal) {
        fixture.model.phase = .booting(kind)
        fixture.model.slowStart.begin(at: fixture.clock.now)
    }

    // MARK: When

    @Test func theStartBeginsTheTimeAndThreeMinutesPutTheNoticeUp() {
        let fixture = fixture()
        fixture.model.start(.normal)
        #expect(fixture.driver.begun.count == 1)
        #expect(fixture.model.slowStart.began == t0)
        fixture.clock.advance(179)
        #expect(!fixture.model.updateSlowStart() && !fixture.model.slowStart.showing)
        #expect(fixture.model.bootElapsedSeconds() == 179)
        fixture.clock.advance(1)
        #expect(fixture.model.updateSlowStart() && fixture.model.slowStart.showing)
        #expect(!fixture.model.updateSlowStart(), "the notice came twice")
    }

    @Test func noNoticeOutsideAStart() {
        let fixture = fixture()
        // Home, a session and a failed run have no start to call slow.
        for phase in [Phase.home, .running(.normal), .failed] {
            fixture.model.phase = phase
            fixture.clock.advance(1_000)
            #expect(!fixture.model.updateSlowStart(), "\(phase)")
            #expect(!fixture.model.slowStart.showing)
        }
    }

    @Test func aRestartBeforeTheBootFinishedKeepsTheStartsTime() {
        let fixture = fixture()
        fixture.model.start(.normal)
        fixture.clock.advance(100)
        fixture.model.restart()
        #expect(fixture.model.phase == .booting(.restart))
        #expect(fixture.model.slowStart.began == t0 && fixture.model.slowStart.restarts == 1)
        fixture.clock.advance(80)
        #expect(fixture.model.updateSlowStart(), "a slow loop of restarts must reach the notice")
    }

    @Test func aRestartOfARunningSessionIsABootOfItsOwn() {
        let fixture = fixture()
        fixture.model.start(.normal)
        fixture.clock.advance(30)
        fixture.model.finishBoot(.normal)
        #expect(fixture.model.slowStart == SlowStart())
        fixture.clock.advance(4_000)
        fixture.model.restart()
        #expect(fixture.model.slowStart.began == fixture.clock.now)
        fixture.clock.advance(179)
        #expect(!fixture.model.updateSlowStart())
    }

    @Test func theNoticeGoesWhenTheBootCompletesOrTheRunEnds() {
        let fixture = fixture()
        fixture.model.start(.normal)
        fixture.clock.advance(200)
        #expect(fixture.model.updateSlowStart())
        fixture.model.finishBoot(.normal)
        #expect(fixture.model.slowStart == SlowStart() && fixture.model.phase == .running(.normal))

        fixture.model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        fixture.model.start(.normal)
        fixture.clock.advance(200)
        #expect(fixture.model.updateSlowStart())
        fixture.model.runEnded(RunEnd(result: -5), uptimeSeconds: 200)
        #expect(fixture.model.slowStart == SlowStart() && fixture.model.phase == .failed)
    }

    // MARK: Stop

    /// The boot line never comes and the guest is otherwise up: the tracker
    /// has seen the screen start and nothing after it. The notice comes from
    /// the clock, and Stop reaches the runtime.
    @Test func stopWorksWhenTheBootLineNeverComes() {
        let fixture = fixture()
        fixture.model.start(.normal)
        for milestone in [BootMilestone.kernelRunning, .androidStarting, .screenStarting] {
            fixture.model.bootTracker.reach(milestone, atSeconds: 20)
        }
        #expect(!fixture.model.bootTracker.isReady)
        fixture.clock.advance(181)
        #expect(fixture.model.updateSlowStart())
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 1 && fixture.model.slowStart.stopping)
        // Asked again, it is not asked of the runtime again.
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 1)
        // The runtime's run returns and the application says whose choice it was.
        fixture.model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 181)
        #expect(fixture.model.phase == .failed)
        #expect(fixture.model.runEnd.stoppedByUser && !fixture.model.runEnd.poweredOff)
        #expect(fixture.model.runReport.hasSuffix("Virtual machine ended: it was stopped from the boot screen after 3:01"))
        #expect(fixture.model.slowStart == SlowStart())
    }

    @Test func stopDuringAnUpdateKeepsTheUpdatePending() {
        let fixture = fixture()
        fixture.model.plan = .update(from: "r1", to: "r2")
        fixture.driver.plan = .update(from: "r1", to: "r2")
        fixture.model.start(.normal)
        fixture.clock.advance(200)
        fixture.model.updateSlowStart()
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 1)
        fixture.model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 200)
        // The update has not finished: what the files say is still an update,
        // which the next launch offers again (`InstanceRules`).
        #expect(fixture.model.plan == .update(from: "r1", to: "r2"))
    }

    @Test func stopAsksNothingOutsideAStart() {
        let fixture = fixture()
        fixture.model.stopStart()
        fixture.model.phase = .running(.normal)
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 0)
    }

    @Test func thePreviewEndsTheStartItself() {
        let fixture = fixture(driver: false)
        begun(fixture)
        fixture.clock.advance(181)
        fixture.model.updateSlowStart()
        fixture.model.stopStart()
        #expect(fixture.model.phase == .failed && fixture.model.runEnd.stoppedByUser)
    }

    /// A start the runtime has not reached (the display is not laid out, or
    /// the disk is being prepared) ends in the driver, at Home, and the
    /// preparation then launches nothing.
    @Test func stopBeforeTheRuntimeRunsEndsAtHomeAndLaunchesNothing() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let release = try SystemFolder.release(in: systemDirectory)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: release.identity, releaseDirectory: systemDirectory)
        let fixture = fixture(driver: false)
        let runtime = LiveRuntime(
            model: fixture.model, storage: storage,
            marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        fixture.model.driver = runtime
        // The runtime counts by its own clock; the test moves this one.
        fixture.model.clock = { [clock = fixture.clock] in clock.now }
        let guest = try #require(fixture.model.guest)
        begun(fixture)
        runtime.begin(guest: guest, mode: .normal, kind: .normal)
        fixture.clock.advance(181)
        fixture.model.updateSlowStart()
        fixture.model.stopStart()
        #expect(fixture.model.phase == .home)
        #expect(fixture.model.runEnd.stoppedByUser && fixture.model.runEnd.poweredOff)
        // A display that lays out now finds no start to run.
        autoreleasepool {
            let view = GuestMetalView(frame: .zero, device: nil)
            runtime.viewDidLayout(view, GuestDisplayGeometry(pixelSize: CGSize(width: 100, height: 100), dotsPerInch: 160))
        }
        #expect(runtime.view == nil)
    }

    // MARK: The screen

    private func button(labelled label: String, in node: NSObject, depth: Int = 0) -> NSObject? {
        if node.accessibilityLabel == label, node.accessibilityTraits.contains(.button) { return node }
        guard depth < 64 else { return nil }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children {
            if let found = button(labelled: label, in: child, depth: depth + 1) { return found }
        }
        return nil
    }

    /// The boot screen in the application's window. SwiftUI publishes its
    /// accessibility elements only while application accessibility is on (the
    /// Simulator preference `ApplicationAccessibilityEnabled`); a run without
    /// it finds none and the test says so.
    private func screen(
        _ fixture: Fixture, in lease: HostWindow.Lease, finding labels: [String],
        contentSize: UIContentSizeCategory? = nil
    ) async throws -> (UIViewController, [String: NSObject]) {
        let controller = UIHostingController(rootView: BootView(kind: .normal).environment(fixture.model))
        if let contentSize { controller.traitOverrides.preferredContentSizeCategory = contentSize }
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        var found: [String: NSObject] = [:]
        let deadline = ContinuousClock.now + .seconds(5)
        while found.count < labels.count, ContinuousClock.now < deadline {
            for label in labels where found[label] == nil {
                if let element = button(labelled: label, in: controller.view) { found[label] = element }
            }
            if found.count < labels.count { try await Task.sleep(for: .milliseconds(50)) }
        }
        return (controller, found)
    }

    @Test func theNoticeHasStopAndItReachesTheRuntime() async throws {
        let fixture = fixture()
        begun(fixture)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (controller, found) = try await screen(fixture, in: lease, finding: ["Stop"])
        guard let stop = found["Stop"] else {
            Issue.record("Stop was not found: is application accessibility on in this Simulator?")
            return
        }
        // Stop asks first: nothing reaches the runtime until the dialog's answer.
        #expect(stop.accessibilityActivate())
        #expect(fixture.model.confirmingStop)
        #expect(fixture.driver.guestStops == 0)
        // The dialog is an alert that offers Stop and Cancel, with the sentence
        // that says how to start again: a popover would show no Cancel.
        var alert: UIAlertController?
        let deadline = ContinuousClock.now + .seconds(5)
        while alert == nil, ContinuousClock.now < deadline {
            alert = controller.presentedViewController as? UIAlertController
            if alert == nil { try await Task.sleep(for: .milliseconds(50)) }
        }
        let shown = try #require(alert, "Stop did not present an alert")
        #expect(shown.preferredStyle == .alert)
        #expect(shown.title == "Stop the virtual machine?")
        #expect(shown.message == "To start it again, close Andarium in the App Switcher and open it again.")
        #expect(shown.actions.map(\.title) == ["Stop", "Cancel"])
        #expect(shown.actions.map(\.style) == [.destructive, .cancel])
        // Cancel leaves the start as it was.
        fixture.model.confirmingStop = false
        #expect(fixture.driver.guestStops == 0 && !fixture.model.slowStart.stopping)
        // The answer Stop reaches the runtime, once.
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 1 && !fixture.model.confirmingStop)
        // Chosen, it says so and takes no second tap.
        let (_, after) = try await screen(fixture, in: lease, finding: ["Stopping\u{2026}"])
        #expect(after["Stopping\u{2026}"] != nil)
        fixture.model.askToStop()
        #expect(!fixture.model.confirmingStop && fixture.driver.guestStops == 1)
    }

    /// UIKit presents the alert, and the sheet the notice opens, in the
    /// appearance of the window: dark while the boot's dark screen is in it
    /// when the system's is light, and the system's over Recovery's own, and
    /// the window gets its own appearance back when the boot ends. A light
    /// alert over the dark screen has its buttons unreadable.
    @Test(arguments: [BootKind.normal, .recovery]) func theAlertAndTheSheetTakeTheAppearanceOfTheScreenUnderThem(
        kind: BootKind
    ) async throws {
        let fixture = fixture()
        begun(fixture, kind: kind)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        let style = lease.window.overrideUserInterfaceStyle
        defer {
            lease.window.overrideUserInterfaceStyle = style
            lease.give()
        }
        lease.window.overrideUserInterfaceStyle = .light
        let host = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(fixture.model),
            model: fixture.model)
        lease.window.rootViewController = host
        host.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(300))
        let expected: UIUserInterfaceStyle = kind == .recovery ? .light : .dark
        func presented(_ present: () -> Void, gone: () -> Void) async throws -> UIUserInterfaceStyle {
            present()
            let deadline = ContinuousClock.now + .seconds(8)
            while host.presentedViewController == nil, ContinuousClock.now < deadline {
                try await Task.sleep(for: .milliseconds(50))
            }
            let shown = try #require(host.presentedViewController, "nothing was presented")
            let appearance = shown.traitCollection.userInterfaceStyle
            gone()
            let closing = ContinuousClock.now + .seconds(8)
            while host.presentedViewController != nil, ContinuousClock.now < closing {
                try await Task.sleep(for: .milliseconds(50))
            }
            return appearance
        }
        let alert = try await presented(
            { fixture.model.askToStop() }, gone: { fixture.model.confirmingStop = false })
        #expect(alert == expected, "the Stop alert is not in the screen's appearance")
        #expect(lease.window.overrideUserInterfaceStyle == (kind == .recovery ? .light : .dark))
        // What follows the boot is in the appearance the window had.
        fixture.model.finishBoot(kind)
        let leaving = ContinuousClock.now + .seconds(5)
        while lease.window.overrideUserInterfaceStyle != .light, ContinuousClock.now < leaving {
            try await Task.sleep(for: .milliseconds(50))
        }
        #expect(lease.window.overrideUserInterfaceStyle == .light, "the window kept the boot screen's appearance")
        #expect(host.traitCollection.userInterfaceStyle == .light)
    }

    @Test func theDialogGoesWithTheStartItWasAskedFor() {
        let fixture = fixture()
        // Nothing asks outside a start, or before the notice's start has begun.
        fixture.model.askToStop()
        #expect(!fixture.model.confirmingStop)
        begun(fixture)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        fixture.model.askToStop()
        #expect(fixture.model.confirmingStop)
        // The boot completes under it: the dialog is gone with the notice.
        fixture.model.finishBoot(.normal)
        #expect(!fixture.model.confirmingStop && fixture.driver.guestStops == 0)
    }

    /// The notice's time is a clock of its own on the screen: it moves with
    /// the model's clock without anything else changing.
    @Test func theNoticeCountsOnWhileItIsOnScreen() async throws {
        let fixture = fixture()
        begun(fixture)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIHostingController(rootView: BootView(kind: .normal).environment(fixture.model))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        func label(containing part: String, in node: NSObject, depth: Int = 0) -> String? {
            if let text = node.accessibilityLabel, text.contains(part) { return text }
            guard depth < 64 else { return nil }
            var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
            if let view = node as? UIView { children += view.subviews }
            for child in children {
                if let found = label(containing: part, in: child, depth: depth + 1) { return found }
            }
            return nil
        }
        func settle(_ part: String) async throws -> Bool {
            let deadline = ContinuousClock.now + .seconds(5)
            while ContinuousClock.now < deadline {
                if label(containing: part, in: controller.view) != nil { return true }
                try await Task.sleep(for: .milliseconds(100))
            }
            return false
        }
        guard try await settle("3 minutes, 5 seconds") else {
            Issue.record("the notice's time was not found: is application accessibility on in this Simulator?")
            return
        }
        fixture.clock.advance(5)
        #expect(try await settle("3 minutes, 10 seconds"), "the notice's time stayed where it was")
    }

    // MARK: A first start

    @Test func aFirstStartAndTheFirstAfterAnUpdateGetTheLaterNoticeAndAPlainTitle() {
        for plan in [StartPlan.firstStart, .update(from: "r1", to: "r2")] {
            let fixture = fixture()
            fixture.model.plan = plan
            fixture.driver.plan = plan
            fixture.model.start(.normal)
            #expect(fixture.model.slowStart.longByNature)
            fixture.clock.advance(300)
            #expect(!fixture.model.updateSlowStart(), "\(plan): the ordinary threshold applied")
            fixture.clock.advance(180)
            #expect(fixture.model.updateSlowStart() && fixture.model.slowStart.showing, "\(plan): no notice at 480 s")
        }
        #expect(BootView.slowTitle(longByNature: true) == "Starting is taking a long time")
        #expect(BootView.slowTitle(longByNature: false) == "Starting is taking longer than usual")
        // Recovery and the rest keep the ordinary three minutes.
        let fixture = fixture()
        fixture.model.start(.recovery)
        #expect(!fixture.model.slowStart.longByNature)
    }

    @Test func aBootBeforeThreeMinutesShowsNoNotice() async throws {
        let fixture = fixture()
        begun(fixture)
        fixture.clock.advance(120)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIHostingController(rootView: BootView(kind: .normal).environment(fixture.model))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(300))
        #expect(button(labelled: "Stop", in: controller.view) == nil)
    }

    // MARK: The buttons' size

    /// The application's ordinary button: the end screen's Copy report is 44
    /// points tall at the default text size, the minimum touch target.
    private static let ordinaryButtonHeight: CGFloat = 44

    /// Every standard content size, the smallest first.
    private static let contentSizes: [UIContentSizeCategory] = [
        .extraSmall, .small, .medium, .large, .extraLarge, .extraExtraLarge, .extraExtraExtraLarge,
    ]

    private func noticeFixture() -> Fixture {
        let fixture = fixture()
        begun(fixture)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        return fixture
    }

    /// Stop alone: as
    /// tall as the application's ordinary button at the default text size, no
    /// less at any size, and as wide as its label and no more: it is not one bar
    /// across the notice.
    private func checkStopAlone(_ fixture: Fixture) async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        for size in Self.contentSizes {
            let (controller, found) = try await screen(fixture, in: lease, finding: ["Stop"], contentSize: size)
            guard let stop = found["Stop"] else {
                Issue.record("Stop was not found at \(size.rawValue): is application accessibility on in this Simulator?")
                return
            }
            let frame = stop.accessibilityFrame
            let context = "Stop alone at \(size.rawValue): \(frame)"
            if size == .large {
                #expect(abs(frame.height - Self.ordinaryButtonHeight) < 0.5, "not the ordinary height: \(context)")
                #expect(frame.width < controller.view.bounds.width * 0.6, "a bar across the notice: \(context)")
                #expect(frame.width >= 100, "a sliver: \(context)")
            } else {
                #expect(frame.height >= Self.ordinaryButtonHeight - 0.5, "under the touch target: \(context)")
            }
        }
    }

    @Test func stopAloneIsTheOrdinaryButtonAndNotABar() async throws {
        try await checkStopAlone(noticeFixture())
    }
}
