// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The Backup section of Home and the set-up sheet, as the application draws
/// it and VoiceOver reads it: the row's title is "Include in backups" and the
/// switch beside it carries the same words as its label, so the switch says
/// what it includes without the text next to it. SwiftUI publishes its
/// accessibility elements only while application accessibility is on (the
/// Simulator preference `ApplicationAccessibilityEnabled`), so a run without it
/// fails with that said.
@MainActor
@Suite(.serialized)
struct BackupRowTests {
    private static let title = "Include in backups"
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private func nodes(in node: NSObject, depth: Int = 0) -> [NSObject] {
        guard depth < 64 else { return [] }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        return [node] + children.flatMap { nodes(in: $0, depth: depth + 1) }
    }

    /// The Backup section with its note, on the application's window.
    private func hostSection(isOn: Bool, in lease: HostWindow.Lease) -> UIHostingController<some View> {
        let view = HomeSection(title: "Backup") {
            BackupOption(isOn: .constant(isOn))
        } footer: {
            BackupNote(host: host)
        }
        .frame(width: 390)
        let controller = UIHostingController(rootView: view)
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    @Test func theSwitchSaysWhatItIncludesAndWhetherItIsOn() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        for isOn in [true, false] {
            let controller = hostSection(isOn: isOn, in: lease)
            var labelled: [NSObject] = []
            let seen = await eventually {
                controller.view.setNeedsLayout()
                controller.view.layoutIfNeeded()
                labelled = nodes(in: controller.view).filter { $0.accessibilityLabel == Self.title }
                return labelled.contains { $0.accessibilityValue != nil }
            }
            let all = nodes(in: controller.view).compactMap(\.accessibilityLabel)
            try #require(seen, "no switch is labelled \(Self.title); is ApplicationAccessibilityEnabled on? \(all)")
            // The switch: the title as its label, and on or off as its value.
            let switches = labelled.filter { $0.accessibilityValue != nil }
            #expect(switches.count == 1, "\(isOn)")
            #expect(switches.first?.accessibilityValue == (isOn ? "1" : "0"), "\(isOn)")
            #expect(switches.first?.accessibilityTraits.contains(.button) == true, "\(isOn)")
            // The row's own text beside it is no second stop: the switch says it. The
            // section's note is under it.
            #expect(
                !labelled.contains { $0.accessibilityValue == nil && $0.accessibilityTraits.contains(.staticText) },
                "\(isOn)")
            #expect(all.contains("Backup"))
            #expect(all.contains { $0.hasPrefix("Puts the virtual machine’s apps and files in iCloud") })
        }
    }
}
