// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import UIKit

/// A mock guest's screen reader, for the preview, which has no runtime behind
/// its screens. The application has none.
@MainActor
protocol ScreenReaderStandIn: AnyObject {
    /// The mock holds the adb channel, as a file transfer does.
    var holdsChannel: Bool { get }
    func read() async -> ScreenReaderState?
    func change(_ on: Bool, from state: ScreenReaderState) async throws -> ScreenReaderState
}

/// Android's screen reader in the session (`ScreenReaderSession`). Every
/// read and change binds to the session it began in and does nothing to a
/// later one; a read that a change overtook is dropped; and nothing turns the
/// screen reader on but the person's own action: the switch in Settings or
/// Turn On on the card.
extension AppModel {
    /// Whether the session offers the screen reader: a normal run, whose
    /// Android the runtime's adb channel reaches. Recovery has none.
    var offersScreenReader: Bool {
        guard capabilities.screenReader, case .running(.normal) = phase else { return false }
        return true
    }

    /// A file transfer or another task holds the adb channel: a read or a
    /// change would only wait for it.
    var screenReaderChannelBusy: Bool {
        busy != nil || fileBrowser?.running == true || screenReaderStandIn?.holdsChannel == true
    }

    /// What Settings shows under the switch.
    var screenReaderRow: ScreenReaderRow { session.reader.row(channelBusy: screenReaderChannelBusy) }

    var screenReaderCanChange: Bool {
        offersScreenReader && spacePause == nil && session.reader.canChange(channelBusy: screenReaderChannelBusy)
    }

    // MARK: Reading

    /// Reads Android's state, so the switch and the display show what Android's
    /// own settings last did. Settings asks while it is open and a loop asks
    /// while VoiceOver runs.
    func refreshScreenReader() async {
        // A guest paused for space is not asked: a connection waits for it.
        guard offersScreenReader, spacePause == nil, !screenReaderChannelBusy else { return }
        let current = session
        let reader = current.reader
        guard reader.changing == nil else { return }
        let ticket = reader.beginRead()
        let answer: ScreenReaderState?
        if let driver {
            answer = await driver.readScreenReader()
        } else {
            answer = await screenReaderStandIn?.read()
        }
        guard session === current, offersScreenReader else { return }
        if reader.apply(read: answer, ticket) { evaluateScreenReaderCard() }
    }

    /// A read for `current`, begun as the session's own operation: it does not
    /// reach a session that came after.
    private func refreshScreenReaderSoon(in current: SessionState) {
        current.reader.run { [weak self] in
            guard let self, session === current, !Task.isCancelled else { return }
            await refreshScreenReader()
        }
    }

    /// Reads again while VoiceOver runs, so the display follows a change made
    /// in Android within a few seconds. One loop per session; it ends with
    /// VoiceOver, with the session or with the run.
    func startScreenReaderWatch() {
        let current = session
        let reader = current.reader
        reader.stop()
        guard voiceOverRunning(), offersScreenReader else { return }
        reader.watcher = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, session === current, voiceOverRunning(), offersScreenReader else { return }
                await refreshScreenReader()
                try? await Task.sleep(for: screenReaderWatchInterval)
            }
        }
    }

    /// VoiceOver turned on or off.
    func voiceOverStatusChanged() {
        if voiceOverRunning() {
            startScreenReaderWatch()
        } else {
            session.reader.stop()
            session.reader.card = nil
        }
    }

    /// The first thing a run does about the screen reader: nothing without
    /// VoiceOver (Settings reads it when it opens), a loop of reads with it.
    func screenReaderBootFinished() {
        if voiceOverObserver == nil {
            voiceOverObserver = NotificationCenter.default.addObserver(
                forName: UIAccessibility.voiceOverStatusDidChangeNotification, object: nil, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.voiceOverStatusChanged() }
            }
        }
        startScreenReaderWatch()
    }

    // MARK: Changing

    /// Turns the screen reader on or off, and VoiceOver says how it ended. The
    /// service to add comes from the last read; the list it joins is read in
    /// the same command that writes it.
    func setScreenReader(_ on: Bool) {
        guard offersScreenReader, spacePause == nil, !screenReaderChannelBusy else { return }
        let current = session
        let reader = current.reader
        guard let state = reader.state, state.isInstalled else {
            refreshScreenReaderSoon(in: current)
            return
        }
        guard state.isOn != on, reader.beginChange(turningOn: on) else { return }
        let service = state.installedService ?? ScreenReader.services[0]
        let driver = driver
        let standIn = screenReaderStandIn
        // The operation belongs to this session: it asks nothing of Android
        // once the session ended, and the session's end cancels it where it
        // waits for Android.
        reader.run { [weak self] in
            guard let self, session === current, !Task.isCancelled else { return }
            do {
                let after: ScreenReaderState
                if let driver {
                    after = try await driver.changeScreenReader(on, service: service)
                } else if let standIn {
                    after = try await standIn.change(on, from: state)
                } else {
                    throw AdbFailure(.io(EIO), activity: .screenReader)
                }
                guard session === current, !Task.isCancelled else { return }
                // Android shows what was asked, its list and its switch in
                // agreement; the command ended well otherwise too, and a
                // half-made change is not one.
                guard after.isOn == on, after.isConsistent else {
                    reader.finishChange(after)
                    failScreenReader(reader, turningOn: on, reason: ScreenReaderWords.notReady)
                    return
                }
                reader.finishChange(after)
                if on {
                    // The way back is said with the news, once. While the run's
                    // start sentence is spoken it is not said over it: the
                    // sentence's end reads again, and says it as at a start.
                    if !reader.runStartAnnouncing {
                        reader.toldOn = true
                        announce(ScreenReaderWords.announcement(on: true, edge: session.keysEdge), highPriority: true)
                    }
                } else {
                    announce(ScreenReaderWords.announcement(on: false, edge: session.keysEdge))
                }
                evaluateScreenReaderCard()
            } catch {
                guard session === current, !Task.isCancelled, !(error is CancellationError) else { return }
                failScreenReader(reader, turningOn: on, reason: error.localizedDescription)
                await refreshScreenReader()
            }
        }
    }

    private func failScreenReader(_ reader: ScreenReaderSession, turningOn on: Bool, reason: String) {
        reader.failChange(turningOn: on, reason: reason)
        announce(ScreenReaderWords.changeFailure(turningOn: on, reason: reason))
    }

    /// Opens Android's Accessibility page; Settings closes for it. A failure
    /// that arrives after the session ended is not the next session's.
    func openAccessibilitySettings() {
        guard offersScreenReader, spacePause == nil, let driver else { return }
        let current = session
        current.reader.run { [weak self] in
            guard let self, session === current, !Task.isCancelled else { return }
            do {
                try await driver.openAccessibilitySettings()
            } catch {
                guard session === current, !Task.isCancelled, !(error is CancellationError) else { return }
                alert = AppAlert(title: ScreenReaderWords.androidSettings, message: error.localizedDescription)
            }
        }
    }

    // MARK: The card

    /// Puts the card over Android's screen, or takes it away, as what Android
    /// last said and the person's earlier answers decide. Only while VoiceOver
    /// runs, and never over Settings or another card.
    func evaluateScreenReaderCard() {
        let reader = session.reader
        guard offersScreenReader, voiceOverRunning() else {
            reader.card = nil
            return
        }
        guard let state = reader.state else { return }
        // The card waits for the run's start sentence: its focus would cut it.
        let canShow = spacePause == nil && busy == nil && sheet == nil && reader.settingsOpen == 0
            && reader.card == nil && !reader.runStartAnnouncing
        if !state.isInstalled {
            if reader.card == .ask { reader.card = nil }
            if canShow, !reader.toldMissing { present(.unavailable, in: reader) }
        } else if state.isOn {
            if reader.card == .ask { reader.card = nil }
            // Android's screen reader is on and VoiceOver is not yet told, once
            // (no card; nothing to ask), after the run's start sentence, not
            // over it. Where it was on before the run began and the display is
            // about to take VoiceOver's focus, nothing is said: the display's
            // own words carry the state and the way back. The news stays where
            // something covers the display, and after the person turns the
            // screen reader on, in Settings or in Android.
            if !reader.toldOn, reader.changing == nil, !reader.runStartAnnouncing {
                reader.toldOn = true
                if !(reader.wasOnAtStart && displayMayTakeFocus) {
                    announce(ScreenReaderWords.announcement(on: true, edge: session.keysEdge), highPriority: true)
                }
            }
        } else if canShow, !reader.declined, reader.changing == nil {
            present(.ask, in: reader)
        }
    }

    private func present(_ card: ScreenReaderSession.Card, in reader: ScreenReaderSession) {
        reader.card = card
        reader.cardFailure = nil
        if card == .unavailable { reader.toldMissing = true }
        // VoiceOver lands on the card's title and reads on from it: the card
        // takes the focus once it is on screen (`ScreenReaderCard`), where a
        // screen change posted now would find the display still first.
    }

    /// Turn On on the card; the card goes when the screen reader is on, and
    /// stays, with the reason, when it did not turn on. When nothing can be
    /// asked of Android just now it says why in Settings' words, and reads
    /// again; while the change runs a press says "Turning on…".
    func turnOnScreenReaderFromCard() {
        let reader = session.reader
        // The button stays enabled while the change runs, and a press says what
        // it shows: the change is under way, and no second one starts.
        if reader.changing != nil {
            announce(ScreenReaderWords.turningOn)
            return
        }
        let reason: String?
        if screenReaderChannelBusy {
            reason = ScreenReaderWords.waitsForTransfer
        } else if reader.state == nil, reader.changing == nil {
            reason = ScreenReaderWords.couldntCheck
        } else {
            reason = nil
        }
        guard let reason else {
            setScreenReader(true)
            return
        }
        reader.cardFailure = reason
        announce(ScreenReaderWords.changeFailure(turningOn: true, reason: reason))
        if reader.state == nil { refreshScreenReaderSoon(in: session) }
    }

    /// What VoiceOver hears as a run begins, in this order: the sentence that
    /// says what the run is and where the side keys are, announced at high
    /// priority (VoiceOver's own speech of the element its cursor lands on a
    /// moment after the screen change would cut a queued or default
    /// announcement off after a second); then, where Android's screen reader
    /// was off, the card that asks, or, where it was on and something covers
    /// the display, the news that it is on (`evaluateScreenReaderCard`), at
    /// high priority. Both wait for the sentence's end, or for
    /// `runStartBound` where VoiceOver reports none. VoiceOver adds speech of
    /// its own: the element it lands on after the screen change (the display,
    /// with its label alone until Android has been read), held behind the
    /// sentence, and its words again when the display becomes a direct-touch
    /// element, which carry the state and the way back where the screen
    /// reader was on at the start. The screen change's argument is no place
    /// for the sentence: VoiceOver does not speak it.
    func announceRunStart(_ phase: Phase) {
        VoiceOver.screenChanged()
        guard let words = RootView.spoken(phase), voiceOverRunning() else { return }
        let current = session
        let reader = current.reader
        reader.runStartGeneration += 1
        let generation = reader.runStartGeneration
        reader.runStartAnnouncing = true
        // Weak, so that an announcement whose end never comes (VoiceOver turned
        // off meanwhile) does not keep an ended session alive.
        let release: @MainActor () -> Void = { [weak self, weak current, weak reader] in
            guard let self, let current, let reader, current === self.session, !reader.ended,
                  reader.runStartAnnouncing, reader.runStartGeneration == generation else { return }
            reader.runStartAnnouncing = false
            evaluateScreenReaderCard()
        }
        runStartAnnouncer(words) { _ in release() }
        let bound = runStartBound
        Task { @MainActor in
            try? await Task.sleep(for: bound)
            release()
        }
    }

    /// Whether VoiceOver's focus may go to the display's direct-touch element:
    /// the run shows it with nothing over it, and the sentence of the run's
    /// start has been spoken.
    var displayMayTakeFocus: Bool {
        guard case .running = phase else { return false }
        let reader = session.reader
        return !reader.runStartAnnouncing && reader.card == nil && reader.settingsOpen == 0
            && busy == nil && sheet == nil && spacePause == nil && alert == nil
    }

    /// What VoiceOver is told of its own accord (`announcer` is a test's). At
    /// high priority it is not cut off by VoiceOver's own speech of the element
    /// its cursor lands on.
    func announce(_ text: String, queued: Bool = false, highPriority: Bool = false) {
        announcer(text, queued, highPriority)
    }

    /// Not Now on the card: no card again in this session.
    func declineScreenReaderCard() {
        session.reader.declined = true
        session.reader.card = nil
        VoiceOver.screenChanged()
    }

    /// OK on the card that says the image has none.
    func dismissScreenReaderCard() {
        session.reader.card = nil
        VoiceOver.screenChanged()
    }

    /// While Settings is open the card does not rise over it. The reader it
    /// returns is the session's that Settings opened in, which closing gives back.
    func settingsOpened() -> ScreenReaderSession {
        let reader = session.reader
        reader.settingsOpen += 1
        return reader
    }

    func settingsClosed(_ reader: ScreenReaderSession) {
        reader.settingsOpen = max(0, reader.settingsOpen - 1)
        if reader === session.reader, reader.settingsOpen == 0 { evaluateScreenReaderCard() }
    }
}
