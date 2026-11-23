// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The words of sending a text message to the virtual machine: Settings' row,
/// the fields, what is wrong with them, and how a send ended. "Text message"
/// is Android's own name for it, and the person's. Each string goes through
/// the String Catalog.
nonisolated enum SmsMessages {
    // MARK: Names

    /// The feature's name, and the placeholder of its message box.
    static var name: String { String(localized: "Text message") }
    /// The line under the bar's title: who receives the message, and as what.
    static var caption: String { String(localized: "Android receives it as an incoming text.") }
    /// Settings' row, which closes Settings and raises the bar over Android.
    static var row: String { String(localized: "Send text message\u{2026}") }
    /// The sender field's label for VoiceOver, and the label before it.
    static var from: String { String(localized: "From") }
    static var fromLabel: String { String(localized: "From:") }
    static var senderPlaceholder: String { String(localized: "Number or name") }
    /// What a sender can be: VoiceOver reads it as the field's hint.
    static var senderHint: String {
        String(localized: "A phone number, or a name of up to \(GuestSms.maximumNameLength) letters A\u{2013}Z, digits and spaces.")
    }
    /// The message box's label for VoiceOver.
    static var message: String { String(localized: "Message") }
    static var send: String { String(localized: "Send") }
    static var sending: String { String(localized: "Sending\u{2026}") }
    static var close: String { String(localized: "Close") }

    // MARK: What is wrong

    static var senderEmpty: String { String(localized: "Enter who the message is from.") }
    static var senderCharacters: String { String(localized: "A name can use only letters A\u{2013}Z, digits and spaces.") }
    static var senderLongName: String {
        String(localized: "A name can have at most \(GuestSms.maximumNameLength) characters.")
    }
    static var senderLongNumber: String {
        String(localized: "A number can have at most \(GuestSms.maximumNumberDigits) digits.")
    }
    /// The text holds a character that cannot be sent (`GuestSms.Refusal.unsendableCharacter`).
    static var unsendableCharacter: String { String(localized: "The message has a character that can\u{2019}t be sent.") }

    /// The room left in the message box, said once it runs short, and how far
    /// over the limit it is.
    static func charactersLeft(_ count: Int) -> String { String(localized: "\(count) characters left") }
    static func charactersOver(_ count: Int) -> String { String(localized: "\(count) characters too many") }

    // MARK: How a send ended

    /// Android took the message. That is all that is known of it, so that is
    /// all these words say.
    static var sentToAndroid: String { String(localized: "Sent to Android") }
    /// An earlier send of this message was handed to Android, and this one asked about it.
    static var deliveredEarlier: String { String(localized: "Android already received this message.") }
    /// Nothing was handed to Android and nothing ever will be for this send.
    static var notSent: String {
        String(localized: "The message wasn\u{2019}t sent. Android didn\u{2019}t receive it.")
    }
    /// Android did not confirm: sending the draft again asks, and cannot hand it over twice.
    static var notConfirmed: String {
        String(localized: "Android didn\u{2019}t confirm the message. Send it again to ask whether it arrived; it won\u{2019}t arrive twice.")
    }
    /// Android closed the message and cannot say whether it arrived.
    static var uncertain: String {
        String(localized: "Android can\u{2019}t tell whether the message arrived. Check before you send it again.")
    }
    /// A message sent in a session that ended before its result was shown.
    static var cutOff: String {
        String(localized: "A message sent earlier may or may not have reached Android. Check before you send it again.")
    }
    /// Sixteen sends wait for Android to confirm them, and a new one cannot be sent.
    static var waiting: String {
        String(localized: "Too many messages are waiting for Android to confirm them. Restart Android to send a new one.")
    }
    static var notRunning: String { String(localized: "Android isn\u{2019}t running.") }
    static var paused: String { String(localized: "Android is paused for lack of space.") }

    /// The words under the message box for a refusal that the model reports.
    static func refusal(_ refusal: GuestSms.Refusal, text: String) -> String {
        switch refusal {
        case .noSender: senderEmpty
        case .senderCharacters: senderCharacters
        case .senderLongName: senderLongName
        case .senderLongNumber: senderLongNumber
        case .noText: message
        case .tooLong: charactersOver(text.utf16.count - GuestSms.maximumUnits)
        case .unsendableCharacter: unsendableCharacter
        }
    }
}
