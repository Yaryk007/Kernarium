// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import Andarium

/// What the launch removes from the temporary directory, on a root of the
/// test's own: the folders of an ended process, and the one an older build
/// staged an email feedback draft in, which an upgrade would otherwise keep (a
/// screenshot, a start log or an attachment may be in it).
@Suite(.serialized)
struct LaunchSweepTests {
    private func seed(_ names: [String], in root: URL) throws {
        for name in names {
            let file = root.appendingPathComponent(name)
            try FileManager.default.createDirectory(
                at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data("x".utf8).write(to: file)
        }
    }

    @Test func theLaunchRemovesWhatAnEndedProcessLeftAndTheLegacyFeedbackFolder() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("LaunchSweepTests-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let leftovers = [
            "BugReports/report.zip", "GuestFiles/\(UUID().uuidString)/export.bin",
            "PickedFiles/\(UUID().uuidString)/app.apk",
            // An older build's feedback draft: a screenshot, a log and a file the sender attached.
            "Feedback/\(UUID().uuidString)/screenshot.png", "Feedback/\(UUID().uuidString)/start-log.txt",
            "Feedback/incoming/attachment.pdf",
        ]
        let neighbour = "Neighbour/keep.txt"
        try seed(leftovers + [neighbour], in: root)

        AndariumProcess.sweepLeftovers(in: root)

        for name in leftovers {
            #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent(name).path), "\(name) was kept")
        }
        for folder in ["BugReports", "GuestFiles", "PickedFiles", "Feedback"] {
            #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent(folder).path), "\(folder) was kept")
        }
        // Nothing else in the temporary directory is touched.
        #expect(FileManager.default.fileExists(atPath: root.appendingPathComponent(neighbour).path))
    }

    @Test func theLegacyFolderIsTheOneAnOlderBuildStagedIn() {
        let root = URL(fileURLWithPath: "/tmp/root", isDirectory: true)
        #expect(LegacyFeedbackFolder.directory(in: root).path == "/tmp/root/Feedback")
    }
}
