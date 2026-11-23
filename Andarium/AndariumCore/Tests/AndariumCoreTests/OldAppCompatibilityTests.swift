// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The adapter for the image's `kernarium_oldapp` command: what it sends, and
/// how each answer is read, on output the command is specified to print.
struct OldAppCompatibilityTests {
    private func answer(_ text: String, _ status: Int? = 0) -> GuestShellOutput {
        GuestShellOutput(text: text, exitCode: status)
    }

    private let header = "kernarium_oldapp 1\n"

    // MARK: Listing

    @Test func aListingYieldsItsFiveFields() throws {
        let text = header
            + "com.example.rally\tRally 2011\ton\t1763200000\t1700000000\n"
            + "com.example.pixelknights\tPixel Knights\toff\t0\t1762000000\n"
        guard case let .apps(apps) = OldAppCompatibility.parseListing(answer(text)) else {
            Issue.record("not a listing")
            return
        }
        #expect(apps.count == 2)
        // Pixel Knights sorts before Rally 2011, whatever the order printed.
        #expect(apps[0] == OldApp(package: "com.example.pixelknights", label: "Pixel Knights", isOn: false, turnedOn: 0, lastUpdate: 1_762_000_000))
        #expect(apps[1] == OldApp(package: "com.example.rally", label: "Rally 2011", isOn: true, turnedOn: 1_763_200_000, lastUpdate: 1_700_000_000))
        #expect(apps[1].id == "com.example.rally")
    }

    @Test func aListingWithNoAppsIsAnAnswer() {
        #expect(OldAppCompatibility.parseListing(answer(header)) == .apps([]))
        #expect(OldAppCompatibility.parseListing(answer(header + "\n\n")) == .apps([]))
    }

    @Test func aTrailingFieldAnImageAddsIsIgnored() {
        let text = header + "com.example.rally\tRally 2011\ton\t17\t18\textra\tmore fields\n"
        #expect(
            OldAppCompatibility.parseListing(answer(text))
                == .apps([OldApp(package: "com.example.rally", label: "Rally 2011", isOn: true, turnedOn: 17, lastUpdate: 18)]))
    }

    @Test func aLabelWithOddCharactersIsShownAsItIs() throws {
        // Quotes, a percent sign, a backslash, an emoji, an Arabic name, a CJK name.
        let labels = ["Tom's \"Quest\" 100% \\ done", "Cats \u{1F431} Run", "\u{0644}\u{0639}\u{0628}\u{0629}", "\u{53E4}\u{3044}\u{30A2}\u{30D7}\u{30EA}", "$(rm -rf) `x` ;|&"]
        for (index, label) in labels.enumerated() {
            let text = header + "com.example.app\(index)\t\(label)\toff\t0\t5\n"
            guard case let .apps(apps) = OldAppCompatibility.parseListing(answer(text)) else {
                Issue.record("not a listing")
                return
            }
            #expect(apps.map(\.label) == [label])
        }
    }

    @Test func bidirectionalControlsAreDroppedSoANameCannotReorderItsTitle() {
        // Embeddings, overrides and isolates (U+202A to U+202E, U+2066 to U+2069) go; the text between stays.
        let marks = "\u{202A}\u{202B}\u{202C}\u{202D}\u{202E}\u{2066}\u{2067}\u{2068}\u{2069}"
        let text = header + "com.example.a\t\(marks)Evil\u{202E}\u{0644}\u{0639}\(marks) Name\toff\t0\t1\n"
        guard case let .apps(apps) = OldAppCompatibility.parseListing(answer(text)) else {
            Issue.record("not a listing")
            return
        }
        #expect(apps.map(\.label) == ["Evil\u{0644}\u{0639} Name"])
        // A name of nothing but those controls is the package name.
        let only = header + "com.example.b\t\(marks)\toff\t0\t1\n"
        #expect(OldAppCompatibility.parseListing(answer(only)) == .apps([OldApp(package: "com.example.b", label: "com.example.b", isOn: false, lastUpdate: 1)]))
    }

    @Test func controlCharactersAreSpacesAndTheNameIsTrimmedAndBounded() throws {
        let text = header
            + "com.example.a\t  Two\u{1B}Words \toff\t0\t1\n"
            + "com.example.b\t   \toff\t0\t1\n"
            + "com.example.c\t\(String(repeating: "x", count: 500))\toff\t0\t1\n"
        guard case let .apps(apps) = OldAppCompatibility.parseListing(answer(text)) else {
            Issue.record("not a listing")
            return
        }
        let byPackage = Dictionary(uniqueKeysWithValues: apps.map { ($0.package, $0.label) })
        #expect(byPackage["com.example.a"] == "Two Words")
        // An empty name is the package name.
        #expect(byPackage["com.example.b"] == "com.example.b")
        #expect(byPackage["com.example.c"]?.count == OldAppCompatibility.longestLabel)
    }

    @Test func aLineThatIsNotAnAppIsSkippedAndTheOthersStay() {
        let text = header
            + "short\tline\n"
            + "not a package\tName\ton\t0\t1\n"
            + "com.example.badstate\tName\tmaybe\t0\t1\n"
            + "com.example.good\tGood\ton\t0\t1\n"
            + "com.example.good\tDuplicate\toff\t0\t2\n"
        #expect(
            OldAppCompatibility.parseListing(answer(text))
                == .apps([OldApp(package: "com.example.good", label: "Good", isOn: true, turnedOn: 0, lastUpdate: 1)]))
    }

    @Test func aNumberThatIsNotOneIsZero() {
        let text = header + "com.example.a\tA\ton\tsoon\t-\n"
        #expect(
            OldAppCompatibility.parseListing(answer(text))
                == .apps([OldApp(package: "com.example.a", label: "A", isOn: true, turnedOn: 0, lastUpdate: 0)]))
    }

    @Test func aLineEndingInCarriageReturnAndNewlineReadsTheSame() {
        let text = "kernarium_oldapp 1\r\ncom.example.a\tA\ton\t3\t4\r\n"
        #expect(
            OldAppCompatibility.parseListing(answer(text))
                == .apps([OldApp(package: "com.example.a", label: "A", isOn: true, turnedOn: 3, lastUpdate: 4)]))
    }

    @Test func aMissingCommandMakesTheFeatureUnavailable() {
        // The shell's status for a command it cannot find.
        #expect(OldAppCompatibility.parseListing(answer("", 127)) == .unavailable)
    }

    @Test func aListThatTheImageAnswersAsAUsageErrorMakesItUnavailableToo() {
        // Status 64: the image does not take `list` as this reader sends it, so it does not speak this version.
        #expect(OldAppCompatibility.parseListing(answer("usage: kernarium_oldapp set <package> on|off\n", 64)) == .unavailable)
        // Other statuses are a listing that got no answer.
        for status in [1, 2, 126, 255] as [Int?] + [nil] {
            #expect(OldAppCompatibility.parseListing(answer(header, status)) == .failed)
        }
    }

    @Test func aVersionAndroidDoesNotKnowMakesItUnavailable() {
        #expect(OldAppCompatibility.parseListing(answer("kernarium_oldapp 2\ncom.example.a\tA\ton\t0\t1\n")) == .unavailable)
        #expect(OldAppCompatibility.parseListing(answer("kernarium_oldapp 0\n")) == .unavailable)
    }

    @Test func anythingElseIsAFailure() {
        // No exit status, another status, no version line, a version that is not a number.
        #expect(OldAppCompatibility.parseListing(answer(header, nil)) == .failed)
        #expect(OldAppCompatibility.parseListing(answer(header, 1)) == .failed)
        #expect(OldAppCompatibility.parseListing(answer("")) == .failed)
        #expect(OldAppCompatibility.parseListing(answer("com.example.a\tA\ton\t0\t1\n")) == .failed)
        #expect(OldAppCompatibility.parseListing(answer("kernarium_oldapp one\n")) == .failed)
        #expect(OldAppCompatibility.parseListing(answer("kernarium_oldapp\n")) == .failed)
    }

    // MARK: Writing

    @Test func theSetCommandNamesThePackageAndTheState() {
        #expect(OldAppCompatibility.setCommand(package: "com.example.rally", on: true) == "kernarium_oldapp set 'com.example.rally' on")
        #expect(OldAppCompatibility.setCommand(package: "com.example.rally", on: false) == "kernarium_oldapp set 'com.example.rally' off")
        #expect(OldAppCompatibility.listCommand == "kernarium_oldapp list")
    }

    @Test func aNameThatIsNotAPackageIsNeverSent() {
        for name in ["", "x", "com.example.a; reboot", "com.example.a b", "com.example.$(id)", "com..a", "1com.a", "a\nb.c", "com.example.a'b"] {
            #expect(OldAppCompatibility.setCommand(package: name, on: true) == nil, "\(name)")
            #expect(OldAppCompatibility.openCommand(package: name) == nil, "\(name)")
        }
    }

    @Test func theSuccessLineAppliesAndNothingElseDoes() {
        let applied = answer("com.example.rally\ton\n")
        #expect(OldAppCompatibility.parseWrite(applied, package: "com.example.rally", on: true) == .applied)
        #expect(OldAppCompatibility.parseWrite(answer("com.example.rally\toff\n"), package: "com.example.rally", on: false) == .applied)
        // The line for the other state, for another package, none, or more than one line.
        #expect(OldAppCompatibility.parseWrite(applied, package: "com.example.rally", on: false) == .failed)
        #expect(OldAppCompatibility.parseWrite(applied, package: "com.example.other", on: true) == .failed)
        #expect(OldAppCompatibility.parseWrite(answer(""), package: "com.example.rally", on: true) == .failed)
        #expect(OldAppCompatibility.parseWrite(answer("com.example.rally\ton\nextra\n"), package: "com.example.rally", on: true) == .failed)
    }

    @Test func theStatusDecidesARefusal() {
        // 2: not eligible. Anything else that is not 0 (usage 64, missing 127, none) fails.
        #expect(OldAppCompatibility.parseWrite(answer("", 2), package: "com.example.a", on: true) == .notEligible)
        for status in [1, 64, 127, 255] as [Int?] + [nil] {
            #expect(OldAppCompatibility.parseWrite(answer("com.example.a\ton\n", status), package: "com.example.a", on: true) == .failed)
        }
    }

    // MARK: Open

    @Test func anOpenAppHasProcessIds() {
        #expect(OldAppCompatibility.parseOpen(answer("12345\n")) == true)
        #expect(OldAppCompatibility.parseOpen(answer("12345 12346\n")) == true)
        #expect(OldAppCompatibility.parseOpen(answer("", 1)) == false)
        // Status 0 with no process id, or with text that is not one, is no process.
        #expect(OldAppCompatibility.parseOpen(answer("", 0)) == false)
        #expect(OldAppCompatibility.parseOpen(answer("abc\n")) == false)
        // Any other status cannot be told.
        #expect(OldAppCompatibility.parseOpen(answer("pidof: not found", 127)) == nil)
        #expect(OldAppCompatibility.parseOpen(answer("12345", nil)) == nil)
    }

    // MARK: Through a shell

    private final class Shell: GuestShell, @unchecked Sendable {
        var answers: [String: GuestShellOutput] = [:]
        var failing = false
        private(set) var commands: [String] = []
        private(set) var limits: [Duration] = []

        func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
            commands.append(command)
            limits.append(limit)
            if failing { throw AdbError.notAnswering }
            return answers[command] ?? GuestShellOutput(text: "", exitCode: 127)
        }
    }

    @Test func theAdapterSendsTheCommandsAndReadsTheAnswers() async {
        let shell = Shell()
        shell.answers[OldAppCompatibility.listCommand] = answer(header + "com.example.rally\tRally 2011\toff\t0\t9\n")
        shell.answers["kernarium_oldapp set 'com.example.rally' on"] = answer("com.example.rally\ton\n")
        shell.answers["pidof 'com.example.rally'"] = answer("", 1)
        let adapter = OldAppCompatibility(shell: shell)
        #expect(await adapter.list() == .apps([OldApp(package: "com.example.rally", label: "Rally 2011", isOn: false, lastUpdate: 9)]))
        #expect(await adapter.isOpen(package: "com.example.rally") == false)
        #expect(await adapter.set(package: "com.example.rally", on: true) == .applied)
        #expect(shell.commands == [
            "kernarium_oldapp list", "pidof 'com.example.rally'", "kernarium_oldapp set 'com.example.rally' on",
        ])
    }

    @Test func aListingIsBoundedByTheLimitItIsGiven() async {
        let shell = Shell()
        shell.answers[OldAppCompatibility.listCommand] = answer(header)
        let adapter = OldAppCompatibility(shell: shell)
        _ = await adapter.list()
        _ = await adapter.list(limit: OldAppCompatibility.installListLimit)
        #expect(shell.limits == [.seconds(20), .seconds(5)])
    }

    @Test func aMissingCommandThroughTheShellIsUnavailable() async {
        // The fake shell answers 127 for a command it was not told.
        #expect(await OldAppCompatibility(shell: Shell()).list() == .unavailable)
    }

    @Test func aGuestThatDoesNotAnswerIsAFailureAndAnUnknownOpenState() async {
        let shell = Shell()
        shell.failing = true
        let adapter = OldAppCompatibility(shell: shell)
        #expect(await adapter.list() == .failed)
        #expect(await adapter.set(package: "com.example.rally", on: true) == .failed)
        #expect(await adapter.isOpen(package: "com.example.rally") == nil)
    }

    @Test func aPackageNameThatIsNotOneReachesNoShell() async {
        let shell = Shell()
        let adapter = OldAppCompatibility(shell: shell)
        #expect(await adapter.set(package: "x; reboot", on: true) == .failed)
        #expect(await adapter.isOpen(package: "x; reboot") == nil)
        #expect(shell.commands.isEmpty)
    }

    // MARK: The install hint

    @Test func onlyARefusalBelowFourteenIsOld() {
        #expect(OldAppInstallHint.isOldTarget(LegacyTarget(appLevel: 7, requiredLevel: 24)))
        #expect(OldAppInstallHint.isOldTarget(LegacyTarget(appLevel: 13, requiredLevel: 24)))
        #expect(!OldAppInstallHint.isOldTarget(LegacyTarget(appLevel: 14, requiredLevel: 24)))
        #expect(!OldAppInstallHint.isOldTarget(LegacyTarget(appLevel: 22, requiredLevel: 24)))
        #expect(!OldAppInstallHint.isOldTarget(LegacyTarget(appLevel: nil, requiredLevel: 24)))
    }

    @Test func theHintNamesTheAppWithTheNewestUpdateTheInstallChanged() {
        let old = OldApp(package: "com.example.old", label: "Old", isOn: false, lastUpdate: 100)
        let added = OldApp(package: "com.example.added", label: "Added", isOn: false, lastUpdate: 500)
        let other = OldApp(package: "com.example.other", label: "Other", isOn: false, lastUpdate: 300)
        #expect(OldAppInstallHint.installedApp(listedBefore: [old], listedAfter: [old, added]) == added)
        // An update of an app already listed counts, and the newest of two changes is the one.
        let updated = OldApp(package: "com.example.old", label: "Old", isOn: false, lastUpdate: 600)
        #expect(OldAppInstallHint.installedApp(listedBefore: [old, other], listedAfter: [updated, other, added]) == updated)
        // Nothing changed (the installed app is not one the switch applies to), or nothing was there.
        #expect(OldAppInstallHint.installedApp(listedBefore: [old, other], listedAfter: [old, other]) == nil)
        #expect(OldAppInstallHint.installedApp(listedBefore: [], listedAfter: []) == nil)
        #expect(OldAppInstallHint.installedApp(listedBefore: [], listedAfter: [added]) == added)
    }
}
