// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Where an older build staged the files of an email feedback draft (its
/// screenshots, start logs and attachments). The feature is gone, but a process
/// of that build that ended with a draft open leaves the folder behind, so a
/// launch of this build removes it (`AndariumProcess.sweepLeftovers`): nothing
/// else reads or writes it.
nonisolated enum LegacyFeedbackFolder {
    static func directory(in temporary: URL = FileManager.default.temporaryDirectory) -> URL {
        temporary.appendingPathComponent("Feedback", isDirectory: true)
    }

    /// Removes the folder with everything in it.
    static func sweep(in temporary: URL = FileManager.default.temporaryDirectory) {
        try? FileManager.default.removeItem(at: directory(in: temporary))
    }
}
