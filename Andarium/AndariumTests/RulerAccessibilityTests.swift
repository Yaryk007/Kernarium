// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// VoiceOver's increment and decrement on the memory ruler: one stop where
/// the ruler is drawn whole, and on a range of about a thousand stops the
/// next labelled gibibyte, laid out at a real width inside the application.
/// SwiftUI publishes its accessibility elements only while application
/// accessibility is on (the Simulator preference
/// `ApplicationAccessibilityEnabled`, as `Kernarium/scripts/
/// run-ios-simulator-tests.sh` sets it), so a run without it ends the test
/// early.
@MainActor
struct RulerAccessibilityTests {
    private final class Position {
        var index = 0
    }

    private struct Harness: View {
        let position: Position
        let stops: [Int]

        var body: some View {
            Ruler(
                index: Binding(get: { position.index }, set: { position.index = $0 }),
                count: stops.count,
                majorEvery: 1024 / Limits.memoryStepMiB,
                majorOffset: (1024 - stops[0] % 1024) % 1024 / Limits.memoryStepMiB,
                minorEvery: 256 / Limits.memoryStepMiB,
                majorNumber: { stops[$0] / 1024 },
                label: { Formats.gibibytes(mib: stops[$0]) },
                valueText: { Formats.size(mib: stops[$0]) },
                accessibilityName: "Memory"
            )
            // A phone's: its stops are 0.34 points apart, so labels every 5 GiB have room.
            .frame(width: 360)
        }
    }

    private func element(labelled label: String, in node: NSObject, depth: Int = 0) -> NSObject? {
        if node.accessibilityLabel == label, node.accessibilityTraits.contains(.adjustable) { return node }
        guard depth < 64 else { return nil }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children {
            if let found = element(labelled: label, in: child, depth: depth + 1) { return found }
        }
        return nil
    }

    private func steps(for stops: [Int], forward: Bool, from start: Int, count: Int) async throws -> [Int]? {
        let position = Position()
        position.index = start
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIHostingController(rootView: Harness(position: position, stops: stops))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        var ruler: NSObject?
        let deadline = ContinuousClock.now + .seconds(5)
        while ruler == nil, ContinuousClock.now < deadline {
            ruler = element(labelled: "Memory", in: controller.view)
            if ruler == nil { try await Task.sleep(for: .milliseconds(50)) }
        }
        guard let ruler else { return nil }
        // The ruler learns its width from its layout, so the first steps wait for it.
        try await Task.sleep(for: .milliseconds(200))
        var visited: [Int] = []
        for _ in 0..<count {
            if forward { ruler.accessibilityIncrement() } else { ruler.accessibilityDecrement() }
            visited.append(stops[position.index])
        }
        return visited
    }

    @Test func aRulerOfAThousandStopsStepsByTheLabelledGibibyte() async throws {
        let stops = Array(stride(from: 1536, through: 65536, by: Limits.memoryStepMiB))
        guard let up = try await steps(for: stops, forward: true, from: 0, count: 3) else { return }
        #expect(up == [5 * 1024, 10 * 1024, 15 * 1024])
        guard let down = try await steps(for: stops, forward: false, from: stops.count - 1, count: 2) else { return }
        #expect(down == [60 * 1024, 55 * 1024])
    }

    @Test func aRulerWithRoomForEverythingStepsByOneStop() async throws {
        let stops = Array(stride(from: 1536, through: 4096, by: Limits.memoryStepMiB))
        guard let up = try await steps(for: stops, forward: true, from: 10, count: 2) else { return }
        #expect(up == [stops[11], stops[12]])
        guard let down = try await steps(for: stops, forward: false, from: 10, count: 2) else { return }
        #expect(down == [stops[9], stops[8]])
    }
}
