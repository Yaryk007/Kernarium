// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The commands run in a real shell against stand-ins for `am`, `pm` and
/// `settings` that keep the setting in a file, so what each command does to
/// the list, and what it prints, is what is checked.
struct ScreenReaderShellTests {
    private let talkBack = ScreenReader.services[0]
    private let googleTalkBack = ScreenReader.services[1]
    private let switchAccess = "com.example.switch/.SwitchService"
    private let other = "com.example.other/.OtherService"

    /// A guest's tools in a temporary directory.
    final class FakeAndroid {
        let directory: URL
        /// The foreground user's number: each user has a store of its own.
        let user: String

        init(packages: [String] = ["com.android.talkback"], user: String = "0") throws {
            self.user = user
            directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try write("packages", packages.map { "package:\($0)" }.joined(separator: "\n"))
            try write("user", user)
            try tool("am", """
                case "$1" in
                get-current-user) cat "$FAKE/user"; [ -e "$FAKE/fail-user" ] && exit 7; exit 0 ;;
                start) echo "am $*" >>"$FAKE/log"; [ -e "$FAKE/fail-start" ] && exit 1; exit 0 ;;
                esac
                """)
            try tool("pm", """
                echo "pm $*" >>"$FAKE/log"
                [ -e "$FAKE/fail-pm" ] && exit 1
                cat "$FAKE/packages"
                """)
            try tool("settings", """
                echo "settings $*" >>"$FAKE/log"
                u=0
                [ "$1" = --user ] && { u=$2; shift 2; }
                action=$1; key=$3
                store="$FAKE/u$u"; mkdir -p "$store"
                case "$action" in
                get) [ -e "$FAKE/fail-get" ] && exit 1
                     if [ -e "$FAKE/fail-get-after-write" ] && grep -q -e ' put ' -e ' delete ' "$FAKE/log"; then exit 1; fi
                     if [ -e "$store/$key" ]; then cat "$store/$key"; else echo null; fi ;;
                put) [ -e "$FAKE/fail-put-$key" ] && exit 1; printf '%s' "$4" >"$store/$key" ;;
                delete) [ -e "$FAKE/fail-delete" ] && exit 1; rm -f "$store/$key"; exit 0 ;;
                esac
                """)
        }

        deinit { try? FileManager.default.removeItem(at: directory) }

        func write(_ name: String, _ text: String) throws {
            try text.write(to: directory.appendingPathComponent(name), atomically: true, encoding: .utf8)
        }

        func tool(_ name: String, _ body: String) throws {
            let url = directory.appendingPathComponent(name)
            try ("#!/bin/sh\n" + body + "\n").write(to: url, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path)
        }

        /// The list `user` has, as Android's own page left it (the foreground
        /// user's unless another is named).
        func set(services: [String]?, enabled: Bool? = nil, user named: String? = nil) throws {
            let store = directory.appendingPathComponent("u\(named ?? user)")
            try FileManager.default.createDirectory(at: store, withIntermediateDirectories: true)
            let key = store.appendingPathComponent("enabled_accessibility_services")
            if let services { try services.joined(separator: ":").write(to: key, atomically: true, encoding: .utf8) }
            else { try? FileManager.default.removeItem(at: key) }
            // Android keeps `accessibility_enabled` on while a service is listed.
            let on = enabled ?? (services.map { !$0.isEmpty } ?? false)
            try (on ? "1" : "0").write(
                to: store.appendingPathComponent("accessibility_enabled"), atomically: true, encoding: .utf8)
        }

        /// A list that is not one, written as it is.
        func setRaw(services: String, user named: String? = nil) throws {
            let store = directory.appendingPathComponent("u\(named ?? user)")
            try FileManager.default.createDirectory(at: store, withIntermediateDirectories: true)
            try services.write(to: store.appendingPathComponent("enabled_accessibility_services"), atomically: true, encoding: .utf8)
        }

        func fail(_ name: String) throws { try write(name, "") }

        func services(of named: String? = nil) -> String? { read("u\(named ?? user)/enabled_accessibility_services") }
        func accessibilityEnabled(of named: String? = nil) -> String? { read("u\(named ?? user)/accessibility_enabled") }
        var services: String? { services() }
        var accessibilityEnabled: String? { accessibilityEnabled() }
        /// Whether a command wrote or deleted anything.
        var wrote: Bool { log.contains { $0.contains(" put ") || $0.contains(" delete ") } }
        var log: [String] { (read("log") ?? "").split(separator: "\n").map(String.init) }

        private func read(_ name: String) -> String? {
            try? String(contentsOf: directory.appendingPathComponent(name), encoding: .utf8)
        }

        func run(_ command: String) throws -> ShellResult {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/bin/sh")
            process.arguments = ["-c", command]
            // Where a pattern in a value could find files to name.
            process.currentDirectoryURL = directory
            process.environment = ["PATH": "\(directory.path):/usr/bin:/bin", "FAKE": directory.path]
            let output = Pipe()
            process.standardOutput = output
            let errors = Pipe()
            process.standardError = errors
            try process.run()
            let data = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return ShellResult(stdout: data, exitCode: Int(process.terminationStatus))
        }
    }

    // MARK: Reading

    @Test func aReadReportsTheGuestsState() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess, talkBack])
        let state = try #require(ScreenReader.parse(try guest.run(ScreenReader.stateCommand)))
        #expect(state.isOn && state.enabledServices == [switchAccess, talkBack])
        #expect(guest.log.allSatisfy { $0.contains("--user 0") })
    }

    @Test func eachFailingCommandMakesTheReadFail() throws {
        for failing in ["fail-pm", "fail-get"] {
            let guest = try FakeAndroid()
            try guest.fail(failing)
            let result = try guest.run(ScreenReader.stateCommand)
            // The shell's own status is zero: the markers carry the failure.
            #expect(result.exitCode == 0)
            #expect(ScreenReader.parse(result) == nil, "\(failing) read as a state")
        }
        let none = try FakeAndroid()
        try none.write("user", "")
        #expect(try none.run(ScreenReader.stateCommand).exitCode != 0)
    }

    @Test func theForegroundUserIsTheOneNamed() throws {
        let guest = try FakeAndroid(user: "10")
        _ = try guest.run(ScreenReader.stateCommand)
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        _ = try guest.run(ScreenReader.settingsCommand)
        #expect(!guest.log.isEmpty && guest.log.allSatisfy { $0.contains("--user 10") })
    }

    // MARK: Changing

    @Test func turningOnAddsTalkBackAfterTheOtherServices() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess])
        let state = try #require(ScreenReader.parse(try guest.run(
            ScreenReader.changeCommand(turningOn: true, service: talkBack))))
        #expect(guest.services == "\(switchAccess):\(talkBack)")
        #expect(guest.accessibilityEnabled == "1")
        #expect(state.isOn && state.enabledServices == [switchAccess, talkBack])
    }

    @Test func turningOnTwiceDoesNotListTalkBackTwice() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack])
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(guest.services == talkBack)
    }

    @Test func turningOffKeepsTheOtherServicesOn() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack, switchAccess, googleTalkBack])
        let state = try #require(ScreenReader.parse(try guest.run(
            ScreenReader.changeCommand(turningOn: false, service: talkBack))))
        #expect(guest.services == switchAccess)
        #expect(guest.accessibilityEnabled == "1")
        #expect(!state.isOn)
    }

    @Test func turningOffTheLastServiceTurnsAccessibilityOff() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack], enabled: true)
        let state = try #require(ScreenReader.parse(try guest.run(
            ScreenReader.changeCommand(turningOn: false, service: talkBack))))
        #expect(guest.services == nil)
        #expect(guest.accessibilityEnabled == "0")
        #expect(state.enabledServices.isEmpty)
    }

    @Test func aServiceAndroidTurnedOnMeanwhileSurvivesBothChanges() throws {
        // The command reads the list itself, so it neither drops `other` nor
        // needs the host's list.
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack, other])
        _ = try guest.run(ScreenReader.changeCommand(turningOn: false, service: talkBack))
        #expect(guest.services == other)
        try guest.set(services: [other, switchAccess])
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(guest.services == "\(other):\(switchAccess):\(talkBack)")
    }

    @Test func aChangeThatCouldNotWriteFails() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess])
        try guest.fail("fail-put-enabled_accessibility_services")
        let result = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(result.exitCode != 0)
        #expect(guest.services == switchAccess)

        let second = try FakeAndroid()
        try second.set(services: [switchAccess])
        try second.fail("fail-put-accessibility_enabled")
        #expect(try second.run(ScreenReader.changeCommand(turningOn: true, service: talkBack)).exitCode != 0)
    }

    @Test func aChangeWhoseReadFailsFailsBeforeItWrites() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess])
        try guest.fail("fail-get")
        let result = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(result.exitCode != 0)
        #expect(!guest.log.contains { $0.contains(" put ") })
    }

    // MARK: The user, the live list and the writes

    @Test func aFailedLookupOfTheUserStopsEveryCommandWhateverItPrinted() throws {
        // `am get-current-user` printed a number and failed: the number is
        // not the user.
        let guest = try FakeAndroid(user: "10")
        try guest.set(services: [switchAccess])
        try guest.fail("fail-user")
        for command in [
            ScreenReader.stateCommand, ScreenReader.settingsCommand,
            ScreenReader.changeCommand(turningOn: true, service: talkBack),
            ScreenReader.changeCommand(turningOn: false, service: talkBack),
        ] {
            let result = try guest.run(command)
            #expect(result.exitCode != 0, "\(command.prefix(40))")
            #expect(ScreenReader.parse(result) == nil)
        }
        #expect(guest.log.isEmpty)
        #expect(guest.services == switchAccess)
    }

    @Test func aListThatIsNotOneIsRefusedBeforeAnythingIsWritten() throws {
        let tooMany = (0..<65).map { "a\($0)/.A" }.joined(separator: ":")
        let tooLong = String(repeating: "p/.B:", count: 1700) + "p/.B"
        let hostile = [
            "*", "a", "a/b/c", "/.A", "a/", "a/.A::b/.B", ":a/.A", "a/.A:", "a b/.B", "a/.A;touch x",
            "a/.A\nb/.B", "$(touch x)/.A", "a/.A\u{7}", tooMany, tooLong, "a/.\u{E9}",
        ]
        for value in hostile {
            for on in [true, false] {
                let guest = try FakeAndroid()
                try guest.setRaw(services: value)
                let result = try guest.run(ScreenReader.changeCommand(turningOn: on, service: talkBack))
                #expect(result.exitCode != 0, "\(value.prefix(20)) was written back")
                #expect(!guest.wrote, "\(value.prefix(20)) was followed by a write")
                #expect(guest.services == value, "\(value.prefix(20)) was changed")
            }
        }
    }

    @Test func aListIsNeverReadAsFilePatterns() throws {
        // `*` in a shell that splits the value would name the files beside it,
        // and `*/*` files whose names look like components.
        for value in ["*", "*/*", "?/??"] {
            let guest = try FakeAndroid()
            try guest.setRaw(services: value)
            let result = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
            #expect(result.exitCode != 0, "\(value)")
            #expect(guest.services == value, "\(value)")
            #expect(!guest.wrote, "\(value)")
        }
    }

    @Test func anEmptyAndANullListAreLists() throws {
        for value in ["", "null"] {
            let guest = try FakeAndroid()
            try value.isEmpty ? guest.setRaw(services: "") : guest.write("u0-null", "")
            let result = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
            #expect(result.exitCode == 0, "\(value)")
            #expect(guest.services == talkBack)
            #expect(guest.accessibilityEnabled == "1")
        }
    }

    @Test func eachWriteAndTheDeleteThatFailsFailsTheChange() throws {
        // Turning on: the list's write, then accessibility's.
        for failing in ["fail-put-enabled_accessibility_services", "fail-put-accessibility_enabled"] {
            let guest = try FakeAndroid()
            try guest.set(services: [switchAccess])
            try guest.fail(failing)
            #expect(try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack)).exitCode != 0, "\(failing)")
        }
        // Turning off the last one: the delete, then accessibility's write.
        for failing in ["fail-delete", "fail-put-accessibility_enabled"] {
            let guest = try FakeAndroid()
            try guest.set(services: [talkBack])
            try guest.fail(failing)
            let result = try guest.run(ScreenReader.changeCommand(turningOn: false, service: talkBack))
            #expect(result.exitCode != 0, "\(failing)")
        }
        // A delete that failed leaves accessibility on and the list as it was;
        // nothing turned it off behind it.
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack])
        try guest.fail("fail-delete")
        _ = try guest.run(ScreenReader.changeCommand(turningOn: false, service: talkBack))
        #expect(guest.services == talkBack && guest.accessibilityEnabled == "1")
        #expect(!guest.log.contains { $0.contains("put secure accessibility_enabled 0") })
    }

    @Test func aWriteThatLeftTheListAndAccessibilityApartIsNotOn() throws {
        // The list took the write and accessibility's did not: the command
        // fails, and a read of what is left is a state that is not "on".
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess])
        try guest.fail("fail-put-accessibility_enabled")
        #expect(try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack)).exitCode != 0)
        #expect(guest.services == "\(switchAccess):\(talkBack)")
        let left = try #require(ScreenReader.parse(try guest.run(ScreenReader.stateCommand)))
        #expect(left.enabledServices.contains(talkBack))
        #expect(left.accessibilitySetting == true && left.isConsistent && left.isOn, "Android's own flag was already on")

        let bare = try FakeAndroid()
        try bare.set(services: [], enabled: false)
        try bare.fail("fail-put-accessibility_enabled")
        #expect(try bare.run(ScreenReader.changeCommand(turningOn: true, service: talkBack)).exitCode != 0)
        let apart = try #require(ScreenReader.parse(try bare.run(ScreenReader.stateCommand)))
        #expect(apart.enabledServices == [talkBack])
        #expect(!apart.isConsistent && !apart.isOn)
    }

    @Test func aReadbackThatFailsAfterTheWritesIsNotAState() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess])
        try guest.fail("fail-get-after-write")
        let result = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        // The writes happened; the report's own read did not, and says so.
        #expect(guest.services == "\(switchAccess):\(talkBack)")
        #expect(ScreenReader.parse(result) == nil)
    }

    @Test func eachAndroidUserHasAStoreOfItsOwn() throws {
        let guest = try FakeAndroid(user: "10")
        try guest.set(services: [other], user: "0")
        try guest.set(services: [switchAccess], user: "10")
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(guest.services(of: "10") == "\(switchAccess):\(talkBack)")
        #expect(guest.services(of: "0") == other)
        #expect(guest.accessibilityEnabled(of: "0") == "1")
        _ = try guest.run(ScreenReader.changeCommand(turningOn: false, service: talkBack))
        #expect(guest.services(of: "10") == switchAccess)
        #expect(guest.services(of: "0") == other)
        let state = try #require(ScreenReader.parse(try guest.run(ScreenReader.stateCommand)))
        #expect(state.enabledServices == [switchAccess])
    }

    @Test func aServiceNameCannotBreakOutOfTheCommand() throws {
        let guest = try FakeAndroid()
        let hostile = "x/.Y';touch \(guest.directory.path)/pwned;'"
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: hostile))
        #expect(guest.services == hostile)
        #expect(!FileManager.default.fileExists(atPath: guest.directory.appendingPathComponent("pwned").path))
    }

    // MARK: Android's page

    @Test func theAccessibilityPageOpensForTheForegroundUser() throws {
        let guest = try FakeAndroid(user: "0")
        #expect(try guest.run(ScreenReader.settingsCommand).exitCode == 0)
        #expect(guest.log == ["am start --user 0 -a android.settings.ACCESSIBILITY_SETTINGS"])
        try guest.fail("fail-start")
        #expect(try guest.run(ScreenReader.settingsCommand).exitCode != 0)
    }
}
