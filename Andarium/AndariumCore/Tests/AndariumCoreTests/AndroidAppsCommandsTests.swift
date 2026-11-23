// SPDX-License-Identifier: GPL-2.0-or-later

import CryptoKit
import Foundation
import Testing

@testable import AndariumCore

private let key1 = "0123456789abcdef"
private let key2 = "fedcba9876543210"
private let digest = String(repeating: "ab", count: 32)

private func shellResult(_ text: String, exit: Int? = 0) -> ShellResult {
    ShellResult(stdout: Data(text.utf8), exitCode: exit)
}

/// An answer of `list`, with `rows` as given and the counts the header should carry.
private func listing(
    rows: [String], count: Int? = nil, end: Int? = nil, version: String = "1", extraHeader: [String] = [], digest: String = digest
) -> String {
    (["kernarium_apps \(version)", "digest\t\(digest)", "count\t\(count ?? rows.count)", "skipped\t0", "locales\ten-US", "user\t0"]
        + extraHeader + ["rows"] + rows + ["end\t\(end ?? rows.count)"]).joined(separator: "\n") + "\n"
}

private func row(_ package: String, _ activity: String, _ label: String, builtIn: String = "0", key: String = key1, extra: String = "") -> String {
    "\(package)\t\(activity)\t\(label)\t\(builtIn)\t\(key)" + extra
}

/// A PNG-shaped byte string of a given side: the signature and an IHDR; the
/// reader checks the framing, the digest and the header, not the pixels.
private func png(side: Int, tail: Int = 20) -> Data {
    var bytes = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13])
    bytes.append(contentsOf: Array("IHDR".utf8))
    for value in [side, side] {
        bytes.append(contentsOf: [UInt8(value >> 24 & 0xFF), UInt8(value >> 16 & 0xFF), UInt8(value >> 8 & 0xFF), UInt8(value & 0xFF)])
    }
    bytes.append(contentsOf: [8, 6, 0, 0, 0])
    bytes.append(Data(repeating: 7, count: tail))
    return bytes
}

private func hex(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

private func record(
    _ component: String, status: String = "ok", side: Int = 192, kind: String = "adaptive", alpha: String = "0", data: Data? = nil,
    declaredLength: Int? = nil, declaredSha: String? = nil
) -> Data {
    if status != "ok" {
        return Data("icon\t\(component)\t\(status)\t0\t0\t-\t0\t0\t-\n".utf8)
    }
    let bytes = data ?? png(side: side)
    var out = Data("icon\t\(component)\tok\t\(side)\t\(side)\t\(kind)\t\(alpha)\t\(declaredLength ?? bytes.count)\t\(declaredSha ?? hex(bytes))\n".utf8)
    out.append(bytes)
    out.append(0x0A)
    return out
}

private func iconsAnswer(_ count: Int, records: [Data], end: Int? = nil, version: String = "1") -> ShellResult {
    var out = Data("kernarium_apps \(version)\nicons\t\(count)\n".utf8)
    for record in records { out.append(record) }
    out.append(Data("end\t\(end ?? count)\n".utf8))
    return ShellResult(stdout: out, exitCode: 0)
}

private let comet = AndroidComponent(packageName: "com.example.comet", activityClass: "com.example.comet.Main")!
private let hop = AndroidComponent(packageName: "com.example.hop", activityClass: "com.example.hop.Main$Launcher")!

private final class ScriptedShell: GuestBinaryShell, @unchecked Sendable {
    private let lock = NSLock()
    var result: Result<ShellResult, Error>
    private(set) var commands: [String] = []
    init(_ result: Result<ShellResult, Error>) { self.result = result }
    func runBinary(_ command: String, limit: Duration) async throws -> ShellResult {
        lock.withLock { commands.append(command) }
        return try result.get()
    }
}

private struct Unreachable: Error {}

struct AndroidAppsCommandsTests {
    // MARK: Commands

    @Test func theCommandsAreTheFourVerbsWithEachComponentInSingleQuotes() {
        #expect(AndroidAppsCommands.digestCommand == "kernarium_apps digest")
        #expect(AndroidAppsCommands.listCommand == "kernarium_apps list")
        #expect(AndroidAppsCommands.launchCommand(for: hop) == "kernarium_apps launch 'com.example.hop/com.example.hop.Main$Launcher'")
        #expect(AndroidAppsCommands.iconsCommand(for: [comet, hop])
            == "kernarium_apps icons 'com.example.comet/com.example.comet.Main' 'com.example.hop/com.example.hop.Main$Launcher'")
    }

    @Test func anIconsCommandNamesOneToEightDistinctComponents() {
        #expect(AndroidAppsCommands.iconsCommand(for: []) == nil)
        #expect(AndroidAppsCommands.iconsCommand(for: [comet, comet]) == nil)
        let nine = (0..<9).map { AndroidComponent(packageName: "com.example.a\($0)", activityClass: "com.example.Main")! }
        #expect(AndroidAppsCommands.iconsCommand(for: nine) == nil)
        #expect(AndroidAppsCommands.iconsCommand(for: Array(nine.prefix(8))) != nil)
    }

    @Test func aClassNameCanNeverCarryAShellOrAnOptionThroughACommand() {
        // The grammar is what keeps a name a name: none of these gets a component, so none gets a command.
        for hostile in ["a'; id; '", "a$(id)", "--es", "a b", "a\nb", "a`id`", "a|b", "a&b", "a>b", "a\"b", "a\\b"] {
            #expect(AndroidComponent(packageName: "com.example.app", activityClass: hostile) == nil, "\(hostile)")
            #expect(AndroidComponent(packageName: hostile, activityClass: "com.example.Main") == nil, "\(hostile)")
        }
    }

    // MARK: Digest

    @Test func aDigestAnswerIsTheDigest() {
        let text = "kernarium_apps 1\ndigest\t\(digest)\ncount\t3\nskipped\t0\nlocales\tja-JP,en-US\nuser\t0\n"
        #expect(AndroidAppsCommands.parseDigest(shellResult(text)) == .digest(digest))
    }

    @Test func aDigestThatIsNotSixtyFourHexCharactersIsAFailure() {
        for bad in ["", "ABCD", String(repeating: "g", count: 64), String(repeating: "A", count: 64), String(repeating: "a", count: 63)] {
            let text = "kernarium_apps 1\ndigest\t\(bad)\ncount\t3\n"
            #expect(AndroidAppsCommands.parseDigest(shellResult(text)) == .failed, "\(bad)")
        }
        #expect(AndroidAppsCommands.parseDigest(shellResult("kernarium_apps 1\ncount\t3\n")) == .failed)
        #expect(AndroidAppsCommands.parseDigest(shellResult("kernarium_apps 1\ndigest\t\(digest)\ndigest\t\(digest)\n")) == .failed)
    }

    @Test func aDigestAnswerNeedsItsWholeHeaderBlock() {
        let whole = "kernarium_apps 1\ndigest\t\(digest)\ncount\t3\nskipped\t0\nlocales\ten-US\nuser\t0\n"
        #expect(AndroidAppsCommands.parseDigest(shellResult(whole)) == .digest(digest))
        for missing in ["count", "skipped", "locales", "user"] {
            let text = whole.split(separator: "\n").filter { !$0.hasPrefix(missing + "\t") }.joined(separator: "\n") + "\n"
            #expect(AndroidAppsCommands.parseDigest(shellResult(text)) == .failed, "no \(missing) line")
        }
        // The digest alone is no answer, nor is a count that is not a number, nor a line that is not part of it.
        #expect(AndroidAppsCommands.parseDigest(shellResult("kernarium_apps 1\ndigest\t\(digest)\n")) == .failed)
        #expect(AndroidAppsCommands.parseDigest(shellResult(whole.replacingOccurrences(of: "count\t3", with: "count\tmany"))) == .failed)
        #expect(AndroidAppsCommands.parseDigest(shellResult(whole.replacingOccurrences(of: "user\t0", with: "user\t-1"))) == .failed)
        #expect(AndroidAppsCommands.parseDigest(shellResult(whole + "garbage\n")) == .failed)
    }

    @Test func aFieldALaterVersionAppendsToAHeaderLineIsIgnoredFieldByField() {
        let text = "kernarium_apps 1\ndigest\t\(digest)\tmore\ncount\t3\tmore\tand more\nskipped\t0\tx\nlocales\ten-US\ty\nuser\t0\tz\n"
        #expect(AndroidAppsCommands.parseDigest(shellResult(text)) == .digest(digest))
        let row = row("com.example.comet", "com.example.comet.Main", "Comet Hop")
        let list = text.replacingOccurrences(of: "count\t3", with: "count\t1") + "rows\n\(row)\nend\t1\textra\n"
        guard case let .apps(parsed) = AndroidAppsCommands.parseListing(shellResult(list)) else { Issue.record("not a listing"); return }
        #expect(parsed.apps.count == 1)
        #expect(parsed.digest == digest)
    }

    @Test func theRowsMarkerIsItsFirstFieldWhateverFollowsIt() {
        let empty = listing(rows: []).replacingOccurrences(of: "rows\n", with: "rows\textra\tmore\n")
        #expect(AndroidAppsCommands.parseListing(shellResult(empty)) == .apps(AndroidAppList(apps: [], digest: digest)))
        let one = listing(rows: [row("com.example.comet", "com.example.comet.Main", "Comet Hop")])
            .replacingOccurrences(of: "rows\n", with: "rows\textra\n")
        guard case let .apps(parsed) = AndroidAppsCommands.parseListing(shellResult(one)) else { Issue.record("not a listing"); return }
        #expect(parsed.apps.count == 1)
        // A line that only starts with the letters is not the marker.
        let other = listing(rows: []).replacingOccurrences(of: "rows\n", with: "rowsy\n")
        #expect(AndroidAppsCommands.parseListing(shellResult(other)) == .failed)
    }

    @Test func anImageWithoutTheToolOrWithAnotherVersionOffersNothing() {
        #expect(AndroidAppsCommands.parseDigest(ShellResult(stderr: Data("kernarium_apps: inaccessible or not found".utf8), exitCode: 127)) == .unavailable)
        #expect(AndroidAppsCommands.parseDigest(shellResult("", exit: 64)) == .unavailable)
        #expect(AndroidAppsCommands.parseDigest(shellResult("kernarium_apps 2\ndigest\t\(digest)\n")) == .unavailable)
        #expect(AndroidAppsCommands.parseListing(shellResult("", exit: 127)) == .unavailable)
        #expect(AndroidAppsCommands.parseListing(shellResult(listing(rows: [], version: "2"))) == .unavailable)
    }

    @Test func anExitOfOneOrAnythingElseIsAFailureWhateverStdoutHolds() {
        #expect(AndroidAppsCommands.parseDigest(shellResult("kernarium_apps 1\ndigest\t\(digest)\n", exit: 1)) == .failed)
        #expect(AndroidAppsCommands.parseListing(shellResult(listing(rows: []), exit: 1)) == .failed)
        #expect(AndroidAppsCommands.parseListing(shellResult(listing(rows: []), exit: nil)) == .failed)
    }

    // MARK: List

    @Test func aListingIsTheAppsWithTheirNamesAndKeys() {
        let text = listing(rows: [
            row("com.android.settings", "com.android.settings.Settings", "Settings", builtIn: "1", key: key2),
            row("com.example.comet", "com.example.comet.Main", "Comet Hop"),
        ])
        guard case let .apps(list) = AndroidAppsCommands.parseListing(shellResult(text)) else { Issue.record("not a listing"); return }
        #expect(list.digest == digest)
        #expect(list.apps.map(\.label) == ["Settings", "Comet Hop"])
        #expect(list.apps.map(\.builtIn) == [true, false])
        #expect(list.apps.map(\.iconTag) == [key2, key1])
        #expect(list.apps[1].id == "com.example.comet/com.example.comet.Main")
    }

    @Test func aListingWithNoAppsIsAnAnswerToo() {
        #expect(AndroidAppsCommands.parseListing(shellResult(listing(rows: []))) == .apps(AndroidAppList(apps: [], digest: digest)))
    }

    @Test func aNewHeaderKeyAndANewTrailingFieldAreIgnored() {
        let text = listing(rows: [row("com.example.comet", "com.example.comet.Main", "Comet Hop", extra: "\tnew\tfields")], extraHeader: ["future\tvalue"])
        guard case let .apps(list) = AndroidAppsCommands.parseListing(shellResult(text)) else { Issue.record("not a listing"); return }
        #expect(list.apps.count == 1)
    }

    @Test func aLabelIsMadeSafeAgainEvenThoughTheToolDidIt() {
        let text = listing(rows: [row("com.example.comet", "com.example.comet.Main", "  Comet\u{202E}Hop \u{7}  ")])
        guard case let .apps(list) = AndroidAppsCommands.parseListing(shellResult(text)) else { Issue.record("not a listing"); return }
        #expect(list.apps[0].label == "CometHop")
        let empty = listing(rows: [row("com.example.comet", "com.example.comet.Main", "")])
        guard case let .apps(named) = AndroidAppsCommands.parseListing(shellResult(empty)) else { Issue.record("not a listing"); return }
        #expect(named.apps[0].label == "com.example.comet")
    }

    @Test func everyBrokenFramingIsAFailureAndNothingOfItIsKept() {
        let good = row("com.example.comet", "com.example.comet.Main", "Comet Hop")
        let cases: [(String, String)] = [
            ("no end line", listing(rows: [good]).replacingOccurrences(of: "end\t1\n", with: "")),
            ("end count differs", listing(rows: [good], end: 2)),
            ("header count differs", listing(rows: [good], count: 2)),
            ("count above the bound", listing(rows: [good], count: 5_000, end: 5_000)),
            ("no rows line", listing(rows: [good]).replacingOccurrences(of: "rows\n", with: "")),
            ("a row of four fields", listing(rows: ["com.example.comet\tcom.example.comet.Main\tComet\t0"])),
            ("a bad component", listing(rows: [row("com.example.comet", "com.example.comet.Main'", "Comet")])),
            ("a leading dot activity", listing(rows: [row("com.example.comet", ".Main", "Comet")])),
            ("a bad built-in flag", listing(rows: [row("com.example.comet", "com.example.comet.Main", "Comet", builtIn: "2")])),
            ("a short icon key", listing(rows: [row("com.example.comet", "com.example.comet.Main", "Comet", key: "abc")])),
            ("an upper-case icon key", listing(rows: [row("com.example.comet", "com.example.comet.Main", "Comet", key: "0123456789ABCDEF")])),
            ("a bad digest", listing(rows: [good], digest: "xyz")),
            ("a carriage return", listing(rows: [good]).replacingOccurrences(of: "\n", with: "\r\n")),
            ("no final line feed", String(listing(rows: [good]).dropLast())),
            ("a repeated header key", listing(rows: [good], extraHeader: ["user\t1"])),
            ("not the tool's text at all", "Segmentation fault\n"),
            ("empty", ""),
        ]
        for (name, text) in cases {
            #expect(AndroidAppsCommands.parseListing(shellResult(text)) == .failed, "\(name)")
        }
        // Every line of the header block is required in a listing too.
        for missing in ["skipped", "locales", "user"] {
            let text = listing(rows: [good]).split(separator: "\n", omittingEmptySubsequences: false).filter { !$0.hasPrefix(missing + "\t") }.joined(separator: "\n")
            #expect(AndroidAppsCommands.parseListing(shellResult(text)) == .failed, "no \(missing) line")
        }
        // Bytes that are not UTF-8.
        var broken = Data(listing(rows: [good]).utf8)
        broken.append(contentsOf: [0xFF, 0xFE, 0x0A])
        #expect(AndroidAppsCommands.parseListing(ShellResult(stdout: broken, exitCode: 0)) == .failed)
    }

    @Test func aRepeatedComponentIsListedOnce() {
        let rowText = row("com.example.comet", "com.example.comet.Main", "Comet Hop")
        guard case let .apps(list) = AndroidAppsCommands.parseListing(shellResult(listing(rows: [rowText, rowText]))) else { Issue.record("not a listing"); return }
        #expect(list.apps.count == 1)
    }

    @Test func aListingLargerThanTheToolMaySendIsAFailure() {
        var text = listing(rows: [])
        text += String(repeating: "x", count: AndroidAppsCommands.longestListing)
        #expect(AndroidAppsCommands.parseListing(shellResult(text)) == .failed)
    }

    // MARK: Icons

    @Test func anIconAnswerIsEachIconInTheOrderAsked() {
        let a = png(side: 192)
        let b = png(side: 128, tail: 40)
        let answer = iconsAnswer(3, records: [
            record(comet.identifier, data: a), record(hop.identifier, side: 128, alpha: "1", data: b),
            record("com.example.gone/com.example.gone.Main", status: "missing"),
        ])
        let gone = AndroidComponent(packageName: "com.example.gone", activityClass: "com.example.gone.Main")!
        let answers = AndroidAppsCommands.parseIcons(answer, asked: [comet, hop, gone])
        #expect(answers == [.png(a, hasAlpha: false), .png(b, hasAlpha: true), .missing])
    }

    @Test func everyStatusButOkIsARecordWithNoData() {
        let answer = iconsAnswer(4, records: [
            record(comet.identifier, status: "noicon"), record(hop.identifier, status: "error"),
            record("com.example.a/com.example.a.Main", status: "deferred"), record("com.example.b/com.example.b.Main", status: "missing"),
        ])
        let a = AndroidComponent(packageName: "com.example.a", activityClass: "com.example.a.Main")!
        let b = AndroidComponent(packageName: "com.example.b", activityClass: "com.example.b.Main")!
        #expect(AndroidAppsCommands.parseIcons(answer, asked: [comet, hop, a, b]) == [.noIcon, .error, .deferred, .missing])
    }

    @Test func anIconWhoseBytesDoNotMatchItsRecordIsAnErrorForThatAppOnly() {
        let good = png(side: 192)
        let notPNG = Data(repeating: 1, count: 40)
        var wrongSize = png(side: 192)
        wrongSize[19] = 191
        let answers = AndroidAppsCommands.parseIcons(iconsAnswer(4, records: [
            record(comet.identifier, data: good),
            record(hop.identifier, data: notPNG),
            record("com.example.a/com.example.a.Main", data: wrongSize),
            record("com.example.b/com.example.b.Main", declaredSha: String(repeating: "0", count: 64)),
        ]), asked: [
            comet, hop, AndroidComponent(packageName: "com.example.a", activityClass: "com.example.a.Main")!,
            AndroidComponent(packageName: "com.example.b", activityClass: "com.example.b.Main")!,
        ])
        #expect(answers == [.png(good, hasAlpha: false), .error, .error, .error])
    }

    @Test func aFieldALaterVersionAppendsToTheCountLinesOfAnIconAnswerIsIgnored() {
        let a = png(side: 192)
        var out = Data("kernarium_apps 1\nicons\t1\tmore\n".utf8)
        out.append(record(comet.identifier, data: a))
        out.append(Data("end\t1\tmore\n".utf8))
        #expect(AndroidAppsCommands.parseIcons(ShellResult(stdout: out, exitCode: 0), asked: [comet]) == [.png(a, hasAlpha: false)])
    }

    @Test func everyBrokenFramingOfAnIconAnswerDiscardsAllOfIt() {
        let a = png(side: 192)
        func discards(_ name: String, _ result: ShellResult, asked: [AndroidComponent] = [comet]) {
            #expect(AndroidAppsCommands.parseIcons(result, asked: asked) == nil, "\(name)")
        }
        discards("a count that differs from the asked", iconsAnswer(2, records: [record(comet.identifier, data: a)]))
        discards("an end line that differs", iconsAnswer(1, records: [record(comet.identifier, data: a)], end: 2))
        discards("another version", iconsAnswer(1, records: [record(comet.identifier, data: a)], version: "2"))
        discards("a component out of order", iconsAnswer(1, records: [record(hop.identifier, data: a)]))
        discards("a length longer than the data", iconsAnswer(1, records: [record(comet.identifier, data: a, declaredLength: a.count + 5)]))
        discards("a length shorter than the data", iconsAnswer(1, records: [record(comet.identifier, data: a, declaredLength: a.count - 5)]))
        discards("a length above the bound", iconsAnswer(1, records: [record(comet.identifier, data: a, declaredLength: AndroidAppsCommands.largestPNG + 1)]))
        discards("a side the tool never renders", iconsAnswer(1, records: [record(comet.identifier, side: 100, data: png(side: 100))]))
        discards("a status it does not have", iconsAnswer(1, records: [Data("icon\t\(comet.identifier)\tmaybe\t0\t0\t-\t0\t0\t-\n".utf8)]))
        discards("data after a status with none", iconsAnswer(1, records: [Data("icon\t\(comet.identifier)\tnoicon\t192\t192\t-\t0\t5\t-\n".utf8)]))
        discards("a non-zero exit", ShellResult(stdout: iconsAnswer(1, records: [record(comet.identifier, data: a)]).stdout, exitCode: 1))
        discards("too many bytes", ShellResult(stdout: iconsAnswer(1, records: [record(comet.identifier, data: a)]).stdout + Data(count: AndroidAppsCommands.longestIconAnswer), exitCode: 0))
        var missingLF = iconsAnswer(1, records: [record(comet.identifier, data: a)]).stdout
        missingLF.remove(at: missingLF.count - "end\t1\n".utf8.count - 1)
        discards("no line feed after the data", ShellResult(stdout: missingLF, exitCode: 0))
        discards("bytes after the end line", ShellResult(stdout: iconsAnswer(1, records: [record(comet.identifier, data: a)]).stdout + Data("x".utf8), exitCode: 0))
    }

    @Test func theToolIsAskedForTheIconsOfAtMostEightApps() async {
        let apps = (0..<9).map { AndroidApp(component: AndroidComponent(packageName: "com.example.a\($0)", activityClass: "com.example.Main")!, label: "A", builtIn: false, iconTag: key1) }
        let shell = ScriptedShell(.success(iconsAnswer(8, records: apps.prefix(8).map { record($0.id, status: "noicon") })))
        let answers = await AndroidAppsCommands(shell: shell).icons(of: apps)
        #expect(shell.commands.count == 1)
        #expect(shell.commands[0].split(separator: " ").count == 10)
        #expect(answers.count == 8)
        #expect(answers[apps[0].id] == .noIcon)
        #expect(answers[apps[8].id] == nil)
    }

    @Test func aComponentIsNeverNamedTwiceInOneAsk() async {
        let a = AndroidApp(component: AndroidComponent(packageName: "com.example.a", activityClass: "com.example.Main")!, label: "A", builtIn: false, iconTag: key1)
        let b = AndroidApp(component: AndroidComponent(packageName: "com.example.b", activityClass: "com.example.Main")!, label: "B", builtIn: false, iconTag: key1)
        // The tool exits 64 on a component twice: the host does not send one.
        let shell = ScriptedShell(.success(iconsAnswer(2, records: [a, b].map { record($0.id, status: "noicon") })))
        let answers = await AndroidAppsCommands(shell: shell).icons(of: [a, b, a])
        #expect(shell.commands.count == 1)
        #expect(shell.commands[0].split(separator: " ").count == 4)
        #expect(answers.count == 2)
    }

    @Test func anIconsAnswerWithAnyNonzeroStatusIsDiscardedWhateverItCarries() {
        let a = AndroidComponent(packageName: "com.example.a", activityClass: "com.example.Main")!
        let good = iconsAnswer(1, records: [record(a.identifier, status: "noicon")]).stdout
        for status in [1, 2, 64, 124, 126, 127, 255] {
            #expect(AndroidAppsCommands.parseIcons(ShellResult(stdout: good, exitCode: status), asked: [a]) == nil, "exit \(status)")
        }
    }

    // MARK: Launch

    @Test func aLaunchIsOpenedRefusedOrFailedByItsExitStatus() {
        let launched = shellResult("launched\t\(comet.identifier)\n")
        #expect(AndroidAppsCommands.parseLaunch(launched, component: comet) == .opened)
        #expect(AndroidAppsCommands.parseLaunch(ShellResult(stderr: Data("kernarium_apps: refused: not a launcher activity".utf8), exitCode: 2), component: comet) == .notFound)
        // Exit 1 is a start that failed for a reason the tool does not give; 127 and 64 an image without the tool.
        #expect(AndroidAppsCommands.parseLaunch(ShellResult(exitCode: 1), component: comet) == .startFailed)
        #expect(AndroidAppsCommands.parseLaunch(ShellResult(exitCode: 127), component: comet) == .unavailable)
        #expect(AndroidAppsCommands.parseLaunch(ShellResult(exitCode: 64), component: comet) == .unavailable)
        #expect(AndroidAppsCommands.parseLaunch(ShellResult(exitCode: 255), component: comet) == .failed)
        #expect(AndroidAppsCommands.parseLaunch(ShellResult(exitCode: nil), component: comet) == .failed)
        // Exit 0 with another component's answer, or none, is no proof the one asked for started.
        #expect(AndroidAppsCommands.parseLaunch(shellResult("launched\t\(hop.identifier)\n"), component: comet) == .failed)
        #expect(AndroidAppsCommands.parseLaunch(shellResult(""), component: comet) == .failed)
    }

    @Test func aLaunchAcknowledgementWithoutItsLineFeedIsNoAcknowledgement() {
        #expect(AndroidAppsCommands.parseLaunch(shellResult("launched\t\(comet.identifier)"), component: comet) == .failed)
        #expect(AndroidAppsCommands.parseLaunch(shellResult("launched\t\(comet.identifier)\n\n"), component: comet) == .failed)
        #expect(AndroidAppsCommands.parseLaunch(shellResult("launched\t\(comet.identifier)\r\n"), component: comet) == .failed)
        // A field a later version appends is ignored.
        #expect(AndroidAppsCommands.parseLaunch(shellResult("launched\t\(comet.identifier)\tnew\n"), component: comet) == .opened)
    }

    // MARK: Through a shell

    @Test func aShellThatCannotBeReachedIsAFailureForEveryQuestion() async {
        let commands = AndroidAppsCommands(shell: ScriptedShell(.failure(Unreachable())))
        #expect(await commands.digest() == .failed)
        #expect(await commands.list() == .failed)
        #expect(await commands.open(comet) == .failed)
        #expect(await commands.icons(of: [AndroidApp(component: comet, label: "x", builtIn: false, iconTag: key1)]).isEmpty)
    }

    @Test func eachQuestionSendsItsOwnCommand() async {
        let shell = ScriptedShell(.success(shellResult(listing(rows: []))))
        let commands = AndroidAppsCommands(shell: shell)
        _ = await commands.digest()
        _ = await commands.list()
        _ = await commands.open(comet)
        #expect(shell.commands == ["kernarium_apps digest", "kernarium_apps list", "kernarium_apps launch '\(comet.identifier)'"])
    }
}
