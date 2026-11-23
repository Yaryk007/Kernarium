// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The scene root's update (`AndariumHostingController.scheduleUpdate`) runs
/// only when something it reads has changed since the last run, however many
/// layouts and geometry updates ask for it, and tells the driver once for each
/// change. The inputs come from a source the test holds, so each can be moved
/// on its own; the real scene's are covered by `SessionOrientationHostingTests`.
@MainActor
@Suite(.serialized)
struct SceneUpdateTests {
    private final class Token {}

    /// The inputs the root reads, which a test changes between triggers.
    private final class Source {
        var inputs: SceneUpdateInputs?
        let scene = Token()
        let driver = Token()

        init() { inputs = base() }

        func base() -> SceneUpdateInputs {
            SceneUpdateInputs(
                scene: ObjectIdentifier(scene), driver: ObjectIdentifier(driver),
                sceneSize: CGSize(width: 393, height: 852), screenSize: CGSize(width: 393, height: 852),
                interfaceOrientation: UIInterfaceOrientation.portrait.rawValue, phase: .home)
        }
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(20))
        }
        return condition()
    }

    /// Everything that can ask for the update: layouts of the root, geometry
    /// updates from the scene delegate (and the phase watcher, which calls the
    /// same entry).
    private func askForTheUpdate<Content: View>(_ controller: AndariumHostingController<Content>) {
        for _ in 0..<5 {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            controller.sceneGeometryDidChange()
        }
    }

    private func settle() async {
        try? await Task.sleep(for: .milliseconds(150))
    }

    private struct Rig {
        let model: AppModel
        let driver: StubDriver
        let controller: AndariumHostingController<Color>
        let source: Source
        let lease: HostWindow.Lease
    }

    private func makeRig() async throws -> Rig {
        let model = AppEntry.fromArguments().makeModel()
        let driver = StubDriver(plan: nil)
        model.driver = driver
        let lease = try await HostWindow.take()
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        let source = Source()
        controller.inputsSource = { [source] in source.inputs }
        lease.window.rootViewController = controller
        return Rig(model: model, driver: driver, controller: controller, source: source, lease: lease)
    }

    @Test func theFirstUpdateTellsTheDriverAndNothingFollowsWhileNothingChanges() async throws {
        let rig = try await makeRig()
        defer { rig.lease.give() }
        #expect(await eventually { rig.driver.geometryChanges == 1 })
        await settle()
        // Layouts and geometry updates by the dozen with every input where the
        // last update found it: nothing is scheduled, so the driver hears
        // nothing.
        for _ in 0..<10 { askForTheUpdate(rig.controller) }
        await settle()
        #expect(rig.driver.geometryChanges == 1)
    }

    @Test func aRootOutsideAnySceneHasNothingToReadAndTellsNoOne() async throws {
        let rig = try await makeRig()
        defer { rig.lease.give() }
        #expect(await eventually { rig.driver.geometryChanges == 1 })
        rig.source.inputs = nil
        askForTheUpdate(rig.controller)
        await settle()
        #expect(rig.driver.geometryChanges == 1)
        // Back in a scene where it was: nothing has changed.
        rig.source.inputs = rig.source.base()
        askForTheUpdate(rig.controller)
        await settle()
        #expect(rig.driver.geometryChanges == 1)
    }

    @Test func eachInputsChangeReachesTheDriverOnceWhateverAsksForTheUpdate() async throws {
        let rig = try await makeRig()
        defer { rig.lease.give() }
        #expect(await eventually { rig.driver.geometryChanges == 1 })
        await settle()
        let other = Token()
        var heard = 1
        let changes: [(String, (inout SceneUpdateInputs) -> Void)] = [
            ("the scene", { $0.scene = ObjectIdentifier(other) }),
            ("the driver", { $0.driver = ObjectIdentifier(other) }),
            ("the scene's width", { $0.sceneSize.width = 320 }),
            ("the scene's height", { $0.sceneSize.height = 600 }),
            ("the screen's width", { $0.screenSize.width = 430 }),
            ("the screen's height", { $0.screenSize.height = 932 }),
            ("the interface orientation", { $0.interfaceOrientation = UIInterfaceOrientation.landscapeLeft.rawValue }),
            ("the phase", { $0.phase = .running(.normal) }),
            ("the mode of the phase", { $0.phase = .running(.recovery) }),
        ]
        for (name, change) in changes {
            var inputs = try #require(rig.source.inputs)
            change(&inputs)
            rig.source.inputs = inputs
            askForTheUpdate(rig.controller)
            heard += 1
            #expect(await eventually { rig.driver.geometryChanges == heard }, "\(name) reaches the driver")
            // The same change asked for again, by anything, is not news.
            askForTheUpdate(rig.controller)
            await settle()
            #expect(rig.driver.geometryChanges == heard, "\(name) reaches it once")
        }
    }

    @Test func aChangeBackToWhereItWasIsAChangeToo() async throws {
        let rig = try await makeRig()
        defer { rig.lease.give() }
        #expect(await eventually { rig.driver.geometryChanges == 1 })
        let portrait = try #require(rig.source.inputs)
        var landscape = portrait
        landscape.interfaceOrientation = UIInterfaceOrientation.landscapeRight.rawValue
        rig.source.inputs = landscape
        askForTheUpdate(rig.controller)
        #expect(await eventually { rig.driver.geometryChanges == 2 })
        rig.source.inputs = portrait
        askForTheUpdate(rig.controller)
        #expect(await eventually { rig.driver.geometryChanges == 3 })
        await settle()
        #expect(rig.driver.geometryChanges == 3)
    }

    @Test func theRealSceneAndModelAreTheInputsWhenNoTestSuppliesThem() async throws {
        let model = AppEntry.fromArguments().makeModel()
        let first = StubDriver(plan: nil)
        model.driver = first
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        lease.window.rootViewController = controller
        #expect(await eventually { first.geometryChanges > 0 })
        await settle()
        let before = first.geometryChanges
        for _ in 0..<10 { askForTheUpdate(controller) }
        await settle()
        #expect(first.geometryChanges == before, "the scene is where it was")
        // A phase change is an input, and reaches the driver at once (a second
        // call may follow a layout the phase change causes).
        model.phase = .running(.normal)
        #expect(await eventually { first.geometryChanges > before })
        await settle()
        #expect(first.geometryChanges <= before + 2)
        // Another driver is told even though the scene did not change.
        let second = StubDriver(plan: nil)
        model.driver = second
        controller.sceneGeometryDidChange()
        #expect(await eventually { second.geometryChanges == 1 })
        await settle()
        #expect(second.geometryChanges == 1)
        model.phase = .home
    }
}
