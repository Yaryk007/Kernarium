// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// What the application makes of the guest's shell for Android's screen
/// reader: the adapter between the connection and the model. A command that
/// did not finish with status 0 did not do what it was asked, whatever it
/// printed; the guest's own words are shown bounded and without control
/// characters.
struct ScreenReaderAdapterTests {
    private let talkBack = ScreenReader.services[0]

    /// A shell that answers as told and remembers what it was asked.
    private final class Guest: @unchecked Sendable {
        var commands: [String] = []
        var limits: [Duration] = []
        let answer: Result<ShellResult, Error>
        init(_ answer: Result<ShellResult, Error>) { self.answer = answer }
        convenience init(exit code: Int?, stdout: String = "", stderr: String = "") {
            self.init(.success(ShellResult(stdout: Data(stdout.utf8), stderr: Data(stderr.utf8), exitCode: code)))
        }
        func shell(_ command: String, _ limit: Duration) async throws -> ShellResult {
            commands.append(command)
            limits.append(limit)
            return try answer.get()
        }
    }

    private func report(enabled: [String] = [], setting: String = "0") -> String {
        "pm=0\nsg=0\nag=0\npackage:com.android.talkback\nservices=\(enabled.joined(separator: ":"))\n"
            + "accessibility=\(setting)\nend=1\n"
    }

    private let noAnswer = AdbFailure(.io(EIO), activity: .screenReader).message

    // MARK: Opening Android's settings

    @Test func theSettingsOpenOnlyWhenTheCommandFinishedWell() async throws {
        let opened = Guest(exit: 0)
        try await ScreenReaderAdapter.openSettings { try await opened.shell($0, $1) }
        #expect(opened.commands == [ScreenReader.settingsCommand])
        #expect(opened.limits == [.seconds(15)])
    }

    @Test(arguments: [9, 1, 127, -1])
    func aSettingsCommandThatExitedWithAnotherStatusDidNotOpenThem(_ code: Int) async throws {
        let guest = Guest(exit: code, stderr: "Error: Activity not started\n")
        let failure = await #expect(throws: AdbFailure.self) {
            try await ScreenReaderAdapter.openSettings { try await guest.shell($0, $1) }
        }
        #expect(failure?.message == "Error: Activity not started")
    }

    @Test func aSettingsCommandWithoutAnExitStatusDidNotOpenThem() async throws {
        let guest = Guest(exit: nil)
        let failure = await #expect(throws: AdbFailure.self) {
            try await ScreenReaderAdapter.openSettings { try await guest.shell($0, $1) }
        }
        #expect(failure?.message == noAnswer)
    }

    @Test func aConnectionThatFailsOrIsCancelledIsToldAsSuch() async throws {
        let refused = Guest(.failure(AdbError.refused))
        let failure = await #expect(throws: AdbFailure.self) {
            try await ScreenReaderAdapter.openSettings { try await refused.shell($0, $1) }
        }
        #expect(failure?.message == AdbFailure(.refused, activity: .screenReader).message)
        let cancelled = Guest(.failure(CancellationError()))
        await #expect(throws: CancellationError.self) {
            try await ScreenReaderAdapter.openSettings { try await cancelled.shell($0, $1) }
        }
    }

    // MARK: Changing

    @Test func aChangeAsksTheGuestForTheServiceItWasGiven() async throws {
        let guest = Guest(exit: 0, stdout: report(enabled: [talkBack], setting: "1"))
        let state = try await ScreenReaderAdapter.change(true, service: talkBack) { try await guest.shell($0, $1) }
        #expect(state == ScreenReaderState(installedService: talkBack, enabledServices: [talkBack], accessibilitySetting: true))
        #expect(guest.commands == [ScreenReader.changeCommand(turningOn: true, service: talkBack)])
        #expect(guest.limits == [.seconds(30)])
    }

    @Test(arguments: [9, 1, 2])
    func aChangeThatExitedWithAnotherStatusFailedWhateverItPrinted(_ code: Int) async throws {
        let guest = Guest(exit: code, stdout: report(enabled: [talkBack], setting: "1"), stderr: "")
        let failure = await #expect(throws: AdbFailure.self) {
            try await ScreenReaderAdapter.change(true, service: talkBack) { try await guest.shell($0, $1) }
        }
        #expect(failure?.message == noAnswer)
    }

    @Test func aChangeWithoutAnExitStatusFailed() async throws {
        let guest = Guest(exit: nil, stdout: report(enabled: [talkBack], setting: "1"))
        await #expect(throws: AdbFailure.self) {
            try await ScreenReaderAdapter.change(true, service: talkBack) { try await guest.shell($0, $1) }
        }
    }

    @Test func aChangeWhoseReplyIsNotTheReportFailed() async throws {
        for stdout in ["", "ok\n", report(enabled: [talkBack], setting: "1").replacingOccurrences(of: "end=1\n", with: "")] {
            let guest = Guest(exit: 0, stdout: stdout)
            let failure = await #expect(throws: AdbFailure.self) {
                try await ScreenReaderAdapter.change(true, service: talkBack) { try await guest.shell($0, $1) }
            }
            #expect(failure?.message == noAnswer)
        }
    }

    /// What the guest says on failing reaches a person only bounded, in one
    /// line, with no control or escape character.
    @Test func theGuestsOwnWordsAreBoundedAndHaveNoControls() async throws {
        let said = "settings: \u{1B}[31mcannot write\u{1B}[0m\r\n\tto the \u{7}store\u{0} " + String(repeating: "x", count: 5000)
        let guest = Guest(exit: 1, stderr: said)
        let failure = try #require(await #expect(throws: AdbFailure.self) {
            try await ScreenReaderAdapter.change(true, service: talkBack) { try await guest.shell($0, $1) }
        })
        #expect(failure.message.count <= 161)
        #expect(failure.message.hasPrefix("settings: [31mcannot write[0m to the store "))
        #expect(!failure.message.unicodeScalars.contains { $0.properties.generalCategory == .control })
        #expect(!failure.message.contains("\n"))
    }

    // MARK: Reading

    @Test func aReadAnswersOnlyWithTheReport() async throws {
        let good = Guest(exit: 0, stdout: report(enabled: [talkBack], setting: "1"))
        let state = await ScreenReaderAdapter.read { try await good.shell($0, $1) }
        #expect(state == ScreenReaderState(installedService: talkBack, enabledServices: [talkBack], accessibilitySetting: true))
        #expect(good.commands == [ScreenReader.stateCommand])
        #expect(good.limits == [.seconds(15)])
    }

    @Test func aReadThatFailedAnswersNothing() async throws {
        let text = report(enabled: [talkBack], setting: "1")
        let guests = [
            Guest(exit: 9, stdout: text), Guest(exit: nil, stdout: text), Guest(.failure(AdbError.refused)),
            Guest(exit: 0, stdout: text.replacingOccurrences(of: "end=1\n", with: "")), Guest(exit: 0),
        ]
        for guest in guests {
            let state = await ScreenReaderAdapter.read { try await guest.shell($0, $1) }
            #expect(state == nil)
        }
    }
}
