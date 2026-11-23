// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// A real guest through the production scene root and runtime. Always wait
/// for shutdown before restoring the test host, so updates finish normally.
/// `prepare` runs on the model once the guest exists and before it starts.
@MainActor
func withAndroidGuest(
    cleanupCommand: String = "true",
    prepare: @MainActor (AppModel) throws -> Void = { _ in },
    _ body: @MainActor (AppModel, AdbGuestOperations) async throws -> Void) async throws {
    let entry = AppEntry.fromArguments()
    let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
    let delegate = try #require(scene.delegate as? AndariumSceneDelegate)
    let model = try #require(delegate.model)
    try #require(model.alert == nil, "\(String(describing: model.alert))")
    if model.guest == nil {
        model.create(entry.overrides.applied(to: Guest.fresh(on: model.host), on: model.host))
        try await waitForAndroidGuest(until: { model.busy == nil }, seconds: 30)
    }
    // The callers set what a run needs (the camera's source, a switch) in
    // `prepare`; a start that skipped it would run every test on the defaults.
    var prepareRuns = 0
    func runPrepare() throws {
        prepareRuns += 1
        try prepare(model)
    }
    try runPrepare()
    try #require(prepareRuns == 1, "prepare must run once, before the guest starts")
    model.start(.normal)
    try #require(model.alert == nil, "\(String(describing: model.alert))")

    let identity = try entry.adbKeyStore.loadOrCreate()
    // The run's mark is renewed when the runtime launches, which is after this
    // line; the operations take it once Android is up, so that an import made
    // through them is one of the run, which its own sweep spares.
    var operations = AdbGuestOperations(
        identity: identity, clock: GuestRunClock(), device: model.host.name, run: GuestRunMark.random())
    var restoreDisplayTimeout = "true"
    do {
        var booted = false
        let deadline = ContinuousClock.now + .seconds(15 * 60)
        while ContinuousClock.now < deadline {
            if let result = try? await operations.shell("getprop sys.boot_completed", limit: .seconds(15)),
               result.text.trimmingCharacters(in: .whitespacesAndNewlines) == "1" {
                booted = true
                break
            }
            if case .failed = model.phase { break }
            try await Task.sleep(for: .seconds(2))
        }
        try #require(booted, "Android did not finish booting: \(model.runReport)")
        if let runtime = model.driver as? LiveRuntime { operations.run = runtime.runMark }
        let timeout = try await operations.shell("settings get system screen_off_timeout")
        let previousTimeout = timeout.text.trimmingCharacters(in: .whitespacesAndNewlines)
        try #require(previousTimeout == "null" || Int(previousTimeout) != nil)
        restoreDisplayTimeout = previousTimeout == "null" ? "settings delete system screen_off_timeout"
            : "settings put system screen_off_timeout \(previousTimeout)"
        // Interactive checks can wait several minutes for a person. Keep
        // Android awake too: its own display timeout is independent of iOS's.
        let awake = try await operations.shell(
            "settings put system screen_off_timeout 600000; input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard")
        try #require(awake.exitCode == 0)
        try await body(model, operations)
        _ = try? await operations.shell("\(restoreDisplayTimeout); \(cleanupCommand); reboot -p")
        try await waitForAndroidGuest(until: { model.phase == .home && model.runEnd.poweredOff }, seconds: 60)
    } catch {
        if let diagnostics = try? await operations.shell("dumpsys power; dumpsys sensorservice; logcat -d -t 120") {
            print("ANDROID diagnostics: \(diagnostics.text)")
        }
        _ = try? await operations.shell("\(restoreDisplayTimeout); \(cleanupCommand); reboot -p")
        // Keep the runtime alive through shutdown even when a probe fails,
        // so a system update can finish before the test host exits.
        try? await waitForAndroidGuest(until: { model.phase == .home && model.runEnd.poweredOff }, seconds: 60)
        throw error
    }
}

@MainActor
private func waitForAndroidGuest(until condition: () -> Bool, seconds: Int) async throws {
    let deadline = ContinuousClock.now + .seconds(seconds)
    while !condition(), ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(100))
    }
    try #require(condition(), "condition was not reached within \(seconds) seconds")
}
