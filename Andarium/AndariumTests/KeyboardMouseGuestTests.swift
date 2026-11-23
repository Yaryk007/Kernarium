// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreGraphics
import Foundation
import GuestHostCore
import Testing
import UIKit
@testable import Andarium

/// Opt-in (`ANDARIUM_KEYBOARD_MOUSE_TESTS=1`, in the test runner's environment
/// as `TEST_RUNNER_ANDARIUM_KEYBOARD_MOUSE_TESTS`, selecting only this suite): a
/// real Android behind the production scene root, with the hardware keyboard
/// and mouse driven through a session of the test's own and read back from
/// Android through adb.
///
/// They need a kernel that follows configuration changes (the hot-plug topic)
/// and the Android release that carries its overlay, as the accepted ones do: on
/// a kernel without the topic the devices are never registered, and every check
/// below that waits for Android to list a device fails. They are skipped unless
/// the switch is set.
///
/// The session under test is the test's, built over the runtime's real entry
/// points with iOS's hardware replaced by `Presence`, so that the plug and the
/// unplug do not depend on a keyboard being attached to the Simulator. The
/// application's own session never plugs anything in these runs: nothing uses
/// hardware it can see.
@MainActor
struct KeyboardMouseGuestTests {
    private static var enabled: Bool {
        ProcessInfo.processInfo.environment["ANDARIUM_KEYBOARD_MOUSE_TESTS"] == "1"
    }

    /// iOS's end of the hardware, driven by the test.
    @MainActor
    private final class Presence: HardwarePresence {
        var keyboardConnected = false
        var mouseConnected = false
        var changed: (@MainActor () -> Void)?
        func start() {}
        func stop() {}

        func report(keyboard: Bool? = nil, mouse: Bool? = nil) {
            if let keyboard { keyboardConnected = keyboard }
            if let mouse { mouseConnected = mouse }
            changed?()
        }
    }

    // MARK: Android's input devices

    private struct Record {
        let type: UInt16
        let code: UInt16
        let value: Int32

        static let key: UInt16 = 0x01
        static let relative: UInt16 = 0x02
        static let absolute: UInt16 = 0x03
        static let relativeWheel: UInt16 = 0x08
        static let leftButton: UInt16 = 0x110
        static let multitouchPositionX: UInt16 = 0x35
        static let multitouchPositionY: UInt16 = 0x36
        static let keyA: UInt16 = 30
    }

    private static func decode(_ data: Data) -> [Record] {
        // 64-bit struct timeval, then type, code and value.
        let size = 24
        return stride(from: 0, to: data.count - size + 1, by: size).map { offset in
            let record = data.subdata(in: offset..<offset + size)
            return Record(
                type: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 16, as: UInt16.self) },
                code: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 18, as: UInt16.self) },
                value: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 20, as: Int32.self) })
        }
    }

    private static let sideKeys = "Kernarium Keyboard"
    private static let keyboard = "Kernarium Hardware Keyboard"
    private static let mouse = "Kernarium Mouse"
    private static let pointer = "Kernarium Pointer"

    private func dumpInput(_ operations: AdbGuestOperations) async throws -> String {
        try await operations.shell("dumpsys input", limit: .seconds(30)).text
    }

    /// The part of `dumpsys input` that describes the device named `name`: from
    /// its "Device N: name" line to the next device's, or nil when Android
    /// lists none.
    private func block(named name: String, in dump: String) -> String? {
        func startsDevice(_ line: String) -> Bool {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard trimmed.hasPrefix("Device ") else { return false }
            let rest = trimmed.dropFirst("Device ".count)
            return rest.first.map { $0.isNumber || $0 == "-" } ?? false
        }
        let lines = dump.components(separatedBy: "\n")
        guard let start = lines.firstIndex(where: { startsDevice($0) && $0.hasSuffix(": \(name)") }) else {
            return nil
        }
        let rest = lines[(start + 1)...]
        let end = rest.firstIndex(where: startsDevice) ?? lines.endIndex
        return lines[start..<end].joined(separator: "\n")
    }

    private func isListed(_ name: String, _ operations: AdbGuestOperations) async throws -> Bool {
        block(named: name, in: try await dumpInput(operations)) != nil
    }

    /// Whether Android lists every one of the devices.
    private func allListed(_ names: [String], _ operations: AdbGuestOperations) async throws -> Bool {
        let dump = try await dumpInput(operations)
        return names.allSatisfy { block(named: $0, in: dump) != nil }
    }

    /// Whether Android lists none of the devices.
    private func noneListed(_ names: [String], _ operations: AdbGuestOperations) async throws -> Bool {
        let dump = try await dumpInput(operations)
        return names.allSatisfy { block(named: $0, in: dump) == nil }
    }

    /// Waits, bounded, for a condition that takes a call to the guest to read.
    private func eventually(
        seconds: Int = 30, _ condition: () async throws -> Bool
    ) async rethrows -> Bool {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while ContinuousClock.now < deadline {
            if try await condition() { return true }
            try? await Task.sleep(for: .milliseconds(500))
        }
        return try await condition()
    }

    /// The event node of the device named `name`.
    private func node(named name: String, _ operations: AdbGuestOperations) async throws -> String {
        let text = try await operations.shell("getevent -p -l", limit: .seconds(15)).text
        let device = try #require(
            text.components(separatedBy: "add device ").dropFirst().first { $0.contains("\"\(name)\"") },
            "getevent lists no \(name): \(text)")
        let line = try #require(device.split(separator: "\n").first)
        let path = try #require(line.split(separator: ":").last).trimmingCharacters(in: .whitespaces)
        try #require(path.hasPrefix("/dev/input/event"), "\(line)")
        return path
    }

    /// Reads a node into a file in the guest, bounded to two minutes, for the
    /// length of `body`, and returns what it delivered.
    private func reading(
        _ operations: AdbGuestOperations, node: String, during body: () async throws -> Void
    ) async throws -> [Record] {
        let file = "/data/local/tmp/andarium-keyboard-mouse.evdev"
        let started = try await operations.shell(
            "rm -f \(file); nohup timeout 120 cat \(node) > \(file) 2>/dev/null </dev/null & echo $!",
            limit: .seconds(15)).text.trimmingCharacters(in: .whitespacesAndNewlines)
        let reader = try #require(Int(started), "no reader process: \(started)")
        try await Task.sleep(for: .seconds(1))
        try await body()
        try await Task.sleep(for: .seconds(1))
        let data = try await operations.shell(
            "kill \(reader); cat \(file); rm -f \(file)", limit: .seconds(15)).stdout
        return Self.decode(data)
    }

    /// The lit state of the keyboard's Caps Lock light as the guest's kernel
    /// has it (the input device's LED class device).
    private func capsLockLight(_ operations: AdbGuestOperations) async throws -> Bool? {
        let text = try await operations.shell(
            """
            for d in /sys/class/input/input*; do
              if grep -q '\(Self.keyboard)' "$d/name" 2>/dev/null; then
                cat "/sys/class/leds/$(basename "$d")::capslock/brightness" 2>/dev/null
              fi
            done
            """,
            limit: .seconds(15)).text.trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? nil : text != "0"
    }

    /// Android's cursor on the display, in pixels, from the input dump.
    private func cursor(_ operations: AdbGuestOperations) async throws -> CGPoint? {
        let dump = try await dumpInput(operations)
        let pattern = #/(?i)(?:cursor|pointer|position)[^\n()]*\(\s*(-?[0-9.]+)\s*,\s*(-?[0-9.]+)\s*\)/#
        guard let match = dump.firstMatch(of: pattern), let x = Double(match.1), let y = Double(match.2) else {
            return nil
        }
        return CGPoint(x: x, y: y)
    }

    // MARK: The session under test

    /// A session over the real runtime that is in front and running.
    private func makeSession(_ presence: Presence) -> KeyboardMouse {
        let session = KeyboardMouse(
            presence: presence, rawMouse: nil, supportsMouse: true, wantsLock: { _ in })
        session.update(running: true, foreground: true)
        return session
    }

    private func position(_ x: Double, _ y: Double) -> GuestPointerMapping.Position {
        GuestPointerMapping.Position(x: UInt32(x * 32767), y: UInt32(y * 32767), inside: true)
    }

    // MARK: Tests

    @Test(.timeLimit(.minutes(25)))
    func theKeyboardIsAnExternalAlphabeticDeviceOfAndroidOnlyWhileItIsPlugged() async throws {
        guard Self.enabled else { return }
        try await withAndroidGuest { _, operations in
            // Before any hardware: the side keys are Android's only keyboard,
            // and they are not alphabetic.
            let before = try await dumpInput(operations)
            #expect(block(named: Self.keyboard, in: before) == nil, "the keyboard is listed before it is plugged")
            let sideKeys = try #require(block(named: Self.sideKeys, in: before), "dumpsys input: \(before)")
            #expect(!sideKeys.contains("KeyboardType: 2"), "the side keys are alphabetic: \(sideKeys)")

            let presence = Presence()
            let session = makeSession(presence)
            defer { session.shutDown() }
            presence.report(keyboard: true)
            #expect(try await eventually { try await isListed(Self.keyboard, operations) }, "the keyboard did not appear")
            let plugged = try #require(block(named: Self.keyboard, in: try await dumpInput(operations)))
            #expect(plugged.contains("IsExternal: true"), "the keyboard is not external: \(plugged)")
            #expect(plugged.contains("KeyboardType: 2"), "the keyboard is not alphabetic: \(plugged)")

            // Android hides its on-screen keyboard while a hardware keyboard
            // is attached: the setting that lets it show is off.
            let shows = try await operations.shell("settings get secure show_ime_with_hard_keyboard").text
            #expect(shows.trimmingCharacters(in: .whitespacesAndNewlines) == "0", "setting: \(shows)")
            let inputMethod = try await operations.shell("dumpsys input_method", limit: .seconds(30)).text
            #expect(!inputMethod.contains("mInputShown=true"), "the on-screen keyboard shows")

            // The keys are positions, and Android's own layout applies.
            let keys = try await node(named: Self.keyboard, operations)
            let records = try await reading(operations, node: keys) {
                session.input.keyDown(usage: 0x04, alphaShift: false)
                try await Task.sleep(for: .milliseconds(300))
                session.input.keyUp(usage: 0x04)
            }
            let presses = records.filter { $0.type == Record.key && $0.code == Record.keyA }.map(\.value)
            #expect(presses == [1, 0], "KEY_A events: \(presses)")

            // Caps Lock: the key toggles Android's light, and the light
            // reaches the host, which taps the key again when iOS's Caps
            // Lock is not the guest's.
            session.input.keyDown(usage: 0x39, alphaShift: true)
            session.input.keyUp(usage: 0x39)
            #expect(try await eventually { try await capsLockLight(operations) == true }, "the light did not come on")
            try await Task.sleep(for: .seconds(2))
            session.input.keyDown(usage: 0x04, alphaShift: false)
            session.input.keyUp(usage: 0x04)
            #expect(
                try await eventually { try await capsLockLight(operations) == false },
                "the host did not bring Android's Caps Lock back to iOS's")

            // The unplug removes the device once iOS reports no keyboard and
            // the delay has passed.
            presence.report(keyboard: false)
            #expect(
                try await eventually(seconds: 30) { try await noneListed([Self.keyboard], operations) },
                "the keyboard stayed after the unplug")
            #expect(try await isListed(Self.sideKeys, operations), "the side keys went with the keyboard")
        }
    }

    @Test(.timeLimit(.minutes(25)))
    func theMouseAndThePointerArePluggedTogetherAndPlaceAndroidsCursor() async throws {
        guard Self.enabled else { return }
        try await withAndroidGuest { _, operations in
            let before = try await dumpInput(operations)
            #expect(block(named: Self.mouse, in: before) == nil && block(named: Self.pointer, in: before) == nil)

            let presence = Presence()
            let session = makeSession(presence)
            defer { session.shutDown() }
            // The mouse is plugged by its use: iOS reports one and the pointer
            // moves over the display.
            presence.report(mouse: true)
            session.input.hover(position(0.25, 0.75), at: CGPoint(x: 1, y: 1), beginning: true)
            session.input.hover(position(0.25, 0.75), at: CGPoint(x: 2, y: 1), beginning: false)
            #expect(
                try await eventually { try await allListed([Self.mouse, Self.pointer], operations) },
                "the mouse and the pointer did not appear together")

            // The guest registers them, and only then is the pointer the
            // guest's: the host's pointer, clicks and wheel reach it.
            let pointerNode = try await node(named: Self.pointer, operations)
            let mouseNode = try await node(named: Self.mouse, operations)
            #expect(await eventually(seconds: 30) { session.input.hidesSystemPointer }, "the guest never registered the pointer")

            let target = (x: 0.25, y: 0.75)
            let pointerEvents = try await reading(operations, node: pointerNode) {
                session.input.hover(position(target.x, target.y), at: CGPoint(x: 3, y: 1), beginning: false)
                try await Task.sleep(for: .milliseconds(300))
                session.input.pointerTouch(
                    .began, at: position(target.x, target.y), buttons: HardwareButtons.primary)
                try await Task.sleep(for: .milliseconds(300))
                session.input.pointerTouch(.ended, at: position(target.x, target.y), buttons: 0)
            }
            let xs = pointerEvents.filter { $0.type == Record.absolute && $0.code == Record.multitouchPositionX }
            let ys = pointerEvents.filter { $0.type == Record.absolute && $0.code == Record.multitouchPositionY }
            #expect(xs.contains { abs(Double($0.value) - target.x * 32767) < 40 }, "X events: \(xs.map(\.value))")
            #expect(ys.contains { abs(Double($0.value) - target.y * 32767) < 40 }, "Y events: \(ys.map(\.value))")
            let buttons = pointerEvents.filter { $0.type == Record.key && $0.code == Record.leftButton }.map(\.value)
            #expect(buttons == [1, 0], "the left button: \(buttons)")

            // Android's cursor is where the pointer is, on the display.
            let size = try await operations.shell("wm size").text
            let cursorAt = try #require(try await cursor(operations), "the input dump shows no cursor")
            let sizes = size.matches(of: #/([0-9]+)x([0-9]+)/#)
            let pixels = try #require(sizes.last, "wm size: \(size)")
            let width = try #require(Double(pixels.1)), height = try #require(Double(pixels.2))
            #expect(abs(cursorAt.x - target.x * width) < 0.05 * width, "cursor \(cursorAt) on \(size)")
            #expect(abs(cursorAt.y - target.y * height) < 0.05 * height, "cursor \(cursorAt) on \(size)")

            // The wheel is the mouse's.
            let wheel = try await reading(operations, node: mouseNode) {
                session.input.scroll(translation: CGPoint(x: 0, y: 64), continuous: true, beginning: true)
            }
            let steps = wheel.filter { $0.type == Record.relative && $0.code == Record.relativeWheel }.map(\.value)
            #expect(steps == [1], "wheel steps: \(steps)")

            // The unplug takes both devices away together.
            presence.report(mouse: false)
            #expect(
                try await eventually(seconds: 30) { try await noneListed([Self.mouse, Self.pointer], operations) },
                "the mouse or the pointer stayed after the unplug")
        }
    }

    @Test(.timeLimit(.minutes(25)))
    func leavingTheFrontReleasesTheKeysAndTheButtonsThatAreDown() async throws {
        guard Self.enabled else { return }
        try await withAndroidGuest { _, operations in
            let presence = Presence()
            let session = makeSession(presence)
            defer { session.shutDown() }
            presence.report(keyboard: true, mouse: true)
            session.input.hover(position(0.5, 0.5), at: CGPoint(x: 1, y: 1), beginning: true)
            session.input.hover(position(0.5, 0.5), at: CGPoint(x: 2, y: 1), beginning: false)
            #expect(
                try await eventually { try await allListed([Self.keyboard, Self.mouse, Self.pointer], operations) },
                "the devices did not appear")
            #expect(await eventually(seconds: 30) { session.input.hidesSystemPointer }, "the guest never registered the pointer")

            let keys = try await node(named: Self.keyboard, operations)
            let pointerNode = try await node(named: Self.pointer, operations)
            let keyEvents = try await reading(operations, node: keys) {
                let pointerEvents = try await reading(operations, node: pointerNode) {
                    session.input.keyDown(usage: 0x04, alphaShift: false)
                    session.input.pointerTouch(.began, at: position(0.5, 0.5), buttons: HardwareButtons.primary)
                    try await Task.sleep(for: .milliseconds(500))
                    // The guest goes behind another app: nothing it held stays
                    // down in Android.
                    session.update(running: true, foreground: false)
                    try await Task.sleep(for: .milliseconds(500))
                }
                let buttons = pointerEvents.filter { $0.type == Record.key && $0.code == Record.leftButton }.map(\.value)
                #expect(buttons == [1, 0], "the left button: \(buttons)")
            }
            let presses = keyEvents.filter { $0.type == Record.key && $0.code == Record.keyA }.map(\.value)
            #expect(presses == [1, 0], "KEY_A events: \(presses)")

            // Back in front, the guest hears keys again.
            session.update(running: true, foreground: true)
            let again = try await reading(operations, node: keys) {
                session.input.keyDown(usage: 0x04, alphaShift: false)
                session.input.keyUp(usage: 0x04)
            }
            #expect(again.filter { $0.type == Record.key && $0.code == Record.keyA }.map(\.value) == [1, 0])
        }
    }
}
