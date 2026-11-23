// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Home's AOT row as the application draws it: the second row of the
/// Performance section, below "120 Hz" where that is offered and above JIT;
/// there only in a build that carries the translated-code framework; its
/// switch is the person's choice (on for a fresh model); and while the next
/// start runs on the JIT engine, which does not use the translated code, it is
/// dimmed and says so. What a start states to the runtime follows the same
/// facts.
@MainActor
@Suite(.serialized)
struct TranslatedLibrariesRowTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func fixture(
        carried: Bool, jit: Bool = true, highRefresh: Bool = false
    ) -> (model: AppModel, driver: StubDriver) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.jitAvailable = jit
        model.translatedLibrariesAvailable = carried
        if highRefresh {
            // A screen that shows 120 frames a second is offered "120 Hz".
            model.statedNativeScale = 3
            model.statedDisplayPoints = CGSize(width: 402, height: 778)
            model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
            model.displayPointsDidChange(CGSize(width: 402, height: 778))
            model.launchApplied = true
        }
        return (model, driver)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    /// The switches under `view`, top to bottom.
    private func switches(in view: UIView) -> [UISwitch] {
        var found: [UISwitch] = []
        func walk(_ node: UIView) {
            if let control = node as? UISwitch { found.append(control) }
            for child in node.subviews { walk(child) }
        }
        walk(view)
        return found.sorted {
            $0.convert($0.bounds, to: view).minY < $1.convert($1.bounds, to: view).minY
        }
    }

    /// Every accessibility node under `node`.
    private func nodes(in node: NSObject, depth: Int = 0) -> [NSObject] {
        guard depth < 64 else { return [] }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        return [node] + children.flatMap { nodes(in: $0, depth: depth + 1) }
    }

    /// The nodes of the AOT row (the ones whose label starts with its title)
    /// once the label `expected` is there, or after ten seconds. SwiftUI
    /// publishes its accessibility elements only while application
    /// accessibility is on (the Simulator preference
    /// `ApplicationAccessibilityEnabled`, as `Kernarium/scripts/run-ios-simulator-tests.sh`
    /// sets it).
    private func aotRow(in view: UIView, expecting expected: String) async -> [NSObject] {
        var found: [NSObject] = []
        let deadline = ContinuousClock.now + .seconds(10)
        repeat {
            view.setNeedsLayout()
            view.layoutIfNeeded()
            found = nodes(in: view).filter { ($0.accessibilityLabel ?? "").hasPrefix("AOT") }
            if found.contains(where: { $0.accessibilityLabel == expected }) { break }
            try? await Task.sleep(for: .milliseconds(50))
        } while ContinuousClock.now < deadline
        return found
    }

    /// Hosts the section the rows are in, on the application's window at a
    /// phone's width. A section that is not there draws nothing, so the caller
    /// says how many switches to wait for, and an absent one is given the time
    /// it would take to appear.
    private func hosted(
        _ model: AppModel, switches count: Int, in lease: HostWindow.Lease
    ) async throws -> UIHostingController<some View> {
        let controller = UIHostingController(rootView: HomePerformanceSection().environment(model).frame(width: 390))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        let drawn = await eventually {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            return switches(in: controller.view).count == count
        }
        try #require(drawn, "the section drew \(switches(in: controller.view).count) switches, not \(count)")
        if count == 0 {
            // Give a section that should not be there the time it would take.
            try? await Task.sleep(for: .milliseconds(400))
            controller.view.layoutIfNeeded()
            #expect(switches(in: controller.view).isEmpty)
        }
        return controller
    }

    @Test func aFreshModelWantsTheLibrariesAndCarriesNoRow() {
        let fresh = fixture(carried: false).model
        #expect(fresh.translatedLibrariesOn)
        #expect(!fresh.translatedLibrariesAvailable)
        #expect(fresh.translatedLibrariesToKeep == true)
    }

    @Test func theRowIsThereOnlyInABuildThatCarriesTheLibraries() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        // The JIT row alone: no framework, no row.
        _ = try await hosted(fixture(carried: false).model, switches: 1, in: lease)
        // With the framework, a second switch is there, above JIT's.
        let carried = try await hosted(fixture(carried: true).model, switches: 2, in: lease)
        let control = try #require(switches(in: carried.view).first)
        #expect(control.isOn)
        #expect(control.isEnabled)
    }

    @Test func theRowIsAboveJitInTheSameSection() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true)
        model.jitOn = true
        model.jitDebuggerAttached = false
        let controller = try await hosted(model, switches: 2, in: lease)
        let all = switches(in: controller.view)
        // JIT's switch is on (the person asked for it) and AOT's, on by
        // default, is above it: the Performance section draws both, so the AOT
        // row is a row of that section.
        #expect(all.map(\.isOn) == [true, true])
        let frames = all.map { $0.convert($0.bounds, to: controller.view) }
        #expect(frames[0].maxY < frames[1].minY, "the AOT switch is not above the JIT switch")
        // They are in one panel: the same width and trailing edge.
        #expect(abs(frames[0].maxX - frames[1].maxX) < 1)
    }

    @Test func theRowIsBetweenTheHundredAndTwentyHertzRowAndJit() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true, highRefresh: true)
        model.jitOn = true
        model.jitDebuggerAttached = false
        model.translatedLibrariesOn = false
        let controller = try await hosted(model, switches: 3, in: lease)
        let all = switches(in: controller.view)
        // "120 Hz" is off at first, AOT is off here, JIT is on: the order is
        // the section's, whatever the states.
        #expect(all.map(\.isOn) == [false, false, true], "120 Hz, AOT, JIT")
        let frames = all.map { $0.convert($0.bounds, to: controller.view) }
        #expect(frames[0].maxY < frames[1].minY && frames[1].maxY < frames[2].minY, "\(frames)")
        #expect(frames.allSatisfy { abs($0.maxX - frames[0].maxX) < 1 }, "one panel: \(frames)")
    }

    @Test func aBuildWithoutAJitHasTheRowAloneInTheSection() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true, jit: false)
        let controller = try await hosted(model, switches: 1, in: lease)
        #expect(switches(in: controller.view).first?.isOn == true)
        // Neither framework nor JIT: no section at all.
        let bare = fixture(carried: false, jit: false).model
        _ = try await hosted(bare, switches: 0, in: lease)
    }

    @Test func noGuestNoRow() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (empty, _) = fixture(carried: true)
        empty.guest = nil
        _ = try await hosted(empty, switches: 0, in: lease)
    }

    @Test func theSwitchIsThePersonsChoiceAndRoundTrips() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true)
        let before = model.guest
        let controller = try await hosted(model, switches: 2, in: lease)
        var control = try #require(switches(in: controller.view).first)
        control.setOn(false, animated: false)
        control.sendActions(for: .valueChanged)
        #expect(await eventually { !model.translatedLibrariesOn })
        // It changes nothing in the guest or in the JIT until a start.
        #expect(model.guest == before)
        #expect(!model.jitOn)

        control = try #require(switches(in: controller.view).first)
        #expect(!control.isOn)
        control.setOn(true, animated: false)
        control.sendActions(for: .valueChanged)
        #expect(await eventually { model.translatedLibrariesOn })
    }

    @Test func theSwitchShowsTheChoiceItIsGiven() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true)
        model.translatedLibrariesOn = false
        let controller = try await hosted(model, switches: 2, in: lease)
        #expect(try #require(switches(in: controller.view).first).isOn == false)
    }

    @Test func whileTheNextStartRunsOnTheJitTheRowIsDimmedAndTheSwitchDisabled() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, driver) = fixture(carried: true)
        driver.debuggerAttached = true
        // The JIT is wanted and a debugger is attached: the next start runs on it.
        model.jitOn = true
        model.jitDebuggerAttached = true
        #expect(model.jitInUseForNextStart)
        let controller = try await hosted(model, switches: 2, in: lease)
        let control = try #require(switches(in: controller.view).first)
        #expect(!control.isEnabled)
        // The choice itself is kept as it was.
        #expect(control.isOn)
        #expect(model.translatedLibrariesOn)
        // VoiceOver hears the title and then the reason, as the label and so
        // with hints off, and nothing is left to a hint that would repeat it.
        let reason = "AOT, Not used while JIT is on."
        let dimmed = await aotRow(in: controller.view, expecting: reason)
        #expect(Set(dimmed.compactMap(\.accessibilityLabel)) == [reason], "\(dimmed.map(\.accessibilityLabel))")
        #expect(dimmed.allSatisfy { ($0.accessibilityHint ?? "").isEmpty }, "the reason is also a hint")

        // The JIT off, or no debugger to prepare it: the start runs on the engine
        // that uses them, and the switch works.
        model.jitOn = false
        #expect(await eventually { switches(in: controller.view).first?.isEnabled == true })
        let note = "AOT, Translated system libraries run faster. Turn off only to troubleshoot."
        let working = await aotRow(in: controller.view, expecting: note)
        #expect(Set(working.compactMap(\.accessibilityLabel)) == [note], "\(working.map(\.accessibilityLabel))")
        #expect(working.allSatisfy { ($0.accessibilityHint ?? "").isEmpty }, "the note is also a hint")
        model.jitOn = true
        driver.debuggerAttached = false
        model.refreshJit()
        #expect(!model.jitInUseForNextStart)
        #expect(await eventually { switches(in: controller.view).first?.isEnabled == true })
    }

    /// The accessibility labels under `view`, in the order the nodes come.
    private func labels(in view: UIView) -> [String] {
        view.setNeedsLayout()
        view.layoutIfNeeded()
        return nodes(in: view).compactMap(\.accessibilityLabel)
    }

    @Test func performanceIsOneHeadingOverThreeGroupsEachWithItsNote() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, driver) = fixture(carried: true, highRefresh: true)
        driver.debuggerAttached = true
        model.jitOn = true
        model.jitDebuggerAttached = true
        let controller = try await hosted(model, switches: 3, in: lease)
        let ready = JitStatus.ready
        #expect(await eventually { labels(in: controller.view).contains(ready) })
        let all = labels(in: controller.view)
        #expect(all.filter { $0 == "Performance" }.count == 1, "one heading: \(all)")
        // Voice Control matches the names as they are on the screen.
        let rows = nodes(in: controller.view)
        let refreshRow = try #require(rows.first { ($0.accessibilityLabel ?? "").hasSuffix(", 120 Hz") })
        let aotRow = try #require(rows.first { ($0.accessibilityLabel ?? "").hasPrefix("AOT, ") })
        #expect(refreshRow.accessibilityUserInputLabels == ["120 Hz"], "\(String(describing: refreshRow.accessibilityUserInputLabels))")
        #expect(aotRow.accessibilityUserInputLabels == ["AOT"], "\(String(describing: aotRow.accessibilityUserInputLabels))")
        // With the 120 Hz group there, its note is drawn (a loose line while VoiceOver is off).
        #expect(all.contains(HighRefreshWords.note), "\(all)")
        let header = try #require(all.firstIndex(of: "Performance"), "\(all)")
        let refresh = try #require(all.firstIndex { $0.hasSuffix(", 120 Hz") }, "\(all)")
        let aot = try #require(all.firstIndex { $0.hasPrefix("AOT, ") }, "\(all)")
        let jit = try #require(all.firstIndex(of: "JIT"), "\(all)")
        let note = try #require(all.firstIndex(of: ready), "\(all)")
        #expect(header < refresh && refresh < aot && aot < jit && jit < note, "heading, 120 Hz, AOT, JIT, its note: \(all)")
        // The three switches are in three panels: more than a row apart.
        let frames = switches(in: controller.view).map { $0.convert($0.bounds, to: controller.view) }
        #expect(frames[1].minY - frames[0].maxY > 40 && frames[2].minY - frames[1].maxY > 40, "\(frames)")
    }

    @Test func onASixtyHertzScreenThereIsNoHundredAndTwentyHertzGroupOrNote() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true)
        let controller = try await hosted(model, switches: 2, in: lease)
        #expect(await eventually { labels(in: controller.view).contains { $0.hasPrefix("AOT, ") } })
        let all = labels(in: controller.view)
        #expect(!all.contains(HighRefreshWords.note), "the 120 Hz note is there on a 60 Hz screen: \(all)")
        #expect(!all.contains { $0.contains("120 Hz") }, "the 120 Hz group is there on a 60 Hz screen: \(all)")
    }

    @Test func theReadyNoteIsDrawnOnlyWhileJitIsOn() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, driver) = fixture(carried: true)
        driver.debuggerAttached = true
        model.jitDebuggerAttached = true
        model.jitOn = false
        let controller = try await hosted(model, switches: 2, in: lease)
        #expect(await eventually { labels(in: controller.view).contains("JIT") })
        try? await Task.sleep(for: .milliseconds(400))
        #expect(!labels(in: controller.view).contains(JitStatus.ready), "the ready line is under a JIT switch that is off")
        model.jitOn = true
        #expect(await eventually { labels(in: controller.view).contains(JitStatus.ready) })
        model.jitOn = false
        #expect(await eventually { !labels(in: controller.view).contains(JitStatus.ready) })
    }

    @Test func aStartThatFellBackLeavesTheRowWorkingWhileTheNoteUnderJitSaysSo() {
        let (model, driver) = fixture(carried: true)
        driver.debuggerAttached = true
        model.jitOn = true
        model.jitDebuggerAttached = true
        #expect(model.jitStatus.footer == JitStatus.ready)
        #expect(model.jitInUseForNextStart)
        // A debugger that came late, and a start that ran without the JIT: the
        // note says so, and so the row no longer says the JIT is in use.
        model.jitLastStart = .failed
        #expect(model.jitStatus.footer == JitAnswer.failed.fallbackSentence)
        #expect(!model.jitInUseForNextStart)
        model.jitLastStart = nil
        #expect(model.jitInUseForNextStart)
    }

    @Test func theBootRequestKeepsTheReleaseOutOnlyWhileTheSwitchIsOffInABuildThatCarriesIt() {
        func field(_ model: AppModel?, jitInUse: Bool = false) -> UInt32 {
            LiveRuntime.aotDisabled(for: LiveRuntime.translatedLibrariesStart(of: model, jitInUse: jitInUse))
        }
        let (carried, _) = fixture(carried: true)
        #expect(field(carried) == 0)
        carried.setTranslatedLibraries(false)
        #expect(field(carried) == 1)
        // The JIT engine does not use them; the choice reaches the request as it is.
        #expect(field(carried, jitInUse: true) == 1)
        carried.setTranslatedLibraries(true)
        #expect(field(carried, jitInUse: true) == 0)
        // A build without the framework launches as it always did, and so does
        // a start with no model.
        let (bare, _) = fixture(carried: false)
        bare.setTranslatedLibraries(false)
        #expect(field(bare) == 0)
        #expect(field(nil) == 0)
    }

    @Test func aBuildWithoutAJitNeverRunsOnIt() {
        let (model, _) = fixture(carried: true)
        // Not available: the person's wish for the JIT means nothing.
        model.jitAvailable = false
        model.jitOn = true
        model.jitDebuggerAttached = true
        #expect(!model.jitInUseForNextStart)
    }

    @Test func theLineUnderTheTitleIsTheNoteOrTheReasonItIsNotUsed() {
        #expect(TranslatedLibrariesWords.title == "AOT")
        #expect(
            TranslatedLibrariesWords.note == "Translated system libraries run faster. Turn off only to troubleshoot.")
        #expect(TranslatedLibrariesWords.notUsedWithJit == "Not used while JIT is on.")
    }

    @Test func aStartStatesTheReleaseOnlyWhileTheSwitchIsOnInABuildThatCarriesIt() {
        let (carried, _) = fixture(carried: true)
        #expect(carried.translatedLibrariesStart(jitInUse: false).statesRelease)
        #expect(carried.translatedLibrariesStart(jitInUse: false).word == .on)
        carried.setTranslatedLibraries(false)
        #expect(!carried.translatedLibrariesStart(jitInUse: false).statesRelease)
        #expect(carried.translatedLibrariesStart(jitInUse: false).word == .off)
        #expect(carried.translatedLibrariesStart(jitInUse: true).word == .notUsedWithJit)

        // A build without the framework launches as it always did.
        let (bare, _) = fixture(carried: false)
        bare.setTranslatedLibraries(false)
        #expect(bare.translatedLibrariesStart(jitInUse: false).statesRelease)
        #expect(bare.translatedLibrariesStart(jitInUse: false).word == .unavailable)
    }
}
