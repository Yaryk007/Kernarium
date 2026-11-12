// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

#if !APP_STORE

/// A development build's record of each run, in the Files app under
/// Andarium › Logs: the guest's console as the runtime hands it over
/// (`guest-console.log`) and what the runtime and Andarium print
/// (`runtime.log`). Each start replaces the last; the previous one is kept as
/// `*.previous.log`, so a run that hung can be read after the next launch.
nonisolated final class RunLog: @unchecked Sendable {
    static let shared = RunLog()

    private let lock = NSLock()
    private var console: FileHandle?
    private var redirected = false

    private static var folder: URL? {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?
            .appendingPathComponent("Logs", isDirectory: true)
    }

    /// Opens a fresh pair of logs for the start that is beginning.
    func begin() {
        guard let folder = Self.folder else { return }
        let manager = FileManager.default
        try? manager.createDirectory(at: folder, withIntermediateDirectories: true)
        lock.withLock {
            try? console?.close()
            console = nil
            let consoleURL = folder.appendingPathComponent("guest-console.log")
            Self.rotate(consoleURL)
            if manager.createFile(atPath: consoleURL.path, contents: nil) {
                console = try? FileHandle(forWritingTo: consoleURL)
            }
            // stdout and stderr follow the file for the rest of the process:
            // the runtime's own lines (the engine it chose and why) land
            // there.
            if !redirected {
                let runtimeURL = folder.appendingPathComponent("runtime.log")
                Self.rotate(runtimeURL)
                if freopen(runtimeURL.path, "a", stdout) != nil {
                    setvbuf(stdout, nil, _IOLBF, 0)
                    _ = dup2(fileno(stdout), fileno(stderr))
                    redirected = true
                }
            }
        }
        print("Andarium: run began \(Date())")
    }

    /// Appends a chunk of the guest's console.
    func console(_ data: Data) {
        lock.withLock { try? console?.write(contentsOf: data) }
    }

    private static func rotate(_ url: URL) {
        let manager = FileManager.default
        guard manager.fileExists(atPath: url.path) else { return }
        let previous = url.deletingPathExtension().appendingPathExtension("previous.log")
        try? manager.removeItem(at: previous)
        try? manager.moveItem(at: url, to: previous)
    }
}

#endif
