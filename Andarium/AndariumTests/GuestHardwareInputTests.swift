// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreGraphics
import GuestHostCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// A sink that keeps what the guest's input hands it.
@MainActor
private final class RecordingSink: GuestHardwareSink {
    enum Event: Equatable {
        case key(UInt32, Bool)
        case pointer(PointerReport)
        case mouse(Int32, Int32, UInt32, Int32, Int32)
        case keyboardUsed
        case pointerUsed
    }

    private(set) var events: [Event] = []

    func hardwareKey(usage: UInt32, pressed: Bool) { events.append(.key(usage, pressed)) }
    func hardwarePointer(_ report: PointerReport) { events.append(.pointer(report)) }
    func hardwareMouse(dx: Int32, dy: Int32, buttons: UInt32, wheel: Int32, horizontalWheel: Int32) {
        events.append(.mouse(dx, dy, buttons, wheel, horizontalWheel))
    }
    func hardwareKeyboardUsed() { events.append(.keyboardUsed) }
    func hardwarePointerUsed() { events.append(.pointerUsed) }
}

/// A touch of a given kind at a given point, for the view's touch methods.
private final class FakeTouch: UITouch {
    var kind: UITouch.TouchType = .direct
    var point = CGPoint.zero
    override var type: UITouch.TouchType { kind }
    override func location(in view: UIView?) -> CGPoint { point }
}

/// What the guest view does with hardware keys and the system pointer, before
/// the application decides what the guest has.
@MainActor
@Suite(.serialized)
struct GuestHardwareInputTests {
    private func makeInput(_ sink: RecordingSink, active: Bool = true, routed: Bool = true) -> GuestHardwareInput {
        let input = GuestHardwareInput(clock: { 10 })
        input.sink = sink
        input.isActive = active
        input.routesPointer = routed
        return input
    }

    private let inside = GuestPointerMapping.Position(x: 300, y: 400, inside: true)
    private let outside = GuestPointerMapping.Position(x: 0, y: 400, inside: false)

    // MARK: Keys

    @Test func keysGoOnlyWhileTheInputIsActive() {
        let sink = RecordingSink()
        let input = makeInput(sink, active: false)
        input.keyDown(usage: 0x04, alphaShift: false)
        #expect(sink.events.isEmpty)
        input.isActive = true
        input.keyDown(usage: 0x04, alphaShift: false)
        input.keyDown(usage: 0x04, alphaShift: false)
        input.keyUp(usage: 0x04)
        #expect(sink.events == [.keyboardUsed, .key(0x04, true), .keyboardUsed, .key(0x04, false)])
    }

    @Test func aUsageThatNamesNoKeyIsDropped() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.keyDown(usage: 0x03, alphaShift: false)
        input.keyDown(usage: 0xE8, alphaShift: false)
        #expect(sink.events.isEmpty)
    }

    @Test func goingInactiveLiftsKeysAndPointerNewestFirst() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.keyDown(usage: 0x04, alphaShift: false)
        input.keyDown(usage: 0xE3, alphaShift: false)
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        let before = sink.events.count
        input.isActive = false
        #expect(sink.events[before...] == [
            .key(0xE3, false), .key(0x04, false),
            .pointer(PointerReport(x: 300, y: 400, buttons: 0, inRange: false)),
        ])
    }

    // MARK: Pointer

    @Test func theSystemPointerGoesToTheMouseOnlyWhenTheApplicationRoutesIt() {
        let sink = RecordingSink()
        let input = makeInput(sink, routed: false)
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        input.scroll(translation: CGPoint(x: 0, y: 64), continuous: true, beginning: true)
        #expect(sink.events.isEmpty)
        #expect(!input.hidesSystemPointer)
        input.routesPointer = true
        #expect(input.hidesSystemPointer)
    }

    @Test func aMouseTheGuestHasNotRegisteredIsOnlyWatchedAndItsPositionComesFirstWhenItIs() {
        let sink = RecordingSink()
        let input = makeInput(sink, routed: false)
        input.watchesPointer = true
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        #expect(sink.events.isEmpty)
        input.hover(inside, at: CGPoint(x: 2, y: 1), beginning: false)
        input.pointerTouchBeganUnrouted()
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        input.scroll(translation: CGPoint(x: 0, y: 64), continuous: true, beginning: true)
        // Using it, and nothing else; the system's pointer is still the person's.
        #expect(sink.events == [.pointerUsed, .pointerUsed, .pointerUsed])
        #expect(!input.hidesSystemPointer)
        // The guest registers it: it hears where the pointer is, then the rest.
        input.routesPointer = true
        input.watchesPointer = false
        #expect(sink.events.last == .pointer(PointerReport(x: 300, y: 400, buttons: 0, inRange: true)))
        let before = sink.events.count
        input.scroll(translation: CGPoint(x: 0, y: 64), continuous: true, beginning: true)
        #expect(sink.events[before...] == [.pointerUsed, .mouse(0, 0, 0, 120, 0)])
    }

    @Test func aPointerThatLeftBeforeTheRegistrationHasNoPositionToSend() {
        let sink = RecordingSink()
        let input = makeInput(sink, routed: false)
        input.watchesPointer = true
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        input.hover(nil, at: nil, beginning: false)
        input.routesPointer = true
        input.watchesPointer = false
        #expect(sink.events.isEmpty)
    }

    @Test func nothingIsWatchedWhileTheInputIsNotActive() {
        let sink = RecordingSink()
        let input = makeInput(sink, active: false, routed: false)
        input.watchesPointer = true
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        input.hover(inside, at: CGPoint(x: 2, y: 1), beginning: false)
        input.pointerTouchBeganUnrouted()
        input.scroll(translation: CGPoint(x: 0, y: 64), continuous: true, beginning: true)
        #expect(sink.events.isEmpty)
    }

    @Test func aHoverThatStartsWithoutMovingIsNotTheMousesUse() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        // The Simulator hovers at the view's centre when its scene activates.
        input.hover(inside, at: CGPoint(x: 50, y: 50), beginning: true)
        input.hover(inside, at: CGPoint(x: 50, y: 50), beginning: false)
        #expect(!sink.events.contains(.pointerUsed))
        input.hover(inside, at: CGPoint(x: 51, y: 50), beginning: false)
        #expect(sink.events.contains(.pointerUsed))
    }

    @Test func aDragGoesOnPastTheEdgeAndItsReleaseIsReportedWhereverItHappens() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        input.pointerTouch(.moved, at: outside, buttons: HardwareButtons.primary)
        input.pointerTouch(.ended, at: outside, buttons: 0)
        let reports = sink.events.compactMap { event -> PointerReport? in
            if case let .pointer(report) = event { return report }
            return nil
        }
        #expect(reports.first == PointerReport(x: 300, y: 400, buttons: HardwareButtons.primary, inRange: true))
        #expect(reports.dropFirst().first?.buttons == HardwareButtons.primary)
        #expect(reports.last?.buttons == 0)
    }

    @Test func aTouchThatBeginsBesideTheDisplayIsNotTheGuests() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.pointerTouch(.began, at: outside, buttons: HardwareButtons.primary)
        input.pointerTouch(.moved, at: inside, buttons: HardwareButtons.primary)
        input.pointerTouch(.ended, at: inside, buttons: 0)
        let reports = sink.events.filter { if case .pointer = $0 { true } else { false } }
        #expect(reports.isEmpty)
    }

    @Test func aWheelAndATrackpadScrollAreDifferentDetents() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.scroll(translation: CGPoint(x: 0, y: -32), continuous: true, beginning: true)
        input.scroll(translation: CGPoint(x: 0, y: -32), continuous: true, beginning: false)
        input.scroll(translation: CGPoint(x: 0, y: -20), continuous: false, beginning: true)
        let wheels = sink.events.compactMap { event -> Int32? in
            if case let .mouse(_, _, _, wheel, _) = event { return wheel }
            return nil
        }
        // Half of a trackpad's 64 points is half a detent in the wheel's 1/120
        // units, and a wheel's 20 points are a whole one.
        #expect(wheels == [-60, -60, -120])
    }

    @Test func whileTheGuestCapturesThePointerTheSystemsPointerSaysNothingButWhereItIs() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        #expect(input.pointerIsOverDisplay)
        input.capturing = true
        // The contact ended, but the pointer is still over the display.
        #expect(input.pointerIsOverDisplay)
        let before = sink.events.count
        input.hover(outside, at: CGPoint(x: -4, y: 1), beginning: false)
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        input.scroll(translation: CGPoint(x: 0, y: -64), continuous: true, beginning: true)
        #expect(sink.events.count == before)
        #expect(!input.pointerIsOverDisplay)
        input.hover(inside, at: CGPoint(x: 5, y: 1), beginning: false)
        #expect(input.pointerIsOverDisplay)
        input.capturing = false
        #expect(!input.pointerIsOverDisplay)
    }

    @Test func theViewIsToldWhenTheSystemPointerShouldHideOrShow() {
        let sink = RecordingSink()
        let input = GuestHardwareInput(clock: { 10 })
        input.sink = sink
        var told = 0
        input.hidesSystemPointerDidChange = { told += 1 }
        input.isActive = true
        input.routesPointer = true
        input.routesPointer = true
        input.isActive = false
        #expect(told == 3)
    }

    // MARK: The view

    @Test func theViewTakesTheMouseRecognizersOnlyWithAnInput() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        let recognizers = { view.gestureRecognizers ?? [] }
        #expect(!recognizers().contains { $0 is UIHoverGestureRecognizer })
        #expect(!view.interactions.contains { $0 is UIPointerInteraction })

        let input = GuestHardwareInput()
        view.hardwareInput = input
        #expect(recognizers().contains { $0 is UIHoverGestureRecognizer })
        let scrolls = recognizers().compactMap { $0 as? UIPanGestureRecognizer }
        #expect(scrolls.count == 2)
        #expect(Set(scrolls.map { $0.allowedScrollTypesMask.rawValue }).count == 2)
        #expect(scrolls.allSatisfy { $0.maximumNumberOfTouches == 0 })
        #expect(view.interactions.contains { $0 is UIPointerInteraction })

        view.hardwareInput = nil
        #expect(!recognizers().contains { $0 is UIHoverGestureRecognizer })
        #expect(!recognizers().contains { $0 is UIPanGestureRecognizer })
        #expect(!view.interactions.contains { $0 is UIPointerInteraction })
    }

    @Test func theViewKeepsTheLastFramesProportionsAndBlackBarsWhileItResizes() {
        // A turn or a window resize changes the layer's bounds before the guest
        // draws for them, and the layer's default would stretch the frame; the
        // bars beside the aspect-fit frame are the layer's own background.
        let frame = CGRect(x: 0, y: 0, width: 200, height: 100)
        for view in [GuestMetalView(frame: frame, device: nil), TextGuestMetalView(frame: frame, device: nil)] {
            #expect(view.layer.contentsGravity == .resizeAspect)
            var white: CGFloat = 1
            var alpha: CGFloat = 0
            let background = view.layer.backgroundColor.map { UIColor(cgColor: $0) }
            #expect(
                background?.getWhite(&white, alpha: &alpha) == true && white == 0 && alpha == 1,
                "the layer's background is \(String(describing: view.layer.backgroundColor)), the view's \(String(describing: view.backgroundColor))")
        }
    }

    @Test func theViewHidesTheSystemPointerOnlyWhileTheMouseIsTheGuests() throws {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        let input = GuestHardwareInput()
        view.hardwareInput = input
        let interaction = try #require(view.interactions.compactMap { $0 as? UIPointerInteraction }.first)
        let style = { view.pointerInteraction(interaction, styleFor: UIPointerRegion(rect: view.bounds)) }
        #expect(style() == nil)
        input.isActive = true
        #expect(style() == nil)
        input.routesPointer = true
        #expect(style() != nil)
        input.isActive = false
        #expect(style() == nil)
    }

    /// A view of 200 x 100 points that draws a square display: 100 points wide,
    /// centred.
    private func squareDisplayView() -> GuestMetalView {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        view.autoResizeDrawable = false
        view.drawableSize = CGSize(width: 200, height: 100)
        view.setGuestDisplay(width: 100, height: 100)
        return view
    }

    @Test func activatingTheDisplaysElementSendsTheGuestNoTouch() {
        // VoiceOver's double-tap on an element that does not take the
        // activation taps its activation point, which `touchesBegan` forwards.
        let view = squareDisplayView()
        var contacts = 0
        view.contactDidChange = { _, _ in contacts += 1 }
        view.applyAccessibility(GuestDisplayAccessibility(label: "Android screen", value: "off"))
        #expect(view.isAccessibilityElement)
        #expect(view.accessibilityValue == "off")
        #expect(view.accessibilityActivate())
        #expect(contacts == 0)
        // Where the display is no element the view keeps UIKit's behaviour.
        view.applyAccessibility(nil)
        #expect(!view.accessibilityActivate())
    }

    @Test func aTouchOfThePointerIsTheMousesOrAFingersFromItsBeginningToItsEnd() throws {
        let sink = RecordingSink()
        let input = makeInput(sink, routed: false)
        let view = squareDisplayView()
        var contacts: [Bool] = []
        view.contactDidChange = { _, pressed in contacts.append(pressed) }
        view.hardwareInput = input
        input.watchesPointer = true

        // It began while the guest had no mouse: a finger until it ends,
        // whatever the routing does meanwhile.
        let tap = FakeTouch()
        tap.kind = .indirectPointer
        tap.point = CGPoint(x: 100, y: 50)
        view.touchesBegan([tap], with: nil)
        #expect(contacts == [true])
        #expect(sink.events == [.pointerUsed])
        input.routesPointer = true
        input.watchesPointer = false
        view.touchesMoved([tap], with: nil)
        view.touchesEnded([tap], with: nil)
        #expect(contacts.first == true && contacts.last == false)
        #expect(!sink.events.contains { if case .pointer(let report) = $0 { report.buttons != 0 } else { false } })

        // It began as the mouse's: the mouse's, though the guest lost the mouse.
        contacts.removeAll()
        let click = FakeTouch()
        click.kind = .indirectPointer
        click.point = CGPoint(x: 100, y: 50)
        view.touchesBegan([click], with: nil)
        #expect(contacts.isEmpty)
        input.routesPointer = false
        let after = sink.events.count
        view.touchesMoved([click], with: nil)
        view.touchesEnded([click], with: nil)
        #expect(contacts.isEmpty)
        #expect(sink.events.count == after)

        // A finger is a finger whatever the routing is.
        input.routesPointer = true
        let finger = FakeTouch()
        finger.point = CGPoint(x: 100, y: 50)
        view.touchesBegan([finger], with: nil)
        view.touchesEnded([finger], with: nil)
        #expect(contacts.first == true && contacts.last == false)
    }

    @Test func theScrollRecognizersDoNotHoldBackTheEndOfTouches() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        view.hardwareInput = GuestHardwareInput()
        let scrolls = (view.gestureRecognizers ?? []).compactMap { $0 as? UIPanGestureRecognizer }
        #expect(scrolls.count == 2)
        #expect(scrolls.allSatisfy { !$0.delaysTouchesBegan && !$0.delaysTouchesEnded && !$0.cancelsTouchesInView })
    }

    @Test func aViewThatHandsTheInputOverLeavesItsNewOwnerTheCallbackAndTheKeys() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        let first = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        let second = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        first.hardwareInput = input
        // SwiftUI makes the new view before it lets go of the old one.
        second.hardwareInput = input
        input.keyDown(usage: 0x04, alphaShift: false)
        first.hardwareInput = nil
        #expect(input.pointerStyleOwner === second)
        #expect(input.hidesSystemPointerDidChange != nil)
        #expect(sink.events.last == .key(0x04, true))
        second.hardwareInput = nil
        #expect(input.pointerStyleOwner == nil)
        #expect(input.hidesSystemPointerDidChange == nil)
        #expect(sink.events.last == .key(0x04, false))
    }

    @Test func theSystemPointerIsHiddenOverTheDisplayAndNotOverTheBarsBesideIt() {
        let view = squareDisplayView()
        let sink = RecordingSink()
        let input = makeInput(sink)
        view.hardwareInput = input
        #expect(view.hiddenPointerRegion(at: CGPoint(x: 100, y: 50)) == CGRect(x: 50, y: 0, width: 100, height: 100))
        #expect(view.hiddenPointerRegion(at: CGPoint(x: 10, y: 50)) == nil)
        #expect(view.hiddenPointerRegion(at: CGPoint(x: 190, y: 50)) == nil)
        // Nothing is hidden where the mouse is not the guest's.
        input.routesPointer = false
        #expect(view.hiddenPointerRegion(at: CGPoint(x: 100, y: 50)) == nil)
    }

    @Test func aWindowThatStopsBeingTheKeyOneLiftsTheKeysHeld() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIViewController()
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        let sink = RecordingSink()
        let input = makeInput(sink)
        let view = GuestMetalView(frame: controller.view.bounds, device: nil)
        view.hardwareInput = input
        controller.view.addSubview(view)
        input.keyDown(usage: 0x04, alphaShift: false)
        input.keyDown(usage: 0xE3, alphaShift: false)
        // Another window or app takes the focus: UIKit sends no release.
        NotificationCenter.default.post(name: UIWindow.didResignKeyNotification, object: lease.window)
        #expect(await eventually { sink.events.suffix(2) == [.key(0xE3, false), .key(0x04, false)] })
        // Another window's resignation is not this one's.
        input.keyDown(usage: 0x05, alphaShift: false)
        NotificationCenter.default.post(name: UIWindow.didResignKeyNotification, object: UIWindow(windowScene: lease.scene))
        try await Task.sleep(for: .milliseconds(200))
        #expect(sink.events.last == .key(0x05, true))
        // A view that has left its window lets go of what is held.
        view.removeFromSuperview()
        #expect(sink.events.last == .key(0x05, false))
    }

    @Test func aViewWithoutAnInputLeavesEveryTouchToTheTouchscreen() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        var reports = 0
        view.touchDidBegin = { reports += 1 }
        view.touchesBegan([UITouch()], with: nil)
        #expect(reports == 1)
        #expect(view.hardwareInput == nil)
    }

    @Test func settingAnotherInputLiftsWhatTheFirstHolds() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        let sink = RecordingSink()
        let first = makeInput(sink)
        view.hardwareInput = first
        first.keyDown(usage: 0x04, alphaShift: false)
        view.hardwareInput = nil
        #expect(sink.events.last == .key(0x04, false))
    }

    @Test func theDisplayHoldsTheKeyboardFocusOnceItIsInAWindowAndLetsGoWhenAsked() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIViewController()
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        let view = GuestMetalView(frame: controller.view.bounds, device: nil)
        let input = GuestHardwareInput()
        view.hardwareInput = input
        // Wanted before there is a window: claimed when it arrives.
        view.setHardwareFocus(true)
        #expect(!view.isFirstResponder)
        controller.view.addSubview(view)
        #expect(await eventually { view.isFirstResponder })
        // A sheet takes the keys, and the keys held are lifted with the focus.
        view.setHardwareFocus(false)
        #expect(!view.isFirstResponder)
        view.setHardwareFocus(true)
        #expect(view.isFirstResponder)
        // Without an input there is nothing to hold the focus for.
        let bare = GuestMetalView(frame: controller.view.bounds, device: nil)
        controller.view.addSubview(bare)
        bare.setHardwareFocus(true)
        #expect(!bare.isFirstResponder)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    // MARK: Positions

    /// The pointer and the touchscreen share one range, so Android's cursor
    /// is where a finger would be.
    @Test(arguments: [
        (CGSize(width: 402, height: 874), CGSize(width: 1206, height: 2622), CGSize(width: 1080, height: 2400)),
        (CGSize(width: 1194, height: 834), CGSize(width: 1194, height: 834), CGSize(width: 1194, height: 834)),
        (CGSize(width: 1000, height: 700), CGSize(width: 2000, height: 1400), CGSize(width: 1920, height: 1080)),
        (CGSize(width: 820, height: 1180), CGSize(width: 1640, height: 2360), CGSize(width: 1024, height: 768)),
    ])
    func thePointersPositionIsTheTouchscreensForTheSamePoint(bounds: CGSize, drawable: CGSize, content: CGSize) throws {
        let mapping = try #require(GuestPointerMapping(boundsSize: bounds, drawableSize: drawable, contentSize: content))
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: .touchscreen, boundsSize: bounds, drawableSize: drawable, contentSize: content))
        let viewport = try #require(GuestDisplayViewport.aspectFit(contentSize: content, in: drawable))
        // Points of the view that fall on the display.
        let fractions: [(CGFloat, CGFloat)] = [(0.001, 0.001), (0.25, 0.4), (0.5, 0.5), (0.9, 0.1), (0.999, 0.999)]
        for (fx, fy) in fractions {
            let drawablePoint = CGPoint(x: viewport.minX + viewport.width * fx, y: viewport.minY + viewport.height * fy)
            let point = CGPoint(x: drawablePoint.x * bounds.width / drawable.width, y: drawablePoint.y * bounds.height / drawable.height)
            var contacts = GuestTouchContacts<Int>()
            let touch = try #require(contacts.begin(1, at: point, geometry: geometry).first)
            let position = try #require(mapping.position(of: point))
            #expect(position.inside)
            #expect(position.x == touch.event.x)
            #expect(position.y == touch.event.y)
        }
    }

    @Test func aPointBesideTheDisplayHasThePositionOfTheNearestEdge() throws {
        let bounds = CGSize(width: 400, height: 800)
        let mapping = try #require(GuestPointerMapping(
            boundsSize: bounds, drawableSize: bounds, contentSize: CGSize(width: 800, height: 400)))
        // The display is a band across the middle of the view.
        let above = try #require(mapping.position(of: CGPoint(x: 200, y: 10)))
        #expect(!above.inside)
        #expect(above.y == 0)
        let below = try #require(mapping.position(of: CGPoint(x: 200, y: 790)))
        #expect(!below.inside)
        #expect(below.y == GuestPointerMapping.maximum)
    }

    // MARK: The scene's pointer lock

    @Test func theRootAsksForThePointerLockWhileTheGuestWantsIt() {
        let model = AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        let controller = AndariumHostingController(rootView: Color.clear, model: model)
        #expect(!controller.prefersPointerLocked)
        model.pointerLockWanted = true
        #expect(controller.prefersPointerLocked)
        model.pointerLockWanted = false
        #expect(!controller.prefersPointerLocked)
    }

    @Test func theSettingReachesTheDriverAndIsKeptForTheInstall() {
        let model = AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        let driver = StubDriver(plan: nil)
        model.driver = driver
        #expect(!model.pointerAsTouch)
        #expect(model.pointerAsTouchToKeep == false)
        model.setPointerAsTouch(true)
        #expect(driver.pointerAsTouch)
        #expect(model.pointerAsTouchToKeep == true)
        // A launch that decided the value keeps nothing.
        model.pointerAsTouchDecidedByLaunch = true
        #expect(model.pointerAsTouchToKeep == nil)
    }

    // MARK: The UI tests' launch

    @Test func theProbeLogsWhatTheGuestWouldReceive() {
        let probe = InputProbe()
        let mouse = KeyboardMouse.probe(probe, supportsMouse: true, guestRegisters: true) { _ in }
        defer { mouse.shutDown() }
        mouse.update(running: true, foreground: true)
        mouse.input.keyDown(usage: 0x04, alphaShift: false)
        mouse.input.keyUp(usage: 0x04)
        #expect(probe.lines.contains("connect keyboard 1"))
        #expect(probe.lines.suffix(2) == ["key 0x4 down", "key 0x4 up"])
        #expect(probe.text.contains("\n"))
    }

    @Test func theProbesGuestRegistersADeviceAfterItsPlugAndLosesItWithTheUnplug() async {
        let runtime = RecordingHardwareInputRuntime()
        runtime.registers = true
        var heard: [String] = []
        runtime.setStatusHandler { status, on in heard.append("\(status) \(on)") }
        #expect(runtime.setConnected(.keyboard, true))
        #expect(heard.isEmpty, "the guest answers after the plug, not within it")
        #expect(runtime.setConnected(.mouse, true))
        #expect(await eventually { heard.count == 2 })
        #expect(heard == ["keyboardRegistered true", "pointerRegistered true"])
        #expect(runtime.setConnected(.mouse, false))
        #expect(await eventually { heard.count == 3 })
        #expect(heard.last == "pointerRegistered false")

        // A guest that is not asked to register says nothing.
        let silent = RecordingHardwareInputRuntime()
        var silentHeard = 0
        silent.setStatusHandler { _, _ in silentHeard += 1 }
        #expect(silent.setConnected(.mouse, true))
        try? await Task.sleep(for: .milliseconds(200))
        #expect(silentHeard == 0)
    }

    @Test func theProbeKeepsTheNewestLinesWithinItsCapacity() {
        let probe = InputProbe()
        for number in 0..<(InputProbe.capacity + 25) { probe.record("line \(number)") }
        #expect(probe.lines.count == InputProbe.capacity)
        #expect(probe.lines.first == "line 25")
        #expect(probe.lines.last == "line \(InputProbe.capacity + 24)")
    }

    @Test func theProbeLaunchIsAHarnessLaunch() {
        let environment = [InputProbeLaunch.environmentName: "1"]
        #expect(InputProbeLaunch.requested(in: environment))
        #expect(!InputProbeLaunch.requested(in: [:]))
        #expect(!InputProbeLaunch.requested(in: [InputProbeLaunch.environmentName: "0"]))
        #expect(InputProbeLaunch.guestRegisters(in: environment))
        // The guest that never registers its devices, like a kernel without the
        // hot-plug topic.
        let unregistered = [InputProbeLaunch.environmentName: InputProbeLaunch.unregisteredValue]
        #expect(InputProbeLaunch.requested(in: unregistered))
        #expect(!InputProbeLaunch.guestRegisters(in: unregistered))
        #expect(!AppEntry.isPersonsLaunch(overrides: LaunchOverrides(), environment: unregistered))
        #expect(!AppEntry.isPersonsLaunch(overrides: LaunchOverrides(), environment: environment))
        #expect(AppEntry.isPersonsLaunch(overrides: LaunchOverrides(), environment: [:]))
    }
}
