// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What the guest's shell does for Android's screen reader, apart from the
/// connection that carries it (`shell`), so that what each exit status and
/// each reply means is the same code the application runs and a test's.
nonisolated enum ScreenReaderAdapter {
    typealias Shell = (_ command: String, _ limit: Duration) async throws -> ShellResult

    /// Android's screen reader in the guest; nil when the guest did not
    /// answer, a command in the read failed, or the reply is not the report.
    static func read(_ shell: Shell) async -> ScreenReaderState? {
        guard let result = try? await shell(ScreenReader.stateCommand, .seconds(15)) else { return nil }
        return ScreenReader.parse(result)
    }

    /// Turns the screen reader on or off, and returns the state the same
    /// command read after it. A command that did not finish with status 0, or
    /// whose reply is not the report, is a failure: the change cannot be told
    /// to have happened.
    static func change(_ on: Bool, service: String, _ shell: Shell) async throws -> ScreenReaderState {
        let result: ShellResult
        do {
            result = try await shell(ScreenReader.changeCommand(turningOn: on, service: service), .seconds(30))
        } catch {
            throw AdbOperationRunner.presented(error, activity: .screenReader)
        }
        guard result.exitCode == 0 else { throw failure(of: result) }
        guard let state = ScreenReader.parse(result) else { throw AdbFailure(.io(EIO), activity: .screenReader) }
        return state
    }

    /// Opens Android's Accessibility page on the guest's screen. The page is
    /// open only when the command finished with status 0.
    static func openSettings(_ shell: Shell) async throws {
        let result: ShellResult
        do {
            result = try await shell(ScreenReader.settingsCommand, .seconds(15))
        } catch {
            throw AdbOperationRunner.presented(error, activity: .screenReader)
        }
        guard result.exitCode == 0 else { throw failure(of: result) }
    }

    /// The guest's own words when it said some, bounded and without control
    /// characters (`ScreenReader.failureText`), else the activity's.
    private static func failure(of result: ShellResult) -> AdbFailure {
        guard let said = ScreenReader.failureText(result.stderr) else {
            return AdbFailure(.io(EIO), activity: .screenReader)
        }
        return AdbFailure(message: said)
    }
}

nonisolated extension AdbGuestOperations {
    /// Android's screen reader in the guest (`ScreenReaderAdapter.read`).
    func screenReader() async -> ScreenReaderState? {
        await ScreenReaderAdapter.read { try await shell($0, limit: $1) }
    }

    /// Turns the screen reader on or off (`ScreenReaderAdapter.change`).
    func changeScreenReader(_ on: Bool, service: String) async throws -> ScreenReaderState {
        try await ScreenReaderAdapter.change(on, service: service) { try await shell($0, limit: $1) }
    }

    /// Opens Android's Accessibility page (`ScreenReaderAdapter.openSettings`).
    func openAccessibilitySettings() async throws {
        try await ScreenReaderAdapter.openSettings { try await shell($0, limit: $1) }
    }
}
