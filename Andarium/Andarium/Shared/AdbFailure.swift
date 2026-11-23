// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What the guest's operations are for, which the words of a failure follow.
nonisolated enum AdbActivity {
    case installApp
    case bugReport
    /// Browsing the guest's files, importing and exporting.
    case files
    /// Turning Android’s screen reader on or off.
    case screenReader

    /// Each sentence is whole for each activity, so a language is free to
    /// order the words of the activity as it needs.
    fileprivate var notReady: String {
        switch self {
        case .installApp: String(localized: "The virtual machine isn\u{2019}t ready to install apps. Try again in a moment.")
        case .bugReport: String(localized: "The virtual machine isn\u{2019}t ready to make a bug report. Try again in a moment.")
        case .files: String(localized: "The virtual machine isn\u{2019}t ready to open its files. Try again in a moment.")
        case .screenReader: String(localized: "The virtual machine isn\u{2019}t ready to change Android\u{2019}s screen reader. Try again in a moment.")
        }
    }

    fileprivate var restarted: String {
        switch self {
        case .installApp:
            String(localized: "The virtual machine\u{2019}s debugging service restarted before the app was installed. Try again.")
        case .bugReport:
            String(localized: "The virtual machine\u{2019}s debugging service restarted before the bug report was finished. Try again.")
        case .files:
            String(localized: "The virtual machine\u{2019}s debugging service restarted before the transfer finished. Try again.")
        case .screenReader:
            String(localized: "The virtual machine\u{2019}s debugging service restarted before Android\u{2019}s screen reader changed. Try again.")
        }
    }

    fileprivate var stoppedResponding: String {
        switch self {
        case .installApp: String(localized: "The virtual machine stopped responding while installing the app.")
        case .bugReport: String(localized: "The virtual machine stopped responding while making the bug report.")
        case .files: String(localized: "The virtual machine stopped responding while copying the files.")
        case .screenReader: String(localized: "The virtual machine stopped responding while changing Android\u{2019}s screen reader.")
        }
    }

    fileprivate var couldNot: String {
        switch self {
        case .installApp: String(localized: "The virtual machine couldn\u{2019}t install the app.")
        case .bugReport: String(localized: "The virtual machine couldn\u{2019}t make the bug report.")
        case .files: String(localized: "The virtual machine couldn\u{2019}t copy the file.")
        case .screenReader: String(localized: "The virtual machine couldn\u{2019}t change Android\u{2019}s screen reader.")
        }
    }
}

/// An adb failure in the words an alert shows.
nonisolated struct AdbFailure: LocalizedError, Equatable {
    var message: String

    var errorDescription: String? { message }

    init(message: String) {
        self.message = message
    }

    init(_ error: AdbError, activity: AdbActivity) {
        switch error {
        case .refused:
            message = activity.notReady
        case .notAnswering:
            message = String(localized: "The virtual machine isn\u{2019}t answering yet. Try again once it has started.")
        case .keyRejected:
            message = String(localized: "The virtual machine didn\u{2019}t accept Andarium\u{2019}s key. Restart the virtual machine and try again.")
        case .guestGone, .streamClosed:
            // adbd restarts when the guest's USB debugging turns off, which
            // ends a connection in use; the next try reaches the new one.
            message = activity.restarted
        case .timedOut:
            message = activity.stoppedResponding
        case let .remote(text):
            message = Self.remote(text, activity: activity)
        case .serviceRefused, .protocolViolation, .io, .cancelled:
            message = activity.couldNot
        }
    }

    /// The text of a `FAIL` from the guest, with the one it sends for a
    /// report under way in plain words.
    private static func remote(_ text: String, activity: AdbActivity) -> String {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let lower = trimmed.lowercased()
        if activity == .bugReport, lower.contains("in progress") {
            return String(localized: "The virtual machine is already making a bug report.")
        }
        if activity == .files {
            if lower.contains("no space left") { return String(localized: "Not enough space in the virtual machine.") }
            if lower.contains("permission denied") || lower.contains("read-only") {
                return String(localized: "Android doesn\u{2019}t allow changes to this folder.")
            }
        }
        return trimmed.isEmpty ? activity.couldNot : trimmed
    }
}

/// Why ADB over network did not start, and what its row says without an
/// address.
nonisolated enum AdbNetworkMessages {
    static func cannotStart(_ error: Error) -> String {
        if let adb = error as? AdbError, adb == .io(EADDRINUSE) {
            return String(
                localized: "Port \(String(AdbNetworkBridge.defaultPort)) is already in use on this device.",
                comment: "The ADB over network port, written as plain digits (5555), never grouped.")
        }
        return String(localized: "ADB over network could not start.")
    }

    static let noAddress = String(localized: "This device has no Wi-Fi address. Connect to Wi-Fi to use ADB over network.")
}
