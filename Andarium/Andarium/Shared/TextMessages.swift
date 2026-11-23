// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Whether Android can take a text message now.
enum TextMessageAvailability: Equatable {
    case ready
    /// No normal run: the virtual machine is off, starting, or in Recovery,
    /// which runs no adb.
    case notRunning
    /// The virtual machine is paused for lack of space, and the guest
    /// takes nothing until it resumes.
    case paused
}

/// The line the bar shows between its fields: what is wrong, how the last
/// send went, or how much room is left.
struct TextMessageStatus: Equatable {
    enum Tone: Equatable {
        /// Something to know: the room left, or that Android is paused.
        case info
        /// Android took the message.
        case good
        /// Something to put right, or a send that was not confirmed.
        case problem
    }

    var words: String
    var tone: Tone
}

/// A text message for the virtual machine, being written in the bar over
/// Android. It belongs to the run: `SessionState` makes one for each run, a
/// send that is under way ends with it, and what a run's send came to is
/// told once, in this run or by the notice (`TextMessageNotice`) at the next
/// opening of the bar, and never twice.
@Observable
final class TextMessageState {
    /// How the last send ended.
    enum Progress: Equatable {
        case idle
        case sending
        /// Android took the message.
        case sent
        /// An earlier send of the message had been handed over.
        case deliveredEarlier
        /// This first attempt did not reach Android, and none ever will.
        case certainlyNotSent
        /// Android did not confirm it: sending the draft again asks.
        case notConfirmed
        /// Android closed the message and cannot say whether it arrived.
        case uncertain
        /// The bar opened over a session that ended before a send's result was shown.
        case cutOff
    }

    /// A line nobody has been told yet: the result of a send that is final, or the cut-off line.
    enum Awaiting: Equatable {
        case result(String)
        case cutOff
    }

    /// The bar is up over Android.
    var isOpen = false
    /// What is typed, as typed: it is cleaned and judged where it is used.
    private(set) var sender = GuestSms.defaultSender
    private(set) var text = ""
    var progress = Progress.idle
    /// The identifiers of the drafts whose send has no final result, and of the final results not yet shown.
    var retries = GuestSms.Retries()
    /// The sender and text of the last send, for the words about it to
    /// apply to that draft alone.
    private(set) var attempt: (sender: String, text: String)?
    /// The run is over: nothing of this is shown, and a late answer is lost.
    private(set) var ended = false
    @ObservationIgnored var sendTask: Task<Void, Never>?
    /// The line that has not been presented yet. It counts as presented once it
    /// was on screen in the open bar while the application was active and,
    /// under VoiceOver, announced or focused (`AppModel.presentTextMessageLine`).
    private(set) var awaiting: Awaiting?
    /// VoiceOver announced the awaited line to its end, or its focus reached the line.
    private(set) var announced = false
    /// The bar opened with the cut-off line while VoiceOver ran: the keyboard's
    /// focus move is skipped, so that it cannot cut the announcement short.
    private(set) var skipsKeyboardFocus = false
    /// Each request to give VoiceOver focus to the status line; the bar follows it.
    private(set) var statusFocusRequests = 0

    /// What a field took that the model refused (a character typed while a
    /// message is being sent), until the next turn of the main loop. A field
    /// that took a character the model refused would go on showing it: the
    /// field shows this for that one turn and then the model's own text, which
    /// is a change for it to redraw.
    private(set) var refusedSender: String?
    private(set) var refusedText: String?

    /// What the sender and the message box show.
    var shownSender: String { refusedSender ?? sender }
    var shownText: String { refusedText ?? text }

    /// A field writes its text back as it is when it is laid out or focused:
    /// only a change is typing. While a message is being sent the fields
    /// refuse edits, so the text that goes is the text that stays until
    /// Android has answered; they keep the keyboard's focus (the bar's view).
    func setSender(_ typed: String) {
        guard typed != sender, typed != refusedSender else { return }
        guard progress != .sending else {
            refusedSender = typed
            Task { @MainActor [weak self] in self?.refusedSender = nil }
            return
        }
        sender = typed
        edited()
    }

    func setText(_ typed: String) {
        guard typed != text, typed != refusedText else { return }
        guard progress != .sending else {
            refusedText = typed
            Task { @MainActor [weak self] in self?.refusedText = nil }
            return
        }
        text = typed
        edited()
    }

    /// Typing after a send began a new message: the words of the earlier
    /// one are not about it. A line nobody has been told yet stays.
    private func edited() {
        guard awaiting == nil else { return }
        switch progress {
        case .sent, .deliveredEarlier, .cutOff: progress = .idle
        default: break
        }
    }

    /// Opens the bar with the sender the person last used, and nothing typed.
    /// With `cutOff` it opens with the cut-off line, which is awaited until it
    /// has been presented.
    func open(sender: String, cutOff: Bool = false, voiceOver: Bool = false) {
        guard !ended else { return }
        if !isOpen {
            self.sender = sender
            text = ""
            progress = .idle
            attempt = nil
            awaiting = nil
            announced = false
            skipsKeyboardFocus = false
            if cutOff {
                progress = .cutOff
                awaiting = .cutOff
                skipsKeyboardFocus = voiceOver
            }
        }
        isOpen = true
    }

    /// Closes the bar and forgets the text. A send under way keeps it up.
    /// Returns the identifier of a final result that was not shown, which the
    /// notice keeps for the next opening.
    @discardableResult
    func close() -> String? {
        guard progress != .sending else { return nil }
        var unshown: String?
        if case .result(let identifier)? = awaiting {
            unshown = identifier
            retries.forget(identifier: identifier)
        }
        // A cut-off line nobody was told stays in the notice, and shows at the next opening.
        isOpen = false
        text = ""
        progress = .idle
        awaiting = nil
        attempt = nil
        announced = false
        return unshown
    }

    /// The run ended: the bar goes, a send under way is dropped, and the
    /// identifiers of the run's drafts are forgotten with the service's own
    /// memory of them; the notice keeps what the person is owed.
    func end() {
        ended = true
        sendTask?.cancel()
        sendTask = nil
        isOpen = false
        text = ""
        progress = .idle
        attempt = nil
        awaiting = nil
        announced = false
        retries = GuestSms.Retries()
    }

    /// A send begins with this draft.
    func begin() {
        attempt = (sender, text)
        progress = .sending
        awaiting = nil
        announced = false
    }

    /// The send of `request` ended in `result`. Returns what to say aloud, or
    /// nil when the run is over and nothing is to be said.
    func finish(result: GuestSms.Result, request: GuestSmsRequest) -> String? {
        guard !ended, progress == .sending else { return nil }
        retries.settle(sender: request.sender, text: request.text, identifier: request.identifier, result: result)
        switch result {
        case .sent:
            progress = .sent
            // Only the text that went: what was typed since stays.
            if text == request.text { text = "" }
        case .deliveredEarlier:
            progress = .deliveredEarlier
            if text == request.text { text = "" }
        case .certainlyNotSent: progress = .certainlyNotSent
        case .notConfirmed: progress = .notConfirmed
        case .uncertain: progress = .uncertain
        }
        announced = false
        awaiting = result.isFinal ? .result(request.identifier) : nil
        return words
    }

    /// The words of the line the last send, or the opening, came to; nil when there is none.
    var words: String? {
        switch progress {
        case .idle, .sending: nil
        case .sent: SmsMessages.sentToAndroid
        case .deliveredEarlier: SmsMessages.deliveredEarlier
        case .certainlyNotSent: SmsMessages.notSent
        case .notConfirmed: SmsMessages.notConfirmed
        case .uncertain: SmsMessages.uncertain
        case .cutOff: SmsMessages.cutOff
        }
    }

    /// The awaited line has been shown (and, under VoiceOver, announced): it is
    /// told, and the record of it can go. Returns what was awaited.
    func presented() -> Awaiting? {
        defer {
            awaiting = nil
            announced = false
        }
        guard let awaiting else { return nil }
        if case .result(let identifier) = awaiting { retries.presented(identifier: identifier) }
        return awaiting
    }

    /// VoiceOver finished announcing `words`, or did not: only the awaited line's own words count.
    func announcementEnded(_ words: String, successfully: Bool) {
        guard successfully, awaiting != nil, words == self.words else { return }
        announced = true
    }

    /// VoiceOver focus reached the status line, which shows `shown`: only the
    /// awaited line's own words are the result told.
    func statusLineFocused(shown: String?) {
        if awaiting != nil, shown == words { announced = true }
    }

    func requestStatusFocus() { statusFocusRequests += 1 }

    /// The text is the draft the last send was of.
    private var isAttemptedDraft: Bool {
        guard let attempt else { return false }
        return attempt.sender == sender && attempt.text == text
    }

    /// A new draft cannot be sent: sixteen sends wait for Android to confirm them.
    var isWaiting: Bool {
        !text.isEmpty && retries.refuses(sender: sender, text: text)
    }

    /// How many UTF-16 units the text may still have.
    var unitsLeft: Int { GuestSms.maximumUnits - text.utf16.count }

    /// The line to show now, or nil for none.
    func status(availability: TextMessageAvailability) -> TextMessageStatus? {
        switch availability {
        case .notRunning: return TextMessageStatus(words: SmsMessages.notRunning, tone: .problem)
        case .paused: return TextMessageStatus(words: SmsMessages.paused, tone: .info)
        case .ready: break
        }
        // A result that waits to be presented keeps the line, whatever is typed
        // after it: counts and refusals wait, so what is told is what was shown.
        if awaiting != nil, let awaited = awaitedStatus { return awaited }
        switch progress {
        case .sending:
            return nil
        case .sent where text.isEmpty:
            return TextMessageStatus(words: SmsMessages.sentToAndroid, tone: .good)
        case .deliveredEarlier where text.isEmpty:
            return TextMessageStatus(words: SmsMessages.deliveredEarlier, tone: .good)
        case .certainlyNotSent where isAttemptedDraft:
            return TextMessageStatus(words: SmsMessages.notSent, tone: .problem)
        case .notConfirmed where isAttemptedDraft:
            return TextMessageStatus(words: SmsMessages.notConfirmed, tone: .problem)
        case .uncertain where isAttemptedDraft:
            return TextMessageStatus(words: SmsMessages.uncertain, tone: .problem)
        case .cutOff where text.isEmpty:
            return TextMessageStatus(words: SmsMessages.cutOff, tone: .problem)
        default:
            break
        }
        return senderStatus ?? textProblem ?? waitingStatus ?? textStatus
    }

    /// The awaited result's line.
    private var awaitedStatus: TextMessageStatus? {
        guard let words else { return nil }
        switch progress {
        case .sent, .deliveredEarlier: return TextMessageStatus(words: words, tone: .good)
        case .certainlyNotSent, .notConfirmed, .uncertain, .cutOff: return TextMessageStatus(words: words, tone: .problem)
        case .idle, .sending: return nil
        }
    }

    /// Sixteen sends wait: this draft is not one of them, and waits for a restart.
    private var waitingStatus: TextMessageStatus? {
        isWaiting ? TextMessageStatus(words: SmsMessages.waiting, tone: .problem) : nil
    }

    /// What is wrong with the text itself: over the limit, or a character that cannot go.
    private var textProblem: TextMessageStatus? {
        if unitsLeft < 0 { return TextMessageStatus(words: SmsMessages.charactersOver(-unitsLeft), tone: .problem) }
        if GuestSms.textRefusal(text) == .unsendableCharacter {
            return TextMessageStatus(words: SmsMessages.unsendableCharacter, tone: .problem)
        }
        return nil
    }

    /// What is wrong with the sender. Nothing says it is missing until there
    /// is a message to send.
    var senderStatus: TextMessageStatus? {
        switch GuestSms.senderRefusal(sender) {
        case nil: nil
        case .noSender?: text.isEmpty ? nil : TextMessageStatus(words: SmsMessages.senderEmpty, tone: .problem)
        case let refusal?: TextMessageStatus(words: SmsMessages.refusal(refusal, text: text), tone: .problem)
        }
    }

    /// Once the room runs short, how much is left.
    var textStatus: TextMessageStatus? {
        if unitsLeft <= 100 && !text.isEmpty { return TextMessageStatus(words: SmsMessages.charactersLeft(unitsLeft), tone: .info) }
        return nil
    }
}

extension AppModel {
    /// Settings offers the entrance: the application carries text messages
    /// (`Capabilities.textMessages`) and this is a normal run, which has adb.
    var offersTextMessage: Bool {
        guard capabilities.textMessages, case .running(.normal) = phase else { return false }
        return true
    }

    var textMessageAvailability: TextMessageAvailability {
        guard case .running(.normal) = phase else { return .notRunning }
        return spacePause == nil ? .ready : .paused
    }

    /// Send may be pressed: Android can take it, no send is under way, the
    /// message passes the rules, and sixteen sends do not already wait for
    /// Android to confirm them. While a result waits to be told, its line
    /// stays and says nothing of a refused draft, so Send stays enabled and a
    /// press only says the result again (`sendTextMessage`); the refusal and
    /// the dimmed Send return together once the result was told.
    var canSendTextMessage: Bool {
        let state = session.textMessage
        guard textMessageAvailability == .ready, state.progress != .sending else { return false }
        if state.awaiting != nil, state.words != nil { return true }
        return GuestSms.refusal(sender: state.sender, text: state.text) == nil && !state.isWaiting
    }

    /// The notice of this instance: the driver's, or, with no guest, one that
    /// lives in memory.
    var textMessageNotice: TextMessageNotice {
        driver?.textMessageNotice ?? previewTextMessageNotice
    }

    /// Settings' row: Settings closes, and the bar comes up over Android
    /// once the sheet has gone.
    func presentTextMessage() {
        let state = session.textMessage
        Task { @MainActor [weak self, weak state] in
            await self?.textMessageSleep(.milliseconds(350))
            // The same run: a restart in between made a session of its own.
            guard let self, let state, state === self.session.textMessage else { return }
            self.openTextMessage()
        }
    }

    /// Opens the bar. When a session ended before the result of a send was
    /// shown, it opens with the cut-off line, and that line is presented
    /// before Send takes a new draft.
    func openTextMessage() {
        guard offersTextMessage else { return }
        let state = session.textMessage
        let wasOpen = state.isOpen
        let remembered = smsSender.sender.flatMap { GuestSms.senderRefusal($0) == nil ? $0 : nil }
        let voiceOver = textMessageVoiceOver()
        state.open(sender: remembered ?? GuestSms.defaultSender, cutOff: textMessageNotice.cutOffPending, voiceOver: voiceOver)
        guard !wasOpen, state.awaiting == .cutOff else { return }
        announceTextMessage(SmsMessages.cutOff)
        if voiceOver { state.requestStatusFocus() }
        presentTextMessageLine()
    }

    func closeTextMessage() {
        let state = session.textMessage
        if let unshown = state.close() { textMessageNotice.leaveForCutOff(unshown) }
    }

    /// The run ended or the guest restarts: what the session left becomes the
    /// notice's cut-off line, and the session's text message state ends.
    func endTextMessageSession() {
        session.textMessage.end()
        textMessageNotice.endSession()
    }

    func setTextMessageSender(_ typed: String) { session.textMessage.setSender(typed) }
    func setTextMessageText(_ typed: String) { session.textMessage.setText(typed) }

    // MARK: What the person has been told

    /// Says `words` and learns whether VoiceOver spoke them to the end.
    private func announceTextMessage(_ words: String) {
        let state = session.textMessage
        textMessageAnnouncer(words) { [weak self, weak state] successful in
            guard let self, let state, state === self.session.textMessage else { return }
            state.announcementEnded(words, successfully: successful)
            self.presentTextMessageLine()
        }
    }

    /// The awaited line is told once it is on screen in the open bar while the
    /// application is active and, with VoiceOver running, announced to its
    /// end or focused. Then the identifier it was kept for is forgotten.
    func presentTextMessageLine() {
        let state = session.textMessage
        guard state.awaiting != nil, state.isOpen, textMessageSceneActive else { return }
        // On screen means the awaited line itself: a pause or a refusal in its place tells nothing.
        guard state.status(availability: textMessageAvailability)?.words == state.words else { return }
        if textMessageVoiceOver(), !state.announced { return }
        let notice = textMessageNotice
        switch state.presented() {
        case .result(let identifier)?: Task { await notice.resolve(identifier) }
        case .cutOff?: Task { await notice.clearCutOff() }
        case nil: break
        }
    }

    /// The scene became active or inactive (the bar tells the model).
    func textMessageSceneChanged(active: Bool) {
        textMessageSceneActive = active
        if active { presentTextMessageLine() }
    }

    /// VoiceOver focus reached the status line.
    func textMessageLineFocused() {
        let state = session.textMessage
        state.statusLineFocused(shown: state.status(availability: textMessageAvailability)?.words)
        presentTextMessageLine()
    }

    /// Hands the typed message to Android. The send and its answer belong to
    /// the run: stopping the virtual machine ends it, and an answer that
    /// comes after is dropped. A first attempt is written to the notice
    /// before anything is dispatched.
    func sendTextMessage() {
        guard textMessageAvailability == .ready else { return }
        let state = session.textMessage
        guard state.progress != .sending else { return }
        // A result nobody has been told yet is not superseded: this press sends
        // nothing, says it again and takes VoiceOver to the line.
        if state.awaiting != nil, let words = state.words {
            state.requestStatusFocus()
            announceTextMessage(words)
            return
        }
        guard canSendTextMessage else { return }
        let sender = state.sender
        let text = state.text
        let next = state.retries.next(sender: sender, text: text)
        guard let request = GuestSmsRequest(sender: sender, text: text, identifier: next.identifier, attempt: next.attempt) else { return }
        smsSender.keep(request.sender)
        state.begin()
        let driver = driver
        let notice = textMessageNotice
        state.sendTask = Task { @MainActor [weak self, weak state] in
            guard !Task.isCancelled else { return }
            if request.attempt == .first {
                do {
                    try await notice.add(request.identifier)
                } catch {
                    // Nothing was dispatched, and nothing will be: the message certainly did not go.
                    self?.finishTextMessage(state, request: request, result: .certainlyNotSent)
                    return
                }
                guard !Task.isCancelled else { return }
            }
            let outcome: GuestSmsChannel.Outcome
            if let driver {
                outcome = await driver.sendSms(request)
            } else {
                // The preview has no guest: Android takes it after a moment.
                try? await Task.sleep(for: .milliseconds(600))
                outcome = .answered(Data("\(GuestSms.payloadVersion).handed-over.\(request.identifier).1".utf8))
            }
            guard !Task.isCancelled else { return }
            self?.finishTextMessage(state, request: request, result: GuestSms.result(for: request, outcome: outcome))
        }
    }

    private func finishTextMessage(_ state: TextMessageState?, request: GuestSmsRequest, result: GuestSms.Result) {
        guard let state, state === session.textMessage, let words = state.finish(result: result, request: request) else { return }
        announceTextMessage(words)
        presentTextMessageLine()
    }
}
