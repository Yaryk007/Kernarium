// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Darwin
import Foundation
import Testing
@testable import Andarium

/// Every place the application reads the device's free space reads one figure,
/// the space a write can use (`HostStorage`, from `statfs(2)`), and none the
/// larger "important usage" figure that counts purgeable space. The system
/// call answers the same on a device and in a Simulator, so each reading is
/// held between two answers of it; the volume is in use by others, hence the
/// slack.
@MainActor
struct FreeSpaceFigureTests {
    private let slack = 256 * Limits.bytesPerMiB

    private func statfsBytes(_ path: String) -> UInt64 {
        var volume = statfs()
        guard statfs(path, &volume) == 0 else { return 0 }
        return UInt64(volume.f_bavail) * UInt64(volume.f_bsize)
    }

    /// Whether the figure `read` returns is what a write could use at the time.
    private func isWhatAWriteCanUse(_ path: String, _ read: () -> UInt64) -> Bool {
        let before = statfsBytes(path)
        let figure = read()
        let after = statfsBytes(path)
        return before > 0 && figure + slack >= min(before, after) && figure <= max(before, after) + slack
    }

    @Test func theApplicationReadsTheSpaceAWriteCanUse() {
        #expect(AppEntry.freeStorageBytes() > 0)
        #expect(isWhatAWriteCanUse(NSHomeDirectory()) { AppEntry.freeStorageBytes() })
    }

    @Test func theLaunchsDeviceStatesThatFigure() {
        #expect(isWhatAWriteCanUse(NSHomeDirectory()) { AppEntry.hostDevice().freeStorageBytes })
    }

    @Test func aBugReportIsStagedAgainstThatFigure() {
        let temporary = FileManager.default.temporaryDirectory.path
        #expect(isWhatAWriteCanUse(temporary) { UInt64(BugReportStaging.availableBytes()) })
    }
}
