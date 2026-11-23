// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Home's Performance section as the application draws it and VoiceOver reads
/// it: "120 Hz" where the screen can show it, JIT where a build can have it,
/// the section when at least one of them is, and under the panel the note of
/// JIT (which a section with no JIT row has none of). SwiftUI publishes its
/// accessibility elements only while application accessibility is on (the
/// Simulator preference `ApplicationAccessibilityEnabled`, as `Kernarium/
/// scripts/run-ios-simulator-tests.sh` sets it).
@MainActor
@Suite(.serialized)
struct HomePerformanceTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let footnote = "Smoother motion, more battery use."

    /// A model with the two rows' conditions stated: a screen that shows 120
    /// frames a second or 60, and a build that has the JIT row or not.
    private func model(highRefresh: Bool, jit: Bool, guest: Bool = true) -> AppModel {
        let model = AppModel(
            guest: guest ? Guest.fresh(on: host) : nil, host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        model.driver = StubDriver(plan: .normal)
        model.statedNativeScale = 3
        model.statedDisplayPoints = CGSize(width: 402, height: 778)
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: highRefresh ? 120 : 60)
        model.displayPointsDidChange(CGSize(width: 402, height: 778))
        model.launchApplied = true
        model.jitAvailable = jit
        return model
    }

    /// Every accessibility node under `node`.
    private func nodes(in node: NSObject, depth: Int = 0) -> [NSObject] {
        guard depth < 64 else { return [] }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        return [node] + children.flatMap { nodes(in: $0, depth: depth + 1) }
    }

    /// The labels of `section` once it has laid out: they are read after the
    /// label `expected` is there, or after the layout has had its time when
    /// nothing is expected (a section that is not drawn).
    private func labels<Content: View>(
        of section: Content, model: AppModel, expecting expected: String?, in lease: HostWindow.Lease
    ) async throws -> [String] {
        let controller = UIHostingController(rootView: section.environment(model).frame(width: 390))
        lease.window.rootViewController = controller
        var found: [String] = []
        let deadline = ContinuousClock.now + .seconds(expected == nil ? 1 : 10)
        repeat {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            found = nodes(in: controller.view).compactMap(\.accessibilityLabel)
            if let expected, found.contains(expected) { break }
            try await Task.sleep(for: .milliseconds(50))
        } while ContinuousClock.now < deadline
        return found
    }

    /// The row of 120 Hz reads as its footnote and then the control's label.
    private var refreshLabel: String { "\(footnote), 120 Hz" }

    @Test func bothRowsAreOneSectionWithTheNoteOfJitBeneathIt() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(highRefresh: true, jit: true)
        let all = try await labels(of: HomePerformanceSection(), model: model, expecting: model.jitStatus.footer, in: lease)
        let header = try #require(all.firstIndex(of: "Performance"), "\(all)")
        let refresh = try #require(all.firstIndex(of: refreshLabel), "\(all)")
        let jit = try #require(all.firstIndex(of: "JIT"), "\(all)")
        let note = try #require(all.firstIndex(of: model.jitStatus.footer), "\(all)")
        #expect(header < refresh && refresh < jit && jit < note, "the order is not Performance, 120 Hz, JIT, the note: \(all)")
        #expect(all.filter { $0 == "Performance" }.count == 1, "one section: \(all)")
    }

    @Test func onAScreenThatShowsSixtyTheSectionHoldsJitAlone() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(highRefresh: false, jit: true)
        let all = try await labels(of: HomePerformanceSection(), model: model, expecting: model.jitStatus.footer, in: lease)
        #expect(all.contains("Performance") && all.contains("JIT"), "\(all)")
        #expect(!all.contains { $0.contains("120 Hz") }, "a 60 Hz screen was offered 120 Hz: \(all)")
    }

    @Test func withNoJitTheSectionHoldsTheHundredAndTwentyHertzRowAloneAndNoNote() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(highRefresh: true, jit: false)
        let all = try await labels(of: HomePerformanceSection(), model: model, expecting: refreshLabel, in: lease)
        #expect(all.contains("Performance"), "\(all)")
        #expect(!all.contains("JIT"), "a build with no JIT has the row: \(all)")
        #expect(!all.contains(model.jitStatus.footer), "the note of JIT is there with no JIT row: \(all)")
    }

    @Test func withNeitherRowThereIsNoSection() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let all = try await labels(
            of: HomePerformanceSection(), model: model(highRefresh: false, jit: false), expecting: nil, in: lease)
        #expect(!all.contains("Performance"), "an empty section: \(all)")
        #expect(all.isEmpty, "\(all)")
    }

    @Test func withNoGuestThereIsNoSection() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let all = try await labels(
            of: HomePerformanceSection(), model: model(highRefresh: true, jit: true, guest: false), expecting: nil, in: lease)
        #expect(all.isEmpty, "\(all)")
    }

    @Test func theHundredAndTwentyHertzRowIsNotInVirtualMachineAnyMore() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(highRefresh: true, jit: true)
        let all = try await labels(of: SystemSection(), model: model, expecting: "Edit system", in: lease)
        #expect(all.contains { $0.hasPrefix("Resolution") }, "\(all)")
        #expect(!all.contains { $0.contains("120 Hz") }, "Virtual machine still has the row: \(all)")
    }

    @Test func theSwitchesKeepTheirStateAndTheirControl() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(highRefresh: true, jit: true)
        let controller = UIHostingController(rootView: HomePerformanceSection().environment(model).frame(width: 390))
        lease.window.rootViewController = controller
        func node(_ label: String) async throws -> NSObject {
            var found: NSObject?
            let deadline = ContinuousClock.now + .seconds(10)
            repeat {
                controller.view.setNeedsLayout()
                controller.view.layoutIfNeeded()
                found = nodes(in: controller.view).first { $0.accessibilityLabel == label }
                if found == nil { try await Task.sleep(for: .milliseconds(50)) }
            } while found == nil && ContinuousClock.now < deadline
            return try #require(found, "no \(label): \(nodes(in: controller.view).compactMap(\.accessibilityLabel))")
        }
        #expect(try await node(refreshLabel).accessibilityValue == "0")
        #expect(try await node("JIT").accessibilityValue == "0")
        model.setHighRefresh(true)
        model.setJit(true)
        #expect(try await node(refreshLabel).accessibilityValue == "1")
        #expect(try await node("JIT").accessibilityValue == "1")
    }
}
