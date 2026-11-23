// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing

/// Identifiers in sequence, for a `Retries` that must not draw at random.
private final class Counter: @unchecked Sendable {
    private var value = 0

    func next() -> String {
        value += 1
        return String(format: "%032x", value)
    }
}

struct GuestSmsTests {
    private static let identifier = "0123456789abcdef0123456789abcdef"

    @Test func sendersAreNumbersOrShortNames() {
        #expect(GuestSms.sender("+98 (912) 123-4567") == "+989121234567")
        #expect(GuestSms.sender("12345") == "12345")
        #expect(GuestSms.sender(" My Bank ") == "My Bank")
        #expect(GuestSms.sender("Twelve Chars") == nil)
        #expect(GuestSms.sender("Bänk") == nil)
        #expect(GuestSms.sender("+123456789012345678901") == nil)
        #expect(GuestSms.sender("") == nil)
    }

    /// The service states the same numbers once (Message.MAXIMUM_UNITS and MAXIMUM_PARTS), and its own test
    /// builds the text that takes the most parts.
    @Test func theLimitsAreTheServices() {
        #expect(GuestSms.maximumUnits == 670)
        #expect(GuestSms.maximumParts == 11)
        // Every part but the last takes at least 65 units of the text.
        #expect((GuestSms.maximumParts - 1) * 65 + 1 <= GuestSms.maximumUnits)
        #expect(GuestSms.maximumParts * 65 + 1 > GuestSms.maximumUnits)
    }

    /// The words say these numbers, so they are the rule's.
    @Test func theSenderLimitsAreTheRules() {
        #expect(GuestSms.maximumNameLength == 11)
        #expect(GuestSms.maximumNumberDigits == 20)
        #expect(GuestSms.sender(String(repeating: "a", count: GuestSms.maximumNameLength)) != nil)
        #expect(GuestSms.sender(String(repeating: "a", count: GuestSms.maximumNameLength + 1)) == nil)
        #expect(GuestSms.sender(String(repeating: "1", count: GuestSms.maximumNumberDigits)) != nil)
        #expect(GuestSms.sender("+" + String(repeating: "1", count: GuestSms.maximumNumberDigits)) != nil)
        #expect(GuestSms.sender(String(repeating: "1", count: GuestSms.maximumNumberDigits + 1)) == nil)
    }

    /// What a number pasted from a web page or a message brings along is made plain before it is judged.
    @Test func aPastedSenderIsCleanedBeforeItIsJudged() {
        // Direction marks around it, no-break spaces and typographic hyphens inside it.
        #expect(GuestSms.sender("\u{200E}+98\u{00A0}912\u{2011}123\u{2013}4567\u{200F}") == "+989121234567")
        #expect(GuestSms.sender("\u{202A}+1 (555) 010\u{2212}0100\u{202C}") == "+15550100100")
        // Every hyphen and dash of U+2010 to U+2013 and the minus sign is a hyphen.
        #expect(GuestSms.cleaned("1\u{2010}2\u{2011}3\u{2012}4\u{2013}5\u{2212}6") == "1-2-3-4-5-6")
        // Full-width digits and plus.
        #expect(GuestSms.sender("\u{FF0B}\u{FF18}\u{FF16}\u{FF11}\u{FF12}\u{FF13}\u{FF14}") == "+861234")
        #expect(GuestSms.sender("\u{FF11}\u{FF12}\u{FF13}") == "123")
        // Full-width letters are the plain letters.
        #expect(GuestSms.sender("\u{FF22}ank") == "Bank")
        // A name keeps its plain letters and its single spaces; the marks around it go.
        #expect(GuestSms.sender("\u{200F}My\u{00A0}Bank\u{200E}") == "My Bank")
        // What no rule makes plain stays refused.
        #expect(GuestSms.sender("Ba\u{200B}nk") == nil)
        #expect(GuestSms.sender("Bank\u{200D}") == nil)
        #expect(GuestSms.sender("Ban\u{0301}k") == nil)
        #expect(GuestSms.sender("\u{0661}\u{0662}\u{0663}") == nil)
        // The refusal judges the cleaned sender, and says what is wrong with it.
        #expect(GuestSms.refusal(sender: "\u{200E}\u{00A0}", text: "hi") == .noSender)
        #expect(GuestSms.refusal(sender: "\u{200E}+98\u{00A0}912\u{2011}123\u{2013}4567", text: "hi") == nil)
    }

    /// A number typed on a Chinese or Japanese keyboard comes out in full-width forms and, on a kana
    /// keyboard, with the prolonged sound mark for a hyphen: it is the number its ASCII form is.
    @Test func aNumberTypedOnACjkKeyboardIsTheSameNumberAsItsAsciiForm() {
        let pairs: [(typed: String, ascii: String)] = [
            ("\u{FF10}\u{FF19}\u{FF10}\u{30FC}\u{FF11}\u{FF12}\u{FF13}\u{FF14}\u{30FC}\u{FF15}\u{FF16}\u{FF17}\u{FF18}", "090-1234-5678"),
            ("\u{FF0B}\u{FF18}\u{FF11}\u{3000}\u{FF19}\u{FF10}\u{FF08}\u{FF11}\u{FF12}\u{FF13}\u{FF14}\u{FF09}\u{FF15}\u{FF16}\u{FF17}\u{FF18}", "+81 90(1234)5678"),
            ("\u{FF0B}\u{FF18}\u{FF16}\u{FF0D}\u{FF11}\u{FF12}\u{FF0E}\u{FF13}\u{FF14}", "+86-12.34"),
            ("\u{FF11}\u{FF12}\u{FF13}\u{FF0D}\u{FF14}\u{FF15}\u{FF16}", "123-456"),
        ]
        for (typed, ascii) in pairs {
            let sender = GuestSms.sender(ascii)
            #expect(sender != nil, "\(ascii)")
            #expect(GuestSms.sender(typed) == sender, "\(typed.debugDescription)")
            #expect(GuestSms.refusal(sender: typed, text: "hi") == nil, "\(typed.debugDescription)")
        }
        #expect(GuestSms.sender("\u{FF10}\u{FF19}\u{FF10}\u{30FC}\u{FF11}\u{FF12}\u{FF13}\u{FF14}\u{30FC}\u{FF15}\u{FF16}\u{FF17}\u{FF18}") == "09012345678")
        // The mark is a hyphen only between two digits.
        for typed in ["\u{30FC}", "1\u{30FC}", "\u{30FC}1", "Ab\u{30FC}12", "1\u{30FC}\u{30FC}2", "\u{30FC}A", "\u{3042}\u{30FC}"] {
            #expect(GuestSms.sender(typed) == nil, "\(typed.debugDescription)")
        }
        // A name typed plainly is unchanged.
        #expect(GuestSms.sender("My Bank 12") == "My Bank 12")
        #expect(GuestSms.cleaned("My Bank 12") == "My Bank 12")
    }

    /// Each limit is stated once, as a constant, and the check and the words follow it.
    @Test func theLimitsOfASenderAreTheConstantsAtTheirEdges() {
        let name = GuestSms.maximumNameLength, digits = GuestSms.maximumNumberDigits
        #expect(GuestSms.senderRefusal(String(repeating: "x", count: name)) == nil)
        #expect(GuestSms.senderRefusal(String(repeating: "x", count: name + 1)) == .senderLongName)
        #expect(GuestSms.senderRefusal(String(repeating: "9", count: digits)) == nil)
        #expect(GuestSms.senderRefusal(String(repeating: "9", count: digits + 1)) == .senderLongNumber)
        // A name of digits and spaces counts its characters, not the digits it has.
        #expect(GuestSms.senderRefusal("1 2 3 4 5 6") == nil)
    }

    @Test func aSenderIsRefusedForTheRightReason() {
        #expect(GuestSms.senderRefusal("") == .noSender)
        #expect(GuestSms.senderRefusal("   ") == .noSender)
        #expect(GuestSms.senderRefusal("Shop & Go") == .senderCharacters)
        #expect(GuestSms.senderRefusal("B\u{00E4}nk") == .senderCharacters)
        #expect(GuestSms.senderRefusal("+") == .senderCharacters)
        #expect(GuestSms.senderRefusal("12+34") == .senderCharacters)
        #expect(GuestSms.senderRefusal("Twelve Chars") == .senderLongName)
        #expect(GuestSms.senderRefusal("123456789012 name") == .senderLongName)
        #expect(GuestSms.senderRefusal(String(repeating: "1", count: 21)) == .senderLongNumber)
        #expect(GuestSms.senderRefusal("+" + String(repeating: "1", count: 21)) == .senderLongNumber)
        #expect(GuestSms.senderRefusal("1234 5678 9012 3456 7890 1") == .senderLongNumber)
        #expect(GuestSms.senderRefusal("Bank") == nil)
        #expect(GuestSms.senderRefusal("+1 (555) 010-0100") == nil)
        #expect(GuestSms.textRefusal("") == .noText)
        #expect(GuestSms.textRefusal("hi") == nil)
        // The sender is judged before the text.
        #expect(GuestSms.refusal(sender: "", text: "") == .noSender)
        #expect(GuestSms.defaultSender == "Andarium")
        #expect(GuestSms.senderRefusal(GuestSms.defaultSender) == nil)
    }

    @Test func refusalsNameWhatIsWrong() {
        #expect(GuestSms.refusal(sender: " ", text: "hi") == .noSender)
        #expect(GuestSms.refusal(sender: "a@b", text: "hi") == .senderCharacters)
        #expect(GuestSms.refusal(sender: "Bank", text: "") == .noText)
        #expect(GuestSms.refusal(sender: "Bank", text: String(repeating: "x", count: 671)) == .tooLong)
        // An emoji is two UTF-16 units, as the guest counts them.
        #expect(GuestSms.refusal(sender: "Bank", text: String(repeating: "😀", count: 336)) == .tooLong)
        #expect(GuestSms.refusal(sender: "Bank", text: String(repeating: "x", count: 670)) == nil)
    }

    /// Android decodes each part as UTF-16 with a byte order mark: U+FEFF travels (the service doubles it
    /// at a part's start) and U+FFFE cannot, anywhere in the text.
    @Test func theReversedMarkIsRefusedAndTheMarkIsNot() {
        #expect(GuestSms.refusal(sender: "Bank", text: "\u{FFFE}") == .unsendableCharacter)
        #expect(GuestSms.refusal(sender: "Bank", text: "ab\u{FFFE}cd") == .unsendableCharacter)
        #expect(GuestSms.refusal(sender: "Bank", text: String(repeating: "x", count: 300) + "\u{FFFE}") == .unsendableCharacter)
        #expect(GuestSmsRequest(sender: "Bank", text: "a\u{FFFE}", identifier: Self.identifier, attempt: .first) == nil)
        #expect(GuestSms.refusal(sender: "Bank", text: "\u{FEFF}abc") == nil)
        #expect(GuestSms.refusal(sender: "Bank", text: "\u{FEFF}") == nil)
        #expect(GuestSms.refusal(sender: "Bank", text: "ab\u{FEFF}") == nil)
        #expect(GuestSms.refusal(sender: "Bank", text: "\u{FFFF}") == nil)
    }

    /// A sender is a number or a short name: nothing else becomes one, whatever it would mean to a shell.
    /// What cleans to a valid sender is listed with the exact value it is sent as; every other input is
    /// refused, and the row asserts that for each of them whether or not the validator agrees.
    @Test func hostileSendersAreRefusedAndNeverBuildARequest() throws {
        let cleaned: [(typed: String, sent: String)] = [
            ("\u{202E}Bank\u{200E}", "Bank"), ("Bank\u{202E}", "Bank"), ("Bank\u{200B}", "Bank"),
            ("\u{FF21}\u{FF22}", "AB"), ("\u{FF0B}\u{FF11}", "+1"),
        ]
        for (typed, sent) in cleaned {
            #expect(GuestSms.sender(typed) == sent, "\(typed.debugDescription)")
            #expect(GuestSms.senderRefusal(typed) == nil, "\(typed.debugDescription)")
            let request = try #require(GuestSmsRequest(sender: typed, text: "hi", identifier: Self.identifier, attempt: .first))
            #expect(request.sender == sent, "\(typed.debugDescription)")
            #expect(request == GuestSmsRequest(sender: sent, text: "hi", identifier: Self.identifier, attempt: .first), "\(typed.debugDescription)")
        }
        let refused = [
            "'; id; '", "$(id)", "`id`", "a\nb", "Bank\u{0}", "a|b", "a&b", "\"Bank\"", "a;b", "a b\\", "../x", "a/b",
            "Ba\u{200B}nk", "\u{0661}\u{0662}", "+", "++1", "1+", "1\u{0}2", "a\tb", "😀",
            "\u{FF07}\u{FF1B}id\u{FF1B}\u{FF07}", "\u{FF04}\u{FF08}id\u{FF09}", "\u{FF5C}", "\u{FF1B}",
        ]
        for sender in refused {
            #expect(GuestSms.sender(sender) == nil, "\(sender.debugDescription)")
            #expect(GuestSms.senderRefusal(sender) != nil, "\(sender.debugDescription)")
            #expect(GuestSmsRequest(sender: sender, text: "hi", identifier: Self.identifier, attempt: .first) == nil, "\(sender.debugDescription)")
        }
    }

    /// The edges of what Android takes, in the units the guest counts: the text that takes the most parts, an
    /// emoji's two units, and the one unit over.
    @Test func theTextLimitIsCountedInUtf16Units() {
        func refusal(_ text: String) -> GuestSms.Refusal? { GuestSms.refusal(sender: "Bank", text: text) }
        #expect(refusal(String(repeating: "x", count: GuestSms.maximumUnits)) == nil)
        #expect(refusal(String(repeating: "x", count: GuestSms.maximumUnits + 1)) == .tooLong)
        #expect(refusal(String(repeating: "😀", count: GuestSms.maximumUnits / 2)) == nil)
        #expect(refusal(String(repeating: "😀", count: GuestSms.maximumUnits / 2) + "x") == .tooLong)
        // A combining sequence is as many units as it has scalars.
        #expect(refusal(String(repeating: "e\u{0301}", count: GuestSms.maximumUnits / 2)) == nil)
        #expect(refusal(String(repeating: "e\u{0301}", count: GuestSms.maximumUnits / 2) + "x") == .tooLong)
        #expect(refusal("") == .noText)
    }

    // MARK: The request bytes (S2)

    /// The payload the host sends is the golden one, for every vector: the same bytes the service's own test
    /// parses to the same fields.
    @Test func theRequestBytesAreTheGoldenOnes() throws {
        let vectors = try SmsFixture.vectors(prefix: "request-")
        #expect(vectors.count >= 6)
        for vector in vectors {
            let attempt = try #require(GuestSms.Attempt(rawValue: vector.fields[0]), Comment(rawValue: vector.name))
            let sender = String(decoding: SmsFixture.bytes(ofHex: vector.fields[2]), as: UTF8.self)
            let text = String(decoding: SmsFixture.bytes(ofHex: vector.fields[3]), as: UTF8.self)
            let request = try #require(
                GuestSmsRequest(sender: sender, text: text, identifier: vector.fields[1], attempt: attempt),
                Comment(rawValue: vector.name))
            #expect(request.payload == vector.bytes, Comment(rawValue: vector.name))
            // Printable ASCII, five fields.
            #expect(request.payload.allSatisfy { (0x21...0x7E).contains($0) }, Comment(rawValue: vector.name))
            #expect(String(decoding: request.payload, as: UTF8.self).split(separator: ".", omittingEmptySubsequences: false).count == 5)
        }
        // The longest request has 2,750 bytes.
        #expect(try SmsFixture.vector("request-first-longest").bytes.count == 2750)
    }

    /// A retry is its first attempt with the attempt field changed and nothing else.
    @Test func aRetryDiffersFromItsFirstAttemptInTheAttemptFieldOnly() throws {
        let first = try #require(GuestSmsRequest(sender: "+1 555", text: "it's $HOME; `x`\nسلام", identifier: Self.identifier, attempt: .first))
        let retry = try #require(GuestSmsRequest(sender: "+1 555", text: "it's $HOME; `x`\nسلام", identifier: Self.identifier, attempt: .retry))
        #expect(String(decoding: first.payload, as: UTF8.self).replacing(".first.", with: ".retry.", maxReplacements: 1)
            == String(decoding: retry.payload, as: UTF8.self))
        #expect(first.sender == "+1555")
    }

    /// Whatever is typed or pasted travels as base64 in the fifth field: the payload is the same shape for every text.
    @Test func hostileTextTravelsOnlyAsBase64() throws {
        let texts = [
            "'; rm -rf /; '", "$(id)", "`id`", "\\'\"", "a\nb\r\nc", "tab\there", "nul\u{0}byte", "|&;<>(){}[]*?~#!",
            "--arg x", "\u{2028}\u{2029}", "\u{202E}reversed", "\u{FEFF}\u{FEFF}", "😀 and 日本語 and العربية",
            "\u{0301}\u{200D}\u{200B}", "%s %n %d", "../../etc/passwd", "content://other", " ", "a.b.c", "...",
        ]
        for text in texts {
            let request = try #require(GuestSmsRequest(sender: "Bank", text: text, identifier: Self.identifier, attempt: .first), "\(text.debugDescription)")
            let fields = String(decoding: request.payload, as: UTF8.self).split(separator: ".", omittingEmptySubsequences: false).map(String.init)
            #expect(fields.count == 5, "\(text.debugDescription)")
            #expect(fields[0] == "2" && fields[1] == "first" && fields[2] == Self.identifier)
            #expect(String(decoding: try #require(Data(base64Encoded: fields[4])), as: UTF8.self) == text)
            #expect(request.payload.allSatisfy { (0x21...0x7E).contains($0) }, "\(text.debugDescription)")
        }
        // The identifier is 32 lower-case hex digits and nothing else.
        for identifier in ["", "0123", Self.identifier.uppercased(), Self.identifier + "0", String(Self.identifier.dropLast()) + "'", String(Self.identifier.dropLast()) + "g"] {
            #expect(GuestSmsRequest(sender: "Bank", text: "hi", identifier: identifier, attempt: .first) == nil)
        }
        #expect(GuestSmsRequest(sender: "", text: "hi", identifier: Self.identifier, attempt: .first) == nil)
        #expect(GuestSmsRequest(sender: "Bank", text: "", identifier: Self.identifier, attempt: .first) == nil)
    }

    // MARK: The answer bytes (S1)

    /// Every golden answer parses to the form it is, and only for its own identifier.
    @Test func theGoldenAnswersParse() throws {
        let vectors = try SmsFixture.vectors(prefix: "answer-")
        #expect(vectors.count == 12)
        for vector in vectors {
            let identifier = vector.fields[0] == "-" ? Self.identifier : vector.fields[0]
            let parsed = GuestSms.answer(vector.bytes, identifier: identifier)
            let expected: GuestSms.Answer? = switch vector.name {
            case "answer-handed-over-1": .handedOver(parts: 1)
            case "answer-handed-over-11": .handedOver(parts: 11)
            case "answer-handed-over-earlier-1": .handedOverEarlier(parts: 1)
            case "answer-handed-over-earlier-11": .handedOverEarlier(parts: 11)
            case "answer-not-handed-over-invalid": .notHandedOver(.invalid)
            case "answer-not-handed-over-plan": .notHandedOver(.plan)
            case "answer-not-handed-over-store": .notHandedOver(.store)
            case "answer-uncertain-interrupted": .uncertain(.interrupted)
            case "answer-uncertain-unknown": .uncertain(.unknown)
            case "answer-uncertain-conflict": .uncertain(.conflict)
            case "answer-unsettled-store": .unsettled
            case "answer-refused-version": .refusedVersion
            default: nil
            }
            #expect(parsed == expected, Comment(rawValue: vector.name))
            #expect(parsed != nil, Comment(rawValue: vector.name))
            // Another identifier's answer is nobody's.
            if vector.fields[0] != "-" {
                #expect(GuestSms.answer(vector.bytes, identifier: String(repeating: "a", count: 32)) == nil, Comment(rawValue: vector.name))
            }
        }
        // The longest answer, 58 bytes.
        #expect(try SmsFixture.vector("answer-not-handed-over-invalid").bytes.count == 58)
    }

    /// Anything that is not exactly one of the forms is not an answer.
    @Test func anythingElseIsNotAnAnswer() {
        let id = Self.identifier
        let other = String(repeating: "b", count: 32)
        let cases: [(String, String)] = [
            ("another identifier", "2.handed-over.\(other).1"),
            ("version 1", "1.handed-over.\(id).1"),
            ("version 3", "3.handed-over.\(id).1"),
            ("version 02", "02.handed-over.\(id).1"),
            ("an unknown outcome", "2.delivered.\(id).1"),
            ("parts 0", "2.handed-over.\(id).0"),
            ("parts 12", "2.handed-over.\(id).12"),
            ("parts 01", "2.handed-over.\(id).01"),
            ("parts +1", "2.handed-over.\(id).+1"),
            ("parts a letter", "2.handed-over.\(id).x"),
            ("parts where a reason belongs", "2.uncertain.\(id).1"),
            ("a reason where parts belong", "2.handed-over.\(id).unknown"),
            ("a missing reason", "2.uncertain.\(id)"),
            ("an empty reason", "2.uncertain.\(id)."),
            ("an unknown reason", "2.uncertain.\(id).maybe"),
            ("an unknown not-handed-over reason", "2.not-handed-over.\(id).busy"),
            ("an unsettled with another reason", "2.unsettled.\(id).invalid"),
            ("a sixth field", "2.handed-over.\(id).1.x"),
            ("a fifth field", "2.uncertain.\(id).unknown.x"),
            ("a cut payload", "2.handed-over.\(id)"),
            ("a trailing dot", "2.handed-over.\(id).1."),
            ("the refusal with a field after it", "2.refused-version.x"),
            ("the refusal without a version", "refused-version"),
            ("the refusal of a version with a sign", "+2.refused-version"),
            ("the refusal of ten digits", "1234567890.refused-version"),
            ("a space", "2.handed-over.\(id).1 "),
            ("a line end", "2.handed-over.\(id).1\n"),
            ("upper case", "2.HANDED-OVER.\(id).1"),
            ("empty", ""),
        ]
        for (name, text) in cases {
            #expect(GuestSms.answer(Data(text.utf8), identifier: id) == nil, Comment(rawValue: name))
        }
        #expect(GuestSms.answer(Data([0x32, 0x2E, 0x80]), identifier: id) == nil)
        // The refusal is accepted whatever the service's version is.
        for version in ["0", "1", "2", "3", "999999999"] {
            #expect(GuestSms.answer(Data("\(version).refused-version".utf8), identifier: id) == .refusedVersion)
        }
    }

    // MARK: The adapter (S4, S5)

    private func request(_ attempt: GuestSms.Attempt) -> GuestSmsRequest {
        GuestSmsRequest(sender: "Bank", text: "code", identifier: Self.identifier, attempt: attempt)!
    }

    private func answered(_ form: String) -> GuestSmsChannel.Outcome {
        .answered(Data("2.\(form)".utf8))
    }

    /// Each row of the adapter's table: what the attempt, the channel's outcome and the service's answer come to.
    @Test func theAdapterMapsEveryRowOfTheTable() {
        let id = Self.identifier
        let first = request(.first)
        let retry = request(.retry)
        typealias R = GuestSms.Result
        let rows: [(String, GuestSmsRequest, GuestSmsChannel.Outcome, R)] = [
            // A first attempt.
            ("first, not sent", first, .notSent, .certainlyNotSent),
            ("first, not taken", first, .notTaken, .certainlyNotSent),
            ("first, no answer", first, .noAnswer, .notConfirmed),
            ("first, handed over", first, answered("handed-over.\(id).2"), .sent(parts: 2)),
            ("first, handed over earlier", first, answered("handed-over-earlier.\(id).3"), .deliveredEarlier(parts: 3)),
            ("first, not handed over (invalid)", first, answered("not-handed-over.\(id).invalid"), .certainlyNotSent),
            ("first, not handed over (plan)", first, answered("not-handed-over.\(id).plan"), .certainlyNotSent),
            ("first, not handed over (store)", first, answered("not-handed-over.\(id).store"), .certainlyNotSent),
            ("first, uncertain (interrupted)", first, answered("uncertain.\(id).interrupted"), .uncertain),
            ("first, uncertain (unknown)", first, answered("uncertain.\(id).unknown"), .uncertain),
            ("first, uncertain (conflict)", first, answered("uncertain.\(id).conflict"), .uncertain),
            ("first, version refusal", first, .answered(Data("2.refused-version".utf8)), .certainlyNotSent),
            ("first, another version's refusal", first, .answered(Data("3.refused-version".utf8)), .certainlyNotSent),
            ("first, unsettled", first, answered("unsettled.\(id).store"), .notConfirmed),
            ("first, malformed", first, .answered(Data("garbage".utf8)), .notConfirmed),
            ("first, another identifier's", first, answered("handed-over.\(String(repeating: "c", count: 32)).1"), .notConfirmed),
            ("first, an empty answer", first, .answered(Data()), .notConfirmed),
            // A retry.
            ("retry, not sent", retry, .notSent, .notConfirmed),
            ("retry, not taken", retry, .notTaken, .notConfirmed),
            ("retry, no answer", retry, .noAnswer, .notConfirmed),
            ("retry, handed over earlier", retry, answered("handed-over-earlier.\(id).1"), .deliveredEarlier(parts: 1)),
            ("retry, uncertain", retry, answered("uncertain.\(id).unknown"), .uncertain),
            ("retry, unsettled", retry, answered("unsettled.\(id).store"), .notConfirmed),
            ("retry, version refusal", retry, .answered(Data("2.refused-version".utf8)), .notConfirmed),
            ("retry, handed over", retry, answered("handed-over.\(id).1"), .notConfirmed),
            ("retry, not handed over", retry, answered("not-handed-over.\(id).store"), .notConfirmed),
            ("retry, malformed", retry, .answered(Data("2.uncertain.\(id)".utf8)), .notConfirmed),
            ("retry, another identifier's", retry, answered("uncertain.\(String(repeating: "c", count: 32)).unknown"), .notConfirmed),
        ]
        for (name, request, outcome, expected) in rows {
            #expect(GuestSms.result(for: request, outcome: outcome) == expected, Comment(rawValue: name))
        }
        // Only a result that is not confirmed keeps its identifier.
        #expect(R.notConfirmed.isFinal == false)
        for result in [R.sent(parts: 1), .deliveredEarlier(parts: 1), .certainlyNotSent, .uncertain] {
            #expect(result.isFinal)
        }
    }

    /// The first attempt is handed over and its answer lost; the retry cannot connect: one identifier throughout,
    /// and the answer to a later press says it was handed over earlier.
    @Test func aRetryThatCannotReachAndroidNeverForgetsTheFirstAttempt() {
        var retries = GuestSms.Retries { Self.identifier }
        let first = retries.next(sender: "Bank", text: "code")
        #expect(first.attempt == .first)
        func settle(_ attempt: GuestSms.Attempt, _ outcome: GuestSmsChannel.Outcome) -> GuestSms.Result {
            let request = GuestSmsRequest(sender: "Bank", text: "code", identifier: first.identifier, attempt: attempt)!
            let result = GuestSms.result(for: request, outcome: outcome)
            retries.settle(sender: "Bank", text: "code", identifier: first.identifier, result: result)
            return result
        }
        #expect(settle(.first, .noAnswer) == .notConfirmed)
        for outcome in [GuestSmsChannel.Outcome.notSent, .notTaken] {
            let next = retries.next(sender: "Bank", text: "code")
            #expect(next.attempt == .retry && next.identifier == first.identifier)
            #expect(settle(.retry, outcome) == .notConfirmed)
        }
        let next = retries.next(sender: "Bank", text: "code")
        #expect(next.attempt == .retry && next.identifier == first.identifier)
        #expect(settle(.retry, answered("handed-over-earlier.\(first.identifier).1")) == .deliveredEarlier(parts: 1))
    }

    // MARK: The identifiers (S3, S6)

    @Test func identifiersAreRandomHex() {
        let identifiers = Set((0..<50).map { _ in GuestSms.newIdentifier() })
        #expect(identifiers.count == 50)
        #expect(identifiers.allSatisfy(GuestSms.isIdentifier))
    }

    /// A draft that was not confirmed keeps its identifier and goes again as a retry; a final result keeps it
    /// until it has been shown, and only then are the same words a new message.
    @Test func aDraftKeepsItsIdentifierUntilItsResultIsShown() {
        let counter = Counter()
        var retries = GuestSms.Retries { counter.next() }
        let first = retries.next(sender: "Bank", text: "code 1")
        #expect(first.attempt == .first)
        // Not settled yet: nothing is kept, and the next press is another first attempt.
        #expect(retries.next(sender: "Bank", text: "code 1").identifier != first.identifier)
        retries.settle(sender: "Bank", text: "code 1", identifier: first.identifier, result: .notConfirmed)
        let again = retries.next(sender: "Bank", text: "code 1")
        #expect(again.attempt == .retry && again.identifier == first.identifier)
        // The sender as typed differs, the sender sent does not.
        #expect(retries.next(sender: " Bank ", text: "code 1").identifier == first.identifier)
        // Another draft has an identifier of its own, and the first stays.
        let other = retries.next(sender: "Bank", text: "code 2")
        #expect(other.attempt == .first && other.identifier != first.identifier)
        retries.settle(sender: "Bank", text: "code 2", identifier: other.identifier, result: .uncertain)
        #expect(retries.next(sender: "Other", text: "code 1").attempt == .first)
        // A final result keeps its identifier until it is shown: receipt alone forgets nothing.
        let held = retries.next(sender: "Bank", text: "code 2")
        #expect(held.attempt == .retry && held.identifier == other.identifier)
        retries.presented(identifier: other.identifier)
        let fresh = retries.next(sender: "Bank", text: "code 2")
        #expect(fresh.attempt == .first && fresh.identifier != other.identifier)
        // Showing a result touches no other draft.
        #expect(retries.next(sender: "Bank", text: "code 1").identifier == first.identifier)
        // An identifier that is not confirmed is not forgotten by a presentation of the same words.
        retries.presented(identifier: first.identifier)
        #expect(retries.next(sender: "Bank", text: "code 1").identifier == first.identifier)
    }

    /// Sixteen identifiers wait for an answer at most, none is dropped, and a new draft is refused while they do;
    /// a held draft still goes, as a retry.
    @Test func sixteenUnresolvedIdentifiersRefuseANewDraftAndDropNone() {
        var retries = GuestSms.Retries()
        var identifiers: [String] = []
        for number in 0..<GuestSms.Retries.capacity {
            #expect(!retries.refuses(sender: "Bank", text: "m\(number)"))
            let next = retries.next(sender: "Bank", text: "m\(number)")
            identifiers.append(next.identifier)
            retries.settle(sender: "Bank", text: "m\(number)", identifier: next.identifier, result: .notConfirmed)
        }
        #expect(retries.unresolvedCount == GuestSms.Retries.capacity && retries.isFull)
        #expect(retries.refuses(sender: "Bank", text: "new"))
        for (number, identifier) in identifiers.enumerated() {
            #expect(!retries.refuses(sender: "Bank", text: "m\(number)"))
            #expect(retries.next(sender: "Bank", text: "m\(number)").identifier == identifier)
        }
        // A result that is final lowers the count once it is shown.
        retries.settle(sender: "Bank", text: "m0", identifier: identifiers[0], result: .deliveredEarlier(parts: 1))
        #expect(!retries.refuses(sender: "Bank", text: "new"))
        retries.presented(identifier: identifiers[0])
        #expect(retries.unresolvedCount == GuestSms.Retries.capacity - 1)
    }

    /// Two texts that Swift treats as equal can be different bytes on the wire.
    @Test func theDraftIsComparedByItsBytes() {
        var retries = GuestSms.Retries()
        let composed = "caf\u{E9}"
        let decomposed = "cafe\u{301}"
        #expect(composed == decomposed)
        let identifier = retries.next(sender: "Bank", text: composed).identifier
        retries.settle(sender: "Bank", text: composed, identifier: identifier, result: .notConfirmed)
        #expect(retries.next(sender: "Bank", text: decomposed).attempt == .first)
    }

    /// A seeded run of a thousand presses over a few drafts with edits and every result: no identifier is a first
    /// attempt twice, and every retry's identifier was a first attempt earlier in the run.
    @Test func noIdentifierIsAFirstAttemptTwice() {
        var generator = SeededGenerator(seed: 0x5EED)
        var retries = GuestSms.Retries()
        var firsts: Set<String> = []
        let drafts = (0..<6).map { "draft \($0)" }
        let results: [GuestSms.Result] = [.sent(parts: 1), .deliveredEarlier(parts: 1), .certainlyNotSent, .notConfirmed, .uncertain, .notConfirmed]
        var inFlight: [String: String] = [:]
        for _ in 0..<1000 {
            let text = drafts.randomElement(using: &generator)!
            if retries.refuses(sender: "Bank", text: text) { continue }
            let next = retries.next(sender: "Bank", text: text)
            switch next.attempt {
            case .first: #expect(firsts.insert(next.identifier).inserted)
            case .retry: #expect(firsts.contains(next.identifier))
            }
            inFlight[text] = next.identifier
            let result = results.randomElement(using: &generator)!
            retries.settle(sender: "Bank", text: text, identifier: next.identifier, result: result)
            if result.isFinal, Bool.random(using: &generator) {
                retries.presented(identifier: next.identifier)
            }
            // The words are edited now and then: a draft that is not held is a new one.
            if Int.random(in: 0..<10, using: &generator) == 0 { retries.forget(identifier: next.identifier) }
        }
        #expect(!firsts.isEmpty)
    }

    @Test func theSenderIsKeptUntilTheVirtualMachineIsErased() throws {
        let suite = "GuestSmsTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let memory = GuestSms.SenderMemory(defaults: defaults)
        #expect(memory.sender == nil)
        memory.keep("My Bank")
        // Another launch reads the same defaults.
        #expect(GuestSms.SenderMemory(defaults: defaults).sender == "My Bank")
        memory.keep("")
        #expect(memory.sender == nil)
        memory.keep("+15551234567")
        memory.forget()
        #expect(memory.sender == nil)
        #expect(defaults.object(forKey: GuestSms.SenderMemory.key) == nil)
    }
}

/// A repeatable stream of numbers for a row that shuffles.
private struct SeededGenerator: RandomNumberGenerator {
    private var state: UInt64

    init(seed: UInt64) { state = seed }

    mutating func next() -> UInt64 {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        return state ^ (state >> 29)
    }
}
