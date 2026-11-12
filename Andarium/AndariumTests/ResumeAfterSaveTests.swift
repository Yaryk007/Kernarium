// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Darwin
import Foundation
import Testing
import UIKit
@testable import Andarium

/// The guest's settings can be saved while the application is away with the
/// guest quiesced: the boot screen saves them once Android has booted, and
/// Android can report that just as the person leaves. A save sets the backup
/// exclusion on the data disk, an extended attribute that moves the disk's
/// change time, between the quiesce that released the disk locks and the
/// resume that verifies them, and the guest must come back. A real guest runs
/// through the application's own lifecycle edges, as `BackgroundLockTests`
/// does, and with the same switch:
///
///     TEST_RUNNER_ANDARIUM_BACKGROUND_LOCK_TESTS=1 \
///         TEST_RUNNER_ANDARIUM_GUEST_INSTANCE=<test instance> \
///         xcodebuild test -scheme Andarium -destination 'platform=macOS,variant=Designed for iPad' \
///         -only-testing:AndariumTests/ResumeAfterSaveTests
///
/// The save flips Back up guest data, so the attribute is written on every
/// host; iOS's Foundation also writes it again on a save that changes
/// nothing. The flip is undone once the guest is back. The test requires
/// that the host held the guest while the application was away, which is
/// what releases the disk locks and, where the guest's RAM is written out,
/// shrinks the process's footprint.
@Suite(.serialized)
@MainActor
struct ResumeAfterSaveTests {
    private static var enabled: Bool {
        ProcessInfo.processInfo.environment["ANDARIUM_BACKGROUND_LOCK_TESTS"] == "1"
    }

    /// Long enough for the whole quiesce, a suspend that outlasts its
    /// ten-second deadline included.
    private static let away = Duration.seconds(30)

    /// How long Android runs on the launcher after it reports the boot
    /// complete, so that its start-up work is over when the person leaves.
    private static let settle = Duration.seconds(45)

    @Test(.timeLimit(.minutes(30))) func aSaveWhileAwayResumesTheGuest() async throws {
        guard Self.enabled else { return }
        let entry = AppEntry.fromArguments()
        let disk = GuestLocations(directory: AppEntry.guestDirectory(instance: entry.overrides.instance)).data
        try await withAndroidGuest { model, operations in
            let included = try #require(model.guest?.includeInBackups)
            _ = try await operations.shell("input keyevent KEYCODE_HOME", limit: .seconds(15))
            try await Task.sleep(for: Self.settle)
            let running = Self.footprint()
            let center = NotificationCenter.default
            center.post(name: UIApplication.willResignActiveNotification, object: UIApplication.shared)
            center.post(name: UIApplication.didEnterBackgroundNotification, object: UIApplication.shared)
            try await Task.sleep(for: Self.away)
            // A guest the host did not hold released no lock, and its resume
            // verifies nothing. One it held and wrote out leaves the process
            // with a fraction of its footprint and answers nothing.
            let away = Self.footprint()
            let answered = try? await operations.shell("echo away", limit: .seconds(10))
            try #require(
                away < running / 10 * 7 && answered == nil,
                "the host did not hold the guest while away (footprint \(running >> 20) MiB, then \(away >> 20) MiB), so this run checks nothing")

            let before = try Self.times(of: disk)
            model.guest?.includeInBackups = !included
            model.persistNow()
            let after = try Self.times(of: disk)
            try #require(after.change != before.change, "the save left the data disk's change time alone")
            #expect(after.modification == before.modification, "the save wrote the data disk")

            center.post(name: UIApplication.willEnterForegroundNotification, object: UIApplication.shared)
            center.post(name: UIApplication.didBecomeActiveNotification, object: UIApplication.shared)
            var back = false
            let deadline = ContinuousClock.now + .seconds(120)
            while !back, ContinuousClock.now < deadline {
                back = (try? await operations.shell("echo ok", limit: .seconds(10)))?.text == "ok\n"
                if !back { try await Task.sleep(for: .seconds(1)) }
            }
            model.guest?.includeInBackups = included
            model.persistNow()
            #expect(back, "the guest did not answer after the return")
        }
    }

    /// The process's physical footprint, which includes the guest's RAM.
    private static func footprint() -> UInt64 {
        var info = task_vm_info_data_t()
        let words = MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size
        var count = mach_msg_type_number_t(words)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: words) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return result == KERN_SUCCESS ? info.phys_footprint : 0
    }

    /// Seconds and nanoseconds of the disk's modification and change times.
    private struct Times {
        var modification: [Int]
        var change: [Int]
    }

    private static func times(of url: URL) throws -> Times {
        var details = stat()
        try #require(stat(url.path, &details) == 0, "stat \(url.lastPathComponent): \(errno)")
        return Times(
            modification: [details.st_mtimespec.tv_sec, details.st_mtimespec.tv_nsec],
            change: [details.st_ctimespec.tv_sec, details.st_ctimespec.tv_nsec])
    }
}
