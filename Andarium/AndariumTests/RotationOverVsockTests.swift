// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
import UIKit
@testable import Andarium

/// Opt-in (`ANDARIUM_ROTATION_TESTS=1`, selecting only this suite): boots
/// Android and verifies that the host override preserves and restores locked
/// and unset guest rotation preferences. With `ANDARIUM_ROTATION_VISUAL=1` a
/// hands-on iPad check has a person turn the interface (Control Center's lock
/// off) and verifies that the guest is held at rotation zero while its
/// display follows the scene, full screen or in a window; pure policy tests
/// do not establish the visible result.
@MainActor
struct RotationOverVsockTests {
    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_ROTATION_VISUAL"] == "1"), .timeLimit(.minutes(20)))
    func theGuestsDisplayFollowsTheInterfaceAsItTurns() async throws {
        try await withAndroidGuest { model, operations in
            try await checkInterfaceTurns(model: model, operations: operations)
        }
    }

    func checkInterfaceTurns(model: AppModel, operations: AdbGuestOperations) async throws {
        try #require(UIDevice.current.userInterfaceIdiom != .phone, "An iPhone's interface does not turn")
        try await awaitScenePolicy(model, operations)
        // The session is host-driven from its start, full screen or not: the
        // guest reports no auto-rotation, rotation zero and the fixed rotation.
        let start = try await settings(operations)
        try #require(start == ["0", "0", "enabled"], "\(start)")
        let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        print("ROTATION: ready for hands-on check; turn the iPad to landscape and back (up to four minutes)")
        var shape = scene.effectiveGeometry.interfaceOrientation.isLandscape
        var turns = 0
        var turnedAt = ContinuousClock.now
        // The shapes of the interface that the guest's display has had.
        var followed = Set<Bool>()
        var previous = ""
        let deadline = ContinuousClock.now + .seconds(240)
        while ContinuousClock.now < deadline {
            let geometry = scene.effectiveGeometry
            let landscape = geometry.interfaceOrientation.isLandscape
            if landscape != shape {
                shape = landscape
                turns += 1
                turnedAt = ContinuousClock.now
            }
            let size = try await displaySize(operations)
            let summary = "interface=\(geometry.interfaceOrientation.rawValue) locked=\(geometry.isInterfaceOrientationLocked) host=\(model.hostDrivesGuestRotation) display=\(size.width)x\(size.height)"
            if summary != previous {
                print("ROTATION scene: \(summary)")
                previous = summary
            }
            try #require(!geometry.isInterfaceOrientationLocked, "The application asked for an interface lock")
            try #require(model.hostDrivesGuestRotation, "The host stopped driving the guest's rotation")
            let current = try await settings(operations)
            try #require(current == ["0", "0", "enabled"], "The guest left rotation zero: \(current)")
            if (size.width > size.height) == landscape {
                followed.insert(landscape)
            } else {
                try #require(
                    ContinuousClock.now - turnedAt < .seconds(60),
                    "The guest's display did not follow the interface within a minute: \(summary)")
            }
            if followed.count == 2 && turns >= 2 { break }
            try await Task.sleep(for: .seconds(1))
        }
        try #require(
            followed.count == 2 && turns >= 2,
            "Expected the interface to turn to landscape and back, with the guest's display following each turn")
        print("ROTATION: the interface turned \(turns) times and the guest's display followed")
    }

    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_ROTATION_TESTS"] == "1"), .timeLimit(.minutes(20)))
    func hostRotationPreservesTheGuestPreferences() async throws {
        try await withAndroidGuest { model, operations in
            try await checkPreferences(model: model, operations: operations)
        }
    }

    func checkPreferences(model: AppModel, operations: AdbGuestOperations) async throws {
        // On an iPad the host already drives the guest's rotation, with its own
        // active override. Start the command test from the saved guest choices.
        print("ROTATION: initial scene host ownership=\(model.hostDrivesGuestRotation)")
        try await awaitScenePolicy(model, operations)
        try await run(GuestRotationPolicy.guest.command, operations)
        let original = try await settings(operations)
        do {
            try await run("wm user-rotation lock 3; wm fixed-to-user-rotation disabled", operations)
            let locked = try await settings(operations)
            try #require(locked == ["0", "3", "disabled"])
            try await run(GuestRotationPolicy.host.command, operations)
            #expect(try await settings(operations) == ["0", "0", "enabled"])
            // Re-entering, including after an app restart, must retain the
            // first snapshot rather than save the override as a preference.
            try await run(GuestRotationPolicy.host.command, operations)
            try await run(GuestRotationPolicy.guest.command, operations)
            let restored = try await settings(operations)
            print("ROTATION: locked=\(locked), restored=\(restored)")
            try #require(restored == locked)

            try await run("settings delete system accelerometer_rotation; settings delete system user_rotation", operations)
            try await run(GuestRotationPolicy.host.command, operations)
            try await run(GuestRotationPolicy.guest.command, operations)
            let unset = try await settings(operations)
            print("ROTATION: restored unset preferences=\(unset)")
            try #require(unset == ["null", "null", "disabled"])
            let marker = try await operations.shell("test ! -e /data/local/tmp/andarium-rotation-state")
            #expect(marker.exitCode == 0)
            try await restore(original, operations)
            if model.hostDrivesGuestRotation { try await run(GuestRotationPolicy.host.command, operations) }
            print("ROTATION: host ownership, repeated entry, full-screen restoration and unset preferences passed")
        } catch {
            try? await run(GuestRotationPolicy.guest.command, operations)
            try? await restore(original, operations)
            throw error
        }
    }

    private func settings(_ operations: AdbGuestOperations) async throws -> [String] {
        let result = try await operations.shell(
            "settings get system accelerometer_rotation; settings get system user_rotation; wm fixed-to-user-rotation")
        try #require(result.exitCode == 0)
        let values = result.text.split(whereSeparator: \.isNewline).map(String.init)
        try #require(values.count == 3)
        try #require(["null", "0", "1"].contains(values[0]))
        try #require(["null", "0", "1", "2", "3"].contains(values[1]))
        try #require(["default", "disabled", "enabled", "enabled_if_no_auto_rotation"].contains(values[2]))
        return values
    }

    /// The guest's display size in pixels: `wm size` prints the physical size,
    /// and an override after it where one is set.
    private func displaySize(_ operations: AdbGuestOperations) async throws -> (width: Int, height: Int) {
        let result = try await operations.shell("wm size")
        try #require(result.exitCode == 0, "\(result.text)")
        let line = try #require(result.text.split(whereSeparator: \.isNewline).last { $0.contains("size:") })
        let numbers = line.split { !$0.isNumber }.compactMap { Int($0) }
        try #require(numbers.count == 2, "\(result.text)")
        return (numbers[0], numbers[1])
    }

    private func awaitScenePolicy(_ model: AppModel, _ operations: AdbGuestOperations) async throws {
        let deadline = ContinuousClock.now + .seconds(20)
        while ContinuousClock.now < deadline {
            if model.phase == .running(.normal) {
                if model.hostDrivesGuestRotation {
                    if try await settings(operations) == ["0", "0", "enabled"] {
                        let rotation = try await operations.shell("wm user-rotation")
                        if rotation.text.trimmingCharacters(in: .whitespacesAndNewlines) == "lock 0" { return }
                    }
                } else {
                    let marker = try await operations.shell("test ! -e /data/local/tmp/andarium-rotation-state")
                    if marker.exitCode == 0 { return }
                }
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        throw AdbFailure(message: "The scene's initial rotation policy did not settle")
    }

    private func restore(_ values: [String], _ operations: AdbGuestOperations) async throws {
        let automatic = values[0] == "null" ? "settings delete system accelerometer_rotation"
            : "settings put system accelerometer_rotation \(values[0])"
        let angle = values[1] == "null" ? "settings delete system user_rotation"
            : "settings put system user_rotation \(values[1])"
        try await run("\(angle); \(automatic); wm fixed-to-user-rotation \(values[2])", operations)
    }

    private func run(_ command: String, _ operations: AdbGuestOperations) async throws {
        let result = try await operations.shell(command, limit: .seconds(15))
        try #require(result.exitCode == 0, "\(result.text)")
    }
}
