// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The text message bar as the model plays it: a send and its answer belong
/// to the run, typing goes on while an answer is awaited, a paused guest
/// takes nothing, and the entrance is held where the application does not
/// carry text messages.
@MainActor
@Suite(.serialized)
struct TextMessageTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    /// Holds sends and the wait before the bar rises until a test lets them
    /// go, so a test decides what happens between a send and its answer.
    @MainActor
    private final class Held<Value: Sendable> {
        private var waiting: [CheckedContinuation<Value, Never>] = []
        /// What was let go before anything waited: the next wait has it at once.
        private var early: [Value] = []
        var pending: Int { waiting.count }
        func wait() async -> Value {
            if !early.isEmpty { return early.removeFirst() }
            return await withCheckedContinuation { waiting.append($0) }
        }
        func release(_ value: Value) {
            if waiting.isEmpty { early.append(value) } else { waiting.removeFirst().resume(returning: value) }
        }
    }

    /// What VoiceOver was told, and the reports of how each announcement ended, which a test sends by hand.
    @MainActor
    private final class Said {
        var lines: [String] = []
        var finishes: [@MainActor @Sendable (Bool) -> Void] = []
        /// Reports the last announcement as spoken to its end, or dropped.
        func finishLast(_ successfully: Bool) { finishes.last?(successfully) }
    }

    private struct Rig {
        let model: AppModel
        let driver: StubDriver
        let answers: Held<SmsReply>
        let said: Said
        let memory: GuestSms.SenderMemory
        let cleanUp: () -> Void

        var state: TextMessageState { model.session.textMessage }
        var files: RecordingNoticeFiles { driver.noticeFiles }
    }

    private static let noticeURL = TextMessageNotice.url(in: URL(fileURLWithPath: "/instance"))

    /// A running session of an application that carries text messages, whose
    /// sends wait for the test and whose kept sender is in a database of its own.
    /// `files` is another launch's notice file, when a test relaunches.
    private func rig(
        capabilities: Capabilities = .preview, mode: StartMode = .normal, voiceOver: Bool = false,
        files: RecordingNoticeFiles? = nil
    ) throws -> Rig {
        let suite = "TextMessageTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suite))
        let memory = GuestSms.SenderMemory(defaults: defaults)
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: capabilities, createRequirementBytes: 1
        )
        model.smsSender = memory
        let said = Said()
        model.textMessageAnnouncer = { words, finished in
            said.lines.append(words)
            said.finishes.append(finished)
        }
        model.textMessageVoiceOver = { voiceOver }
        model.textMessageSleep = { _ in }
        let answers = Held<SmsReply>()
        let driver = StubDriver(plan: .normal)
        driver.smsAnswer = { _ in await answers.wait() }
        // Another launch over the same instance: its notice reads what the first wrote.
        if let files { driver.noticeFiles = files }
        model.driver = driver
        model.phase = .running(mode)
        return Rig(model: model, driver: driver, answers: answers, said: said, memory: memory, cleanUp: {
            defaults.removePersistentDomain(forName: suite)
        })
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    /// Long enough for a task that was let go to do whatever it will.
    private func settle() async {
        try? await Task.sleep(for: .milliseconds(80))
    }

    // MARK: A send and what is typed meanwhile

    @Test func theFieldsRefuseEditsWhileAMessageIsBeingSent() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("one")
        let sender = state.sender
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        #expect(state.progress == .sending && !model.canSendTextMessage)
        // Nothing is typed while Android has not answered: what goes is what stays.
        model.setTextMessageText("two")
        model.setTextMessageSender("Other")
        #expect(state.text == "one" && state.sender == sender)
        rig.answers.release(.handedOver(1))
        #expect(await eventually { state.progress == .sent })
        #expect(state.text.isEmpty)
        #expect(rig.said.lines == [SmsMessages.sentToAndroid])
        // Typing after the answer is a new message, which nothing says was sent, and it can go.
        model.setTextMessageText("two")
        #expect(state.status(availability: .ready) == nil)
        #expect(model.canSendTextMessage)
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedOver(1))
        #expect(await eventually { state.progress == .sent && state.text.isEmpty })
        #expect(rig.driver.smsSent.map(\.text) == ["one", "two"])
        #expect(state.status(availability: .ready) == TextMessageStatus(words: SmsMessages.sentToAndroid, tone: .good))
        #expect(rig.said.lines == [SmsMessages.sentToAndroid, SmsMessages.sentToAndroid])
    }

    @Test func aFieldWritingItsTextBackIsNotTyping() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedOver(1))
        #expect(await eventually { state.progress == .sent })
        // Laying the emptied field out writes the same text and sender back.
        model.setTextMessageText("")
        model.setTextMessageSender(state.sender)
        #expect(state.progress == .sent)
        #expect(state.status(availability: .ready)?.words == SmsMessages.sentToAndroid)
        model.setTextMessageText("t")
        #expect(state.progress == .idle)
    }

    @Test func aSecondSendWaitsForTheFirstAnswer() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        await settle()
        #expect(rig.driver.smsSent.count == 1)
        rig.answers.release(.handedOver(1))
        #expect(await eventually { rig.state.progress == .sent })
    }

    @Test func closingWhileAMessageIsBeingSentKeepsTheBarUp() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        model.closeTextMessage()
        #expect(rig.state.isOpen && rig.state.text == "one")
        rig.answers.release(.handedOver(1))
        #expect(await eventually { rig.state.progress == .sent })
        model.closeTextMessage()
        #expect(!rig.state.isOpen && rig.state.text.isEmpty && rig.state.progress == .idle)
    }

    // MARK: How a send ends (S4, S5)

    /// Types `text`, sends, lets the guest's reply go and waits until the send ended.
    private func send(_ rig: Rig, _ text: String, _ reply: SmsReply) async -> Bool {
        rig.model.setTextMessageText(text)
        rig.model.sendTextMessage()
        guard await eventually({ rig.answers.pending == 1 }) else { return false }
        rig.answers.release(reply)
        return await eventually { rig.state.progress != .sending }
    }

    private func noticeText(_ rig: Rig) -> String {
        rig.files.contents(Self.noticeURL).map { String(decoding: $0, as: UTF8.self) } ?? "(none)"
    }

    private let header = "kernarium-sms-pending 1\n"

    /// Each reply to a first attempt is the state the table says, says its own words, keeps or empties the
    /// box, and decides whether the next press of the draft is a retry (only a message not confirmed) or a
    /// new message.
    @Test func eachAnswerToAFirstAttemptIsTheStateTheTableSays() async throws {
        let other = String(repeating: "c", count: 32)
        typealias Row = (String, SmsReply, TextMessageState.Progress, String)
        let rows: [Row] = [
            ("not sent", .notSent, .certainlyNotSent, SmsMessages.notSent),
            ("not taken", .notTaken, .certainlyNotSent, SmsMessages.notSent),
            ("no answer", .noAnswer, .notConfirmed, SmsMessages.notConfirmed),
            ("handed over", .handedOver(2), .sent, SmsMessages.sentToAndroid),
            ("handed over earlier", .handedOverEarlier(1), .deliveredEarlier, SmsMessages.deliveredEarlier),
            ("not handed over, invalid", .notHandedOver("invalid"), .certainlyNotSent, SmsMessages.notSent),
            ("not handed over, plan", .notHandedOver("plan"), .certainlyNotSent, SmsMessages.notSent),
            ("not handed over, store", .notHandedOver("store"), .certainlyNotSent, SmsMessages.notSent),
            ("uncertain, interrupted", .uncertain("interrupted"), .uncertain, SmsMessages.uncertain),
            ("uncertain, unknown", .uncertain("unknown"), .uncertain, SmsMessages.uncertain),
            ("uncertain, conflict", .uncertain("conflict"), .uncertain, SmsMessages.uncertain),
            ("version refusal", .refusedVersion, .certainlyNotSent, SmsMessages.notSent),
            ("unsettled", .unsettled, .notConfirmed, SmsMessages.notConfirmed),
            ("malformed", .bytes("garbage"), .notConfirmed, SmsMessages.notConfirmed),
            ("another identifier's", .bytes("2.handed-over.\(other).1"), .notConfirmed, SmsMessages.notConfirmed),
        ]
        for (name, reply, progress, words) in rows {
            let rig = try rig()
            defer { rig.cleanUp() }
            let (model, state) = (rig.model, rig.state)
            model.openTextMessage()
            #expect(await send(rig, "one", reply), Comment(rawValue: name))
            #expect(state.progress == progress, Comment(rawValue: name))
            // Its own words, said once, when it was reached.
            #expect(rig.said.lines == [words], Comment(rawValue: name))
            // The box is emptied only by a message that went.
            let went = progress == .sent || progress == .deliveredEarlier
            #expect((state.text == "one") == !went, Comment(rawValue: name))
            if !went { #expect(state.status(availability: .ready) == TextMessageStatus(words: words, tone: .problem), Comment(rawValue: name)) }
            // The notice: a message not confirmed waits for its answer; a final one was shown and is forgotten.
            #expect(await eventually { (progress == .notConfirmed) == (noticeText(rig) != header) }, Comment(rawValue: name))
            // The next press of the same draft.
            model.setTextMessageText("one")
            model.sendTextMessage()
            #expect(await eventually { rig.answers.pending == 1 }, Comment(rawValue: name))
            let first = rig.driver.smsSent[0], second = rig.driver.smsSent[1]
            if progress == .notConfirmed {
                #expect(second.attempt == .retry && second.identifier == first.identifier, Comment(rawValue: name))
            } else {
                #expect(second.attempt == .first && second.identifier != first.identifier, Comment(rawValue: name))
            }
            rig.answers.release(.noAnswer)
        }
    }

    /// The first attempt's answer is lost and its retries cannot get through: one identifier throughout, and
    /// the press after those asks, and is answered that it was handed over earlier.
    @Test func aRetryThatCannotGetThroughNeverMakesTheMessageANewOne() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "one", .noAnswer))
        #expect(state.progress == .notConfirmed)
        for reply in [SmsReply.notSent, .notTaken, .noAnswer, .unsettled, .refusedVersion] {
            model.sendTextMessage()
            #expect(await eventually { rig.answers.pending == 1 })
            rig.answers.release(reply)
            #expect(await eventually { state.progress != .sending })
            #expect(state.progress == .notConfirmed, "\(reply)")
            #expect(state.status(availability: .ready)?.words == SmsMessages.notConfirmed)
        }
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedOverEarlier(1))
        #expect(await eventually { state.progress == .deliveredEarlier })
        #expect(state.text.isEmpty)
        let sent = rig.driver.smsSent
        #expect(sent.count == 7 && Set(sent.map(\.identifier)).count == 1)
        #expect(sent.map(\.attempt) == [.first] + Array(repeating: GuestSms.Attempt.retry, count: 6))
        // The identifier is forgotten once the result was shown: the same words are a new message.
        #expect(await send(rig, "one", .handedOver(1)))
        #expect(rig.driver.smsSent[7].attempt == .first && rig.driver.smsSent[7].identifier != sent[0].identifier)
    }

    /// An answer a retry cannot get (handed over, not handed over, another identifier's) is not confirmed, and
    /// the identifier is kept.
    @Test func aRetryIsNeverToldItHandedTheMessageOver() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "one", .noAnswer))
        for reply in [SmsReply.handedOver(1), .notHandedOver("store"), .bytes("2.uncertain.\(String(repeating: "d", count: 32)).unknown")] {
            model.sendTextMessage()
            #expect(await eventually { rig.answers.pending == 1 })
            rig.answers.release(reply)
            #expect(await eventually { state.progress != .sending })
            #expect(state.progress == .notConfirmed, "\(reply)")
        }
        #expect(Set(rig.driver.smsSent.map(\.identifier)).count == 1)
    }

    // MARK: Uncertain and cut off (S7, S8)

    @Test func theSameWordsAfterUncertainAreANewMessage() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "one", .uncertain("unknown")))
        #expect(state.progress == .uncertain && state.text == "one")
        // The result was shown at once (no VoiceOver), so the same words go as a first attempt with a new identifier.
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        let sent = rig.driver.smsSent
        #expect(sent.count == 2 && sent[1].attempt == .first && sent[1].identifier != sent[0].identifier)
        rig.answers.release(.handedOver(1))
        #expect(await eventually { state.progress == .sent })
    }

    /// Under VoiceOver a result nobody was told is not superseded: Send sends nothing, says it again and takes
    /// VoiceOver to the line; once the announcement ended, or focus reached the line, Send works.
    @Test func aResultNobodyWasToldIsNotSupersededUnderVoiceOver() async throws {
        let rig = try rig(voiceOver: true)
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "one", .uncertain("unknown")))
        #expect(rig.said.lines == [SmsMessages.uncertain])
        #expect(state.awaiting != nil)
        // The entry stays in the notice until the announcement is done.
        #expect(rig.driver.textMessageNotice.currentEntries.count == 1)
        let focuses = state.statusFocusRequests
        model.sendTextMessage()
        await settle()
        #expect(rig.driver.smsSent.count == 1, "a send went before the result was told")
        #expect(rig.said.lines == [SmsMessages.uncertain, SmsMessages.uncertain])
        #expect(state.statusFocusRequests == focuses + 1)
        // An announcement VoiceOver dropped does not count.
        rig.said.finishLast(false)
        await settle()
        #expect(state.awaiting != nil && rig.driver.textMessageNotice.currentEntries.count == 1)
        model.sendTextMessage()
        await settle()
        #expect(rig.driver.smsSent.count == 1)
        // Spoken to its end: it is told, and the entry goes.
        rig.said.finishLast(true)
        #expect(await eventually { state.awaiting == nil && noticeText(rig) == header })
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        #expect(rig.driver.smsSent.count == 2 && rig.driver.smsSent[1].attempt == .first)
        rig.answers.release(.handedOver(1))
    }

    /// Focus reaching the line counts as told, announcement or not.
    @Test func voiceOverFocusOnTheLineCountsAsTold() async throws {
        let rig = try rig(voiceOver: true)
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "one", .handedOver(1)))
        #expect(state.awaiting != nil)
        model.textMessageLineFocused()
        #expect(await eventually { state.awaiting == nil && noticeText(rig) == header })
    }

    /// The line is what is told: a result nobody was told stays on it whatever is typed after the result, so a
    /// count, a refusal or the cap never stands where the result was, and VoiceOver's focus or Send's repeat
    /// reaches the result's words.
    @Test func anUntoldResultStaysOnTheLineWhateverIsTypedAfterIt() async throws {
        let rig = try rig(voiceOver: true)
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "one", .uncertain("unknown")))
        // The announcement was dropped: the result is still to be told.
        rig.said.finishLast(false)
        await settle()
        #expect(state.awaiting != nil && rig.driver.textMessageNotice.currentEntries.count == 1)
        let drafts: [(sender: String, text: String)] = [
            (GuestSms.defaultSender, String(repeating: "a", count: GuestSms.maximumUnits - 40)),
            (GuestSms.defaultSender, String(repeating: "a", count: GuestSms.maximumUnits + 12)),
            (GuestSms.defaultSender, "Code \u{FFFE} 482913"),
            ("Shop & Go", "two"),
        ]
        let awaited = TextMessageStatus(words: SmsMessages.uncertain, tone: .problem)
        for draft in drafts {
            model.setTextMessageSender(draft.sender)
            model.setTextMessageText(draft.text)
            #expect(state.status(availability: .ready) == awaited, "the line shows the result under \(draft.text.prefix(12))")
        }
        // Send, with the result untold, sends nothing and names it again.
        let focuses = state.statusFocusRequests
        model.sendTextMessage()
        await settle()
        #expect(rig.driver.smsSent.count == 1)
        #expect(rig.said.lines.last == SmsMessages.uncertain && state.statusFocusRequests == focuses + 1)
        // Focus reached the line, which says the result: it is told, and the draft's own line follows.
        model.textMessageLineFocused()
        #expect(await eventually { state.awaiting == nil && noticeText(rig) == header })
        #expect(state.status(availability: .ready)?.words != SmsMessages.uncertain)
    }

    /// A line that is not the result's (Android is paused, and says so) is never counted as the result told:
    /// not by VoiceOver's focus, and not without VoiceOver, where the result waits for its line to be on screen.
    @Test func aLineThatIsNotTheResultsIsNeverCountedAsTheResultTold() async throws {
        let withVoiceOver = try rig(voiceOver: true)
        defer { withVoiceOver.cleanUp() }
        withVoiceOver.model.openTextMessage()
        #expect(await send(withVoiceOver, "one", .uncertain("unknown")))
        withVoiceOver.said.finishLast(false)
        withVoiceOver.model.guestPausingForSpace(freeBytes: 100_000_000)
        #expect(withVoiceOver.state.status(availability: withVoiceOver.model.textMessageAvailability)?.words == SmsMessages.paused)
        withVoiceOver.model.textMessageLineFocused()
        await settle()
        #expect(withVoiceOver.state.awaiting != nil && withVoiceOver.driver.textMessageNotice.currentEntries.count == 1)
        withVoiceOver.model.spacePause = nil
        withVoiceOver.model.textMessageLineFocused()
        #expect(await eventually { withVoiceOver.state.awaiting == nil && noticeText(withVoiceOver) == header })

        // Without VoiceOver the answer comes while Android is paused: the pause's line is on screen, not the
        // result's, and the result is told once the pause ends (the bar asks the model when availability changes).
        let plain = try rig()
        defer { plain.cleanUp() }
        plain.model.openTextMessage()
        plain.model.setTextMessageText("one")
        plain.model.sendTextMessage()
        #expect(await eventually { plain.answers.pending == 1 })
        plain.model.guestPausingForSpace(freeBytes: 100_000_000)
        plain.answers.release(.uncertain("unknown"))
        #expect(await eventually { plain.state.progress == .uncertain })
        await settle()
        #expect(plain.state.awaiting != nil && plain.driver.textMessageNotice.currentEntries.count == 1)
        plain.model.spacePause = nil
        plain.model.presentTextMessageLine()
        #expect(await eventually { plain.state.awaiting == nil && noticeText(plain) == header })
    }

    @Test func aRunThatEndedWithTheAnswerAwaitedLeavesTheCutOffLineForTheNextOpening() async throws {
        for restart in [false, true] {
            let rig = try rig()
            defer { rig.cleanUp() }
            let model = rig.model
            model.openTextMessage()
            model.setTextMessageText("one")
            model.sendTextMessage()
            #expect(await eventually { rig.answers.pending == 1 })
            if restart { model.restart() } else { model.runEnded(RunEnd(result: 0), uptimeSeconds: 1) }
            rig.answers.release(.handedOver(1))
            await settle()
            // The notice was written before the dispatch and outlives the run.
            #expect(noticeText(rig).contains(rig.driver.smsSent[0].identifier))
            model.phase = .running(.normal)
            model.openTextMessage()
            let state = model.session.textMessage
            #expect(state.progress == .cutOff && state.text.isEmpty)
            #expect(state.status(availability: .ready) == TextMessageStatus(words: SmsMessages.cutOff, tone: .problem))
            #expect(rig.said.lines == [SmsMessages.cutOff])
            // Shown at once (no VoiceOver): the flag goes and the file with it.
            #expect(await eventually { noticeText(rig) == header })
            #expect(!rig.driver.textMessageNotice.cutOffPending)
            // The same words sent now are a new message.
            #expect(await send(rig, "one", .handedOver(1)))
            #expect(rig.driver.smsSent.count == 2 && rig.driver.smsSent[1].attempt == .first)
            #expect(rig.driver.smsSent[1].identifier != rig.driver.smsSent[0].identifier)
        }
    }

    // MARK: The notice (S10)

    @Test func theNoticeIsWrittenAndFlushedBeforeAnythingIsDispatched() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        let seen = SeenText()
        rig.driver.smsAnswer = { [files = rig.files] _ in
            seen.set(files.contents(Self.noticeURL).map { String(decoding: $0, as: UTF8.self) })
            return .handedOver(1)
        }
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.state.progress == .sent })
        let identifier = rig.driver.smsSent[0].identifier
        #expect(seen.value == header + identifier + "\n")
        // The sequence: replace, flush the file, flush the directory, then the channel; and the same three for the removal.
        let three = ["replace", "flushFile", "flushDirectory"]
        #expect(await eventually { rig.files.calls == three + three })
    }

    private final class SeenText: @unchecked Sendable {
        private let lock = NSLock()
        private var text: String?
        var value: String? { lock.withLock { text } }
        func set(_ value: String?) { lock.withLock { text = value } }
    }

    @Test func aNoticeThatCannotBeWrittenSendsNothingAndTheMessageCertainlyDidNotGo() async throws {
        for step in ["replace", "flushFile", "flushDirectory"] {
            let rig = try rig()
            defer { rig.cleanUp() }
            let (model, state) = (rig.model, rig.state)
            model.openTextMessage()
            rig.files.fail(step)
            model.setTextMessageText("one")
            model.sendTextMessage()
            #expect(await eventually { state.progress == .certainlyNotSent }, Comment(rawValue: step))
            #expect(rig.driver.smsSent.isEmpty && rig.answers.pending == 0, Comment(rawValue: step))
            #expect(rig.said.lines == [SmsMessages.notSent], Comment(rawValue: step))
            #expect(rig.driver.textMessageNotice.currentEntries.isEmpty, Comment(rawValue: step))
            #expect(!state.retries.holds(sender: state.sender, text: "one"), Comment(rawValue: step))
            #expect(state.text == "one", Comment(rawValue: step))
        }
    }

    /// X unresolved; the add of Y fails; the add of Z succeeds: the file kept X through the failure and holds X and Z.
    @Test func aFailedAddKeepsTheEarlierEntriesInTheFile() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "x", .noAnswer))
        let x = rig.driver.smsSent[0].identifier
        rig.files.fail("replace")
        model.setTextMessageText("y")
        model.sendTextMessage()
        #expect(await eventually { state.progress == .certainlyNotSent })
        #expect(noticeText(rig) == header + x + "\n")
        rig.files.stop()
        #expect(await send(rig, "z", .noAnswer))
        let z = rig.driver.smsSent[1].identifier
        #expect(rig.driver.smsSent.map(\.text) == ["x", "z"])
        #expect(noticeText(rig) == header + x + "\n" + z + "\n")
    }

    /// An answer that comes while the application is inactive is not told: the entry stays, a new launch says
    /// cut off, and the line is told once the application is active again.
    @Test func aResultThatCameWhileTheApplicationWasInactiveIsNotToldYet() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.textMessageSceneChanged(active: false)
        #expect(await send(rig, "one", .handedOver(1)))
        await settle()
        let identifier = rig.driver.smsSent[0].identifier
        #expect(state.awaiting == .result(identifier))
        #expect(noticeText(rig) == header + identifier + "\n")
        // Another launch over the same file: cut off.
        let relaunched = try self.rig(files: rig.files)
        defer { relaunched.cleanUp() }
        relaunched.model.openTextMessage()
        #expect(relaunched.state.progress == .cutOff)
        // The application is active again, and the line is on screen: told.
        model.textMessageSceneChanged(active: true)
        #expect(await eventually { state.awaiting == nil })
        #expect(await eventually { noticeText(rig) == header })
    }

    /// The overlay as the application has it: a view in a hosting controller that UIKit owns, where SwiftUI's
    /// `scenePhase` stays at .background and never says the application is active. The model must still learn
    /// it from UIKit: the flag follows the application's own state at the overlay's first appearance and its
    /// become-active and resign-active notifications, and a result on the open bar is told, which is what
    /// lets Send take the next message.
    @Test func theOverlayInAUIKitHostedViewKeepsTheModelsActiveFlagAndToldLinesFollow() async throws {
        try #require(UIApplication.shared.applicationState == .active, "the test host is the active application")
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        // What a scene phase of .background left the flag as.
        model.textMessageSceneActive = false
        let lease = try await HostWindow.take()
        defer { lease.give() }
        try await hosted(TextMessageBarOverlay().environment(model), in: lease, height: 900, width: 390) { _ in
            #expect(await eventually { model.textMessageSceneActive }, "the overlay read the application's state")
            // The result of a send on the open bar is told at once, and the identifier it was kept for goes.
            model.openTextMessage()
            #expect(await send(rig, "one", .handedOver(1)))
            #expect(await eventually { state.awaiting == nil && noticeText(rig) == header })
            // Typing after a told result is a new message, which Send takes.
            model.setTextMessageText("two")
            #expect(model.canSendTextMessage)

            // Resigning active: the next result is not told while the application is not active...
            NotificationCenter.default.post(name: UIApplication.willResignActiveNotification, object: UIApplication.shared)
            #expect(await eventually { !model.textMessageSceneActive })
            #expect(await send(rig, "two", .handedOver(1)))
            await settle()
            try #require(rig.driver.smsSent.count == 2)
            let identifier = rig.driver.smsSent[1].identifier
            #expect(state.awaiting == .result(identifier))
            // ... and is, once the application is active again.
            NotificationCenter.default.post(name: UIApplication.didBecomeActiveNotification, object: UIApplication.shared)
            #expect(await eventually { model.textMessageSceneActive })
            #expect(await eventually { state.awaiting == nil && noticeText(rig) == header })
        }
    }

    @Test func aLateAnswerOfAnEndedSessionTouchesNothingOfANewerSend() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        model.restart()
        model.phase = .running(.normal)
        // The new session's send, under way while the old one's answer is still to come.
        model.openTextMessage()
        model.setTextMessageText("two")
        model.sendTextMessage()
        #expect(await eventually { rig.driver.textMessageNotice.currentEntries.count == 1 })
        let entry = rig.driver.textMessageNotice.currentEntries
        rig.answers.release(.handedOver(1))
        await settle()
        #expect(rig.driver.textMessageNotice.currentEntries == entry)
        #expect(model.session.textMessage.progress == .sending)
    }

    @Test func anotherInstanceHasNoNoticeOfThisOnesMessages() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        #expect(await send(rig, "one", .noAnswer))
        rig.model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        let other = try self.rig()
        defer { other.cleanUp() }
        other.model.openTextMessage()
        #expect(other.state.progress == .idle && !other.driver.textMessageNotice.cutOffPending)
    }

    @Test func aDamagedNoticeSaysCutOffOnceAndTheFileIsValidAfterwards() async throws {
        let files = RecordingNoticeFiles()
        files.put(Data("garbage".utf8), at: Self.noticeURL)
        let rig = try rig(files: files)
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        #expect(rig.state.progress == .cutOff)
        #expect(await eventually { noticeText(rig) == header })
        let again = try self.rig(files: files)
        defer { again.cleanUp() }
        again.model.openTextMessage()
        #expect(again.state.progress == .idle)
    }

    /// A removal that cannot be written changes nothing for this session; a new launch says cut off once.
    @Test func aRemovalThatFailedMakesTheNextLaunchSayCutOffOnce() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        rig.driver.smsAnswer = { [files = rig.files] _ in
            // The removal is the write after this answer: it fails.
            files.fail("replace")
            return .handedOver(1)
        }
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { state.progress == .sent })
        await settle()
        #expect(state.awaiting == nil && rig.driver.textMessageNotice.currentEntries.isEmpty)
        rig.files.stop()
        let relaunched = try self.rig(files: rig.files)
        defer { relaunched.cleanUp() }
        relaunched.model.openTextMessage()
        #expect(relaunched.state.progress == .cutOff)
        #expect(await eventually { noticeText(relaunched) == header })
        let third = try self.rig(files: rig.files)
        defer { third.cleanUp() }
        third.model.openTextMessage()
        #expect(third.state.progress == .idle)
    }

    /// A session that ends without the bar opening adds nothing and clears nothing.
    @Test func aSessionThatNeverOpenedTheBarClearsNothing() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        model.restart()
        rig.answers.release(.noAnswer)
        await settle()
        model.phase = .running(.normal)
        // The next session runs and ends without the bar.
        model.restart()
        model.phase = .running(.normal)
        model.openTextMessage()
        #expect(model.session.textMessage.progress == .cutOff)
    }

    @Test func aResultNotToldBeforeTheBarClosedBecomesTheCutOffLine() async throws {
        let rig = try rig(voiceOver: true)
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "one", .handedOver(1)))
        #expect(state.awaiting != nil)
        model.closeTextMessage()
        #expect(!state.isOpen && rig.driver.textMessageNotice.currentEntries.isEmpty)
        #expect(rig.driver.textMessageNotice.cutOffPending)
        model.openTextMessage()
        #expect(model.session.textMessage.progress == .cutOff)
    }

    /// Under VoiceOver the cut-off line is read first: no keyboard focus move, the words announced and VoiceOver
    /// sent to the line; it counts as told only when its announcement ended or focus reached it.
    @Test func theCutOffLineUnderVoiceOverIsToldOnlyWhenItWasSpoken() async throws {
        let files = RecordingNoticeFiles()
        files.put(Data((header + "cut-off\n").utf8), at: Self.noticeURL)
        let rig = try rig(voiceOver: true, files: files)
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(state.skipsKeyboardFocus && state.progress == .cutOff)
        #expect(rig.said.lines == [SmsMessages.cutOff] && state.statusFocusRequests == 1)
        rig.said.finishLast(false)
        await settle()
        #expect(rig.driver.textMessageNotice.cutOffPending)
        // No Send for a new draft before it was told: the press says the line again.
        model.setTextMessageText("one")
        model.sendTextMessage()
        await settle()
        #expect(rig.driver.smsSent.isEmpty && rig.said.lines == [SmsMessages.cutOff, SmsMessages.cutOff])
        #expect(state.statusFocusRequests == 2)
        rig.said.finishLast(true)
        #expect(await eventually { !rig.driver.textMessageNotice.cutOffPending && noticeText(rig) == header })
        // Without VoiceOver the line has no such rule: the bar opens, the line is told at once.
        let plain = try self.rig(files: { let f = RecordingNoticeFiles(); f.put(Data((header + "cut-off\n").utf8), at: Self.noticeURL); return f }())
        defer { plain.cleanUp() }
        plain.model.openTextMessage()
        #expect(!plain.state.skipsKeyboardFocus)
        #expect(await eventually { !plain.driver.textMessageNotice.cutOffPending })
    }

    // MARK: Sixteen unresolved (S11)

    @Test func sixteenUnresolvedMessagesRefuseANewDraftAndStillSendAHeldOneAsARetry() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        for number in 0..<GuestSms.Retries.capacity {
            #expect(await send(rig, "draft \(number)", .noAnswer), "\(number)")
        }
        #expect(rig.driver.textMessageNotice.currentEntries.count == 16)
        // A new draft: the waiting line before Send, and Send unavailable. Nothing is dispatched, no identifier
        // is made and nothing enters the notice.
        model.setTextMessageText("a new one")
        #expect(state.status(availability: .ready) == TextMessageStatus(words: SmsMessages.waiting, tone: .problem))
        #expect(!model.canSendTextMessage)
        model.sendTextMessage()
        await settle()
        #expect(rig.driver.smsSent.count == 16 && rig.driver.textMessageNotice.currentEntries.count == 16)
        // A held draft goes as a retry, with the identifier it had.
        model.setTextMessageText("draft 3")
        #expect(model.canSendTextMessage)
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        #expect(rig.driver.smsSent[16].attempt == .retry && rig.driver.smsSent[16].identifier == rig.driver.smsSent[3].identifier)
        rig.answers.release(.handedOverEarlier(1))
        #expect(await eventually { state.progress == .deliveredEarlier })
        // Its result, shown, lowers the count: the new draft can go.
        model.setTextMessageText("a new one")
        #expect(model.canSendTextMessage)
        // A restart clears the count, and the next opening says cut off.
        model.restart()
        model.phase = .running(.normal)
        model.openTextMessage()
        #expect(model.session.textMessage.retries.unresolvedCount == 0)
        #expect(model.session.textMessage.progress == .cutOff)
    }

    // MARK: The run

    @Test func theEndOfTheRunDropsTheSendAndItsLateAnswer() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        let earlier = rig.state
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        #expect(model.session.textMessage !== earlier)
        #expect(earlier.ended && !earlier.isOpen && earlier.text.isEmpty)
        rig.answers.release(.handedOver(1))
        await settle()
        #expect(rig.said.lines.isEmpty)
        #expect(earlier.progress == .idle)
        #expect(!model.session.textMessage.isOpen && model.session.textMessage.progress == .idle)
    }

    @Test func aRestartDropsItAndTheNewRunShowsNothingOfIt() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        let earlier = rig.state
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        model.restart()
        #expect(earlier.ended && !earlier.isOpen)
        rig.answers.release(.handedOver(1))
        await settle()
        #expect(rig.said.lines.isEmpty)
        // The run that follows has its own bar: nothing typed, and the one thing it says of the earlier
        // run is that a message sent then may or may not have arrived.
        model.phase = .running(.normal)
        #expect(!model.session.textMessage.isOpen)
        model.openTextMessage()
        #expect(model.session.textMessage.isOpen && model.session.textMessage.text.isEmpty)
        #expect(model.session.textMessage.progress == .cutOff)
        #expect(model.session.textMessage.status(availability: .ready)?.words == SmsMessages.cutOff)
    }

    @Test func aRunThatFailedLeavesNoBarEither() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        model.setTextMessageText("one")
        model.runEnded(RunEnd(result: 1), uptimeSeconds: 1)
        #expect(!rig.state.isOpen && rig.state.ended)
        // A bar that came up late for that run does not open.
        model.openTextMessage()
        #expect(!rig.state.isOpen)
    }

    @Test func aRestartBeforeTheBarComesUpLeavesNoBarInTheNewRun() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        let wait = Held<Void>()
        model.textMessageSleep = { _ in await wait.wait() }
        model.presentTextMessage()
        #expect(await eventually { wait.pending == 1 })
        model.restart()
        model.phase = .running(.normal)
        wait.release(())
        await settle()
        #expect(!model.session.textMessage.isOpen)
    }

    @Test func settingsClosedAfterTheBarWentUpLeavesWhatWasTyped() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.presentTextMessage()
        #expect(await eventually { rig.state.isOpen })
        model.setTextMessageText("code 1234")
        // Settings opened and closed again over the bar, and its row pressed again.
        model.sessionSettingsOpen = true
        model.sessionSettingsOpen = false
        model.presentTextMessage()
        await settle()
        #expect(rig.state.isOpen && rig.state.text == "code 1234")
        // While the bar is up, the keys are the bar's.
        #expect(!model.guestHasKeys)
        model.closeTextMessage()
        #expect(model.guestHasKeys)
    }

    // MARK: Low space

    @Test func aGuestPausedForSpaceTakesNothing() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("one")
        #expect(model.textMessageAvailability == .ready && model.canSendTextMessage)
        model.guestPausingForSpace(freeBytes: 100_000_000)
        #expect(model.textMessageAvailability == .paused && !model.canSendTextMessage)
        model.sendTextMessage()
        await settle()
        #expect(rig.driver.smsSent.isEmpty && state.progress == .idle)
        #expect(state.status(availability: model.textMessageAvailability)
            == TextMessageStatus(words: SmsMessages.paused, tone: .info))
        model.spacePause = nil
        #expect(model.textMessageAvailability == .ready && model.canSendTextMessage)
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedOver(1))
        #expect(await eventually { state.progress == .sent })
    }

    /// The channel shares nothing with adb: a transfer that holds adb's one connection holds nothing here.
    @Test func aTransferOnTheAdbChannelDoesNotHoldTheSend() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.fileBrowser = FileBrowserModel(backend: FileBrowserModelTests.ScriptedBackend(), center: NotificationCenter())
        model.openTextMessage()
        model.setTextMessageText("one")
        #expect(model.canSendTextMessage)
        // An import that asks what to do with a name already there holds the channel.
        model.fileBrowser?.conflict = ImportConflict(picks: [], clashing: [], folder: "/sdcard")
        #expect(model.canSendTextMessage)
    }

    // MARK: Where it is offered

    /// The device carries text messages. A set of capabilities without them (a
    /// runtime that has no service to send to) offers no entrance and opens no bar.
    @Test func aRuntimeWithoutTextMessagesOffersNoEntranceAndOpensNoBar() async throws {
        var without = Capabilities.device
        without.textMessages = false
        let rig = try rig(capabilities: without)
        defer { rig.cleanUp() }
        #expect(!rig.model.offersTextMessage)
        rig.model.openTextMessage()
        #expect(!rig.state.isOpen)
        // The row's own path opens the bar from a task: it is asserted once the task has run, and
        // the same path opens it where the application carries text messages.
        rig.model.presentTextMessage()
        let carrying = try self.rig()
        defer { carrying.cleanUp() }
        carrying.model.presentTextMessage()
        #expect(await eventually { carrying.state.isOpen })
        await settle()
        #expect(!rig.state.isOpen)
    }

    @Test func theDeviceOffersItOnlyToANormalRun() throws {
        let normal = try rig(capabilities: .device)
        defer { normal.cleanUp() }
        #expect(normal.model.offersTextMessage)
        let recovery = try rig(capabilities: .device, mode: .recovery)
        defer { recovery.cleanUp() }
        #expect(!recovery.model.offersTextMessage)
        recovery.model.openTextMessage()
        #expect(!recovery.state.isOpen)
        #expect(recovery.model.textMessageAvailability == .notRunning)
        normal.model.phase = .home
        #expect(normal.model.textMessageAvailability == .notRunning && !normal.model.canSendTextMessage)
    }

    // MARK: The sender

    @Test func theSenderIsAndariumUntilAnotherIsUsedAndThenThatOne() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        #expect(rig.state.sender == "Andarium" && rig.state.text.isEmpty)
        model.setTextMessageSender("My Bank")
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(rig.memory.sender == "My Bank")
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedOver(1))
        #expect(await eventually { rig.state.progress == .sent })
        // A new run opens with the sender of the last.
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        model.phase = .running(.normal)
        model.openTextMessage()
        #expect(model.session.textMessage.sender == "My Bank")
        // A kept sender the rules would refuse is not offered again.
        rig.memory.keep("Bad_Name!")
        model.session.textMessage.close()
        model.openTextMessage()
        #expect(model.session.textMessage.sender == "Andarium")
    }

    @Test func theSenderIsCleanedBeforeItIsJudged() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        // A direction mark and a no-break space, as a paste may bring.
        model.setTextMessageSender("\u{202E}My\u{00A0}Bank\u{200E}")
        model.setTextMessageText("one")
        #expect(model.canSendTextMessage)
        model.sendTextMessage()
        #expect(rig.memory.sender == "My Bank")
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedOver(1))
        #expect(await eventually { rig.state.progress == .sent })
    }

    // MARK: What is refused, and how it is said

    @Test func eachRefusalHasItsOwnWordsAndSendsNothing() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        let units = String(repeating: "a", count: GuestSms.maximumUnits)
        let cases: [(sender: String, text: String, words: String?)] = [
            ("Andarium", "", nil),
            ("", "hello", SmsMessages.senderEmpty),
            ("A_B", "hello", SmsMessages.senderCharacters),
            ("Bank of the North", "hello", SmsMessages.senderLongName),
            (String(repeating: "1", count: 21), "hello", SmsMessages.senderLongNumber),
            ("Andarium", units + "a", SmsMessages.charactersOver(1)),
            ("Andarium", "a\u{FFFE}b", SmsMessages.unsendableCharacter),
        ]
        for (sender, text, words) in cases {
            model.setTextMessageSender(sender)
            model.setTextMessageText(text)
            #expect(!model.canSendTextMessage, "\(sender.debugDescription) \(text.count)")
            let status = state.status(availability: .ready)
            #expect(status?.words == words, "\(sender.debugDescription): \(status?.words ?? "none")")
            if words != nil { #expect(status?.tone == .problem) }
            model.sendTextMessage()
        }
        await settle()
        #expect(rig.driver.smsSent.isEmpty)
        // The room left shows once it runs short, and the full limit passes.
        model.setTextMessageSender("Andarium")
        model.setTextMessageText(units)
        #expect(model.canSendTextMessage)
        #expect(state.status(availability: .ready) == TextMessageStatus(words: SmsMessages.charactersLeft(0), tone: .info))
        model.setTextMessageText("short")
        #expect(state.status(availability: .ready) == nil)
    }

    // MARK: Without a runtime (the preview)

    @Test func aPreviewWithNoGuestPlaysTheSend() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.driver = nil
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(rig.state.progress == .sending)
        #expect(await eventually { rig.state.progress == .sent })
        #expect(rig.said.lines == [SmsMessages.sentToAndroid])
    }

    // MARK: The views

    /// What a drawn view shows: its accessibility labels, the UIKit text inputs, the top of each
    /// element, the traits of each labelled element, and whether a text input has the keyboard.
    private typealias Drawn = (
        labels: [String], inputs: [(placeholder: String?, type: UITextContentType?)], frames: [(label: String, y: CGFloat)],
        traits: [(label: String, traits: UIAccessibilityTraits)], keyboard: Bool
    )

    /// Hosts `view` in a window tall enough for it and gives the accessibility
    /// labels and UIKit text inputs it draws once it has settled.
    private func draw<Content: View>(_ view: Content, height: CGFloat = 900, width: CGFloat = 390) async throws -> Drawn {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        return try await hosted(view, in: lease, height: height, width: width) { hosting in
            try await Task.sleep(for: .milliseconds(800))
            return gather(from: hosting)
        }
    }

    /// `view` drawn once it has settled, and drawn again in the same hosting view once `change` has
    /// been made to it. A view that is hosted again is not the one the person has: it takes the keyboard
    /// only when it comes up idle, so a bar drawn again in a new view sits where the keyboard leaves it
    /// (and the keyboard's height differs from one Simulator to another, and with what ran before),
    /// while the bar the person sees keeps its keyboard through the change. Each drawing waits until
    /// the view stops moving, which a keyboard that comes up late would otherwise catch half way.
    private func draw<Content: View>(
        _ view: Content, height: CGFloat = 900, width: CGFloat = 390, across change: () async throws -> Void
    ) async throws -> (before: Drawn, after: Drawn) {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        return try await hosted(view, in: lease, height: height, width: width) { hosting in
            let before = try await stillDrawing(of: hosting)
            try await change()
            return (before, try await stillDrawing(of: hosting))
        }
    }

    private func hosted<Content: View, Result>(
        _ view: Content, in lease: HostWindow.Lease, height: CGFloat, width: CGFloat,
        _ body: (UIView) async throws -> Result
    ) async throws -> Result {
        let container = UIViewController()
        lease.window.rootViewController = container
        let hosting = UIHostingController(rootView: view)
        container.addChild(hosting)
        hosting.view.frame = CGRect(x: 0, y: 0, width: width, height: height)
        container.view.addSubview(hosting.view)
        hosting.didMove(toParent: container)
        defer {
            hosting.willMove(toParent: nil)
            hosting.view.removeFromSuperview()
            hosting.removeFromParent()
        }
        return try await body(hosting.view)
    }

    /// The drawing once two looks, a quarter of a second apart, agree on every element (at most
    /// three seconds after the first 800 ms).
    private func stillDrawing(of root: UIView) async throws -> Drawn {
        try await Task.sleep(for: .milliseconds(800))
        var last = gather(from: root)
        for _ in 0..<12 {
            try await Task.sleep(for: .milliseconds(250))
            let now = gather(from: root)
            if now.labels == last.labels, now.frames.map(\.y) == last.frames.map(\.y), now.keyboard == last.keyboard { return now }
            last = now
        }
        return last
    }

    private func gather(from root: UIView) -> Drawn {
        root.setNeedsLayout()
        root.layoutIfNeeded()
        var inputs: [(String?, UITextContentType?)] = []
        func walk(_ node: UIView) {
            if let field = node as? UITextField { inputs.append((field.placeholder, field.textContentType)) }
            if let text = node as? UITextView { inputs.append((text.accessibilityLabel, text.textContentType)) }
            node.subviews.forEach(walk)
        }
        walk(root)
        let elements = SideKeysTouchTests.elements(in: root)
        // The same walk, for the traits of each labelled element.
        var traits: [(label: String, traits: UIAccessibilityTraits)] = []
        var seen = Set<ObjectIdentifier>()
        func walkTraits(_ node: NSObject, depth: Int) {
            guard depth < 64, seen.insert(ObjectIdentifier(node)).inserted else { return }
            if let label = node.accessibilityLabel, !label.isEmpty { traits.append((label, node.accessibilityTraits)) }
            var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
            if let view = node as? UIView { children += view.subviews }
            for child in children { walkTraits(child, depth: depth + 1) }
        }
        walkTraits(root, depth: 0)
        return (elements.map(\.label), inputs, elements.map { ($0.label, $0.frame.minY) }, traits, focusedInput(in: root) != nil)
    }

    @Test func onlyTheMessageBoxHasTheOneTimeCodeContentType() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        let drawn = try await draw(TextMessageBar().environment(rig.model))
        #expect(!drawn.inputs.isEmpty)
        let typed = drawn.inputs.filter { $0.type != nil }
        #expect(typed.count == 1 && typed.first?.type == .oneTimeCode, "\(drawn.inputs)")
        #expect(drawn.labels.contains(SmsMessages.from) && drawn.labels.contains(SmsMessages.message))
        #expect(drawn.labels.contains(SmsMessages.send) && drawn.labels.contains(SmsMessages.close))
    }

    /// The text input that has the keyboard in `root`, if one does: the sender is a
    /// text field and the message box, which grows, a text view.
    private func focusedInput(in root: UIView) -> UIView? {
        if root.isFirstResponder, root is UITextField || root is UITextView { return root }
        for child in root.subviews { if let found = focusedInput(in: child) { return found } }
        return nil
    }

    @Test func theKeyboardStaysWithTheFieldsWhileAMessageIsBeingSent() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("123456")
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let hosting = UIHostingController(rootView: TextMessageBar().environment(model))
        container.addChild(hosting)
        hosting.view.frame = CGRect(x: 0, y: 0, width: 390, height: 600)
        container.view.addSubview(hosting.view)
        hosting.didMove(toParent: container)
        defer {
            hosting.willMove(toParent: nil)
            hosting.view.removeFromSuperview()
            hosting.removeFromParent()
        }
        // The message box takes the keys once the bar has come up.
        #expect(await eventually { focusedInput(in: hosting.view) is UITextView })
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        try await Task.sleep(for: .milliseconds(300))
        // The keyboard is still up for the message box, locked: a typed character changes nothing.
        let box = try #require(focusedInput(in: hosting.view), "the keyboard went down at Send")
        #expect(box is UITextView, "the keyboard is on the sender, not the message box")
        (box as? UIKeyInput)?.insertText("7")
        #expect(state.text == "123456")
        // And the field shows what the model holds: a refused edit does not stay on the screen.
        try await Task.sleep(for: .milliseconds(300))
        #expect((box as? UITextView)?.text == "123456", "the box shows \((box as? UITextView)?.text ?? "nothing")")
        // Android answers: the keys are still there, and the message box has them.
        rig.answers.release(.handedOver(1))
        #expect(await eventually { state.progress == .sent })
        #expect(await eventually { focusedInput(in: hosting.view) is UITextView })
    }

    /// The initial VoiceOver focus (the title, 700 ms after the bar rises) cannot be
    /// tested without VoiceOver (docs/testing.md); what the focus lands on can: the
    /// title is a heading and the first labelled element of the bar, and the caption
    /// follows it. Whether the tile's symbol is an element of its own is not seen
    /// here: an image with no label does not appear in this walk.
    @Test func theBarsTitleIsItsHeadingAndTheCaptionFollowsIt() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        let drawn = try await draw(TextMessageBar().environment(rig.model))
        let title = try #require(drawn.labels.firstIndex(of: SmsMessages.name))
        #expect(title == 0, "an element comes before the title: \(drawn.labels)")
        let traits = try #require(drawn.traits.first { $0.label == SmsMessages.name })
        #expect(traits.traits.contains(.header), "the title is not a heading")
        // The title, then the line under it, then Close, then the fields.
        #expect(drawn.labels.contains(SmsMessages.caption))
        #expect(drawn.labels.firstIndex(of: SmsMessages.caption) == title + 1, "\(drawn.labels)")
        #expect(drawn.labels.firstIndex(of: SmsMessages.close).map { $0 > title } == true)
    }

    /// The line that says what is wrong has a symbol beside its words, as the good line has its check; VoiceOver
    /// reads the words once, and the symbol is not an element.
    @Test func aStatusLineIsReadOnceAndItsSymbolIsNotAnElement() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        #expect(await send(rig, "one", .uncertain("unknown")))
        let problem = try await draw(TextMessageBar().environment(rig.model))
        #expect(problem.labels.filter { $0 == SmsMessages.uncertain }.count == 1, "\(problem.labels)")
        #expect(!problem.labels.contains { $0.localizedCaseInsensitiveContains("warning") || $0.contains("exclamation") },
                "the warning symbol is an element: \(problem.labels)")
        rig.model.setTextMessageText("")
        #expect(await send(rig, "two", .handedOver(1)))
        let good = try await draw(TextMessageBar().environment(rig.model))
        #expect(good.labels.filter { $0 == SmsMessages.sentToAndroid }.count == 1, "\(good.labels)")
        #expect(!good.labels.contains { $0.localizedCaseInsensitiveContains("check") }, "the check is an element: \(good.labels)")
    }

    /// The bar where the session puts it (`TextMessageBarOverlay`): at the bottom of the screen,
    /// so a line that makes it taller grows it upward and what is at its foot stays. Drawn alone in
    /// a hosting view, a view is centred, and a taller bar moves everything in it by half its growth.
    private func placedAsTheSessionPlacesIt<Content: View>(_ bar: Content) -> some View {
        bar.frame(maxHeight: .infinity, alignment: .bottom)
    }

    /// Send is where it was while the message goes, disabled and labelled Sending: the
    /// element VoiceOver focus is on stays in the tree. The bar is drawn once, with the keyboard the
    /// message box takes, and again in the same view while the message goes (`draw(across:)`), at the
    /// widths of a small and an ordinary iPhone and of an iPad.
    @Test(arguments: [320.0, 390.0, 834.0])
    func sendStaysInPlaceDisabledAndSayingSendingWhileAMessageGoes(width: CGFloat) async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        rig.model.setTextMessageText("123456")
        let bar = placedAsTheSessionPlacesIt(TextMessageBar()).environment(rig.model)
        let (idle, during) = try await draw(bar, width: width) {
            rig.model.sendTextMessage()
            #expect(await eventually { rig.answers.pending == 1 })
        }
        #expect(idle.keyboard && during.keyboard, "the message box keeps the keyboard through the send")
        let before = try #require(idle.traits.first { $0.label == SmsMessages.send })
        #expect(before.traits.contains(.button) && !before.traits.contains(.notEnabled))
        let beforeY = try #require(idle.frames.first { $0.label == SmsMessages.send }?.y)
        #expect(!during.labels.contains(SmsMessages.send), "Send went while the message goes")
        let sending = try #require(during.traits.first { $0.label == SmsMessages.sending })
        #expect(sending.traits.contains(.button) && sending.traits.contains(.notEnabled))
        let duringY = try #require(during.frames.first { $0.label == SmsMessages.sending }?.y)
        #expect(abs(duringY - beforeY) < 1, "Send moved from \(beforeY) to \(duringY)")
        rig.answers.release(.handedOver(1))
        #expect(await eventually { rig.state.progress == .sent })
    }

    /// The accessibility element labelled `label` in a drawn tree, as UIKit walks them.
    private func element(labelled label: String, in root: UIView) -> NSObject? {
        var seen = Set<ObjectIdentifier>()
        func walk(_ node: NSObject, depth: Int) -> NSObject? {
            guard depth < 64, seen.insert(ObjectIdentifier(node)).inserted else { return nil }
            if node.accessibilityLabel == label { return node }
            var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
            if let view = node as? UIView { children += view.subviews }
            for child in children { if let found = walk(child, depth: depth + 1) { return found } }
            return nil
        }
        return walk(root, depth: 0)
    }

    /// With a result untold on the line, a refused draft leaves Send enabled, since the line does not say
    /// what is wrong with the draft; its press only says the result again. Once the result was told, the
    /// refusal line and the dimmed Send come back together. The presses go through the drawn button.
    @Test func aRefusedDraftAfterAnUntoldResultLeavesSendEnabledUntilTheResultIsTold() async throws {
        let rig = try rig(voiceOver: true)
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(await send(rig, "one", .uncertain("unknown")))
        rig.said.finishLast(false)
        await settle()
        #expect(state.awaiting != nil)
        let refused: [(sender: String, text: String)] = [
            (GuestSms.defaultSender, String(repeating: "a", count: GuestSms.maximumUnits + 12)),
            (GuestSms.defaultSender, "Code \u{FFFE} 482913"),
            ("Shop & Go", "two"),
            (GuestSms.defaultSender, "three"),
        ]
        for (index, draft) in refused.enumerated() {
            if index == refused.count - 1 {
                // Sixteen sends wait for Android to confirm them: this draft is not one of them.
                for number in 0..<GuestSms.Retries.capacity {
                    state.retries.settle(
                        sender: GuestSms.defaultSender, text: "Held \(number)", identifier: GuestSms.newIdentifier(),
                        result: .notConfirmed)
                }
            }
            model.setTextMessageSender(draft.sender)
            model.setTextMessageText(draft.text)
            let said = rig.said.lines.count
            try await withHostedBar(model) { view in
                try await Task.sleep(for: .milliseconds(800))
                let button = try #require(element(labelled: SmsMessages.send, in: view), "no Send under \(draft.text.prefix(12))")
                #expect(!button.accessibilityTraits.contains(.notEnabled), "Send is dimmed with the result on the line")
                #expect(button.accessibilityActivate(), "the press did nothing")
            }
            await settle()
            #expect(rig.driver.smsSent.count == 1, "a refused draft was sent")
            #expect(rig.said.lines.count == said + 1 && rig.said.lines.last == SmsMessages.uncertain, "\(rig.said.lines)")
            #expect(state.awaiting != nil && state.status(availability: .ready)?.words == SmsMessages.uncertain)
        }
        // Told: the draft's own refusal is on the line and Send is dimmed with it.
        model.textMessageLineFocused()
        #expect(await eventually { state.awaiting == nil })
        let told = try await draw(TextMessageBar().environment(model))
        #expect(told.labels.contains(SmsMessages.waiting), "the refusal is not on the line: \(told.labels)")
        let dimmed = try #require(told.traits.first { $0.label == SmsMessages.send })
        #expect(dimmed.traits.contains(.notEnabled), "Send is enabled beside a refusal")
    }

    /// The same for the cut-off line: a refused draft leaves Send enabled while the line waits to be told,
    /// and a press through the drawn button says it again and sends nothing.
    @Test func aRefusedDraftAfterAnAwaitedCutOffLineLeavesSendEnabledUntilItIsTold() async throws {
        let files = RecordingNoticeFiles()
        files.put(Data((header + "cut-off\n").utf8), at: Self.noticeURL)
        let rig = try rig(voiceOver: true, files: files)
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        #expect(state.awaiting != nil && state.progress == .cutOff)
        rig.said.finishLast(false)
        await settle()
        model.setTextMessageText(String(repeating: "a", count: GuestSms.maximumUnits + 12))
        try await withHostedBar(model) { view in
            try await Task.sleep(for: .milliseconds(800))
            let button = try #require(element(labelled: SmsMessages.send, in: view))
            #expect(!button.accessibilityTraits.contains(.notEnabled), "Send is dimmed with the cut-off line waiting")
            #expect(button.accessibilityActivate(), "the press did nothing")
        }
        await settle()
        #expect(rig.driver.smsSent.isEmpty && rig.said.lines == [SmsMessages.cutOff, SmsMessages.cutOff], "\(rig.said.lines)")
        model.textMessageLineFocused()
        #expect(await eventually { state.awaiting == nil })
        let told = try await draw(TextMessageBar().environment(model))
        #expect(told.labels.contains(SmsMessages.charactersOver(12)), "the refusal is not on the line: \(told.labels)")
        let dimmed = try #require(told.traits.first { $0.label == SmsMessages.send })
        #expect(dimmed.traits.contains(.notEnabled), "Send is enabled beside a refusal")
    }

    /// Hosts the bar in a window and gives `body` the view; the window goes after.
    private func withHostedBar(_ model: AppModel, _ body: (UIView) async throws -> Void) async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let hosting = UIHostingController(rootView: TextMessageBar().environment(model))
        container.addChild(hosting)
        hosting.view.frame = CGRect(x: 0, y: 0, width: 390, height: 600)
        container.view.addSubview(hosting.view)
        hosting.didMove(toParent: container)
        defer {
            hosting.willMove(toParent: nil)
            hosting.view.removeFromSuperview()
            hosting.removeFromParent()
        }
        try await body(hosting.view)
    }

    /// While Android is paused the keyboard stays down, so that the pause card does not cover the bar's
    /// title row and Close.
    @Test func noKeyboardComesUpWhileAndroidIsPaused() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        rig.model.guestPausingForSpace(freeBytes: 100_000_000)
        try await withHostedBar(rig.model) { view in
            try await Task.sleep(for: .milliseconds(1500))
            #expect(focusedInput(in: view) == nil, "the keyboard came up under the pause card")
        }
        // Not paused: the message box takes the keys.
        let running = try self.rig()
        defer { running.cleanUp() }
        running.model.openTextMessage()
        try await withHostedBar(running.model) { view in
            #expect(await eventually { focusedInput(in: view) is UITextView })
        }
    }

    /// Under VoiceOver the bar that opens with the cut-off line gives the line VoiceOver focus first: the
    /// keyboard's focus move is skipped.
    @Test func theCutOffOpeningUnderVoiceOverTakesNoKeyboardFocus() async throws {
        let files = RecordingNoticeFiles()
        files.put(Data((header + "cut-off\n").utf8), at: Self.noticeURL)
        let rig = try rig(voiceOver: true, files: files)
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        try await withHostedBar(rig.model) { view in
            try await Task.sleep(for: .milliseconds(1500))
            #expect(focusedInput(in: view) == nil, "the keyboard took the focus from the cut-off line")
        }
        // Without VoiceOver the same opening puts the cursor in the box.
        let plain = try self.rig(files: { let f = RecordingNoticeFiles(); f.put(Data((header + "cut-off\n").utf8), at: Self.noticeURL); return f }())
        defer { plain.cleanUp() }
        plain.model.openTextMessage()
        try await withHostedBar(plain.model) { view in
            #expect(await eventually { focusedInput(in: view) is UITextView })
        }
    }

    // MARK: The connectors (U3)

    /// The text message connector names port 5560 and no other; the adb connector, which the network bridge
    /// opens its streams with, names 5555 and no other.
    @Test func eachConnectorNamesItsOwnPortAndNoOther() throws {
        final class Ports: @unchecked Sendable {
            private let lock = NSLock()
            private var seen: [UInt32] = []
            func add(_ port: UInt32) { lock.withLock { seen.append(port) } }
            var value: [UInt32] { lock.withLock { seen } }
        }
        let sms = Ports()
        let connector = TextMessageConnector(connect: { port, _ in sms.add(port); return -Int32(ECONNREFUSED) })
        for _ in 0..<3 { #expect(throws: POSIXError.self) { try connector.open(timeoutMilliseconds: 250) } }
        #expect(sms.value == [5560, 5560, 5560] && GuestSmsChannel.port == 5560)
        let adb = Ports()
        let vsock = VsockConnector(connect: { port, _ in adb.add(port); return -Int32(ECONNREFUSED) })
        #expect(throws: AdbError.self) { try vsock.openStream() }
        #expect(throws: AdbError.self) { _ = try vsock.connect() }
        #expect(adb.value == [5555, 5555] && VsockConnector.adbPort == 5555)
    }

    @Test func settingsHasTheRowOnlyWhereTheApplicationCarriesTextMessages() async throws {
        var without = Capabilities.device
        without.textMessages = false
        let held = try rig(capabilities: without)
        defer { held.cleanUp() }
        let heldLabels = try await draw(SessionSettingsSheet(large: true).environment(held.model), height: 2600).labels
        #expect(heldLabels.contains { $0.contains("ADB over network") })
        #expect(!heldLabels.contains { $0.contains(SmsMessages.row) }, "\(heldLabels)")

        let offered = try rig(capabilities: .device)
        defer { offered.cleanUp() }
        let offeredLabels = try await draw(SessionSettingsSheet(large: true).environment(offered.model), height: 2600).labels
        #expect(offeredLabels.contains { $0.contains(SmsMessages.row) }, "\(offeredLabels)")
        // The row is a group of its own under ADB over network's footer, so the footer describes the toggle alone.
        let ys = try await draw(SessionSettingsSheet(large: true).environment(offered.model), height: 2600).frames
        let toggleY = try #require(ys.first { $0.label.contains("ADB over network") }?.y, "\(ys)")
        let footerY = try #require(
            ys.first { $0.label.contains("USB debugging") || $0.label.contains("same network") }?.y, "\(ys)")
        let rowY = try #require(ys.first { $0.label.contains(SmsMessages.row) }?.y)
        #expect(toggleY < footerY && footerY < rowY, "\(ys)")
    }
}
