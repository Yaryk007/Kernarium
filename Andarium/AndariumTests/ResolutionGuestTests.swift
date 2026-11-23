// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Metal
import Testing
import UIKit
@testable import Andarium

/// The resolution on a real guest: Home states the pixels the guest will get,
/// and Android's interface keeps its size whatever the scale. Opt-in
/// (`TEST_RUNNER_ANDARIUM_RESOLUTION_TESTS=1`), on an isolated guest
/// (`TEST_RUNNER_ANDARIUM_GUEST_INSTANCE`) with the scale the launch states
/// (`TEST_RUNNER_KERNARIUM_DISPLAY_SCALE`); run it once for each scale to
/// compare and read the `RESOLUTION_DP:` lines; with
/// `TEST_RUNNER_ANDARIUM_RESOLUTION_IDLE_SECONDS` it then leaves the home screen
/// idle that long and prints the process's footprint and graphics memory
/// (`RESOLUTION_MEMORY:`), and `TEST_RUNNER_ANDARIUM_RESOLUTION_WINDOW=WxH` asks a
/// Mac for a window that size in points, to stand for a smaller device. It needs
/// the Android release and a Mac or a device (the Simulator draws on the CPU).
@MainActor
struct ResolutionGuestTests {
    /// The number after `label` in `text`: "Physical size: 1206x2334" or
    /// "Physical density: 480".
    private func numbers(after label: String, in text: String) -> [Double] {
        guard let range = text.range(of: label) else { return [] }
        let rest = text[range.upperBound...].prefix { $0 != "\n" }
        return rest.split { !($0.isNumber || $0 == ".") }.compactMap { Double($0) }
    }

    private func layoutText(_ layout: GuestDisplayLayout) -> String {
        "pixels=\(layout.width)x\(layout.height) dpi=\(layout.dotsPerInch)"
    }

    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_RESOLUTION_TESTS"] == "1"), .timeLimit(.minutes(25)))
    func theGuestGetsWhatHomeStatedAndItsInterfaceKeepsItsSize() async throws {
        let environment = ProcessInfo.processInfo.environment
        try #require(environment["ANDARIUM_GUEST_INSTANCE"] != nil)
        let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let model = try #require((scene.delegate as? AndariumSceneDelegate)?.model)
        let wanted: CGSize?
        if let text = environment["ANDARIUM_RESOLUTION_WINDOW"] {
            let parts = text.lowercased().split(separator: "x").compactMap { Double($0) }
            try #require(parts.count == 2, "ANDARIUM_RESOLUTION_WINDOW is WxH")
            let size = CGSize(width: parts[0], height: parts[1])
            scene.sizeRestrictions?.minimumSize = size
            scene.sizeRestrictions?.maximumSize = size
            wanted = size
        } else {
            wanted = nil
        }
        // What Home states before the start, once the window has been read
        // (and has the size asked for).
        let deadline = ContinuousClock.now + .seconds(60)
        while let wanted, abs((model.displayPoints?.width ?? 0) - wanted.width) > 1, ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        while model.resolutionLayout == nil, ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        let promised = try #require(model.resolutionLayout, "Home stated no resolution")
        let points = try #require(model.displayPoints)
        try await withAndroidGuest { model, adb in
            let size = try await adb.shell("wm size")
            let density = try await adb.shell("wm density")
            let property = try await adb.shell("getprop ro.sf.lcd_density")
            let display = try await adb.shell("dumpsys display | grep -m2 -E 'mBaseDisplayInfo|real [0-9]+ x [0-9]+'")
            let pixels = numbers(after: "Physical size:", in: size.text)
            let dpi = numbers(after: "Physical density:", in: density.text)
            print("RESOLUTION_DP: scale=\(model.resolution.text) native=\(model.screenNativeScale) points=\(points.width)x\(points.height) promised=\(promised.width)x\(promised.height)@\(promised.dotsPerInch) wm_size=\(pixels) wm_density=\(dpi) lcd_density=\(property.text.trimmingCharacters(in: .whitespacesAndNewlines))")
            print("RESOLUTION_DP: display \(display.text.replacingOccurrences(of: "\n", with: " | "))")
            try #require(pixels.count == 2 && dpi.count == 1, "\(size.text) \(density.text)")
            // Home stated what the guest got, to the pixel and the dot.
            #expect(Int(pixels[0]) == promised.width && Int(pixels[1]) == promised.height)
            #expect(Int(dpi[0]) == promised.dotsPerInch)
            // The interface's size in dp is the display's in points, within one dp.
            let dpWidth = pixels[0] * 160 / dpi[0]
            let dpHeight = pixels[1] * 160 / dpi[0]
            print("RESOLUTION_DP: dp=\(dpWidth)x\(dpHeight) points=\(points.width)x\(points.height)")
            #expect(abs(dpWidth - Double(points.width)) <= 1)
            #expect(abs(dpHeight - Double(points.height)) <= 1)
            // The session's display is the size Home read before the start.
            #expect(model.displayPoints == points, "\(String(describing: model.displayPoints)) != \(points)")
            if let idle = environment["ANDARIUM_RESOLUTION_IDLE_SECONDS"].flatMap({ Int($0) }) {
                _ = try await adb.shell("input keyevent KEYCODE_HOME")
                // Samples across the idle time, the last being the settled figure.
                for second in stride(from: 0, through: idle, by: max(idle / 4, 1)) {
                    try await Task.sleep(for: .seconds(second == 0 ? 0 : max(idle / 4, 1)))
                    let values = andarium_test_memory_snapshot()
                    let readout = model.session.readout
                    print("RESOLUTION_MEMORY: scale=\(model.resolution.text) \(layoutText(promised)) t=\(second)s footprint=\(values["footprint"] ?? "?") graphics=\(values["graphics"] ?? "?") metal_allocated=\(MTLCreateSystemDefaultDevice()?.currentAllocatedSize ?? 0) peak=\(values["peak_footprint"] ?? "?") vram=\(readout.vramText) memory=\(readout.memoryUsedText) guest_mib=\(model.guest?.memoryMiB ?? 0)")
                }
            }
        }
    }
}
