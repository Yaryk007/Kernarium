// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The notice that iOS denies Andarium the network in the model: the path
/// watcher runs only while the application is active and has a guest, a
/// denial shows once on Home and in the session until it is dismissed or the
/// network is allowed, no network at all shows nothing, and the runtime is
/// told which denial is in force. What a path means is `NetworkPathReport`'s
/// (tested in `andarium-core`); nothing here waits on a clock or the system.
@MainActor
struct NetworkNoticeFlowTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private let denied = NetworkPathReport(status: .unsatisfied, reason: .cellularDenied)
    private let wifiDenied = NetworkPathReport(status: .unsatisfied, reason: .wifiDenied)
    private let noSignal = NetworkPathReport(status: .unsatisfied, reason: .notAvailable)
    private let allowed = NetworkPathReport(status: .satisfied)

    private func model(
        guest: Bool = true, source: ScriptedNetworkPathSource, driver: StubDriver? = nil
    ) -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: guest ? Guest.fresh(on: host) : nil, host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        model.driver = driver
        model.networkWatcher = NetworkAccessWatcher(source: source)
        return model
    }

    @Test func theSourceRunsOnlyWhileTheApplicationIsActiveWithAGuest() {
        let source = ScriptedNetworkPathSource()
        let model = model(source: source)
        #expect(source.isStarted && source.starts == 1)
        model.applicationActive = false
        #expect(!source.isStarted && source.stops == 1)
        model.applicationActive = true
        #expect(source.isStarted && source.starts == 2)
        // Erasing the guest leaves nothing to deny a network to.
        model.guest = nil
        #expect(!source.isStarted)
        model.guest = Guest.fresh(on: host)
        #expect(source.isStarted)
    }

    @Test func noGuestNoWatchAtAll() {
        let source = ScriptedNetworkPathSource()
        let model = model(guest: false, source: source)
        #expect(source.starts == 0 && !source.isStarted)
        #expect(model.networkDenialShowing == nil)
    }

    @Test func aDenialShowsOnHomeAndInTheSessionAndAnAllowedPathTakesItAway() {
        let source = ScriptedNetworkPathSource()
        let model = model(source: source)
        #expect(model.networkDenialShowing == nil)
        source.send(denied)
        #expect(model.networkDenialShowing == .cellular)
        model.phase = .running(.normal)
        #expect(model.networkDenialShowing == .cellular, "the session shows the same notice")
        source.send(allowed)
        #expect(model.networkDenialShowing == nil)
    }

    @Test func noNetworkAtAllIsNotTheNotice() {
        let source = ScriptedNetworkPathSource()
        let model = model(source: source)
        source.send(noSignal)
        #expect(model.networkDenialShowing == nil)
        source.send(NetworkPathReport(status: .unsatisfied, reason: .localNetworkDenied))
        #expect(model.networkDenialShowing == nil)
        source.send(NetworkPathReport(status: .unsatisfied, reason: .vpnInactive))
        #expect(model.networkDenialShowing == nil)
    }

    @Test func dismissingHoldsForTheSameCauseUntilTheNetworkIsAllowed() {
        let source = ScriptedNetworkPathSource()
        let model = model(source: source)
        source.send(denied)
        model.dismissNetworkNotice()
        #expect(model.networkDenialShowing == nil)
        // Reported again, and after a stop and start of the watch.
        source.send(denied)
        model.applicationActive = false
        model.applicationActive = true
        source.send(denied)
        #expect(model.networkDenialShowing == nil, "a dismissed notice came back without the network being allowed")
        // The other cause is another notice.
        source.send(wifiDenied)
        #expect(model.networkDenialShowing == .wifi)
        // Allowed, then denied again: a new notice.
        source.send(allowed)
        source.send(denied)
        #expect(model.networkDenialShowing == .cellular)
    }

    @Test func theRuntimeIsToldWhichDenialIsInForceDismissedOrNot() {
        let source = ScriptedNetworkPathSource()
        let driver = StubDriver(plan: .normal)
        let model = model(source: source, driver: driver)
        // A driver set at once hears the state, which is none.
        #expect(driver.networkNotices.last == .some(nil))
        source.send(denied)
        #expect(driver.networkNotices.last == .some(.cellular))
        model.dismissNetworkNotice()
        #expect(driver.networkNotices.last == .some(.cellular), "a dismissal is not the end of the denial")
        source.send(noSignal)
        #expect(driver.networkNotices.last == .some(nil))
        source.send(wifiDenied)
        #expect(driver.networkNotices.last == .some(.wifi))
        source.send(allowed)
        #expect(driver.networkNotices.last == .some(nil))
    }

    @Test func aRunSaysTheDenialAgainOnceItHasLoadedTheRuntime() {
        let source = ScriptedNetworkPathSource()
        let driver = StubDriver(plan: .normal)
        let model = model(source: source, driver: driver)
        source.send(denied)
        let before = driver.networkNotices.count
        model.reportNetworkDenial()
        #expect(driver.networkNotices.count == before + 1 && driver.networkNotices.last == .some(.cellular))
    }

    @Test func aModelWithNoWatcherNeverShowsTheNotice() {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .preview,
            createRequirementBytes: 1
        )
        #expect(model.networkWatcher == nil && model.networkDenialShowing == nil)
        model.dismissNetworkNotice()
        model.applicationActive = false
        #expect(model.networkDenialShowing == nil)
    }

    @Test func thePreviewHookPlaysADenialTheWatcherNeverSaw() {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .preview,
            createRequirementBytes: 1
        )
        model.previewNetworkNotice(.wifi)
        #expect(model.networkDenialShowing == .wifi)
    }

    // MARK: The words and the screens

    @Test func theWordsNameWhereToAllowTheNetworkAndNoProduct() {
        for denial in [NetworkDenial.cellular, .wifi] {
            let title = NetworkWords.title(for: denial)
            let text = NetworkWords.text(for: denial)
            #expect(text.contains("Settings") && text.contains("Andarium"))
            #expect(!title.isEmpty && title.count <= 40, "\(title)")
            for name in ["Google", "Pixel", "Gemini", "Chrome", "Play"] {
                #expect(!title.contains(name) && !text.contains(name))
            }
        }
        #expect(NetworkWords.title(for: .cellular) != NetworkWords.title(for: .wifi))
        #expect(NetworkWords.text(for: .cellular).contains("Cellular Data"))
        #expect(NetworkWords.text(for: .wifi).contains(NetworkWords.wifiName))
    }

    // MARK: The session banner

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

    /// The session's overlay in a window, with the banner for a denial the
    /// path reported. SwiftUI publishes its accessibility elements only while
    /// application accessibility is on (the Simulator preference
    /// `ApplicationAccessibilityEnabled`, as `Kernarium/scripts/
    /// run-ios-simulator-tests.sh` sets it); a run without it finds none and
    /// the test fails, for it checks nothing then.
    @Test func theBannerOffersSettingsAndDismissAndTheyReachTheModel() async throws {
        let source = ScriptedNetworkPathSource()
        let model = model(source: source)
        model.phase = .running(.normal)
        var opened = 0
        model.openSettings = { opened += 1 }
        source.send(denied)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIHostingController(rootView: SessionNoticesOverlay().environment(model))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        var settings: NSObject?
        var dismiss: NSObject?
        let deadline = ContinuousClock.now + .seconds(5)
        while settings == nil || dismiss == nil, ContinuousClock.now < deadline {
            settings = button(labelled: NetworkWords.settingsLabel, in: controller.view)
            dismiss = button(labelled: NetworkWords.dismissLabel, in: controller.view)
            if settings == nil || dismiss == nil { try await Task.sleep(for: .milliseconds(50)) }
        }
        guard let settings, let dismiss else {
            Issue.record("The banner's buttons were not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(settings.accessibilityActivate())
        #expect(opened == 1)
        #expect(model.networkDenialShowing == .cellular, "opening Settings is not a dismissal")
        #expect(dismiss.accessibilityActivate())
        #expect(model.networkDenialShowing == nil)
    }

    /// The Home card is the same words as the banner: its close button says
    /// what `NetworkWords` says, and runs the card's close action.
    @Test func theHomeCardTakesItsCloseLabelFromTheWords() async throws {
        let source = ScriptedNetworkPathSource()
        let model = model(source: source)
        source.send(denied)
        let denial = try #require(model.networkDenialShowing)
        var closed = 0
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let card = NoticeCard(
            symbol: NetworkWords.symbol(for: denial), color: .orange,
            title: NetworkWords.title(for: denial), text: NetworkWords.text(for: denial),
            closeLabel: NetworkWords.dismissLabel, onClose: { closed += 1 })
        let controller = UIHostingController(rootView: card)
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        var close: NSObject?
        let deadline = ContinuousClock.now + .seconds(5)
        while close == nil, ContinuousClock.now < deadline {
            close = button(labelled: NetworkWords.dismissLabel, in: controller.view)
            if close == nil { try await Task.sleep(for: .milliseconds(50)) }
        }
        guard let close else {
            Issue.record("The card's close button was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(close.accessibilityActivate())
        #expect(closed == 1)
    }

    // MARK: The runtime's word for a denial

    @Test func theRuntimeIsToldTheConstantsOfItsHeader() {
        #expect(LiveRuntime.networkNoticeCause(nil) == Int32(KERNARIUM_RUNTIME_NETWORK_NOTICE_NONE))
        #expect(
            LiveRuntime.networkNoticeCause(.cellular) == Int32(KERNARIUM_RUNTIME_NETWORK_NOTICE_CELLULAR_DENIED))
        #expect(LiveRuntime.networkNoticeCause(.wifi) == Int32(KERNARIUM_RUNTIME_NETWORK_NOTICE_WIFI_DENIED))
        let causes = [nil, NetworkDenial.cellular, .wifi].map { LiveRuntime.networkNoticeCause($0) }
        #expect(Set(causes).count == 3, "three states, three words")
    }
}
