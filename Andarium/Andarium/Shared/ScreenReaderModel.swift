// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation
import UIKit

/// What a person is told of Android's screen reader in Settings: the line under
/// the switch.
enum ScreenReaderRow: Equatable {
    /// Not read yet.
    case checking
    /// The last read got no answer.
    case couldntCheck
    /// The system image has no screen reader.
    case unavailable
    case off
    case on
    /// A change is under way.
    case switching(turningOn: Bool)
    /// The last change did not happen.
    case failed(turningOn: Bool, reason: String)
    /// A file transfer holds the adb channel.
    case waitsForTransfer

    /// The words under the switch; nil when the switch says it all.
    var line: String? {
        switch self {
        case .checking: ScreenReaderWords.checking
        case .couldntCheck: ScreenReaderWords.couldntCheck
        case .unavailable: ScreenReaderWords.notInImage
        case .off, .on: nil
        case .switching(let on): on ? ScreenReaderWords.turningOn : ScreenReaderWords.turningOff
        case .failed(let on, _): ScreenReaderWords.didntChange(turningOn: on)
        case .waitsForTransfer: ScreenReaderWords.waitsForTransfer
        }
    }

    var isFailure: Bool {
        if case .failed = self { return true }
        return false
    }

    /// Something Try Again can repeat.
    var offersTryAgain: Bool {
        switch self {
        case .couldntCheck, .failed: true
        default: false
        }
    }
}

/// Android's screen reader in one session: what Android last said of it, the
/// change under way, the last change that failed and the card that asks about
/// it. A session that ends takes all of it along, and every task that
/// works on it checks it is still the session's (`AppModel+ScreenReader`).
@Observable
final class ScreenReaderSession {
    /// A failed change.
    struct Failure: Equatable {
        var turningOn: Bool
        var reason: String
    }

    /// The card over Android's screen while VoiceOver runs.
    enum Card: Equatable {
        /// Asks whether to turn the screen reader on.
        case ask
        /// Says the system image has none.
        case unavailable
    }

    /// Where a read stands: it applies only if no change began or ended since
    /// it started and no newer read has applied.
    struct Ticket: Equatable {
        fileprivate var generation: Int
        fileprivate var order: Int
    }

    /// Android's state as last read or changed; nil until it has been, and
    /// again after a read that got no answer.
    private(set) var state: ScreenReaderState?
    /// The last read got no answer.
    private(set) var readFailed = false
    /// The change under way: the state it turns to.
    private(set) var changing: Bool?
    /// The last change that failed, until another begins or Android shows the
    /// state that was asked for.
    private(set) var failure: Failure?
    /// The card over Android's screen, if one shows.
    var card: Card?
    /// Why the card's last Turn On did not turn it on.
    var cardFailure: String?
    /// The person answered the card's question with Not Now, or turned the
    /// screen reader off, in Settings or in Android: no card again in this
    /// session.
    var declined = false
    /// VoiceOver has been told that the screen reader is on, and how to come
    /// back, in this session: by the change that turned it on, or at the first
    /// read that found it on, where the display's own words did not carry it.
    var toldOn = false
    /// Android's first answer in this session found the screen reader on: it
    /// was on before the run began, and no change of this session turned it on.
    @ObservationIgnored private(set) var wasOnAtStart = false
    @ObservationIgnored private var answered = false
    /// The card that says the image has none has been shown in this session.
    var toldMissing = false
    /// How many Settings sheets watch the state.
    var settingsOpen = 0
    /// The sentence that says what the run is, announced as the run began, is
    /// still being spoken to VoiceOver: neither the card that asks nor the
    /// display's direct-touch element takes VoiceOver's focus until it has
    /// been.
    var runStartAnnouncing = false
    /// The sentence of the run's start is being spoken and Android's state is
    /// still unread: what VoiceOver says of the display on landing there is
    /// composed now and spoken after the sentence, so a value that says
    /// "checking" would be stale by then. The display carries none yet.
    var displayWaitsForRead: Bool { runStartAnnouncing && state == nil }
    /// Which announcement of the run's start `runStartAnnouncing` is for: the
    /// end of an earlier one does not release a later one.
    @ObservationIgnored var runStartGeneration = 0
    /// The loop that reads while VoiceOver runs.
    @ObservationIgnored var watcher: Task<Void, Never>?
    /// The session is over: nothing begins in it, and what was under way was
    /// cancelled.
    @ObservationIgnored private(set) var ended = false
    /// The tasks that work on Android for this session: a change, a read that
    /// Turn On asked for, the opening of Android's Settings. Each stays until
    /// it finishes, or until the session ends and cancels it.
    @ObservationIgnored private var operations: [Int: Task<Void, Never>] = [:]
    @ObservationIgnored private var operationCount = 0

    /// Android has shown the screen reader on in this session.
    @ObservationIgnored private var hasBeenOn = false
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var started = 0
    @ObservationIgnored private var applied = 0

    var isOn: Bool { state?.isOn == true }

    /// Stops the loop that reads.
    func stop() {
        watcher?.cancel()
        watcher = nil
    }

    /// How many operations are outstanding.
    var outstandingOperations: Int { operations.count }

    /// Starts work for this session, which keeps it until it finishes. Nothing
    /// starts once the session ended; `body` checks, before it asks anything of
    /// Android, that the session is still the model's.
    func run(_ body: @escaping @MainActor () async -> Void) {
        guard !ended else { return }
        operationCount += 1
        let key = operationCount
        operations[key] = Task { [weak self] in
            await body()
            self?.operations[key] = nil
        }
    }

    /// The session is over: the loop and every operation are cancelled, so
    /// that nothing queued reaches the run that follows.
    func end() {
        ended = true
        stop()
        let outstanding = operations.values
        operations = [:]
        for task in outstanding { task.cancel() }
    }

    // MARK: Reads

    func beginRead() -> Ticket {
        started += 1
        return Ticket(generation: generation, order: started)
    }

    /// Takes a read's answer unless a change or a newer read overtook it;
    /// an answer of nil says Android did not answer, and the state is then
    /// unknown rather than guessed. Returns whether it was taken.
    @discardableResult
    func apply(read answer: ScreenReaderState?, _ ticket: Ticket) -> Bool {
        guard ticket.generation == generation, changing == nil, ticket.order > applied else { return false }
        applied = ticket.order
        state = answer
        readFailed = answer == nil
        if let answer {
            if !answered {
                answered = true
                wasOnAtStart = answer.isOn
            }
            noticeTurnedOff(answer)
            // What the card said about not being able to ask is answered.
            if failure == nil { cardFailure = nil }
        }
        // The state a change asked for, with the list and `accessibility_enabled`
        // agreeing: a write that left them apart is not confirmed by the list.
        if let failure, let answer, answer.isOn == failure.turningOn, answer.isConsistent { self.failure = nil }
        return true
    }

    // MARK: Changes

    /// Starts a change; false while one is under way.
    func beginChange(turningOn on: Bool) -> Bool {
        guard changing == nil else { return false }
        generation += 1
        changing = on
        failure = nil
        cardFailure = nil
        return true
    }

    /// The change ended with the state Android reported.
    func finishChange(_ after: ScreenReaderState) {
        generation += 1
        changing = nil
        state = after
        readFailed = false
        answered = true
        noticeTurnedOff(after)
    }

    /// A screen reader that was on and is shown off was turned off, by the
    /// person in Settings or in Android: that answers the card's question as
    /// Not Now does.
    private func noticeTurnedOff(_ shown: ScreenReaderState) {
        if shown.isOn {
            hasBeenOn = true
        } else if hasBeenOn {
            declined = true
        }
    }

    /// The change did not happen. What Android has now is not known until a
    /// read says.
    func failChange(turningOn on: Bool, reason: String) {
        generation += 1
        changing = nil
        failure = Failure(turningOn: on, reason: reason)
        cardFailure = card == nil ? nil : reason
    }

    // MARK: Row

    func row(channelBusy: Bool) -> ScreenReaderRow {
        if let changing { return .switching(turningOn: changing) }
        if let failure { return .failed(turningOn: failure.turningOn, reason: failure.reason) }
        if let state, !state.isInstalled { return .unavailable }
        // Nothing is asked while a transfer holds the channel, so what is not
        // known yet waits for it too.
        if channelBusy { return .waitsForTransfer }
        guard let state else { return readFailed ? .couldntCheck : .checking }
        return state.isOn ? .on : .off
    }

    /// Whether the switch can be used now.
    func canChange(channelBusy: Bool) -> Bool {
        guard changing == nil, !channelBusy, let state else { return false }
        return state.isInstalled
    }
}
