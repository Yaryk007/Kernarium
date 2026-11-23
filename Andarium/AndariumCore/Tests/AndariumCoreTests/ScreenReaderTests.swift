// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct ScreenReaderTests {
    private let talkBack = ScreenReader.services[0]
    private let googleTalkBack = ScreenReader.services[1]
    private let switchAccess = "com.example.switch/.SwitchService"

    /// The report a guest prints, record for record; `accessibility` follows
    /// the list unless a row says otherwise.
    private func report(
        packages: [String] = [], services: String, pm: Int = 0, settings: Int = 0, ag: Int = 0,
        accessibility: String? = nil
    ) -> String {
        let listed = !(services.isEmpty || services == "null")
        return (["pm=\(pm)", "sg=\(settings)", "ag=\(ag)"] + packages.map { "package:\($0)" }
            + ["services=\(services)", "accessibility=\(accessibility ?? (listed ? "1" : "0"))", "end=1"])
            .joined(separator: "\n") + "\n"
    }

    // MARK: Reading

    @Test func readsAGuestWithTalkBackOn() throws {
        let state = try #require(ScreenReader.parse(
            report(packages: ["com.android.talkback"], services: "\(switchAccess):\(talkBack)")))
        #expect(state.installedService == talkBack)
        #expect(state.enabledServices == [switchAccess, talkBack])
        #expect(state.isOn)
    }

    @Test func readsAGuestThatNeverHadAService() throws {
        let state = try #require(ScreenReader.parse(report(services: "null")))
        #expect(state.installedService == nil)
        #expect(state.enabledServices.isEmpty)
        #expect(!state.isOn && !state.isInstalled)
    }

    @Test func recognisesGooglesTalkBack() throws {
        let state = try #require(ScreenReader.parse(
            report(packages: ["com.google.android.marvin.talkback"], services: "")))
        #expect(state.installedService == googleTalkBack)
        #expect(!state.isOn)
    }

    @Test func prefersTheSystemBuildWhenBothAreInstalled() throws {
        let state = try #require(ScreenReader.parse(report(
            packages: ["com.google.android.marvin.talkback", "com.android.talkback"], services: googleTalkBack)))
        #expect(state.installedService == talkBack)
        // The Google build is on, not the one the switch drives.
        #expect(!state.isOn)
    }

    @Test func anotherPackageNamedLikeTalkBackIsNotTalkBack() throws {
        let state = try #require(ScreenReader.parse(report(packages: ["com.example.talkbackhelper"], services: "null")))
        #expect(!state.isInstalled)
    }

    @Test func aReadWhoseCommandFailedIsNotAState() {
        // The services line alone, or with any command's status not zero, is
        // what an old answer or a failing command prints; none is "off".
        #expect(ScreenReader.parse("") == nil)
        #expect(ScreenReader.parse("services=null\n") == nil)
        #expect(ScreenReader.parse(report(services: "", pm: 1)) == nil)
        #expect(ScreenReader.parse(report(services: "", settings: 255)) == nil)
        #expect(ScreenReader.parse(report(services: "", ag: 1)) == nil)
        #expect(ScreenReader.parse("pm=0\nsg=0\nag=0\npackage:com.android.talkback\n") == nil)
        let ok = Data(report(services: "null").utf8)
        #expect(ScreenReader.parse(ShellResult(stdout: ok, exitCode: 1)) == nil)
        #expect(ScreenReader.parse(ShellResult(stdout: ok, exitCode: nil)) == nil)
        #expect(ScreenReader.parse(ShellResult(stdout: ok, exitCode: 0)) != nil)
    }

    // MARK: The grammar

    /// Each of these is a reply that is not a report, however much of it looks
    /// like one: it is no state, so never on, off or "no screen reader".
    @Test func aReplyThatIsNotExactlyTheReportIsNotAState() {
        let good = report(packages: ["com.android.talkback"], services: switchAccess)
        #expect(ScreenReader.parse(good) != nil)
        let bad: [(String, String)] = [
            ("a failed status after a successful one", "pm=1\n" + report(services: "null")),
            ("pm repeated, the second failing", good.replacingOccurrences(of: "pm=0\n", with: "pm=0\npm=1\n")),
            ("a second services record", good.replacingOccurrences(of: "end=1", with: "services=\(talkBack)\nend=1")),
            ("services twice, the later one winning",
             good.replacingOccurrences(of: "services=", with: "services=\(talkBack)\nservices=")),
            ("an unknown record", good.replacingOccurrences(of: "end=1", with: "noise=1\nend=1")),
            ("a line after the terminator", good + "package:com.android.talkback\n"),
            ("a repeated terminator", good + "end=1\n"),
            ("no terminator", good.replacingOccurrences(of: "end=1\n", with: "")),
            ("a report cut after services=", "pm=0\nsg=0\nag=0\nservices="),
            ("a report cut after services= with its line break", "pm=0\nsg=0\nag=0\nservices=\n"),
            ("no final line break", String(good.dropLast())),
            ("records out of their order", "sg=0\npm=0\nag=0\nservices=null\naccessibility=0\nend=1\n"),
            ("a missing accessibility record", good.replacingOccurrences(of: "accessibility=1\n", with: "")),
            ("an accessibility value that is not 0, 1 or null", report(services: "null", accessibility: "2")),
            ("an empty accessibility value", report(services: "null", accessibility: "")),
            ("a control character in a service", report(services: "a/.A\u{7}")),
            ("a tab in a service", report(services: "a/.A\t:b/.B")),
            ("a carriage return", good.replacingOccurrences(of: "\n", with: "\r\n")),
            ("a space in a service", report(services: "a/.A b/.B")),
            ("a letter that is not ASCII", report(services: "a/.\u{E9}")),
            ("a component with no class", report(services: "a/")),
            ("a component with no slash", report(services: "a.b.C")),
            ("a component with two slashes", report(services: "a/b/c")),
            ("an empty component in the list", report(services: "a/.A::b/.B")),
            ("a trailing colon", report(services: "a/.A:")),
            ("a package line with a path", report(packages: ["../x"], services: "null")),
            ("a package line that is empty", report(packages: [""], services: "null")),
            ("a glob where a service is", report(services: "*")),
        ]
        for (name, output) in bad {
            #expect(ScreenReader.parse(output) == nil, "\(name) read as a state")
        }
    }

    @Test func aReplyThatIsTooLongIsNotAState() {
        let many = (0..<20_000).map { "a\($0)/.A" }.joined(separator: ":")
        #expect(ScreenReader.parse(report(services: many)) == nil)
        let sixtyFive = (0..<65).map { "a\($0)/.A" }.joined(separator: ":")
        #expect(ScreenReader.parse(report(services: sixtyFive)) == nil)
        let sixtyFour = (0..<64).map { "a\($0)/.A" }.joined(separator: ":")
        #expect(ScreenReader.parse(report(services: sixtyFour))?.enabledServices.count == 64)
        let longComponent = "a/." + String(repeating: "B", count: 300)
        #expect(ScreenReader.parse(report(services: longComponent)) == nil)
        let longValue = (0..<40).map { "p\($0)/." + String(repeating: "C", count: 250) }.joined(separator: ":")
        #expect(longValue.utf8.count > 8192 && ScreenReader.parse(report(services: longValue)) == nil)
        let packages = (0..<65).map { "com.example.p\($0)" }
        #expect(ScreenReader.parse(report(packages: packages, services: "null")) == nil)
    }

    @Test func aReplyThatIsNotUtf8IsNotAState() {
        var bytes = Array(report(services: "a/.A").utf8)
        #expect(ScreenReader.parse(ShellResult(stdout: Data(bytes), exitCode: 0)) != nil)
        let at = bytes.firstIndex(of: UInt8(ascii: "A"))!
        bytes[at] = 0xFF
        #expect(ScreenReader.parse(ShellResult(stdout: Data(bytes), exitCode: 0)) == nil)
        // A lone continuation byte decodes to U+FFFD, which is not ASCII either.
        bytes[at] = 0x80
        #expect(ScreenReader.parse(ShellResult(stdout: Data(bytes), exitCode: 0)) == nil)
        #expect(ScreenReader.parse(report(services: "a/.A\u{FFFD}")) == nil)
    }

    @Test func aFramedEmptyOrNullSettingIsAnAnswer() throws {
        for value in ["", "null"] {
            let state = try #require(ScreenReader.parse(report(services: value)), "\(value)")
            #expect(state.enabledServices.isEmpty && !state.isOn && state.isConsistent)
        }
        // The setting is a framed value however it is empty; a whole reply
        // that is empty is none.
        #expect(ScreenReader.parse("") == nil)
        #expect(ScreenReader.parse("\n") == nil)
        #expect(ScreenReader.parse("null\n") == nil)
    }

    @Test func aNestedClassAndOtherNamesTheGuestUsesAreComponents() throws {
        let state = try #require(ScreenReader.parse(report(services: "com.example.x_1/com.example.A$B:com.y/.Z")))
        #expect(state.enabledServices == ["com.example.x_1/com.example.A$B", "com.y/.Z"])
    }

    // MARK: accessibility_enabled

    @Test func aListThatNamesTalkBackWhileAccessibilityIsOffIsNotOn() throws {
        let state = try #require(ScreenReader.parse(
            report(packages: ["com.android.talkback"], services: talkBack, accessibility: "0")))
        #expect(state.enabledServices == [talkBack])
        #expect(!state.isOn && !state.isConsistent)
        let listless = try #require(ScreenReader.parse(report(services: "null", accessibility: "1")))
        #expect(!listless.isOn && !listless.isConsistent)
        let agreeing = try #require(ScreenReader.parse(
            report(packages: ["com.android.talkback"], services: talkBack)))
        #expect(agreeing.isOn && agreeing.isConsistent)
        // A mock guest reads nothing: its list says it all.
        #expect(ScreenReaderState(installedService: talkBack, enabledServices: [talkBack]).isOn)
        #expect(ScreenReaderState(installedService: talkBack, enabledServices: [talkBack]).isConsistent)
    }

    // MARK: A guest's error text

    @Test func aGuestsErrorTextIsBoundedAndHasNoControls() {
        #expect(ScreenReader.failureText(Data()) == nil)
        #expect(ScreenReader.failureText(Data("  \n\t ".utf8)) == nil)
        #expect(ScreenReader.failureText(Data("Error: no service\n".utf8)) == "Error: no service")
        let hostile = "ok\u{1B}[31m red\u{7}\r\nsecond\u{202E}line\u{0}\u{200B}x"
        let cleaned = ScreenReader.failureText(Data(hostile.utf8)) ?? ""
        #expect(cleaned == "ok[31m red secondlinex")
        #expect(!cleaned.unicodeScalars.contains { $0.properties.generalCategory == .control || $0.properties.generalCategory == .format })
        let long = ScreenReader.failureText(Data(String(repeating: "word ", count: 5_000).utf8)) ?? ""
        #expect(long.count <= ScreenReader.maximumFailureText + 1)
        #expect(long.hasSuffix("\u{2026}"))
        #expect(ScreenReader.failureText(Data([0xFF, 0xFE, 0x41])) == "A")
    }

    @Test func everyCommandStartsByNamingTheForegroundUser() {
        let commands = [
            ScreenReader.stateCommand, ScreenReader.settingsCommand,
            ScreenReader.changeCommand(turningOn: true, service: talkBack),
            ScreenReader.changeCommand(turningOn: false, service: talkBack),
        ]
        for command in commands {
            #expect(command.hasPrefix("user=$(am get-current-user) || exit 1;"))
            // Every `pm list packages`, `settings` and `am start` carries its --user.
            for word in ["pm list packages", "settings", "am start"].map({ $0 + " " }) {
                let all = command.components(separatedBy: word).count - 1
                let named = command.components(separatedBy: "\(word)--user $user").count - 1
                #expect(all == named, "\(word) without --user in \(command)")
            }
        }
        #expect(ScreenReader.stateCommand.contains("pm list packages --user $user"))
        #expect(ScreenReader.settingsCommand.contains("am start --user $user"))
    }

    // MARK: The switch

    @Test func theSwitchOffersWhatTheGuestHas() {
        let off = ScreenReaderState(installedService: talkBack, enabledServices: [])
        let on = ScreenReaderState(installedService: talkBack, enabledServices: [talkBack])
        let none = ScreenReaderState(installedService: nil, enabledServices: [switchAccess])
        #expect(ScreenReaderOffer(state: nil) == .unknown)
        #expect(ScreenReaderOffer(state: on) == .on)
        #expect(ScreenReaderOffer(state: off) == .off)
        #expect(ScreenReaderOffer(state: none) == .unavailable)
    }

    @Test func aReadThatFailedIsNotTheStateBeforeIt() {
        let on = ScreenReaderState(installedService: talkBack, enabledServices: [talkBack])
        #expect(ScreenReaderOffer(state: on, readFailed: true) == .failed)
        #expect(ScreenReaderOffer(state: nil, readFailed: true) == .failed)
    }

    @Test func onlyAKnownStateCanBeChanged() {
        #expect(ScreenReaderOffer.on.canChange && ScreenReaderOffer.off.canChange)
        #expect(!ScreenReaderOffer.unknown.canChange && !ScreenReaderOffer.unavailable.canChange)
        #expect(!ScreenReaderOffer.failed.canChange)
        #expect(ScreenReaderOffer.on.isOn && !ScreenReaderOffer.off.isOn)
    }

    @Test func aMockGuestChangesAsTheShellWould() {
        let state = ScreenReaderState(installedService: talkBack, enabledServices: [switchAccess, talkBack])
        #expect(ScreenReader.state(afterTurningOn: false, state).enabledServices == [switchAccess])
        #expect(ScreenReader.state(afterTurningOn: true, ScreenReaderState(
            installedService: talkBack, enabledServices: [switchAccess])).enabledServices == [switchAccess, talkBack])
    }
}
