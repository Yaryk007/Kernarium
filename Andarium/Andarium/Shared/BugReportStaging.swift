// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import UIKit

/// Where a bug report waits before it is shared: a folder of the temporary
/// directory, so it is never in Documents (the harness's) and never in a
/// backup. A report is deleted on every outcome of the share sheet, and a
/// launch removes whatever an ended process left.
nonisolated enum BugReportStaging {
    static var directory: URL { directory(in: FileManager.default.temporaryDirectory) }

    /// The folder in `temporary`: a test (the launch's sweep) gives a root of its own.
    static func directory(in temporary: URL) -> URL {
        temporary.appendingPathComponent("BugReports", isDirectory: true)
    }

    /// Removes the folder with everything in it.
    static func sweep(in temporary: URL = FileManager.default.temporaryDirectory) {
        try? FileManager.default.removeItem(at: directory(in: temporary))
    }

    /// The room a write to the volume can use, in bytes: the figure the rest of
    /// the application uses (`HostStorage`).
    static func availableBytes() -> Int64 {
        Int64(clamping: HostStorage.freeBytes(on: FileManager.default.temporaryDirectory))
    }

    /// Room kept beyond the report itself: the reserve every transfer keeps
    /// (`TransferRoom`), which is above where the run is paused for space.
    static let reserveBytes = Int64(clamping: TransferRoom.deviceReserveBytes)

    nonisolated struct NoRoom: LocalizedError, Equatable {
        var bytes: UInt64
        var device: String

        var errorDescription: String? {
            "There isn\u{2019}t enough free space on this \(device) for the report (\(Formats.size(bytes: bytes)))."
        }
    }

    /// Makes the staged file for a report of `size` bytes named after
    /// `remotePath`, or throws `NoRoom` when the volume cannot take it.
    static func makeSink(
        remotePath: String, size: UInt64, device: String, available: Int64 = availableBytes()
    ) throws -> StagedFileSink {
        let needed = Int64(clamping: size)
        guard available >= needed + reserveBytes else {
            throw NoRoom(bytes: UInt64(needed), device: device)
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let url = directory.appendingPathComponent(BugReport.stagedName(for: remotePath))
        try? FileManager.default.removeItem(at: url)
        guard FileManager.default.createFile(atPath: url.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        return try StagedFileSink(url: url)
    }
}

/// A staged file the report is pulled into.
nonisolated final class StagedFileSink: AdbSink, @unchecked Sendable {
    let url: URL
    private let handle: FileHandle

    init(url: URL) throws {
        self.url = url
        handle = try FileHandle(forWritingTo: url)
    }

    func write(_ data: Data) throws {
        try handle.write(contentsOf: data)
    }

    func close() {
        try? handle.close()
    }
}

/// The system share sheet over the screens, where Save to Files is one of the
/// actions. It is presented from the key window's root view controller; on an
/// iPad (and on a Mac) it is a popover at the window's centre with no arrow,
/// since a popover with no source crashes.
@MainActor
enum BugReportSharing {
    /// Shows the sheet; `finished` runs once, after any outcome, including
    /// Cancel. Returns false, without running `finished`, when there is no
    /// window to present from.
    @discardableResult
    static func present(_ file: URL, finished: @escaping @MainActor () -> Void) -> Bool {
        present([file]) { _ in finished() }
    }

    /// Shows the sheet on several files; `completed` says whether an action
    /// took them.
    @discardableResult
    static func present(_ files: [URL], completed: @escaping @MainActor (Bool) -> Void) -> Bool {
        guard let root = rootViewController() else { return false }
        var top = root
        while let presented = top.presentedViewController, !presented.isBeingDismissed { top = presented }
        let sheet = UIActivityViewController(activityItems: files, applicationActivities: nil)
        sheet.completionWithItemsHandler = { _, done, _, _ in
            Task { @MainActor in completed(done) }
        }
        if let popover = sheet.popoverPresentationController {
            popover.sourceView = root.view
            popover.sourceRect = CGRect(x: root.view.bounds.midX, y: root.view.bounds.midY, width: 0, height: 0)
            popover.permittedArrowDirections = []
        }
        top.present(sheet, animated: true)
        return true
    }

    private static func rootViewController() -> UIViewController? {
        let windows = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap(\.windows)
        return (windows.first(where: \.isKeyWindow) ?? windows.first)?.rootViewController
    }
}
