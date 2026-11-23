// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

private let phoneHost = HostDevice(
    name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
)

/// A model with a guest on screen, as a running session has it.
@MainActor
private func makeModel() -> AppModel {
    let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18",
        architecture: "arm64, 4 KiB pages", appVersion: "1")
    let model = AppModel(
        guest: Guest.fresh(on: phoneHost), host: phoneHost, system: system, capabilities: .device,
        createRequirementBytes: 1)
    model.phase = .running(.normal)
    return model
}

/// The words the session used to show when iOS reported critically low memory.
private let retiredMemoryWords = ["Memory is critically low", "iOS may close Andarium."]

/// iOS's memory pressure has no notice in the session, at any level: the
/// runtime answers it itself and the application hears nothing of it, so the
/// notices that remain are the network, graphics, thermal and camera ones, in
/// that order. The notices are drawn in a window as the session draws them.
/// SwiftUI publishes its accessibility elements only while application
/// accessibility is on (the Simulator preference `ApplicationAccessibilityEnabled`,
/// as `Kernarium/scripts/run-ios-simulator-tests.sh` sets it); a run without it
/// finds none and the tests fail, for they check nothing then.
@MainActor
struct SessionNoticesTests {
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

    private func all(in node: NSObject, depth: Int = 0, _ match: (NSObject) -> Bool) -> [NSObject] {
        guard depth < 64 else { return [] }
        var found = match(node) ? [node] : []
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children { found += all(in: child, depth: depth + 1, match) }
        return found
    }

    private func identifier(of node: NSObject) -> String? {
        (node as? UIAccessibilityIdentification)?.accessibilityIdentifier
    }

    /// The notices' own buttons, one for each notice that is up: the buttons
    /// that say Dismiss, each counted once by its place on the screen.
    private func dismissButtons(in controller: UIViewController) -> [NSObject] {
        let labels = [NetworkWords.dismissLabel, GraphicsNotice.dismissLabel, ThermalNotice.dismissLabel]
        var places = Set<CGRect>()
        return all(in: controller.view) {
            labels.contains($0.accessibilityLabel ?? "") && $0.accessibilityTraits.contains(.button)
        }.filter { places.insert($0.accessibilityFrame).inserted }
    }

    /// The element labelled `label` in `controller`, waiting for SwiftUI to
    /// publish it, for at most five seconds.
    private func element(labelled label: String, in controller: UIViewController) async throws -> NSObject? {
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            if let found = find(in: controller.view, { $0.accessibilityLabel == label }) { return found }
            try await Task.sleep(for: .milliseconds(50))
        }
        return nil
    }

    /// The notices in a window of `width` (and `height`, when a test gives the
    /// room a height of its own).
    private func hosted(
        _ model: AppModel, width: CGFloat, height: CGFloat? = nil, in lease: HostWindow.Lease
    ) -> UIViewController {
        let controller = UIHostingController(
            rootView: SessionNoticesOverlay().environment(model).dynamicTypeSize(.large)
                .frame(width: width, height: height))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    /// The session's overlays as `Live/SessionView` stacks them: the statistics
    /// capsule and the notices from the top, the side keys on the edge over
    /// them.
    private func sessionHosted(
        _ model: AppModel, width: CGFloat, in lease: HostWindow.Lease
    ) -> UIViewController {
        let actions = SideKeyActions(
            power: {}, powerHold: {}, volumeUp: {}, volumeDown: {}, back: {}, home: {}, settings: {})
        let controller = UIHostingController(
            rootView: Color.clear
                .overlay(alignment: .top) {
                    StatsPill(readout: model.session.readout, memoryMiB: 2048)
                        .padding(.top, StatsPlacement.top)
                        .allowsHitTesting(false)
                }
                .overlay(alignment: .top) { SessionNoticesOverlay() }
                .overlay {
                    SideKeysOverlay(
                        mode: .normal, lit: true, open: .constant(false), tip: SideKeysTip(offersInstall: false), actions: actions)
                }
                .environment(model).dynamicTypeSize(.large).frame(width: width))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    private func scrollViews(under view: UIView) -> [UIScrollView] {
        var found: [UIScrollView] = []
        if let scroll = view as? UIScrollView { found.append(scroll) }
        for sub in view.subviews { found += scrollViews(under: sub) }
        return found
    }

    /// A session with every notice but one of memory up: network, graphics,
    /// thermal and camera.
    private func modelWithEveryNotice() -> AppModel {
        let model = makeModel()
        model.cameraAccess = .notDetermined
        model.cameraCaptureChanged(.blockedInUse)
        model.previewNetworkNotice(.wifi)
        model.previewGraphicsNotice()
        model.previewThermalNotice()
        return model
    }

    // MARK: No notice for memory

    /// The retired words are in no language of the application. The check
    /// reads the compiled bundle, so a string that returns has to come back
    /// with the code that shows it.
    @Test func noWordsForMemoryPressureAreInTheApplicationInAnyLanguage() throws {
        let languages = Bundle.main.localizations.filter { $0 != "Base" }
        #expect(languages.contains("zh-Hans") && languages.contains("zh-Hant") && languages.contains("ja"), "\(languages)")
        for language in languages {
            let path = try #require(Bundle.main.path(forResource: language, ofType: "lproj"), "\(language)")
            let strings = try #require(Bundle(path: path), "\(language)")
            // A key the bundle does not have comes back as the value given.
            for words in retiredMemoryWords {
                #expect(
                    strings.localizedString(forKey: words, value: "(none)", table: nil) == "(none)",
                    "\(language) still has: \(words)")
            }
            // The lookup finds a string that is there (English is its own key, so has no entry to find).
            if language != "en" {
                #expect(
                    strings.localizedString(forKey: NetworkWords.dismissLabel, value: "(none)", table: nil) != "(none)",
                    "\(language): the lookup finds no string at all")
            }
        }
    }

    /// The session on screen shows the notices that remain, one button each,
    /// and none for memory: the runtime reports the pressure and answers it
    /// itself, and nothing in the application listens.
    @Test func aSessionOnScreenShowsTheOtherNoticesAsTheyWereAndNothingForMemory() async throws {
        let model = modelWithEveryNotice()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, height: 1400, in: lease)
        guard let camera = try await element(labelled: CameraWords.offTitle, in: controller),
            let network = try await element(labelled: NetworkWords.title(for: .wifi), in: controller),
            let graphics = try await element(labelled: GraphicsNotice.title, in: controller),
            let thermal = try await element(labelled: ThermalNotice.text, in: controller)
        else {
            Issue.record("A notice was not found: is application accessibility on in this Simulator?")
            return
        }
        // The notices keep their order, each below the one before.
        let order = [network, graphics, thermal, camera].map { $0.accessibilityFrame.minY }
        #expect(order == order.sorted(), "network, graphics, thermal, then camera: \(order)")
        #expect(Set(order).count == order.count, "each notice has a place of its own")
        // One button for each of the four; none for a fifth.
        #expect(dismissButtons(in: controller).count == 4)
        for words in retiredMemoryWords {
            #expect(find(in: controller.view) { $0.accessibilityLabel == words } == nil, "\(words) is on the screen")
        }
        #expect(find(in: controller.view) { identifier(of: $0) == "memory-notice" } == nil)
        // The notices go as they did: a dismissal puts away its own notice and no other.
        #expect(model.cameraNoticeShowing && model.graphicsNotice.isShowing && model.thermalNotice.isShowing)
        model.dismissCameraNotice()
        #expect(!model.cameraNoticeShowing && model.graphicsNotice.isShowing && model.thermalNotice.isShowing)
    }

    /// A session with nothing to say draws no notice, and no room for one.
    @Test func aSessionWithNothingToSayDrawsNoNotice() async throws {
        let model = makeModel()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, height: 700, in: lease)
        try await Task.sleep(for: .milliseconds(300))
        controller.view.layoutIfNeeded()
        #expect(dismissButtons(in: controller).isEmpty)
        #expect(scrollViews(under: controller.view).isEmpty)
    }

    // MARK: The thermal notice's words

    /// Whether a UIKit alert is the topmost presentation of any window now.
    private var alertIsUp: Bool {
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            for window in scene.windows {
                var top = window.rootViewController
                while let next = top?.presentedViewController { top = next }
                if top is UIAlertController { return true }
            }
        }
        return false
    }

    /// The words of the thermal notice, with a chevron, are a button with a
    /// hint of their own; Dismiss stays a button of its own that changes
    /// nothing else. Activating the words puts the question to the model and
    /// the system's alert up; Dismiss puts the notice away and leaves the
    /// frame rate limit on.
    @Test func theThermalNoticesWordsAreAButtonThatAsksAndDismissStaysItsOwn() async throws {
        let model = makeModel()
        model.previewThermalNotice()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, height: 600, in: lease)
        guard let words = try await element(labelled: ThermalNotice.text, in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(words.accessibilityTraits.contains(.button), "the words are not a button")
        #expect(words.accessibilityHint == "Asks to turn off the frame rate limit")
        let dismiss = dismissButtons(in: controller)
        #expect(dismiss.count == 1, "Dismiss is its own button")
        #expect(dismiss.first?.accessibilityFrame.intersects(words.accessibilityFrame) == false)

        #expect(words.accessibilityActivate())
        #expect(model.askingToTurnOffHeatLimit)
        #expect(model.thermalNotice.isShowing)
        // The alert is the system's: UIKit has it on top of the window.
        let deadline = ContinuousClock.now + .seconds(5)
        while !alertIsUp, ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(50)) }
        #expect(alertIsUp, "no alert came up")
        model.cancelTurnOffHeatLimit()
        let gone = ContinuousClock.now + .seconds(5)
        while alertIsUp, ContinuousClock.now < gone { try await Task.sleep(for: .milliseconds(50)) }
        #expect(!alertIsUp, "the alert stayed after Cancel")
        #expect(model.thermalNotice.isShowing && model.heatLimitOn)

        // Dismiss changes nothing but the notice.
        let again = dismissButtons(in: controller)
        #expect(again.first?.accessibilityActivate() == true)
        #expect(!model.thermalNotice.isShowing)
        #expect(model.heatLimitOn)
        #expect(!model.askingToTurnOffHeatLimit)
    }

    /// The notices with the heat question's patience and its way of telling
    /// whether an alert is up in the hands of the test.
    private func hostedWithQuestion(
        _ model: AppModel, patience: Duration, alertIsUp: @escaping @MainActor () -> Bool, in lease: HostWindow.Lease
    ) -> UIViewController {
        let controller = UIHostingController(
            rootView: SessionNoticesOverlay().environment(model).dynamicTypeSize(.large)
                .environment(\.heatQuestionPatience, patience).environment(\.alertIsUp, alertIsUp)
                .frame(width: 390, height: 600))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    /// UIKit can refuse the alert while a sheet is coming or going, and SwiftUI
    /// does not ask again. A question whose alert did not come ends as Cancel
    /// does: the notice stays and goes back to its timer, the guest has the keys
    /// again, and the limit is untouched.
    @Test func aQuestionWhoseAlertDoesNotComeEndsAsCancelDoes() async throws {
        let model = makeModel()
        model.previewThermalNotice()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        _ = hostedWithQuestion(model, patience: .milliseconds(300), alertIsUp: { false }, in: lease)
        model.askToTurnOffHeatLimit()
        #expect(model.askingToTurnOffHeatLimit && !model.guestHasKeys)
        let deadline = ContinuousClock.now + .seconds(5)
        while model.askingToTurnOffHeatLimit, ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(50))
        }
        #expect(!model.askingToTurnOffHeatLimit, "the question stood without its alert")
        #expect(model.guestHasKeys, "the guest did not get the keys back")
        #expect(model.thermalNotice.isShowing && model.heatLimitOn)
        // The words ask again.
        model.askToTurnOffHeatLimit()
        #expect(model.askingToTurnOffHeatLimit)
        model.cancelTurnOffHeatLimit()
    }

    /// A question whose alert is up stands as long as the alert does.
    @Test func aQuestionWhoseAlertIsUpStandsUntilItIsAnswered() async throws {
        let model = makeModel()
        model.previewThermalNotice()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        _ = hostedWithQuestion(model, patience: .milliseconds(400), alertIsUp: { PresentedAlert.isUp }, in: lease)
        model.askToTurnOffHeatLimit()
        let deadline = ContinuousClock.now + .seconds(5)
        while !alertIsUp, ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(50)) }
        #expect(alertIsUp, "no alert came up")
        try await Task.sleep(for: .seconds(1))
        #expect(model.askingToTurnOffHeatLimit, "the question ended under its alert")
        #expect(alertIsUp)
        model.cancelTurnOffHeatLimit()
        let gone = ContinuousClock.now + .seconds(5)
        while alertIsUp, ContinuousClock.now < gone { try await Task.sleep(for: .milliseconds(50)) }
        #expect(!alertIsUp)
    }

    // MARK: Coexistence

    /// The camera notice and the graphics notice in the column the session
    /// gives its notices: the later is below the earlier, nothing overlaps
    /// another, and the column stays clear of the statistics capsule above and
    /// the closed side keys on the edge.
    @Test(arguments: [true, false])
    func theNoticesStackClearOfTheCapsuleAndTheKeys(statsShowing: Bool) async throws {
        let model = makeModel()
        model.cameraAccess = .notDetermined
        model.previewGraphicsNotice()
        model.cameraCaptureChanged(.blockedInUse)
        model.session.showStats = statsShowing
        #expect(model.cameraNoticeKind == .cameraOff)
        #expect(model.graphicsNotice.isShowing)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let width: CGFloat = 390
        let controller = sessionHosted(model, width: width, in: lease)
        guard let graphicsTitle = try await element(labelled: GraphicsNotice.title, in: controller),
            let cameraTitle = try await element(labelled: CameraWords.offTitle, in: controller),
            let cameraDetail = try await element(labelled: CameraWords.offDetail, in: controller)
        else {
            Issue.record("A notice was not found: is application accessibility on in this Simulator?")
            return
        }
        let dismiss = dismissButtons(in: controller)
        #expect(dismiss.count == 2)
        let graphicsFrame = graphicsTitle.accessibilityFrame
        let cameraFrame = cameraTitle.accessibilityFrame.union(cameraDetail.accessibilityFrame)
        #expect(cameraFrame.minY >= graphicsFrame.maxY, "the camera notice is below the graphics notice")
        #expect(!cameraFrame.intersects(graphicsFrame), "they do not overlap")
        let screen = lease.window.bounds
        let column = controller.view.convert(controller.view.bounds, to: nil)
            .insetBy(dx: (controller.view.bounds.width - width) / 2, dy: 0)
        for frame in [graphicsFrame, cameraFrame] {
            #expect(
                frame.minX >= column.minX - 0.5 && frame.maxX <= column.maxX + 0.5, "inside the column: \(frame)")
        }
        if statsShowing {
            let spoken = model.session.readout.spoken(memoryMiB: 2048, recovery: false)
            guard let capsule = find(in: controller.view, { $0.accessibilityLabel == spoken }) else {
                Issue.record("The statistics capsule was not found")
                return
            }
            #expect(!capsule.accessibilityFrame.intersects(graphicsFrame), "the capsule keeps its place")
            #expect(!capsule.accessibilityFrame.intersects(cameraFrame))
            #expect(graphicsFrame.minY >= capsule.accessibilityFrame.maxY, "the notices hang below the capsule")
        }
        guard let keys = find(in: controller.view, { $0.accessibilityLabel == "Side keys" }) else {
            Issue.record("The side keys were not found")
            return
        }
        // The closed keys' capsules sit in the last 12 points of their 36-point frame, on the edge the
        // session's column keeps a gutter from.
        let keysFrame = keys.accessibilityFrame
        for frame in [graphicsFrame, cameraFrame] {
            #expect(frame.maxX <= keysFrame.maxX - 12 + 0.5, "short of the keys' capsules: \(frame) and \(keysFrame)")
        }
        for button in dismiss {
            #expect(screen.contains(button.accessibilityFrame), "a button is in the window: \(button.accessibilityFrame)")
        }
    }

    /// The column scrolls when the room is shorter than its notices, and not
    /// otherwise.
    @Test func aColumnTallerThanItsRoomScrollsAndAColumnThatFitsDoesNot() async throws {
        let model = makeModel()
        model.cameraAccess = .notDetermined
        model.cameraCaptureChanged(.blockedInUse)
        model.previewGraphicsNotice()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let tall = hosted(model, width: 390, height: 700, in: lease)
        _ = try await element(labelled: CameraWords.offTitle, in: tall)
        #expect(scrollViews(under: tall.view).isEmpty, "two notices at the default size fit 700 points")
        let buttons = dismissButtons(in: tall)
        #expect(buttons.count == 2)
        for button in buttons { #expect(lease.window.bounds.contains(button.accessibilityFrame)) }
        let short = hosted(model, width: 390, height: 160, in: lease)
        _ = try await element(labelled: CameraWords.offTitle, in: short)
        #expect(scrollViews(under: short.view).count == 1, "two notices do not fit 160 points: they scroll")
    }
}
