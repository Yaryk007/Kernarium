// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit

/// What Andarium tells VoiceOver of its own accord: a new screen, an operation
/// that began or ended, TalkBack turning on or off. Each call does nothing
/// while VoiceOver is off.
enum VoiceOver {
    /// Speaks `text`. A queued announcement waits for what VoiceOver is
    /// saying; otherwise it interrupts it.
    ///
    /// A `priority` of `.high` interrupts what VoiceOver is saying and is not
    /// interrupted once started, not even by what VoiceOver says of the
    /// element its cursor lands on a moment after a screen change; a queued
    /// or default announcement is cut off by that.
    static func announce(_ text: String, queued: Bool = false, priority: UIAccessibilityPriority? = nil) {
        guard UIAccessibility.isVoiceOverRunning else { return }
        var attributes: [NSAttributedString.Key: Any] = [.accessibilitySpeechQueueAnnouncement: queued]
        if let priority { attributes[.accessibilitySpeechAnnouncementPriority] = priority }
        let words = NSAttributedString(string: text, attributes: attributes)
        UIAccessibility.post(notification: .announcement, argument: words)
    }

    /// Speaks `text` as `announce` does and calls `finished` once VoiceOver has
    /// finished or dropped it: true when it was spoken to its end. Nothing is
    /// called while VoiceOver is off.
    static func announce(
        _ text: String, queued: Bool = false, priority: UIAccessibilityPriority? = nil,
        finished: @escaping @MainActor @Sendable (Bool) -> Void
    ) {
        guard UIAccessibility.isVoiceOverRunning else { return }
        let observer = Observer()
        observer.token = NotificationCenter.default.addObserver(
            forName: UIAccessibility.announcementDidFinishNotification, object: nil, queue: .main
        ) { note in
            guard note.userInfo?[UIAccessibility.announcementStringValueUserInfoKey] as? String == text else { return }
            let successful = note.userInfo?[UIAccessibility.announcementWasSuccessfulUserInfoKey] as? Bool ?? false
            if let token = observer.token { NotificationCenter.default.removeObserver(token) }
            observer.token = nil
            Task { @MainActor in finished(successful) }
        }
        announce(text, queued: queued, priority: priority)
    }

    private final class Observer: @unchecked Sendable {
        var token: NSObjectProtocol?
    }

    /// The screen changed as a whole: VoiceOver moves to its first element.
    /// It did not speak `text` where that was a run's start sentence, so a
    /// sentence is said with `announce`.
    static func screenChanged(_ text: String? = nil) {
        guard UIAccessibility.isVoiceOverRunning else { return }
        UIAccessibility.post(notification: .screenChanged, argument: text)
    }
}
