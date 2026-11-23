// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// A model whose host has a fixed free space. Home and Welcome state the
/// device's, in a width of its own, and a test that lays a screen out twice
/// to compare the two would otherwise see a layout change where the disk
/// merely filled or emptied in between.
@MainActor
private func steadyModel(_ entry: AppEntry = .fromArguments()) -> AppModel {
    let model = entry.makeModel()
    model.host.freeStorageBytes = 40_000_000_000
    return model
}

/// The production root with a run on a display that folds: the guest's
/// display where the plan puts it, Andarium's pieces off the fold, the guest
/// kept through every fold and turn, and the rotation rule of the inner
/// display. The readings are those the iPhone Duo Simulator gives in each pose
/// (`FoldLayoutTests` has the numbers), supplied through the reader's stub into
/// a window of the pose's size, so every pose is tested on every Simulator;
/// `TheRealDisplay` reads the Simulator's own display in the pose the
/// harness put it in (`TEST_RUNNER_ANDARIUM_FOLD_POSE`).
@MainActor
@Suite(.serialized)
struct FoldSessionHostingTests {
    static func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
        CGRect(x: x, y: y, width: w, height: h)
    }

    /// A pose of the Spread design, with the session's safe area (the status
    /// bar hidden).
    enum Pose: CaseIterable, CustomStringConvertible, Sendable {
        case cover, uprightBook, uprightFlat, turnedBook, turnedFlat

        var description: String {
            switch self {
            case .cover: "cover"
            case .uprightBook: "upright, book"
            case .uprightFlat: "upright, flat"
            case .turnedBook: "turned, book (standing)"
            case .turnedFlat: "turned, flat"
            }
        }

        /// The poses of `poses` whose sheet height can be tested on this system. The sheet of a system
        /// before iOS 27 ignores a custom detent on the iPad the tests run on, so the turned, bent pose,
        /// the one that asks for one, is left out below it; from iOS 27.0 every pose is tested.
        static func keepingSheetHeights(_ poses: [Pose]) -> [Pose] {
            if #available(iOS 27.0, *) { return poses }
            return poses.filter { $0 != .turnedBook }
        }

        var reading: FoldReading {
            switch self {
            case .cover:
                return FoldReading(
                    window: rect(0, 0, 466, 678), safeArea: rect(0, 0, 382, 644), hinge: .closed,
                    occlusions: [rect(399.7, 29.3, 37, 37), rect(382, 0, 84, 82)])
            case .uprightBook, .uprightFlat:
                let bent = self == .uprightBook
                return FoldReading(
                    window: rect(0, 0, 951, 669), safeArea: rect(0, 0, 951, 635),
                    hinge: bent ? .partiallyOpen : .fullyOpen,
                    division: .init(frame: rect(455.5, 0, 40, 669), isActive: bent))
            case .turnedBook, .turnedFlat:
                let bent = self == .turnedBook
                return FoldReading(
                    window: rect(0, 0, 669, 951), safeArea: rect(0, 0, 669, 917),
                    hinge: bent ? .partiallyOpen : .fullyOpen,
                    division: .init(frame: rect(0, 455.5, 669, 40), isActive: bent))
            }
        }

        /// The guest's rectangle in the window, from the design's table.
        var guest: CGRect {
            switch self {
            case .cover: rect(0, 0, 382, 644)
            case .uprightBook, .uprightFlat: rect(0, 0, 951, 635)
            case .turnedBook: rect(0, 0, 669, 455.5)
            case .turnedFlat: rect(0, 0, 669, 917)
            }
        }

        var inner: Bool { self != .cover }

        var foldPose: FoldPose {
            switch self {
            case .cover: .cover
            case .uprightBook: .upright(bent: true)
            case .uprightFlat: .upright(bent: false)
            case .turnedBook: .turned(bent: true)
            case .turnedFlat: .turned(bent: false)
            }
        }
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(30))
        }
        return condition()
    }

    private func descendants(of view: UIView) -> [UIView] {
        view.subviews + view.subviews.flatMap { descendants(of: $0) }
    }

    /// Ends the session and lets the root lay out once, so that it hands the
    /// window back as the application left it (a running session paints the
    /// window black) before the next test takes it.
    private func endSession(of model: AppModel, _ controller: UIViewController) {
        model.phase = .home
        controller.view.setNeedsLayout()
        controller.view.layoutIfNeeded()
    }

    /// The scene root with a run on screen, in a view the size of the pose that
    /// sits in the application's window. A window cannot take another size, so
    /// the root is a child of the window's controller in a view of its own,
    /// whose hit testing is asked.
    @MainActor
    private final class Rig {
        let model: AppModel
        let lease: HostWindow.Lease
        /// The window as the application left it: a running session paints it
        /// black, and the root puts it back when its phase ends.
        private let windowBacking: UIColor?
        let container = UIViewController()
        let holder: UIView
        let controller: AndariumHostingController<AnyView>
        let driver = StubDriver(plan: nil)
        var pose: Pose
        /// The scene reports no hinge, as an iPad does, in the pose's own window.
        let plain: Bool
        private(set) var guest: GuestMetalView!

        /// `phase` is what the root shows; Home and the boot have no guest's
        /// display to wait for, `withoutGuest` is the screen before a virtual
        /// machine exists, and `withMachine` is Home with one to start.
        init(
            _ pose: Pose, phase: Phase = .running(.normal), withoutGuest: Bool = false, withMachine: Bool = false,
            plain: Bool = false, test: FoldSessionHostingTests
        ) async throws {
            self.pose = pose
            self.plain = plain
            let entry = AppEntry.fromArguments()
            let model = steadyModel(entry)
            model.launchApplied = true
            self.model = model
            lease = try await HostWindow.take()
            windowBacking = lease.window.backgroundColor
            holder = UIView(frame: pose.reading.window)
            controller = AndariumHostingController(rootView: AnyView(RootView(entry: entry).environment(model)), model: model)
            model.driver = driver
            controller.foldReader.stub = { [unowned self] in self.plain ? nil : self.pose.reading }
            container.view.backgroundColor = .black
            lease.window.rootViewController = container
            container.view.addSubview(holder)
            container.addChild(controller)
            holder.addSubview(controller.view)
            controller.view.frame = holder.bounds
            controller.didMove(toParent: container)
            if withoutGuest { model.guest = nil }
            if withMachine { model.guest = Guest.fresh(on: model.host) }
            model.phase = phase
            controller.view.layoutIfNeeded()
            guard case .running = phase else {
                // The window is the application's own, not the pose's: give the
                // root the pose's safe area, as the screens below it lay out in it.
                let window = pose.reading.window, safe = pose.reading.safeArea
                let have = controller.view.safeAreaInsets, extra = controller.additionalSafeAreaInsets
                controller.additionalSafeAreaInsets = UIEdgeInsets(
                    top: extra.top + safe.minY - window.minY - have.top,
                    left: extra.left + safe.minX - window.minX - have.left,
                    bottom: extra.bottom + window.maxY - safe.maxY - have.bottom,
                    right: extra.right + window.maxX - safe.maxX - have.right)
                _ = await test.eventually { model.fold == (plain ? nil : pose.reading) }
                try? await Task.sleep(for: .milliseconds(700))
                controller.view.layoutIfNeeded()
                return
            }
            var display: GuestMetalView?
            let found = await test.eventually {
                self.controller.view.layoutIfNeeded()
                display = test.descendants(of: self.controller.view).compactMap { $0 as? GuestMetalView }.first
                return display != nil && display!.bounds.width > 0
            }
            guard found, let display else {
                end()
                throw Failure.noDisplay
            }
            guest = display
        }

        enum Failure: Error { case noDisplay }

        func end() {
            // The session ends first, so that the root hands the window back.
            model.phase = .home
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            lease.window.backgroundColor = windowBacking
            controller.willMove(toParent: nil)
            controller.view.removeFromSuperview()
            controller.removeFromParent()
            holder.removeFromSuperview()
            lease.give()
        }

        /// The scene moves to `next`: the view takes its size and the root
        /// reads the pose again, as a fold or a turn does.
        func move(to next: Pose, test: FoldSessionHostingTests) async {
            pose = next
            holder.frame = next.reading.window
            controller.view.frame = holder.bounds
            controller.view.setNeedsLayout()
            holder.layoutIfNeeded()
            _ = await test.eventually { self.model.fold == next.reading }
            controller.view.layoutIfNeeded()
            try? await Task.sleep(for: .milliseconds(250))
            controller.view.layoutIfNeeded()
        }

        var guestFrame: CGRect { guest.convert(guest.bounds, to: holder) }

        func hit(_ point: CGPoint) -> UIView? { holder.hitTest(point, with: nil) }

        /// What the screen's accessibility tree names, in the order it is read,
        /// with each element's frame in the view's coordinates: Andarium's
        /// keys, statistics and notices are the elements.
        func labelled() -> [(label: String, frame: CGRect)] {
            FoldSessionHostingTests.labelled(in: controller.view, relativeTo: holder)
        }

        /// `labelled()` for the application's own elements, and the guest display's own element apart: the
        /// display view, found by identity and not by its label or its size. The guest's picture spans the fold by
        /// design, and the element that stands for it is that picture, not one of the application's controls.
        func labelledApartFromDisplay() -> (
            controls: [(label: String, frame: CGRect)], display: [(node: NSObject, label: String, frame: CGRect)]
        ) {
            FoldSessionHostingTests.labelled(in: controller.view, relativeTo: holder, apartFrom: guest)
        }

        func reachesGuest(_ point: CGPoint) -> Bool {
            guard let view = hit(point) else { return false }
            return view === guest || view.isDescendant(of: guest)
        }
    }

    /// The accessibility elements under `root`, in reading order, with their
    /// frames in `holder`'s coordinates: those the root's own tree names, then
    /// those of the views inside it (a navigation stack's pages are views of
    /// their own).
    static func labelled(in root: UIView, relativeTo holder: UIView) -> [(label: String, frame: CGRect)] {
        var found: [(String, CGRect)] = []
        var seen: Set<String> = []
        func add(_ node: NSObject) {
            guard let label = node.accessibilityLabel, !label.isEmpty, node !== root,
                let screen = holder.window?.screen else { return }
            let frame = holder.convert(node.accessibilityFrame, from: screen.coordinateSpace)
            if seen.insert("\(label)|\(Int(frame.minX))|\(Int(frame.minY))").inserted { found.append((label, frame)) }
        }
        func walk(_ node: NSObject, depth: Int) {
            guard depth < 10 else { return }
            let count = node.accessibilityElementCount()
            if count != NSNotFound, count > 0 {
                for index in 0..<count {
                    if let child = node.accessibilityElement(at: index) as? NSObject { walk(child, depth: depth + 1) }
                }
            }
            add(node)
        }
        func walkViews(_ view: UIView) {
            walk(view, depth: 0)
            for subview in view.subviews { walkViews(subview) }
        }
        walkViews(root)
        return found
    }

    /// `labelled(in:relativeTo:)` with `apart` taken out of the list by identity and returned beside it, with
    /// its frame. The walk and what is left of it are that one's.
    static func labelled(in root: UIView, relativeTo holder: UIView, apartFrom apart: NSObject) -> (
        controls: [(label: String, frame: CGRect)], display: [(node: NSObject, label: String, frame: CGRect)]
    ) {
        var controls: [(String, CGRect)] = []
        var display: [(NSObject, String, CGRect)] = []
        var seen: Set<String> = []
        func add(_ node: NSObject) {
            guard let label = node.accessibilityLabel, !label.isEmpty, node !== root,
                let screen = holder.window?.screen else { return }
            let frame = holder.convert(node.accessibilityFrame, from: screen.coordinateSpace)
            if node === apart {
                if !display.contains(where: { $0.0 === node }) { display.append((node, label, frame)) }
            } else if seen.insert("\(label)|\(Int(frame.minX))|\(Int(frame.minY))").inserted {
                controls.append((label, frame))
            }
        }
        func walk(_ node: NSObject, depth: Int) {
            guard depth < 10 else { return }
            let count = node.accessibilityElementCount()
            if count != NSNotFound, count > 0 {
                for index in 0..<count {
                    if let child = node.accessibilityElement(at: index) as? NSObject { walk(child, depth: depth + 1) }
                }
            }
            add(node)
        }
        func walkViews(_ view: UIView) {
            walk(view, depth: 0)
            for subview in view.subviews { walkViews(subview) }
        }
        walkViews(root)
        return (controls, display)
    }

    /// Every label a view tree carries: its accessibility elements, and the
    /// views UIKit draws a toolbar's buttons with.
    static func allLabels(in root: UIView) -> [String] {
        var labels: [String] = []
        func walk(_ view: UIView) {
            if let label = view.accessibilityLabel, !label.isEmpty { labels.append(label) }
            for element in view.accessibilityElements ?? [] {
                if let label = (element as? NSObject)?.accessibilityLabel, !label.isEmpty { labels.append(label) }
            }
            for subview in view.subviews { walk(subview) }
        }
        walk(root)
        return labels
    }

    private func near(_ a: CGRect, _ b: CGRect, tolerance: CGFloat = 0.6) -> Bool {
        abs(a.minX - b.minX) < tolerance && abs(a.minY - b.minY) < tolerance
            && abs(a.width - b.width) < tolerance && abs(a.height - b.height) < tolerance
    }

    // MARK: Where the guest is

    @Test(arguments: Pose.allCases)
    func theGuestIsWhereThePlanPutsItInEveryPose(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        #expect(await eventually { near(rig.guestFrame, pose.guest) }, "\(pose): the guest is at \(rig.guestFrame)")
        #expect(rig.model.sessionPlan?.guest == pose.guest, "\(pose)")
        let frame = rig.guestFrame
        // Inside the guest's corners a touch reaches it; beside it, it does not.
        for point in [
            CGPoint(x: frame.minX + 2, y: frame.minY + 2), CGPoint(x: frame.minX + 2, y: frame.maxY - 2),
            CGPoint(x: frame.midX - 100, y: frame.midY),
        ] {
            #expect(rig.reachesGuest(point), "\(pose): \(point)")
        }
        let window = pose.reading.window
        for point in [CGPoint(x: window.maxX - 2, y: frame.midY), CGPoint(x: frame.midX, y: window.maxY - 2)]
        where !frame.contains(point) {
            #expect(!rig.reachesGuest(point), "\(pose): \(point) is beside the guest")
        }
        // Whatever is beside the guest stays black, in the light appearance
        // too: the window and the root carry it.
        #expect(rig.controller.view.backgroundColor == .black, "\(pose)")
        #expect(rig.lease.window.backgroundColor == .black, "\(pose)")
    }

    // MARK: Nothing of Andarium's on the fold while bent

    @Test(arguments: [Pose.uprightBook])
    func nothingOfAndariumsIsOnTheFoldWhenTheGuestSpansItWhileBent(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        // The pieces that can be on screen: the statistics and the keys.
        rig.model.session.showStats = true
        rig.model.session.sideKeysOpen = false
        try await Task.sleep(for: .milliseconds(600))
        rig.controller.view.layoutIfNeeded()
        let band = try #require(pose.reading.division).frame
        for open in [false, true] {
            rig.model.session.sideKeysOpen = open
            try await Task.sleep(for: .milliseconds(700))
            rig.controller.view.layoutIfNeeded()
            // Along the fold's band, edge to edge and a little in from each
            // margin, every touch is the guest's: no key, no capsule and no
            // notice is there.
            var points: [CGPoint] = []
            if band.height > band.width {
                for y in stride(from: CGFloat(10), to: pose.guest.height - 10, by: 25) {
                    points += [CGPoint(x: band.midX, y: y), CGPoint(x: band.minX + 2, y: y), CGPoint(x: band.maxX - 2, y: y)]
                }
            } else {
                for x in stride(from: CGFloat(10), to: pose.guest.width - 10, by: 25) {
                    points += [CGPoint(x: x, y: band.midY), CGPoint(x: x, y: band.minY + 2), CGPoint(x: x, y: band.maxY - 2)]
                }
            }
            for point in points {
                #expect(rig.reachesGuest(point), "\(pose), keys open \(open): \(point) is on the fold")
            }
            // And none of the application's own elements is there. The guest's picture spans the fold by design,
            // and the element that stands for it is the picture, not a control: it alone is left out, by identity,
            // and it has to be the display's, as big as the guest. A second element that spans the fold, or a
            // control that takes the display's label, is still found.
            let elements = rig.labelledApartFromDisplay()
            #expect(
                elements.display.count == 1 && elements.display.first?.node === rig.guest,
                "\(pose), keys open \(open): the display's own element is \(elements.display.map(\.label))")
            if let display = elements.display.first {
                #expect(
                    near(display.frame, rig.guestFrame),
                    "\(pose), keys open \(open): \(display.label) is at \(display.frame), the guest at \(rig.guestFrame)")
            }
            for element in elements.controls {
                #expect(!element.frame.intersects(band), "\(pose), keys open \(open): \(element.label) at \(element.frame)")
            }
            // Open, a touch on a key stays with the key: the keys take their
            // own touches on the inner display as they do on a phone.
            if open {
                let names = ["Power", "Settings", "Back", "Home", "Volume up", "Volume down"]
                let keys = rig.labelled().filter { names.contains($0.label) }
                #expect(Set(keys.map(\.label)) == Set(names), "\(pose): the open keys are \(keys.map(\.label))")
                for key in keys {
                    let centre = CGPoint(x: key.frame.midX, y: key.frame.midY)
                    #expect(!rig.reachesGuest(centre), "\(pose): \(key.label) at \(centre) reached the guest")
                }
            }
        }
    }

    /// Flat, the session is the iPad's and nothing is placed by the fold: the
    /// statistics hang centred from the top of the display, across the fold.
    @Test(arguments: [Pose.uprightFlat, .turnedFlat])
    func flatTheStatisticsHangCentredFromTheTopOfTheDisplayAsOnAnIPad(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        rig.model.session.showStats = true
        rig.model.session.sideKeysOpen = false
        try await Task.sleep(for: .milliseconds(700))
        rig.controller.view.layoutIfNeeded()
        let stats = try #require(rig.labelled().first { $0.label.hasPrefix("CPU") }, "no statistics in \(rig.labelled())").frame
        #expect(abs(stats.midX - pose.guest.midX) < 1.5, "\(pose): the statistics \(stats) are not centred on \(pose.guest)")
        #expect(stats.minY < pose.guest.minY + 80, "\(pose): the statistics \(stats) are not at the top")
        if pose == .uprightFlat {
            // Upright, the top centre is where the fold runs: nothing avoids it.
            let band = try #require(pose.reading.division).frame
            #expect(stats.intersects(band), "\(pose): the statistics \(stats) avoid the fold \(band)")
        }
    }

    @Test func standingTheLowerHalfHoldsTheKeysAndTheFoldHoldsNothing() async throws {
        let rig = try await Rig(.turnedBook, test: self)
        defer { rig.end() }
        rig.model.session.showStats = true
        try await Task.sleep(for: .milliseconds(700))
        rig.controller.view.layoutIfNeeded()
        let window = Pose.turnedBook.reading.window
        let band = try #require(Pose.turnedBook.reading.division).frame
        // The guest is the upper half alone: nothing from the fold down
        // reaches Android, and the upper half does.
        for y in stride(from: band.minY + 2, to: window.maxY - 2, by: 40) {
            #expect(!rig.reachesGuest(CGPoint(x: 330, y: y)), "y \(y) is below the guest")
        }
        #expect(rig.reachesGuest(CGPoint(x: 330, y: 400)))
        // Andarium's keys are in the lower half, none on the fold: the deck.
        let deck = CGRect(x: 0, y: band.maxY, width: window.width, height: window.height - band.maxY)
        let names = ["Back", "Home", "Settings", "Volume down", "Volume up", "Power"]
        let elements = rig.labelled()
        for name in names {
            let frames = elements.filter { $0.label == name }.map(\.frame)
            #expect(!frames.isEmpty, "no \(name) key on the deck: \(elements.map(\.label))")
            for frame in frames {
                #expect(deck.contains(frame.insetBy(dx: 1, dy: 1)), "\(name) is at \(frame)")
            }
        }
        // Nothing of Andarium's is in the band between the halves, and the
        // guest does not reach into it.
        for element in elements {
            #expect(!element.frame.intersects(band), "\(element.label) is on the fold at \(element.frame)")
        }
        for x in stride(from: CGFloat(10), to: window.width - 10, by: 25) {
            for y in [band.minY + 2, band.midY, band.maxY - 2] {
                #expect(!rig.reachesGuest(CGPoint(x: x, y: y)), "\(x), \(y)")
            }
        }
    }

    /// A session that began on the cover is unfolded: the keys come up on the
    /// inner display, closed, with their tip over them, as at the start of a
    /// session there. The tip's popover must not take a touch meant for the
    /// keys it points at (UIKit's popover dismisses itself on the first touch
    /// outside its card), whichever inner pose the device is unfolded to.
    @Test(arguments: [Pose.uprightBook, .uprightFlat, .turnedFlat])
    func theTipAtAnUnfoldLetsATouchOnTheKeysThrough(_ pose: Pose) async throws {
        let rig = try await Rig(.cover, test: self)
        defer { rig.end() }
        await rig.move(to: pose, test: self)
        let found = await eventually { rig.lease.window.rootViewController?.presentedViewController != nil }
        try #require(found, "\(pose): the tip did not show at the unfold")
        let popover = try #require(rig.lease.window.rootViewController?.presentedViewController?.popoverPresentationController)
        let screen = try #require(popover.presentingViewController.view)
        #expect(
            await eventually { popover.passthroughViews?.contains { $0 === screen } ?? false },
            "\(pose): the popover takes the touch on the keys: \(String(describing: popover.passthroughViews))")
    }

    // MARK: Folding, turning and bending

    /// Every change the walk makes, with the size the guest's display takes
    /// by it: only a change of the guest's shape gives a new one.
    @Test func theGuestIsNeverReplacedAndIsResizedOnlyWhereItsShapeChanges() async throws {
        let rig = try await Rig(.cover, test: self)
        defer { rig.end() }
        let display = rig.guest!
        #expect(await eventually { near(rig.guestFrame, Pose.cover.guest) })
        var sizes: [CGSize] = [display.bounds.size]
        let observation = display.layer.observe(\.bounds, options: [.new]) { layer, _ in
            MainActor.assumeIsolated {
                if layer.bounds.size != sizes.last { sizes.append(layer.bounds.size) }
            }
        }
        defer { observation.invalidate() }
        let walk: [(Pose, resizes: Bool)] = [
            (.uprightBook, true),   // the unfold
            (.uprightFlat, false),  // the hinge moves, held upright: nothing
            (.uprightBook, false),
            (.turnedFlat, true),    // the turn
            (.turnedBook, true),    // stand the device like a laptop
            (.turnedFlat, true),
            (.uprightFlat, true),
            (.cover, true),         // the fold
            (.cover, false),
        ]
        var previous = Pose.cover
        for (next, resizes) in walk {
            let before = sizes.count
            await rig.move(to: next, test: self)
            #expect(await eventually { near(rig.guestFrame, next.guest) }, "\(previous) to \(next): at \(rig.guestFrame)")
            #expect(sizes.count - before == (resizes ? 1 : 0), "\(previous) to \(next): sizes \(sizes.suffix(sizes.count - before))")
            #expect(rig.guest === display, "\(previous) to \(next): the guest's display was replaced")
            #expect(rig.model.phase == .running(.normal), "\(previous) to \(next)")
            #expect(SessionPlan.resizesGuest(from: SessionPlan(previous.reading), to: SessionPlan(next.reading)) == resizes)
            previous = next
        }
        // The sizes the guest took, one for each change of its shape, to the
        // pixel grid (the display lays out on the screen's pixels).
        let expected = [
            CGSize(width: 382, height: 644), CGSize(width: 951, height: 635), CGSize(width: 669, height: 917),
            CGSize(width: 669, height: 455.5), CGSize(width: 669, height: 917), CGSize(width: 951, height: 635),
            CGSize(width: 382, height: 644),
        ]
        #expect(sizes.count == expected.count)
        for (got, want) in zip(sizes, expected) {
            #expect(abs(got.width - want.width) < 0.34 && abs(got.height - want.height) < 0.34, "\(got) is not \(want)")
        }
    }

    // MARK: Rotation

    @Test(arguments: Pose.allCases)
    func theInnerDisplayTurnsTheGuestAsAnIPadDoesAndTheCoverAsAnIPhoneDoes(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        // An iPad's interface turns on every display; a phone's only on the
        // inner display of a folding one.
        let rotates = !phone || pose.inner
        #expect(await eventually { rig.model.hostDrivesGuestRotation == rotates }, "\(pose)")
        #expect(rig.controller.supportedInterfaceOrientations == (rotates ? .all : .portrait), "\(pose)")
        #expect(!rig.controller.prefersInterfaceOrientationLocked, "\(pose)")
        // A move to the other display changes the policy once the scene
        // root has read it, the session going on.
        let other: Pose = pose.inner ? .cover : .uprightBook
        await rig.move(to: other, test: self)
        let otherRotates = !phone || other.inner
        #expect(await eventually { rig.model.hostDrivesGuestRotation == otherRotates }, "\(pose) to \(other)")
        #expect(rig.controller.supportedInterfaceOrientations == (otherRotates ? .all : .portrait), "\(pose) to \(other)")
        #expect(rig.model.phase == .running(.normal))
    }

    /// Only the iPhone Duo folds: every other Simulator, device and Mac reports
    /// no hinge, and the scene reads as one that does not fold. It is asserted
    /// there, after the interaction has had the time to report one.
    @Test func aDisplayThatDoesNotFoldHasNoPoseAndKeepsItsLayout() async throws {
        var info = utsname()
        uname(&info)
        let machine = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"]
            ?? withUnsafeBytes(of: &info.machine) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        guard machine != "iPhone19,4" else { return }
        let entry = AppEntry.fromArguments()
        let model = steadyModel(entry)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        lease.window.rootViewController = controller
        defer { endSession(of: model, controller) }
        model.phase = .running(.normal)
        try await Task.sleep(for: .milliseconds(500))
        controller.view.layoutIfNeeded()
        #expect(!controller.foldReader.reportsHinge, "\(machine) reports a hinge")
        #expect(model.fold == nil, "\(machine)")
        #expect(model.sessionPlan == nil, "\(machine)")
        #expect(model.foldPose == .plain, "\(machine)")
    }

    // MARK: VoiceOver

    /// The statistics and the notices are read before the keys in every pose,
    /// as on the phone, where the keys are the last thing on the screen.
    @Test(arguments: Pose.allCases)
    func theStatisticsAreReadBeforeTheKeys(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        rig.model.session.showStats = true
        rig.model.session.sideKeysOpen = true
        try await Task.sleep(for: .milliseconds(900))
        rig.controller.view.layoutIfNeeded()
        let memoryMiB = rig.model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound
        let spoken = rig.model.session.readout.spoken(memoryMiB: memoryMiB, recovery: false)
        let labels = rig.labelled().map(\.label)
        let statistics = try #require(labels.firstIndex(of: spoken), "no statistics among \(labels)")
        let power = try #require(labels.firstIndex(of: "Power"), "no Power among \(labels)")
        #expect(statistics < power, "\(pose): the statistics come after Power in \(labels)")
    }

    // MARK: The other screens

    /// Welcome as `pose` lays it out, or as a display with no hinge does in
    /// the same window when `plain`: its column and its footer, and whether it
    /// is in a scroll view. Welcome is the view alone in a view the size of the
    /// pose, with the pose's safe area: the navigation stack Home puts it in
    /// lays out by the window the system has, which is not the pose's here, and
    /// its bar and strip are the captures'.
    private func welcome(on pose: Pose, plain: Bool = false) async throws -> (column: CGRect, footer: CGRect, scrolls: Bool) {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = steadyModel()
        model.guest = nil
        model.fold = plain ? nil : pose.reading
        let container = UIViewController()
        lease.window.rootViewController = container
        let holder = UIView(frame: pose.reading.window)
        container.view.addSubview(holder)
        let host = UIHostingController(rootView: WelcomeView().environment(model))
        container.addChild(host)
        host.view.frame = holder.bounds
        holder.addSubview(host.view)
        host.didMove(toParent: container)
        defer {
            host.willMove(toParent: nil)
            host.view.removeFromSuperview()
            host.removeFromParent()
            holder.removeFromSuperview()
        }
        // The window is the application's own, not the pose's: give the view
        // the pose's safe area, as the screen lays out in it.
        host.view.layoutIfNeeded()
        let window = pose.reading.window, safe = pose.reading.safeArea
        let have = host.view.safeAreaInsets, extra = host.additionalSafeAreaInsets
        host.additionalSafeAreaInsets = UIEdgeInsets(
            top: extra.top + safe.minY - window.minY - have.top,
            left: extra.left + safe.minX - window.minX - have.left,
            bottom: extra.bottom + window.maxY - safe.maxY - have.bottom,
            right: extra.right + window.maxX - safe.maxX - have.right)
        try await Task.sleep(for: .milliseconds(800))
        host.view.layoutIfNeeded()
        let elements = Self.labelled(in: host.view, relativeTo: holder)
        let shown = elements.filter {
            $0.label.contains("No virtual machine yet") || $0.label.contains("Set up virtual machine")
        }
        let column = try #require(shown.map(\.frame).reduce(nil as CGRect?) { $0?.union($1) ?? $1 }, "no Welcome")
        let footer = try #require(elements.first { $0.label.contains("free on this") }?.frame, "no footer")
        // An iPad app on a Mac has no accessibility element for a scroll indicator: there Welcome scrolls when a
        // scroll view holds it.
        func holdsScrollView(_ view: UIView) -> Bool { view is UIScrollView || view.subviews.contains(where: holdsScrollView) }
        let scrolls = ProcessInfo.processInfo.isiOSAppOnMac
            ? holdsScrollView(host.view) : elements.contains { $0.label.hasPrefix("Vertical scroll bar") }
        return (column, footer, scrolls)
    }

    /// Flat, Welcome is where it is on a display that does not fold, as on an
    /// iPad, in the same window; bent, it is on the half where the system puts
    /// its alerts, off the fold.
    @Test(arguments: Pose.allCases.filter(\.inner))
    func welcomeIsTheIPadsWhenFlatAndOnAHalfWhenBent(_ pose: Pose) async throws {
        let shown = try await welcome(on: pose)
        let band = try #require(pose.reading.division).frame
        if pose == .uprightFlat || pose == .turnedFlat {
            let expected = try await welcome(on: pose, plain: true)
            #expect(near(shown.column, expected.column), "\(pose): Welcome is at \(shown.column), not as with no hinge at \(expected.column)")
            #expect(near(shown.footer, expected.footer), "\(pose): the footer is at \(shown.footer), not as with no hinge at \(expected.footer)")
        } else {
            let card = try #require(FoldHalves(pose.reading)).card
            #expect(!shown.column.intersects(band), "\(pose): Welcome is on the fold at \(shown.column)")
            #expect(card.insetBy(dx: -1, dy: -1).contains(shown.column), "\(pose): Welcome \(shown.column) is not on \(card)")
        }
    }

    /// Welcome scrolls when taller than its place only while the device is
    /// bent, where a half can be shorter than the column (the end screen has
    /// the same modifier); flat and on the cover it is what a display with no
    /// hinge shows.
    @Test(arguments: Pose.allCases)
    func welcomeIsInAScrollViewOnlyWhileBent(_ pose: Pose) async throws {
        let shown = try await welcome(on: pose)
        #expect(shown.scrolls == pose.reading.pose.spreads, "\(pose): Welcome scrolls: \(shown.scrolls)")
        if pose == .uprightFlat {
            let plain = try await welcome(on: pose, plain: true)
            #expect(!plain.scrolls, "with no hinge Welcome scrolls")
        }
    }

    /// Home's Start, as a screen reads it: the bar's button (not Start in
    /// recovery), on `pose` or, when `plain`, on a display with no hinge. The
    /// rig is ended before it returns, for the window has one holder at a time.
    private func homeStart(on pose: Pose, plain: Bool = false) async throws -> CGRect {
        let rig = try await Rig(pose, phase: .home, withMachine: true, plain: plain, test: self)
        defer { rig.end() }
        return try #require(
            rig.labelled().first { $0.label == "Start" || $0.label.hasPrefix("Start and") }, "no Start in \(rig.labelled())"
        ).frame
    }

    /// Flat, Home is the iPad's: no pages, so Start is the same bar it is
    /// with no hinge, at the foot of the display across the fold; bent, Start
    /// is at the foot of the first page and clear of the fold.
    @Test(arguments: Pose.allCases.filter(\.inner))
    func homeIsTheIPadsListWhenFlatAndTwoPagesWhenBent(_ pose: Pose) async throws {
        let start = try await homeStart(on: pose)
        let band = try #require(pose.reading.division).frame
        if pose == .uprightFlat || pose == .turnedFlat {
            let expected = try await homeStart(on: pose, plain: true)
            #expect(near(start, expected), "\(pose): Start is at \(start), not as with no hinge at \(expected)")
        } else {
            #expect(!start.intersects(band), "\(pose): Start is on the fold at \(start)")
            if pose == .uprightBook {
                #expect(start.maxX <= band.minX + 1, "\(pose): Start \(start) is not on the leading page")
            } else {
                #expect(start.maxY <= band.minY + 1, "\(pose): Start \(start) is not above the fold")
            }
        }
    }

    /// A popup of the app: what sits over a screen, and the screen it sits on.
    enum Popup: CaseIterable, CustomStringConvertible, Sendable {
        case boot, slowBoot, busy, lowSpace, end, stopped

        var description: String {
            switch self {
            case .boot: "boot"
            case .slowBoot: "boot with the slow-start notice"
            case .busy: "busy card"
            case .lowSpace: "low-space card"
            case .end: "end card"
            case .stopped: "stopped end card"
            }
        }

        var phase: Phase {
            switch self {
            case .boot, .slowBoot: .booting(.normal)
            case .end, .stopped: .failed
            case .busy, .lowSpace: .home
            }
        }

        /// What the model shows over `phase`.
        @MainActor func show(on model: AppModel) {
            switch self {
            case .busy:
                model.busy = BusyState(title: "Erasing virtual machine", detail: "Deleting everything.", progress: 0.65)
            case .lowSpace:
                model.spacePause = SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .notPaused)
            case .slowBoot:
                // A start that has run 3 minutes 17 seconds, by the model's own clock.
                let now = model.clock()
                model.slowStart.begin(at: now - 197)
                _ = model.slowStart.update(at: now)
            case .stopped:
                model.runEnd = RunEnd(result: -5, stoppedByUser: true)
            case .boot, .end:
                break
            }
        }

        /// Words of the popup, whose frames stand for it.
        var words: [String] {
            switch self {
            case .boot: ["Linux kernel", "System image", "Storage", "Startup"]
            // The notice itself: the steps beneath it may need scrolling in a half shorter than the
            // card, as in the standing pose's upper half, and a scrolled-off step is still a frame.
            case .slowBoot: ["Starting is taking longer than usual", "Stop"]
            case .busy: ["Erasing virtual machine", "65%", "Deleting everything."]
            case .lowSpace: [SpacePauseWords.title(SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .notPaused))]
            case .end: ["The virtual machine stopped unexpectedly"]
            case .stopped: ["You stopped the virtual machine"]
            }
        }
    }

    /// The frame of `popup` on `pose`, or on a display that reports no hinge
    /// when `plain`: the union of what its words name, and the space the root
    /// is given. The space is the pose's safe area on a display as large as
    /// the pose; a smaller window cannot give the root less than its own safe
    /// area.
    private func frame(of popup: Popup, on pose: Pose, plain: Bool = false) async throws -> (frame: CGRect, space: CGRect) {
        let rig = try await Rig(pose, phase: popup.phase, plain: plain, test: self)
        defer { rig.end() }
        popup.show(on: rig.model)
        try await Task.sleep(for: .milliseconds(900))
        rig.controller.view.layoutIfNeeded()
        let shown = rig.labelled().filter { element in popup.words.contains { element.label.contains($0) } }
        let frame = try #require(
            shown.map(\.frame).reduce(nil as CGRect?) { $0?.union($1) ?? $1 }, "\(pose): no \(popup) in \(rig.labelled())")
        return (frame, rig.controller.view.bounds.inset(by: rig.controller.view.safeAreaInsets))
    }

    /// Flat, every popup is where it is on a display that does not fold, as on
    /// an iPad, in the same window; bent, it keeps to a half, the boot of a
    /// standing device to the upper one.
    @Test(arguments: Pose.allCases.filter(\.inner), Popup.allCases)
    func everyPopupIsTheIPadsWhenFlatAndKeepsToAHalfWhenBent(_ pose: Pose, _ popup: Popup) async throws {
        let band = try #require(pose.reading.division).frame
        let (shown, _) = try await frame(of: popup, on: pose)
        if pose == .uprightFlat || pose == .turnedFlat {
            let (plain, _) = try await frame(of: popup, on: pose, plain: true)
            #expect(near(shown, plain), "\(pose): the \(popup) is at \(shown), not as with no hinge at \(plain)")
        } else {
            let halves = try #require(FoldHalves(pose.reading))
            let place = (popup == .boot || popup == .slowBoot) && pose == .turnedBook ? halves.first : halves.card
            #expect(!shown.intersects(band), "\(pose): the \(popup) is on the fold at \(shown)")
            #expect(place.insetBy(dx: -1, dy: -1).contains(shown), "\(pose): the \(popup) \(shown) is not on \(place)")
        }
    }

    /// A card is kept to its place by the space it is given, not by the
    /// window: in a sheet that is only the leading half wide, the
    /// busy card must not be padded away, and in a sheet the
    /// system centres on a flat display it is centred in the sheet.
    @Test(arguments: [(pose: Pose.uprightBook, sheet: CGRect(x: 0, y: 0, width: 455.5, height: 500)),
                      (pose: Pose.uprightFlat, sheet: CGRect(x: 150, y: 0, width: 651, height: 500)),
                      (pose: Pose.turnedFlat, sheet: CGRect(x: 60, y: 200, width: 549, height: 600))])
    func aCardInASheetsOwnFrameIsNotPaddedAwayByTheWindowsHalves(_ test: (pose: Pose, sheet: CGRect)) async throws {
        let model = steadyModel()
        model.fold = test.pose.reading
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let host = UIHostingController(rootView: Text("Sending").onCardHalf().environment(model))
        // The window is the application's own, and its safe area is not the
        // sheet's: the sheet takes none.
        host.safeAreaRegions = []
        container.addChild(host)
        let holder = UIView(frame: test.pose.reading.window)
        container.view.addSubview(holder)
        host.view.frame = test.sheet
        holder.addSubview(host.view)
        host.didMove(toParent: container)
        try await Task.sleep(for: .milliseconds(700))
        host.view.layoutIfNeeded()
        let text = try #require(Self.labelled(in: host.view, relativeTo: holder).first { $0.label == "Sending" }, "no card")
        #expect(text.frame.width > 40, "the card collapsed to \(text.frame)")
        #expect(host.view.frame.insetBy(dx: -1, dy: -1).contains(text.frame), "the card is at \(text.frame)")
        if test.pose != .uprightBook {
            #expect(abs(text.frame.midX - test.sheet.midX) < 1, "\(test.pose): the card \(text.frame) is not centred in the sheet")
        }
        host.willMove(toParent: nil)
        host.view.removeFromSuperview()
        host.removeFromParent()
        holder.removeFromSuperview()
    }

    /// A card taller than its place is centred on it where nothing places it
    /// by a hinge (a display with none, the cover and a flat inner display),
    /// as it always was, and overflows from the top of the half it keeps to
    /// only while the device is bent, where it is the first thing read.
    @Test(arguments: [Pose?.none, .cover, .uprightFlat, .turnedFlat, .turnedBook])
    func aCardTallerThanItsPlaceIsCentredOnItExceptOnABentHalf(_ pose: Pose?) async throws {
        final class Frame: @unchecked Sendable { var rect = CGRect.zero }
        let seen = Frame()
        let model = steadyModel()
        model.fold = pose?.reading
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let tall = Color.red.frame(width: 200, height: 900)
            .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { seen.rect = $0 }
        let host = UIHostingController(rootView: tall.onCardHalf().environment(model))
        host.safeAreaRegions = []
        container.addChild(host)
        let holder = UIView(frame: CGRect(x: 0, y: 0, width: 400, height: 500))
        container.view.addSubview(holder)
        host.view.frame = holder.bounds
        holder.addSubview(host.view)
        host.didMove(toParent: container)
        defer {
            host.willMove(toParent: nil)
            host.view.removeFromSuperview()
            host.removeFromParent()
            holder.removeFromSuperview()
        }
        try await Task.sleep(for: .milliseconds(700))
        host.view.layoutIfNeeded()
        let card = seen.rect
        let name = pose.map(String.init(describing:)) ?? "no hinge"
        #expect(card.height > 880, "\(name): the card is \(card)")
        if let pose, pose == .turnedBook {
            // A bent half: the overflow goes off the bottom, the top stays at
            // the half's.
            let top = try #require(FoldHalves(pose.reading)).insets(for: .card, in: holder.bounds).top
            #expect(abs(card.minY - top) < 1, "\(name): the card \(card) does not start at the half's top, \(top)")
        } else {
            // Centred on the space: with no hinge, on the cover and flat.
            #expect(abs(card.midY - holder.bounds.midY) < 1, "\(name): the card \(card) is not centred on \(holder.bounds)")
        }
    }

    /// Turning the display with a sheet open leaves the sheet's content as it
    /// is: a sheet decides its presentation once, when it is presented.
    @Test(arguments: [(from: Pose.uprightBook, to: Pose.turnedFlat), (from: Pose.turnedBook, to: Pose.uprightFlat)])
    func turningTheDisplayWithASheetOpenKeepsItsContent(_ turn: (from: Pose, to: Pose)) async throws {
        let model = steadyModel()
        model.fold = turn.from.reading
        let counter = SheetProbe.Counter()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let host = UIHostingController(rootView: SheetProbe(model: model, counter: counter))
        lease.window.rootViewController = host
        #expect(await eventually { counter.appeared == 1 }, "the sheet did not appear")
        let token = counter.tokens.first
        model.fold = turn.to.reading
        try await Task.sleep(for: .milliseconds(900))
        model.fold = turn.from.reading
        try await Task.sleep(for: .milliseconds(900))
        #expect(counter.appeared == 1, "\(turn): the sheet's content was built \(counter.appeared) times")
        #expect(counter.disappeared == 0, "\(turn): the sheet's content went away")
        #expect(counter.tokens.count == 1 && counter.tokens.first == token, "\(turn): the sheet's state was lost")
        host.dismiss(animated: false)
        #expect(await eventually { host.presentedViewController == nil })
        lease.window.rootViewController = nil
    }

    /// A sheet is kept below the fold only while the device stands bent; flat,
    /// it is the system's, as on an iPad.
    @Test(arguments: Pose.keepingSheetHeights([.turnedBook, .turnedFlat, .uprightFlat]))
    func aSheetRisesToTheFoldOnlyWhileTheDeviceStandsBent(_ pose: Pose) async throws {
        let model = steadyModel()
        model.fold = pose.reading
        let counter = SheetProbe.Counter()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let host = UIHostingController(rootView: SheetProbe(model: model, counter: counter))
        lease.window.rootViewController = host
        #expect(await eventually { counter.appeared == 1 }, "the sheet did not appear")
        let detents = host.presentedViewController?.sheetPresentationController?.detents ?? []
        if pose == .turnedBook {
            #expect(!detents.isEmpty && !detents.contains { $0.identifier == .large }, "\(pose): the detents are \(detents)")
        } else {
            #expect(detents.allSatisfy { $0.identifier == .large }, "\(pose): the detents are \(detents)")
        }
        host.dismiss(animated: false)
        #expect(await eventually { host.presentedViewController == nil })
        lease.window.rootViewController = nil
    }

    /// A sheet with a counter of how many times its content was built and a
    /// piece of state that stands for what the person typed.
    struct SheetProbe: View {
        @MainActor final class Counter {
            var appeared = 0
            var disappeared = 0
            var tokens: Set<UUID> = []
        }

        let model: AppModel
        let counter: Counter
        @State private var presented = true

        var body: some View {
            Color.clear.sheet(isPresented: $presented) {
                Content(counter: counter).foldSheet(model)
            }
        }

        struct Content: View {
            let counter: Counter
            @State private var typed = UUID()

            var body: some View {
                Text("typed")
                    .onAppear {
                        counter.appeared += 1
                        counter.tokens.insert(typed)
                    }
                    .onDisappear { counter.disappeared += 1 }
            }
        }
    }

    // MARK: The file browser

    private static let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000)

    /// A model whose guest is running with its storage up, and a browser in
    /// `Download`.
    private func browsing() async -> (AppModel, FileBrowserModel) {
        let host = Self.host
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        driver.storage = true
        model.driver = driver
        model.refreshPlan()
        let download = GuestPath.child("Download", of: GuestPath.root)
        let backend = FileBrowserModelTests.ScriptedBackend()
        backend.folders[download] = [
            GuestFile(path: GuestPath.child("Trip.jpg", of: download), isFolder: false, size: 4_000_000, modified: .now),
        ]
        let browser = FileBrowserModel(backend: backend, center: NotificationCenter())
        model.fileBrowser = browser
        model.start(.normal)
        model.finishBoot(.normal)
        await model.refreshStorage()
        browser.route = [FolderRoute(path: GuestPath.root), FolderRoute(path: download)]
        await browser.load(download)
        return (model, browser)
    }

    /// On the cover Import is a round button of the toolbar, and the bar at
    /// the foot has the trail alone until something is selected; in every
    /// other pose, and on a display that does not fold, it is the labelled
    /// button in the bar and the toolbar has none.
    /// A nil pose is a display that reports no hinge.
    @Test(arguments: [Pose?.none] + Pose.allCases.map { Pose?.some($0) })
    func importIsARoundToolbarButtonOnTheCoverAndLabelledInTheBarElsewhere(_ pose: Pose?) async throws {
        let (model, browser) = await browsing()
        model.fold = pose?.reading
        let window = (pose ?? .uprightFlat).reading.window
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let download = GuestPath.child("Download", of: GuestPath.root)
        func present<Root: View>(_ root: Root) async throws -> ([String], () -> Void) {
            let host = UIHostingController(rootView: root.environment(model))
            container.addChild(host)
            host.view.frame = window
            container.view.addSubview(host.view)
            host.didMove(toParent: container)
            try await Task.sleep(for: .milliseconds(900))
            host.view.layoutIfNeeded()
            return (Self.allLabels(in: host.view), {
                host.willMove(toParent: nil)
                host.view.removeFromSuperview()
                host.removeFromParent()
            })
        }
        let importLabel = "Import"
        let cover = pose == .cover
        let pose = pose.map { "\($0)" } ?? "no hinge"
        // The toolbar.
        let (page, removePage) = try await present(NavigationStack {
            FolderPage(browser: browser, route: FolderRoute(path: download), close: {})
        })
        #expect(page.contains(importLabel) == cover, "\(pose): the page's labels are \(page)")
        removePage()
        // The bar.
        let (bar, removeBar) = try await present(FilesBar(browser: browser, usable: true, bottomInset: 0))
        #expect(bar.contains(importLabel) == !cover, "\(pose): the bar's labels are \(bar)")
        #expect(!bar.contains("Export"), "\(pose): nothing is selected: \(bar)")
        removeBar()
        // Something selected: Export replaces Import, and stays in the bar.
        let file = try #require(
            browser.entries(in: download).first(where: { !$0.isFolder }), "the folder has no file to select")
        browser.selection = [file]
        let (selected, removeSelected) = try await present(FilesBar(browser: browser, usable: true, bottomInset: 0))
        #expect(selected.contains("Export"), "\(pose): with a selection the bar's labels are \(selected)")
        #expect(!selected.contains(importLabel), "\(pose): with a selection the bar's labels are \(selected)")
        removeSelected()
        browser.selection = []
    }

    // MARK: The notices

    /// The notices hang from the top of the pose's HUD: the camera notice's
    /// words and its button inside the HUD, inside the window, and clear of the
    /// fold while the screen is bent.
    @Test(arguments: Pose.allCases)
    func aNoticeIsInTheHudOfEveryPose(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        let plan = try #require(rig.model.sessionPlan, "\(pose): no plan")
        rig.model.cameraAccess = .notDetermined
        rig.model.cameraCaptureChanged(.blockedInUse)
        var parts: (title: CGRect, detail: CGRect, dismiss: CGRect)?
        _ = await eventually {
            rig.controller.view.layoutIfNeeded()
            let labelled = rig.labelled()
            guard let title = labelled.first(where: { $0.label == CameraWords.offTitle })?.frame,
                let detail = labelled.first(where: { $0.label == CameraWords.offDetail })?.frame,
                let dismiss = labelled.first(where: { $0.label == NetworkWords.dismissLabel })?.frame
            else { return false }
            parts = (title, detail, dismiss)
            return true
        }
        let found = try #require(
            parts, "\(pose): the notice was not found: is application accessibility on in this Simulator?")
        let frame = found.title.union(found.detail).union(found.dismiss)
        #expect(
            plan.hud.insetBy(dx: -0.5, dy: -0.5).contains(frame), "\(pose): \(frame) is not in the HUD \(plan.hud)")
        #expect(pose.reading.window.contains(frame), "\(pose): \(frame) is not in the window")
        if let division = pose.reading.division, division.isActive {
            #expect(!frame.intersects(division.frame), "\(pose): \(frame) is on the fold \(division.frame)")
        }
    }
}

/// The Simulator's own display, in the pose the harness put it in
/// (`TEST_RUNNER_ANDARIUM_FOLD_POSE`: `cover`, `upright-book`, `upright-flat`,
/// `turned-book`, `turned-flat`). Without the variable the suite checks
/// nothing, so the ordinary runs on other devices pass.
@MainActor
@Suite(.serialized)
struct TheRealDisplay {
    private var expected: FoldSessionHostingTests.Pose? {
        switch ProcessInfo.processInfo.environment["ANDARIUM_FOLD_POSE"] {
        case "cover": .cover
        case "upright-book": .uprightBook
        case "upright-flat": .uprightFlat
        case "turned-book": .turnedBook
        case "turned-flat": .turnedFlat
        default: nil
        }
    }

    private func descendants(of view: UIView) -> [UIView] {
        view.subviews + view.subviews.flatMap { descendants(of: $0) }
    }

    /// Ends the session and lets the root lay out once, so that it hands the
    /// window back as the application left it (a running session paints the
    /// window black) before the next test takes it.
    private func endSession(of model: AppModel, _ controller: UIViewController) {
        model.phase = .home
        controller.view.setNeedsLayout()
        controller.view.layoutIfNeeded()
    }

    @Test func theSceneReadsAsTheExpectedPoseAndTheGuestFollowsThePlan() async throws {
        guard let pose = expected else { return }
        let entry = AppEntry.fromArguments()
        let model = steadyModel(entry)
        model.launchApplied = true
        model.driver = StubDriver(plan: nil)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: RootView(entry: entry).environment(model), model: model)
        lease.window.rootViewController = controller
        defer { endSession(of: model, controller) }
        model.phase = .running(.normal)
        let deadline = ContinuousClock.now + .seconds(20)
        while model.sessionPlan == nil, ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(50)) }
        try await Task.sleep(for: .milliseconds(600))
        controller.view.layoutIfNeeded()
        // The pose the system reports, and the plan the design gives it.
        #expect(model.foldPose == pose.foldPose, "the scene reads as \(model.foldPose)")
        let plan = try #require(model.sessionPlan)
        #expect(plan.guest == pose.guest, "the plan's guest is \(plan.guest)")
        // The guest's display is there.
        var display: GuestMetalView?
        let end = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < end {
            controller.view.layoutIfNeeded()
            display = descendants(of: controller.view).compactMap { $0 as? GuestMetalView }.first
            if let display, display.bounds.width > 0 { break }
            try await Task.sleep(for: .milliseconds(50))
        }
        let shown = try #require(display)
        let frame = shown.convert(shown.bounds, to: lease.window)
        #expect(abs(frame.minX - pose.guest.minX) < 0.6 && abs(frame.minY - pose.guest.minY) < 0.6
                && abs(frame.width - pose.guest.width) < 0.6 && abs(frame.height - pose.guest.height) < 0.6,
                "the display is at \(frame)")
        // The rotation rule.
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        let rotates = !phone || pose.inner
        #expect(model.hostDrivesGuestRotation == rotates)
        #expect(controller.supportedInterfaceOrientations == (rotates ? .all : .portrait))
    }
}
