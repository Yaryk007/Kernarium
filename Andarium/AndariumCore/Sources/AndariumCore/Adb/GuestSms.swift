// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A text message sent to the guest as if from the network. The guest's
/// KernariumSms service (android/lineage-24/overlay/sms) takes it from the host
/// over its own vsock port (`GuestSmsChannel`) and delivers it as the radio
/// stack would; the rules here are that service's, so the guest refuses
/// nothing the host lets through.
public enum GuestSms {
    /// The longest text, in UTF-16 units.
    public static let maximumUnits = 670
    /// The most parts a text of `maximumUnits` takes. The service puts 67
    /// units in a part, gives one back for the U+FEFF it adds before a part
    /// that starts with one, and ends a part a unit early rather than split
    /// a surrogate pair: every part but the last takes at least 65 units, so
    /// 670 take at most 11. Message.MAXIMUM_PARTS is the service's own count.
    public static let maximumParts = 11
    /// The longest name a sender can be, in characters, and the most digits of
    /// a number (the service's `Message.NAME` and `Message.NUMBER`).
    public static let maximumNameLength = 11
    public static let maximumNumberDigits = 20
    /// The sender a message carries when the person has kept none.
    public static let defaultSender = "Andarium"

    public enum Refusal: Equatable, Sendable {
        /// The sender is empty.
        case noSender
        /// The sender is neither a number nor a name: it has a character that is not a digit, a plain letter
        /// A to Z or a space.
        case senderCharacters
        /// A name of letters, digits and spaces that is longer than a name can be.
        case senderLongName
        /// A number with more digits than a number can have.
        case senderLongNumber
        case noText, tooLong
        /// The text holds U+FFFE, which Android's decoder cannot return: it
        /// swaps the bytes after it or replaces it (see the service's
        /// DeliverPdu), so the text would not arrive as typed.
        case unsendableCharacter
    }

    /// The sender with what a pasted or typed number brings along that the rules
    /// refuse made plain: direction marks dropped, no-break and ideographic
    /// spaces made ordinary, hyphens of every width made "-", the full-width
    /// forms of ASCII (U+FF01 to U+FF5E: digits, plus, brackets, full stop,
    /// letters) made ASCII, and the prolonged sound mark a Japanese keyboard
    /// types for a hyphen (U+30FC) made "-" between two digits.
    public static func cleaned(_ typed: String) -> String {
        var scalars: [Unicode.Scalar] = []
        for scalar in typed.unicodeScalars {
            switch scalar.value {
            case 0x061C, 0x200E, 0x200F, 0x202A...0x202E, 0x2066...0x2069: continue
            case 0x00A0, 0x2007, 0x202F, 0x3000: scalars.append(" ")
            case 0x2010...0x2013, 0x2212: scalars.append("-")
            case 0xFF01...0xFF5E: scalars.append(Unicode.Scalar(scalar.value - 0xFEE0) ?? scalar)
            default: scalars.append(scalar)
            }
        }
        func isDigit(_ scalar: Unicode.Scalar?) -> Bool { scalar.map { ("0"..."9").contains($0) } ?? false }
        for index in scalars.indices where scalars[index].value == 0x30FC {
            let before = index > scalars.startIndex ? scalars[index - 1] : nil
            let after = index + 1 < scalars.endIndex ? scalars[index + 1] : nil
            if isDigit(before), isDigit(after) { scalars[index] = "-" }
        }
        var out = String.UnicodeScalarView()
        out.append(contentsOf: scalars)
        return String(out)
    }

    /// `text` is an optional "+" and then digits, at most `limit` of them when
    /// there is a limit.
    private static func isNumber(_ text: String, atMost limit: Int?) -> Bool {
        var digits = text.unicodeScalars[...]
        if digits.first == "+" { digits = digits.dropFirst() }
        guard !digits.isEmpty, digits.allSatisfy({ ("0"..."9").contains($0) }) else { return false }
        return limit.map { digits.count <= $0 } ?? true
    }

    /// `text` is letters A to Z, digits and spaces, at most `limit` of them
    /// when there is a limit.
    private static func isName(_ text: String, atMost limit: Int?) -> Bool {
        let scalars = text.unicodeScalars
        guard !scalars.isEmpty, scalars.allSatisfy({
            ("A"..."Z").contains($0) || ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == " "
        }) else { return false }
        return limit.map { scalars.count <= $0 } ?? true
    }

    /// The sender as the guest takes it, once it is cleaned: a phone number of
    /// at most `maximumNumberDigits` digits, international with a leading +,
    /// with the spaces, dashes and brackets people type left out; or a name of
    /// at most `maximumNameLength` letters, digits and spaces, as a network
    /// sends a service's. nil for anything else.
    public static func sender(_ typed: String) -> String? {
        let trimmed = cleaned(typed).trimmingCharacters(in: .whitespaces)
        let digits = trimmed.filter { !" -().".contains($0) }
        if isNumber(digits, atMost: maximumNumberDigits) { return digits }
        if isName(trimmed, atMost: maximumNameLength) { return trimmed }
        return nil
    }

    /// Why the sender is refused, or nil when it is not.
    public static func senderRefusal(_ typed: String) -> Refusal? {
        let trimmed = cleaned(typed).trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty { return .noSender }
        if sender(typed) != nil { return nil }
        if isNumber(trimmed.filter({ !" -().".contains($0) }), atMost: nil) { return .senderLongNumber }
        if isName(trimmed, atMost: nil) { return .senderLongName }
        return .senderCharacters
    }

    /// Why the text is refused, or nil when it is not.
    public static func textRefusal(_ text: String) -> Refusal? {
        if text.isEmpty { return .noText }
        if text.utf16.count > maximumUnits { return .tooLong }
        if text.unicodeScalars.contains("\u{FFFE}") { return .unsendableCharacter }
        return nil
    }

    /// Why the message is refused, the sender first, or nil when it is not.
    public static func refusal(sender typed: String, text: String) -> Refusal? {
        senderRefusal(typed) ?? textRefusal(text)
    }

    /// The sender the person last sent a message from, kept across launches
    /// and forgotten when the virtual machine is erased: it is a choice of
    /// that virtual machine's.
    public struct SenderMemory {
        /// The stored value's key in the defaults database.
        public static let key = "andarium.textMessageSender"

        private let defaults: UserDefaults

        public init(defaults: UserDefaults = .standard) {
            self.defaults = defaults
        }

        /// The last sender kept; nil until one is, and after `forget`.
        public var sender: String? {
            defaults.string(forKey: Self.key).flatMap { $0.isEmpty ? nil : $0 }
        }

        public func keep(_ sender: String) {
            defaults.set(sender, forKey: Self.key)
        }

        public func forget() {
            defaults.removeObject(forKey: Self.key)
        }
    }

    /// An identifier for one message: 128 random bits in lower-case hex. The
    /// service hands a message over once for each identifier it is given.
    public static func newIdentifier() -> String {
        (0..<16).map { _ in String(format: "%02x", UInt8.random(in: .min ... .max)) }.joined()
    }

    public static func isIdentifier(_ text: String) -> Bool {
        text.wholeMatch(of: /[0-9a-f]{32}/) != nil
    }

    // MARK: The request

    /// The version of the payload (the service's `Message` and `Answer`). The
    /// frame around it has a version of its own (`GuestSmsChannel`).
    public static let payloadVersion = 2

    /// Whether a request is the first the host makes for its identifier or a
    /// later one. Only a first attempt can make the service hand the message
    /// over; a retry only asks what became of the first.
    public enum Attempt: String, Equatable, Sendable {
        case first, retry
    }

    // MARK: The adapter

    /// What a send came to, as the bar tells it: the request, the channel's
    /// outcome and the service's answer decide it (`result(for:outcome:)`).
    public enum Result: Equatable, Sendable {
        /// This request handed the message to Android, in this many parts.
        /// That is all the service reports: it does not wait for an
        /// application to read it.
        case sent(parts: Int)
        /// An earlier request with this identifier handed it over once.
        case deliveredEarlier(parts: Int)
        /// This first attempt did not reach Android and none ever will: the
        /// identifier is closed.
        case certainlyNotSent
        /// Nothing is known of this request, or of an earlier one: the
        /// identifier is kept, and sending the draft again asks about it.
        case notConfirmed
        /// The service closed the identifier and cannot say whether its attempt
        /// acted.
        case uncertain

        /// The identifier is settled for good once the result has been shown.
        public var isFinal: Bool { self != .notConfirmed }
    }

    /// The service's answer to a request (`Answer.java`).
    public enum Answer: Equatable, Sendable {
        case handedOver(parts: Int)
        case handedOverEarlier(parts: Int)
        case notHandedOver(NotHandedOverReason)
        case uncertain(UncertainReason)
        case unsettled
        /// The service speaks another payload version and did nothing.
        case refusedVersion
    }

    public enum NotHandedOverReason: String, Equatable, Sendable {
        case invalid, plan, store
    }

    public enum UncertainReason: String, Equatable, Sendable {
        case interrupted, unknown, conflict
    }

    /// The answer in `bytes`, only when it is exactly one of the service's forms
    /// for this identifier and nothing is left over; nil for anything else. The
    /// version refusal is accepted in its form for every version number.
    public static func answer(_ bytes: Data, identifier: String) -> Answer? {
        guard !bytes.isEmpty, bytes.allSatisfy({ (0x21...0x7E).contains($0) }) else { return nil }
        let text = String(decoding: bytes, as: UTF8.self)
        let fields = text.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
        func isVersion(_ field: String) -> Bool { field.wholeMatch(of: /0|[1-9][0-9]{0,8}/) != nil }
        func parts(_ field: String) -> Int? {
            guard field.wholeMatch(of: /[1-9]|1[01]/) != nil else { return nil }
            return Int(field)
        }
        if fields.count == 2 {
            return isVersion(fields[0]) && fields[1] == "refused-version" ? .refusedVersion : nil
        }
        guard fields.count == 4, fields[0] == String(payloadVersion), fields[2] == identifier else { return nil }
        let detail = fields[3]
        switch fields[1] {
        case "handed-over": return parts(detail).map { .handedOver(parts: $0) }
        case "handed-over-earlier": return parts(detail).map { .handedOverEarlier(parts: $0) }
        case "not-handed-over": return NotHandedOverReason(rawValue: detail).map { .notHandedOver($0) }
        case "uncertain": return UncertainReason(rawValue: detail).map { .uncertain($0) }
        case "unsettled": return detail == "store" ? .unsettled : nil
        default: return nil
        }
    }

    /// What a send came to. `notSent`, `notTaken` and the refusals are
    /// definitive only for a first attempt: on a retry they say nothing of the
    /// first, so the identifier is kept and the next press asks again. An
    /// answer that is malformed, another identifier's or not one the attempt
    /// can get is not confirmed too.
    public static func result(for request: GuestSmsRequest, outcome: GuestSmsChannel.Outcome) -> Result {
        let first = request.attempt == .first
        switch outcome {
        case .notSent, .notTaken:
            return first ? .certainlyNotSent : .notConfirmed
        case .noAnswer:
            return .notConfirmed
        case .answered(let bytes):
            guard let answer = answer(bytes, identifier: request.identifier) else { return .notConfirmed }
            switch answer {
            case .handedOver(let parts): return first ? .sent(parts: parts) : .notConfirmed
            case .handedOverEarlier(let parts): return .deliveredEarlier(parts: parts)
            case .notHandedOver: return first ? .certainlyNotSent : .notConfirmed
            case .uncertain: return .uncertain
            case .unsettled: return .notConfirmed
            case .refusedVersion: return first ? .certainlyNotSent : .notConfirmed
            }
        }
    }

    // MARK: The identifiers

    /// The drafts whose send has no final answer yet, with their identifiers.
    /// A draft sent again after a send that was not confirmed keeps its
    /// identifier and goes as a retry: the service answers it from its record
    /// and never hands the message over twice. A final result keeps the
    /// identifier until it has been shown (`presented`); then the same words
    /// sent again are a new message. A new draft is refused while
    /// `capacity` identifiers are unresolved, and none is dropped in silence.
    public struct Retries: Sendable {
        /// The most identifiers waiting for an answer.
        public static let capacity = 16

        private struct Draft: Hashable, Sendable {
            let sender: [UInt8]
            let text: [UInt8]
        }

        private struct Held: Sendable {
            var identifier: String
            var final: Bool
        }

        private var held: [Draft: Held] = [:]
        private let make: @Sendable () -> String

        public init(makeIdentifier: @escaping @Sendable () -> String = GuestSms.newIdentifier) {
            make = makeIdentifier
        }

        private static func draft(_ sender: String, _ text: String) -> Draft {
            Draft(sender: Array((GuestSms.sender(sender) ?? sender).utf8), text: Array(text.utf8))
        }

        /// The identifiers still unresolved: sent without a final answer.
        public var unresolvedCount: Int { held.values.filter { !$0.final }.count }

        /// There is no room for a draft the service has not been asked about.
        public var isFull: Bool { unresolvedCount >= Self.capacity }

        /// This draft has an identifier already.
        public func holds(sender: String, text: String) -> Bool {
            held[Self.draft(sender, text)] != nil
        }

        /// A new draft cannot be sent now: the identifiers are used up and
        /// this one holds none.
        public func refuses(sender: String, text: String) -> Bool {
            isFull && !holds(sender: sender, text: text)
        }

        /// The identifier to send this draft with now, and the attempt: a
        /// first attempt with an identifier made now, or a retry with the one
        /// the draft holds. An identifier is never a first attempt twice.
        public func next(sender: String, text: String) -> (identifier: String, attempt: Attempt) {
            if let held = held[Self.draft(sender, text)] { return (held.identifier, .retry) }
            return (make(), .first)
        }

        /// Records how the send with `identifier` ended.
        public mutating func settle(sender: String, text: String, identifier: String, result: Result) {
            held[Self.draft(sender, text)] = Held(identifier: identifier, final: result.isFinal)
        }

        /// The final result of `identifier` has been shown: it is forgotten.
        public mutating func presented(identifier: String) {
            held = held.filter { !($0.value.final && $0.value.identifier == identifier) }
        }

        /// Forgets `identifier` whatever it came to, for a result the bar
        /// closed on before it was shown.
        public mutating func forget(identifier: String) {
            held = held.filter { $0.value.identifier != identifier }
        }
    }
}

/// One message to the guest: the cleaned sender, the text, the identifier and
/// the attempt, which are the service's request (`GuestSms.Attempt`).
public struct GuestSmsRequest: Equatable, Sendable {
    public let sender: String
    public let text: String
    public let identifier: String
    public let attempt: GuestSms.Attempt

    /// nil when the sender or text is refused or the identifier is not one.
    public init?(sender typed: String, text: String, identifier: String, attempt: GuestSms.Attempt) {
        guard GuestSms.refusal(sender: typed, text: text) == nil, let sender = GuestSms.sender(typed),
              GuestSms.isIdentifier(identifier) else { return nil }
        self.sender = sender
        self.text = text
        self.identifier = identifier
        self.attempt = attempt
    }

    /// `2.<attempt>.<identifier>.<sender64>.<text64>`: printable ASCII, the
    /// sender and the text as base64 of UTF-8 so that no character of theirs
    /// can break the form. Only the attempt differs between a first attempt
    /// and its retry.
    public var payload: Data {
        let fields = [
            String(GuestSms.payloadVersion), attempt.rawValue, identifier,
            Data(sender.utf8).base64EncodedString(), Data(text.utf8).base64EncodedString(),
        ]
        return Data(fields.joined(separator: ".").utf8)
    }
}
