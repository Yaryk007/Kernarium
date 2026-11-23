// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// What the reader takes from the version file, rule by rule. The file is
/// untrusted: a bad entry skips only itself, a file that is no answer is
/// silence, and only the highest build above the installed one is offered.
struct UpdateFileTests {
    private typealias Fixture = UpdateFixture

    // MARK: The choice

    @Test func theHighestBuildAboveTheInstalledOneIsOffered() {
        let data = Fixture.file(apps: [Fixture.app(versions: [
            Fixture.entry(version: "1.1", build: "16"), Fixture.entry(version: "1.0", build: "15"),
        ])])
        let reading = Fixture.read(data, installed: 15)
        guard case .read(let version?) = reading else { Issue.record("no version: \(reading)"); return }
        #expect(version.build == 16)
        #expect(version.version == "1.1")
        #expect(version.label == "1.1\u{A0}(16)")
        #expect(version.sizeBytes == 1_390_000_000)
        #expect(version.released == UpdateDay(year: 2026, month: 11, day: 20))
        #expect(version.minimumOS == UpdateOSVersion(major: 26))
        #expect(version.appFile.absoluteString == "https://example.com/releases/andarium-1.1-16.ipa")
    }

    @Test func aBuildAtOrBelowTheInstalledOneIsNeverOffered() {
        let data = Fixture.file(apps: [Fixture.app(versions: [
            Fixture.entry(version: "1.1", build: "16"), Fixture.entry(version: "1.0", build: "15"),
        ])])
        #expect(Fixture.offered(Fixture.read(data, installed: 16)) == nil)
        #expect(Fixture.read(data, installed: 16) == .read(nil))
        #expect(Fixture.read(data, installed: 99) == .read(nil))
    }

    @Test func theFileListedFirstIsNotTheOneChosenUnlessItIsTheHighest() {
        let oldestFirst = Fixture.file(apps: [Fixture.app(versions: [
            Fixture.entry(version: "0.9", build: "14"), Fixture.entry(version: "1.0", build: "15"),
            Fixture.entry(version: "1.2", build: "17"), Fixture.entry(version: "1.1", build: "16"),
        ])])
        #expect(Fixture.offered(Fixture.read(oldestFirst, installed: 10)) == 17)
        #expect(Fixture.offered(Fixture.read(oldestFirst, installed: 16)) == 17)
    }

    @Test func buildsCompareAsNumbersNotAsText() {
        let data = Fixture.file(apps: [Fixture.app(versions: [
            Fixture.entry(version: "1.0", build: "9"), Fixture.entry(version: "1.1", build: "10"),
        ])])
        #expect(Fixture.offered(Fixture.read(data, installed: 8)) == 10)
        #expect(Fixture.offered(Fixture.read(data, installed: 9)) == 10)
        #expect(Fixture.offered(Fixture.read(data, installed: 10)) == nil)
    }

    @Test func aVersionThatNeedsANewerIOSIsNotOfferedAndTheNextOneIs() {
        let data = Fixture.file(apps: [Fixture.app(versions: [
            Fixture.entry(version: "2.0", build: "17", minOS: "\"27.0\""),
            Fixture.entry(version: "1.1", build: "16", minOS: "\"26.0\""),
        ])])
        #expect(Fixture.offered(Fixture.read(data, installed: 15, os: UpdateOSVersion(major: 26, minor: 5))) == 16)
        #expect(Fixture.offered(Fixture.read(data, installed: 15, os: UpdateOSVersion(major: 27))) == 17)
        let only = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "17", minOS: "\"27.0\"")])])
        #expect(Fixture.read(only, installed: 15) == .read(nil))
    }

    @Test func theMinimumIOSIsComparedAsDottedNumbers() {
        let data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "16", minOS: "\"26.10\"")])])
        #expect(Fixture.read(data, os: UpdateOSVersion(major: 26, minor: 2, patch: 1)) == .read(nil))
        #expect(Fixture.offered(Fixture.read(data, os: UpdateOSVersion(major: 26, minor: 10))) == 16)
        #expect(Fixture.offered(Fixture.read(data, os: UpdateOSVersion(major: 26, minor: 11))) == 16)
    }

    @Test func aVersionThatStopsAtAnOlderIOSIsNotOffered() {
        let data = Fixture.file(apps: [Fixture.app(versions: [
            Fixture.entry(build: "16", minOS: "\"26.0\"", extra: "\"maxOSVersion\": \"26.4\""),
        ])])
        #expect(Fixture.offered(Fixture.read(data, os: UpdateOSVersion(major: 26, minor: 4))) == 16)
        #expect(Fixture.read(data, os: UpdateOSVersion(major: 26, minor: 5)) == .read(nil))
    }

    @Test func aVersionWithNoMinimumRunsAnywhere() {
        let data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "16", minOS: nil)])])
        #expect(Fixture.offered(Fixture.read(data, os: UpdateOSVersion(major: 18))) == 16)
    }

    // MARK: Other applications

    @Test func theEntriesOfOtherApplicationsAreSkipped() {
        let data = Fixture.file(apps: [
            Fixture.app("com.example.kernarium", versions: [Fixture.entry(version: "0.4", build: "99")]),
            Fixture.app(versions: [Fixture.entry(version: "1.1", build: "16")]),
        ])
        #expect(Fixture.offered(Fixture.read(data, installed: 15)) == 16)
    }

    @Test func aFileThatDoesNotListTheApplicationMeansNoUpdate() {
        let data = Fixture.file(apps: [Fixture.app("com.example.kernarium", versions: [Fixture.entry(build: "99")])])
        #expect(Fixture.read(data) == .read(nil))
        #expect(Fixture.read(Fixture.file(apps: [])) == .read(nil))
        #expect(Fixture.read(Data("{}".utf8)) == .read(nil))
    }

    @Test func theIdentifierIsMatchedExactlyAndNotAsAPrefixOrAConfusion() {
        for other in ["com.example.andarium.ABCDE12345", "com.example.Andarium", "com.example.andariu", " com.example.andarium"] {
            let data = Fixture.file(apps: [Fixture.app(other, versions: [Fixture.entry(build: "16")])])
            #expect(Fixture.read(data) == .read(nil), "\(other)")
        }
    }

    @Test func aBrokenEntryOfAnotherApplicationDoesNotSpoilThisOne() {
        let data = Fixture.file(apps: [
            "{ \"bundleIdentifier\": 42, \"versions\": \"x\" }",
            "{ \"name\": \"no identifier\" }",
            "\"just a string\"",
            Fixture.app("com.example.kernarium", versions: ["{ \"junk\": true }", "7"]),
            Fixture.app(versions: [Fixture.entry(build: "16")]),
        ])
        #expect(Fixture.offered(Fixture.read(data)) == 16)
    }

    @Test func twoEntriesOfTheApplicationAreBothConsidered() {
        let data = Fixture.file(apps: [
            Fixture.app(versions: [Fixture.entry(version: "1.1", build: "16")]),
            Fixture.app(versions: [Fixture.entry(version: "1.2", build: "17")]),
        ])
        #expect(Fixture.offered(Fixture.read(data)) == 17)
    }

    // MARK: Unknown fields

    @Test func unknownFieldsAreIgnoredEverywhere() {
        let data = Fixture.file(
            apps: [Fixture.app(
                versions: [Fixture.entry(build: "16", extra: "\"sha256\": \"abc\", \"future\": { \"x\": [1, 2] }")],
                extra: "\"tintColor\": \"#1A7C78\", \"appPermissions\": { \"entitlements\": [], \"privacy\": {} }, \"future\": null")],
            block: "{ \"format\": 1, \"future\": true, \"apps\": { \"com.example.andarium\": { \"future\": 1, \"versions\": { \"16\": { \"future\": 2 } } } } }",
            extra: "\"news\": [], \"somethingNew\": [1, 2, 3]")
        #expect(Fixture.offered(Fixture.read(data)) == 16)
    }

    @Test func aPlainAltStoreSourceWithoutOurBlockWorks() {
        let data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "16")])])
        guard case .read(let version?) = Fixture.read(data) else { Issue.record("no version"); return }
        #expect(version.build == 16)
        #expect(version.notes == nil)
    }

    // MARK: A bad entry skips only itself

    /// Each fault, in an entry that would otherwise be the highest build.
    private static let faults: [(String, String)] = [
        ("no build", "{ \"version\": \"1.2\", \"date\": \"2026-11-21\", \"downloadURL\": \"https://example.com/a.ipa\", \"size\": 1 }"),
        ("build 0", UpdateFixture.entry(version: "1.2", build: "0")),
        ("build negative", UpdateFixture.entry(version: "1.2", build: "-3")),
        ("build with a dot", UpdateFixture.entry(version: "1.2", build: "17.5")),
        ("build in words", UpdateFixture.entry(version: "1.2", build: "seventeen")),
        ("build empty", UpdateFixture.entry(version: "1.2", build: "")),
        ("build too long", UpdateFixture.entry(version: "1.2", build: "1000000000")),
        ("build a number", UpdateFixture.entry(version: "1.2", build: "#17")),
        ("date in words", UpdateFixture.entry(version: "1.2", build: "17", date: "tomorrow")),
        ("date not a day", UpdateFixture.entry(version: "1.2", build: "17", date: "2026-02-30")),
        ("date with a month 13", UpdateFixture.entry(version: "1.2", build: "17", date: "2026-13-01")),
        ("date without dashes", UpdateFixture.entry(version: "1.2", build: "17", date: "20261121")),
        ("minimum not a version", UpdateFixture.entry(version: "1.2", build: "17", minOS: "\"latest\"")),
        ("minimum with four numbers", UpdateFixture.entry(version: "1.2", build: "17", minOS: "\"26.0.0.1\"")),
        ("minimum a number", UpdateFixture.entry(version: "1.2", build: "17", minOS: "26")),
        ("maximum not a version", UpdateFixture.entry(version: "1.2", build: "17", extra: "\"maxOSVersion\": \"x\"")),
        ("size a string", UpdateFixture.entry(version: "1.2", build: "17", size: "\"big\"")),
        ("size a fraction", UpdateFixture.entry(version: "1.2", build: "17", size: "1.5")),
        ("size zero", UpdateFixture.entry(version: "1.2", build: "17", size: "0")),
        ("size negative", UpdateFixture.entry(version: "1.2", build: "17", size: "-5")),
        ("size over a tebibyte", UpdateFixture.entry(version: "1.2", build: "17", size: "1099511627777")),
        ("version too long", UpdateFixture.entry(version: "1.2.3.4.5.6.7.8.9", build: "17")),
        ("version with a space", UpdateFixture.entry(version: "1.2 beta", build: "17")),
        ("version with a symbol", UpdateFixture.entry(version: "1.2-beta", build: "17")),
        ("version empty", UpdateFixture.entry(version: "", build: "17")),
        ("link missing", "{ \"version\": \"1.2\", \"buildVersion\": \"17\", \"date\": \"2026-11-21\", \"size\": 1 }"),
        ("link http", UpdateFixture.entry(version: "1.2", build: "17", url: "http://example.com/a.ipa")),
        ("link on another host", UpdateFixture.entry(version: "1.2", build: "17", url: "https://evil.example.net/a.ipa")),
        ("link a scheme of an app", UpdateFixture.entry(version: "1.2", build: "17", url: "altstore://install?url=https://example.com/a.ipa")),
        ("link a number", "{ \"version\": \"1.2\", \"buildVersion\": \"17\", \"date\": \"2026-11-21\", \"downloadURL\": 5, \"size\": 1 }"),
        ("an array in place of an entry", "[1, 2]"),
        ("a number in place of an entry", "17"),
        ("null in place of an entry", "null"),
    ]

    @Test(arguments: UpdateFileTests.faults.map(\.0))
    func aBadEntrySkipsOnlyItself(fault: String) throws {
        let bad = try #require(Self.faults.first { $0.0 == fault }).1
        let data = Fixture.file(apps: [Fixture.app(versions: [bad, Fixture.entry(version: "1.1", build: "16")])])
        #expect(Fixture.offered(Fixture.read(data, installed: 15)) == 16, "\(fault)")
        // And alone it offers nothing, and is no malformed file.
        let alone = Fixture.file(apps: [Fixture.app(versions: [bad])])
        #expect(Fixture.read(alone, installed: 15) == .read(nil), "\(fault)")
    }

    @Test func aBadEntryAfterTheGoodOneChangesNothing() {
        let data = Fixture.file(apps: [Fixture.app(versions: [
            Fixture.entry(version: "1.1", build: "16"), "{ \"junk\": 1 }", "[]", "\"x\"",
        ])])
        #expect(Fixture.offered(Fixture.read(data)) == 16)
    }

    // MARK: A file that is no answer

    @Test func aFileThatIsNotReadableIsNoAnswer() {
        let good = String(decoding: Fixture.file(apps: [Fixture.app(versions: [Fixture.entry()])]), as: UTF8.self)
        let cases: [(String, Data)] = [
            ("empty", Data()),
            ("not json", Data("<html>404</html>".utf8)),
            ("truncated", Data(good.prefix(good.count / 2).utf8)),
            ("an array", Data("[]".utf8)),
            ("a string", Data("\"hello\"".utf8)),
            ("a number", Data("7".utf8)),
            ("null", Data("null".utf8)),
            ("apps a string", Data("{ \"apps\": \"none\" }".utf8)),
            ("apps an object", Data("{ \"apps\": {} }".utf8)),
            ("binary", Data([0, 1, 2, 255, 254])),
        ]
        for (name, data) in cases {
            #expect(Fixture.read(data) == .unreadable, "\(name)")
        }
    }

    /// The reader decodes plain JSON only. Foundation's JSON5 mode traps on a
    /// byte that is no UTF-8 where a key is expected, which no handler can
    /// catch, so the reader neither turns that mode on nor passes such a body
    /// on to the decoder.
    @Test func aByteThatIsNoUTF8WhereAKeyIsExpectedIsNoAnswerAndNoCrash() {
        let bad = Data([0xFF])
        func data(_ head: String, _ tail: String) -> Data { Data(head.utf8) + bad + Data(tail.utf8) }
        let cases: [(String, Data)] = [
            ("the short form", data("{", "\"apps\": []}")),
            ("after a member", data("{\"a\": 1,", "\"apps\": []}")),
            ("after a comma in the block", data("{\"apps\": [], \"kernariumUpdates\": {\"format\": 1,", "\"apps\": {}}}")),
            ("a lone byte", bad),
            ("a truncated sequence", Data([0x7B, 0xE2, 0x82])),
            ("an overlong sequence", Data([0x7B, 0xC0, 0xAF, 0x7D])),
            ("a surrogate", Data([0x7B, 0xED, 0xA0, 0x80, 0x7D])),
        ]
        for (name, input) in cases {
            #expect(Fixture.read(input) == .unreadable, "\(name)")
        }
    }

    /// The input that trapped Foundation's JSON5 decoder in review: a source
    /// file with one 0xFF where a key of its block is expected.
    private static let crashInput = Data(
        base64Encoded:
        "eyAibmFtZSI6ICJQbGFjZWhvbGRlciBBcHBzIiwgImlkZW50aWZpZXIiOiAiY29tLmV4YW1wbGUuc291cmNlIiwgImFwcHMi" +
        "OiBbeyAibmFtZSI6ICJQbGFjZWhvbGRlciIsICJidW5kbGVJZGVudGlmaWVyIjogImNvbS5leGFtcGxlLmFuZGFyaXVtIiwg" +
        "ImRldmVsb3Blck5hbWUiOiAiRXhhbXBsZSBEZXZlbG9wZXIiLAogICJsb2NhbGl6ZWREZXNjcmlwdGlvbiI6ICJOZXZlciBz" +
        "aG93bi4iLCAiaWNvblVSTCI6ICJodHRwczovL2V4YW1wbGUuY29tL2ljb24ucG5nIiwKICAidmVyc2lvbnMiOiBbeyAidmVz" +
        "aW9uIjogIjEuMSIsICJidWlsZFZlcnNpb24iOiAiMTYiLCAiZGF0ZSI6ICIyMDI2LTExLTIwIiwgImRvd25sb2FkVVJMIjog" +
        "Imh0dHBzOi8vZXhhbXBsZS5jb20vcmVsZWFzZXMvYW5kYXJpdW0tMS4xLTE2LmlwYSIsICJzaXplIjogMTM5MDAwMDAwMCwg" +
        "Im1pbk9TVmVyc2lvbiI6ICIyNi4wIiB9LCB7ICJ2ZXJzaW9uIjogIjEuMiIsICJidWlsZFZlcnNpb24iOiAiMTciLCAiZGF0" +
        "ZSI6ICIyMDI2LTExLTIxVDA5OjMwOjAwLjUrMDg6MDAiLCAiZG93bmxvYWRVUkwiOiAiaHR0cHM6Ly9leGFtcGxlLmNvbS9y" +
        "ZWxlYXNlcy9hbmRhcml1bS0xLjEtMTYuaXBhIiwgInNpemUiOiAxMzkwMDAwMDAwLCAibWluT1NWZXJzaW9uIjogIjI2LjAi" +
        "IH0sIHsgInZlcnNpb24iOiAiMC45IiwgImJ1aWxkVmVyc2lvbiI6ICIxNCIsICJkYXRlIjogIjIwMjYtMTEtMjAiLCAiZG93" +
        "bmxvYWRVUkwiOiAiaHR0cHM6Ly9leGFtcGxlLmNvbS9yZWxlYXNlcy9hbmRhcml1bS0xLjEtMTYuaXBhIiwgInNpemUiOiAx" +
        "MzkwMDAwMDAwLCAibWluT1NWZXJzaW9uIjogIjI2LjAuMSIsICJtYXhPU1ZlcnNpb24iOiAiMjciIH1dIH1dLCAia2VybmFy" +
        "aXVtVXBkYXRlcyI6IHsgImZvcm1hdCI6IDEsICJhcHBzIjogeyAiY29tLmV4YW1wbGUuYW5kYXJpdW0iOiB7/yAidmVyc2lv" +
        "bnMiOiB7ICIxNyI6IHsgIm5vdGVzVVJMIjogImh0dHBzOi8vZXhhbXBsZS5jb20vbiIgfSB9IH0gfSB9IH0=")!

    @Test func theInputThatTrappedTheJSON5ReaderIsNoAnswer() {
        #expect(Self.crashInput.contains(0xFF))
        #expect(Fixture.read(Self.crashInput, installed: 15) == .unreadable)
    }

    /// The reviewer's input, rebuilt: a good file whose block holds one 0xFF
    /// where the application's identifier is followed by its first key.
    @Test func aGoodFileWithOneBadByteInItsBlockIsNoAnswer() {
        let block = "{ \"format\": 1, \"apps\": { \"com.example.andarium\": {"
            + " \"versions\": { \"16\": { \"notesURL\": \"https://example.com/releases/andarium-1.1\" } } } } }"
        let good = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "16")])], block: block)
        #expect(Fixture.offered(Fixture.read(good)) == 16)
        let marker = Data("\"com.example.andarium\": {".utf8)
        guard let range = good.range(of: marker, options: .backwards) else { Issue.record("no marker"); return }
        var broken = good
        broken.insert(0xFF, at: range.upperBound)
        #expect(Fixture.read(broken) == .unreadable)
    }

    /// The file's rules say it is plain JSON: a comment, an unquoted key, single
    /// quotes and a hexadecimal number are JSON5, and the owner publishes plain JSON.
    @Test func aFileThatIsJSON5AndNotJSONIsNoAnswer() {
        let json5: [(String, String)] = [
            ("a line comment", "{ // none\n \"apps\": [] }"),
            ("an unquoted key", "{ apps: [] }"),
            ("single quotes", "{ 'apps': [] }"),
            ("hexadecimal", "{ \"apps\": [], \"n\": 0x10 }"),
        ]
        for (name, text) in json5 {
            #expect(Fixture.read(Data(text.utf8)) == .unreadable, "\(name)")
        }
    }

    @Test func aBodyOverTheLimitIsNoAnswerEvenIfItIsAGoodFile() {
        let padding = String(repeating: " ", count: UpdateFile.largestBytes)
        var data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry()])])
        #expect(Fixture.offered(Fixture.read(data)) == 16)
        data.append(Data(padding.utf8))
        #expect(data.count > UpdateFile.largestBytes)
        #expect(Fixture.read(data) == .unreadable)
    }

    @Test func aBodyExactlyAtTheLimitIsRead() {
        var data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry()])])
        data.append(Data(repeating: 0x20, count: UpdateFile.largestBytes - data.count))
        #expect(data.count == UpdateFile.largestBytes)
        #expect(Fixture.offered(Fixture.read(data)) == 16)
    }

    // MARK: The block of our own

    private static let notes16 = "{ \"format\": 1, \"apps\": { \"com.example.andarium\": { \"versions\": { \"16\": { \"notesURL\": \"https://example.com/releases/andarium-1.1\" } } } } }"

    @Test func theBlockGivesTheReleasePageOfTheBuild() {
        let data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "16")])], block: Self.notes16)
        guard case .read(let version?) = Fixture.read(data) else { Issue.record("no version"); return }
        #expect(version.notes?.absoluteString == "https://example.com/releases/andarium-1.1")
    }

    @Test func aPageForAnotherBuildOrAnotherAppIsNotUsed() {
        let data = Fixture.file(
            apps: [Fixture.app(versions: [Fixture.entry(build: "17")])],
            block: "{ \"format\": 1, \"apps\": { \"com.example.andarium\": { \"versions\": { \"16\": { \"notesURL\": \"https://example.com/a\" } } }, \"com.example.kernarium\": { \"versions\": { \"17\": { \"notesURL\": \"https://example.com/b\" } } } } }")
        guard case .read(let version?) = Fixture.read(data) else { Issue.record("no version"); return }
        #expect(version.build == 17)
        #expect(version.notes == nil)
    }

    @Test func aBlockOfAnotherFormatIsNoAnswer() {
        let apps = [Fixture.app(versions: [Fixture.entry(build: "16")])]
        for format in ["2", "0", "-1", "1.5", "\"1\"", "null", "true", "[1]"] {
            let data = Fixture.file(apps: apps, block: "{ \"format\": \(format) }")
            #expect(Fixture.read(data) == .unreadable, "format \(format)")
        }
        #expect(Fixture.read(Fixture.file(apps: apps, block: "{ \"apps\": {} }")) == .unreadable)
        #expect(Fixture.read(Fixture.file(apps: apps, block: "[1]")) == .unreadable)
        #expect(Fixture.read(Fixture.file(apps: apps, block: "\"1\"")) == .unreadable)
        #expect(Fixture.read(Fixture.file(apps: apps, block: "null")) == .unreadable)
    }

    @Test func aMissingBlockIsFormatOneWithNoPages() {
        let data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "16")])])
        #expect(Fixture.offered(Fixture.read(data)) == 16)
    }

    @Test func aReleasePageThatFailsTheLinkRulesCostsOnlyItsRow() {
        for page in ["http://example.com/r", "https://evil.example.net/r", "javascript:alert(1)", "https://user@example.com/r", "7", "null"] {
            let block = "{ \"format\": 1, \"apps\": { \"com.example.andarium\": { \"versions\": { \"16\": { \"notesURL\": \(page.hasPrefix("7") || page == "null" ? page : "\"\(page)\"") } } } } }"
            let data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "16")])], block: block)
            guard case .read(let version?) = Fixture.read(data) else { Issue.record("no version for \(page)"); continue }
            #expect(version.build == 16, "\(page)")
            #expect(version.notes == nil, "\(page)")
        }
    }

    @Test func aBlockWhosePagesAreMalformedLosesThePagesAndNotTheFile() {
        for pages in ["\"apps\": 5", "\"apps\": [1]", "\"apps\": { \"com.example.andarium\": 3 }",
            "\"apps\": { \"com.example.andarium\": { \"versions\": [1] } }"] {
            let data = Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(build: "16")])], block: "{ \"format\": 1, \(pages) }")
            #expect(Fixture.offered(Fixture.read(data)) == 16, "\(pages)")
        }
    }

    @Test func aPageIsMatchedByTheBuildAsANumber() {
        let data = Fixture.file(
            apps: [Fixture.app(versions: [Fixture.entry(build: "016")])],
            block: "{ \"format\": 1, \"apps\": { \"com.example.andarium\": { \"versions\": { \"16\": { \"notesURL\": \"https://example.com/x\" } } } } }")
        guard case .read(let version?) = Fixture.read(data) else { Issue.record("no version"); return }
        #expect(version.build == 16)
        #expect(version.notes?.absoluteString == "https://example.com/x")
    }

    // MARK: Links

    @Test func theAppFileMustBeOnTheFilesHostOrOnAHostTheBuildNames() {
        let data = Fixture.file(apps: [Fixture.app(versions: [
            Fixture.entry(build: "16", url: "https://downloads.example.org/a.ipa"),
        ])])
        #expect(Fixture.read(data) == .read(nil))
        let named = UpdateLinkPolicy(sourceHost: "example.com", hosts: [UpdateHostRule("downloads.example.org")!])
        #expect(Fixture.offered(Fixture.read(data, links: named)) == 16)
        let wildcard = UpdateLinkPolicy(sourceHost: "example.com", hosts: [UpdateHostRule("*.example.org")!])
        #expect(Fixture.offered(Fixture.read(data, links: wildcard)) == 16)
    }

    @Test func theFileCannotWidenTheListOfHosts() {
        let data = Fixture.file(
            apps: [Fixture.app(versions: [Fixture.entry(build: "16", url: "https://cdn.example.net/a.ipa")],
                extra: "\"linkHosts\": [\"cdn.example.net\"]")],
            block: "{ \"format\": 1, \"linkHosts\": [\"cdn.example.net\"], \"hosts\": [\"cdn.example.net\"] }",
            extra: "\"hosts\": [\"cdn.example.net\"], \"allowedHosts\": [\"cdn.example.net\"]")
        #expect(Fixture.read(data) == .read(nil))
    }
}
