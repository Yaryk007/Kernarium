// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the application knows about one game controller iOS has connected.
public struct GamepadController: Equatable, Sendable {
    /// The application's own number for the controller, the same for as long
    /// as it stays connected and never reused while the process lives.
    public var id: Int
    public var family: GamepadFamily
    /// The controller's own name, which a generic profile shows to the guest.
    public var name: String
    /// What a generic controller has.
    public var elements: GamepadElements
    public var haptics: GamepadHaptics

    public init(
        id: Int, family: GamepadFamily, name: String = "", elements: GamepadElements = .extended,
        haptics: GamepadHaptics = .none
    ) {
        self.id = id
        self.family = family
        self.name = name
        self.elements = elements
        self.haptics = haptics
    }
}

/// What the guest's drivers ask of a controller besides rumble: its light and
/// its player number. The application applies each to the controller it
/// names; every call is made on the session's one queue.
public protocol GamepadIndicators: AnyObject, Sendable {
    func setLight(controller: Int, red: UInt8, green: UInt8, blue: UInt8)
    /// iOS's player index, 0 through 3, or nil for none.
    func setPlayerIndex(controller: Int, index: Int?)
}

/// Gives the game controllers iOS has connected to the guest as HID devices,
/// and plays what the guest's drivers ask of them.
///
/// **A device for each controller.** The application tells the session each
/// controller that connects, with its states, and each that disconnects. A
/// controller takes the lowest of 8 slots while it is connected (a ninth is
/// left out), and while a relay answers in the guest, each slot's controller
/// is one device there: made when the relay says hello (or when the controller
/// connects after that), given its current state once the guest has started
/// it, and destroyed at the controller's disconnect. A guest that restarts, a
/// relay that does, and the end of the run take every device with them; the
/// next hello makes them again.
///
/// **When it connects.** The relay starts early in boot, so a run (a normal
/// start, from its first moment until it ends or the guest restarts) connects
/// at once and tries again every second. Once Android has booted, five
/// attempts in a row that fail end the attempts for that boot, silently: an
/// older image has no relay, and a person pressed no switch. A connection that
/// was made and is lost is tried again the same way.
///
/// **States.** A state that changes a button, the direction pad or a trigger's
/// press is sent at once, and never joined with another, so no tap is lost; a
/// state that changes only an analog value or the battery is sent at once if
/// the last report went more than `analogInterval` ago and otherwise when that
/// has passed, with the newest values. A button going down also wakes a guest
/// that suspended itself, as a key does (`wake`). Only a state that is
/// sent matters: the connection keeps nothing for a guest that is not reading.
///
/// **In front.** iOS delivers a controller's input to the application in front
/// only. Leaving the front lets go of every controller's state (a device still
/// being made is given the neutral state too), sends each device one neutral
/// report, so that nothing stays pressed, stops every rumble and keeps the
/// devices; returning to it sends each device the state it holds, which is the
/// let go one: iOS says nothing of a controller that has not reported since, so
/// what it holds reaches the guest with its next report.
///
/// **Whose reply.** The relay answers each `CREATE` once, in order, and its
/// other frames name only a slot's number. A controller that leaves while its
/// `CREATE` is unanswered, or while its output is in flight, may be replaced at
/// once by another that takes the slot, so the session counts the `CREATE`s of
/// each slot that the relay has not answered and takes nothing of the slot
/// (its events, its output, the requests of its driver) until the `CREATED` of
/// the latest: what comes before it is of a device that is gone.
///
/// **Rumble.** An output report of the guest's driver sets a motor's level
/// until the next one; each motor plays as a tone of the haptics' vocabulary on
/// the controller's handle (`GamepadRumble`), which the `HapticPlayer` plays.
/// Something else may end a tone (the system stopping the engine, a stop of
/// every target), so a level change on a tone that is not there plays it
/// again.
///
/// Every call returns at once. The work is in order on one queue, and the
/// streams and the player are only touched there.
public final class GamepadSession: @unchecked Sendable {
    /// Attempts that fail in a row, after Android has booted, before the
    /// session stops trying for that boot.
    public static let attemptsAfterBoot = 5
    /// The shortest time between two reports that change only an analog value.
    public static let analogInterval: UInt64 = 8_000_000
    /// How long after a device is ready its state is sent once more: a driver
    /// that is still being probed refuses a report.
    public static let settleDelay: UInt64 = 300_000_000

    private enum Phase { case absent, creating, ready, failed }

    private struct Tone {
        var began: UInt64
        var intensity: Float
    }

    private struct Slot {
        var controller: GamepadController
        var profile: any GamepadProfile
        var encoder: any GamepadReportEncoder
        var phase = Phase.absent
        var created = false
        var started = false
        /// The newest state, which a device is given when it is ready.
        var state: GamepadState
        var sent: GamepadState?
        var sentAt: UInt64 = 0
        var flushScheduled = false
        var tones: [HapticTarget: Tone] = [:]
        var light: GamepadOutputEffects.Light?
        var player: GamepadOutputEffects.Player?
        /// Which incarnation of the device this is, so that work scheduled for
        /// an earlier one finds it gone.
        var incarnation: UInt64 = 0
    }

    private let opener: any GuestStreamOpener
    private let player: any HapticPlayer
    private let indicators: any GamepadIndicators
    private let wake: @Sendable () -> Void
    private let log: @Sendable (String) -> Void
    private let retryDelay: DispatchTimeInterval
    private let clock: @Sendable () -> UInt64
    private let schedule: @Sendable (_ nanoseconds: UInt64, _ work: @escaping @Sendable () -> Void) -> Void
    private let queue = DispatchQueue(label: "andarium-gamepad-session")

    /// Which controller holds which slot, readable from any thread: the player's
    /// engine source asks for it while the session's queue is playing.
    private let tableLock = NSLock()
    private var table = [Int?](repeating: nil, count: GamepadProtocol.slotCount)

    // Touched only on `queue`.
    private var booted = false
    private var running = false
    private var foreground = false
    private var stream: GamepadStream?
    private var run: UInt64 = 0
    private var connected = false
    private var greeted = false
    private var slotLimit = GamepadProtocol.slotCount
    private var failedAttempts = 0
    private var gaveUp = false
    private var slots = [Slot?](repeating: nil, count: GamepadProtocol.slotCount)
    /// The `CREATE`s of each slot that the relay has not answered, kept across
    /// the slot being emptied and filled again.
    private var unanswered = [Int](repeating: 0, count: GamepadProtocol.slotCount)
    private var nextIncarnation: UInt64 = 0
    private var scoreSequence: UInt32 = 0

    /// `wake` is called on the session's queue when a button goes down in a
    /// state that is sent; `log` says what the session leaves out.
    public init(
        opener: any GuestStreamOpener, player: any HapticPlayer, indicators: any GamepadIndicators,
        retryDelay: DispatchTimeInterval = .seconds(1),
        clock: @escaping @Sendable () -> UInt64 = GamepadSession.uptimeNanoseconds,
        schedule: @escaping @Sendable (UInt64, @escaping @Sendable () -> Void) -> Void = GamepadSession.dispatchAfter,
        wake: @escaping @Sendable () -> Void, log: @escaping @Sendable (String) -> Void = { _ in }
    ) {
        self.opener = opener
        self.player = player
        self.indicators = indicators
        self.retryDelay = retryDelay
        self.clock = clock
        self.schedule = schedule
        self.wake = wake
        self.log = log
    }

    deinit { stream?.stop() }

    public static let uptimeNanoseconds: @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }

    /// Runs `work` after `nanoseconds` on `DispatchTime`'s clock.
    public static let dispatchAfter: @Sendable (UInt64, @escaping @Sendable () -> Void) -> Void = { nanoseconds, work in
        let delay = DispatchTimeInterval.nanoseconds(Int(min(nanoseconds, UInt64(Int.max))))
        DispatchQueue.global(qos: .userInteractive).asyncAfter(deadline: .now() + delay, execute: work)
    }

    // MARK: What is wanted

    /// What is wanted now: `running` is a normal run of the guest, `booted`
    /// that Android has reported boot completed in it, and `foreground` that
    /// the application is in front and holds the guest running.
    public func update(running: Bool, booted: Bool, foreground: Bool) {
        queue.async { [self] in
            let bootedNow = booted && !self.booted
            self.booted = booted
            if bootedNow { failedAttempts = 0 }
            self.running = running
            if running {
                if stream == nil && !gaveUp { beginRun() }
            } else {
                if stream != nil { endRun() }
                gaveUp = false
            }
            let frontChanged = foreground != self.foreground
            self.foreground = foreground
            guard frontChanged else { return }
            if foreground { enterFront() } else { leaveFront() }
        }
    }

    /// The guest restarted itself: what was connected to is gone, and the next
    /// boot begins the attempts again.
    public func restart() {
        queue.async { [self] in
            if stream != nil { endRun() }
            gaveUp = false
            failedAttempts = 0
        }
    }

    /// A controller connected, in the state it is in. One that is already known,
    /// or that finds no free slot, changes nothing.
    public func connect(_ controller: GamepadController, state: GamepadState) {
        queue.async { [self] in
            guard slot(of: controller.id) == nil else { return }
            guard let index = slots.firstIndex(where: { $0 == nil }) else {
                log("gamepad: no free slot for \(controller.name)")
                return
            }
            let profile = GamepadProfiles.profile(family: controller.family, name: controller.name, elements: controller.elements)
            // A controller that connects while the application is away holds
            // nothing: its input is not the application's to give.
            slots[index] = Slot(
                controller: controller, profile: profile, encoder: profile.makeEncoder(),
                state: foreground ? state : state.released)
            tableLock.withLock { table[index] = controller.id }
            createIfPossible(index)
        }
    }

    /// A controller disconnected: its device is destroyed and its slot freed.
    public func disconnect(id: Int) {
        queue.async { [self] in
            guard let index = slot(of: id), let slot = slots[index] else { return }
            if slot.phase != .absent { stream?.send(.destroy(slot: index)) }
            for locality in HapticTarget.ControllerLocality.allCases {
                player.forget(.controller(slot: index, locality: locality))
            }
            slots[index] = nil
            tableLock.withLock { table[index] = nil }
        }
    }

    /// A controller's inputs, newest first (its battery is the one
    /// `updateBattery` set). While the application is not in front none is
    /// taken: the controller is let go of.
    public func submit(_ state: GamepadState, for id: Int) {
        queue.async { [self] in
            guard foreground, let index = slot(of: id), var slot = slots[index] else { return }
            let previous = slot.state
            // The battery is the application's own reading (`updateBattery`),
            // which a state of the inputs does not carry.
            slot.state = state
            slot.state.battery = previous.battery
            slots[index] = slot
            guard slot.phase == .ready else { return }
            if !state.newlyPressed(since: previous).isEmpty { wake() }
            sendIfDue(index)
        }
    }

    /// A controller's battery, as iOS reports it now.
    public func updateBattery(_ battery: GamepadBattery, for id: Int) {
        queue.async { [self] in
            guard let index = slot(of: id), var slot = slots[index], slot.state.battery != battery else { return }
            slot.state.battery = battery
            slots[index] = slot
            if foreground && slot.phase == .ready { sendIfDue(index) }
        }
    }

    /// Returns when everything asked of the session so far has been done.
    public func settle() {
        queue.sync {}
    }

    /// Whether the guest's relay is connected and has said hello.
    public var isConnected: Bool { queue.sync { connected && greeted } }

    /// Whether the session stopped trying for this boot.
    public var hasGivenUp: Bool { queue.sync { gaveUp } }

    /// The slot a controller holds, from any thread.
    public func slot(ofController id: Int) -> Int? {
        tableLock.withLock { table.firstIndex(of: id) }
    }

    /// The controller that holds a slot, from any thread.
    public func controller(inSlot slot: Int) -> Int? {
        tableLock.withLock { table.indices.contains(slot) ? table[slot] : nil }
    }

    private func slot(of id: Int) -> Int? {
        slots.firstIndex { $0?.controller.id == id }
    }

    // MARK: A run

    private func beginRun() {
        run &+= 1
        let current = run
        failedAttempts = 0
        let stream = GamepadStream(opener: opener, retryDelay: retryDelay) { [weak self] event in
            self?.queue.async { [weak self] in
                guard let self, run == current else { return }
                handle(event)
            }
        }
        self.stream = stream
        stream.start()
    }

    private func endRun() {
        run &+= 1
        stream?.stop()
        stream = nil
        connected = false
        greeted = false
        forgetDevices()
    }

    private func giveUp() {
        gaveUp = true
        run &+= 1
        stream?.stop()
        stream = nil
        connected = false
        greeted = false
        forgetDevices()
    }

    /// Every device is gone from the guest: the controllers stay, to be made
    /// again at the next hello.
    private func forgetDevices() {
        unanswered = [Int](repeating: 0, count: GamepadProtocol.slotCount)
        for index in slots.indices {
            guard var slot = slots[index] else { continue }
            slot.phase = .absent
            slot.created = false
            slot.started = false
            slot.sent = nil
            slot.flushScheduled = false
            slot.encoder = slot.profile.makeEncoder()
            slot.incarnation = newIncarnation()
            slots[index] = slot
        }
        letGo()
    }

    /// Stops every rumble the session started. The guest's own vibrator is the
    /// haptics session's, so it is left alone.
    private func letGo() {
        for index in slots.indices {
            guard let slot = slots[index] else { continue }
            for target in slot.tones.keys { player.stop(target) }
            slots[index]?.tones.removeAll()
        }
    }

    private func newIncarnation() -> UInt64 {
        nextIncarnation &+= 1
        return nextIncarnation
    }

    private func handle(_ event: GamepadStream.Event) {
        switch event {
        case .connected:
            connected = true
            greeted = false
            failedAttempts = 0
        case .attemptFailed:
            guard booted else { return }
            failedAttempts += 1
            if failedAttempts >= Self.attemptsAfterBoot { giveUp() }
        case .lost:
            connected = false
            greeted = false
            forgetDevices()
        case .frame(let frame):
            handle(frame)
        }
    }

    // MARK: Front

    private func leaveFront() {
        // Every controller's state is let go of, a device that is still being
        // made included: it is given the neutral state when it is ready. A
        // session with no relay does it too, since the devices it makes next
        // start from that state.
        for index in slots.indices {
            guard var slot = slots[index] else { continue }
            slot.state = slot.state.released
            slots[index] = slot
            sendNow(index)
        }
        letGo()
    }

    private func enterFront() {
        guard stream != nil else { return }
        for index in slots.indices {
            guard let slot = slots[index], slot.phase == .ready else { continue }
            sendNow(index)
            if let light = slot.light { indicators.setLight(controller: slot.controller.id, red: light.red, green: light.green, blue: light.blue) }
            applyPlayer(slot)
        }
    }

    // MARK: Devices

    private func createIfPossible(_ index: Int) {
        guard greeted, index < slotLimit, var slot = slots[index], slot.phase == .absent, let stream else { return }
        slot.phase = .creating
        slot.created = false
        slot.started = false
        slot.sent = nil
        slot.encoder = slot.profile.makeEncoder()
        slot.incarnation = newIncarnation()
        slots[index] = slot
        if stream.send(.create(slot.profile.create(slot: index))) { unanswered[index] += 1 }
    }

    private func becomeReady(_ index: Int) {
        guard var slot = slots[index], slot.phase == .creating else { return }
        slot.phase = .ready
        slots[index] = slot
        sendNow(index)
        // A driver still being probed refuses a report, which would leave a
        // held button unseen: once more after the probe has had time to end.
        let incarnation = slot.incarnation
        schedule(Self.settleDelay) { [weak self] in
            self?.queue.async { [weak self] in
                guard let self, let slot = slots[index], slot.incarnation == incarnation, slot.phase == .ready else { return }
                if foreground { sendNow(index) }
            }
        }
    }

    // MARK: Sending states

    /// Sends the state at once, whatever the last report was.
    private func sendNow(_ index: Int) {
        guard var slot = slots[index], slot.phase == .ready, let stream else { return }
        let reports = slot.encoder.reports(for: slot.state, after: slot.sent)
        slot.sent = slot.state
        slot.sentAt = clock()
        slot.flushScheduled = false
        slots[index] = slot
        for report in reports { stream.send(.input(slot: index, report: report)) }
    }

    /// Sends the state if something worth sending changed, at once for a
    /// button, the direction pad or a trigger's press, and for an analog value
    /// once the interval since the last report has passed.
    private func sendIfDue(_ index: Int) {
        guard var slot = slots[index], slot.phase == .ready else { return }
        guard let sent = slot.sent else {
            sendNow(index)
            return
        }
        guard slot.state != sent else { return }
        let now = clock()
        let elapsed = now > slot.sentAt ? now - slot.sentAt : 0
        if slot.state.digitalDiffers(from: sent) || elapsed >= Self.analogInterval {
            sendNow(index)
            return
        }
        guard !slot.flushScheduled else { return }
        slot.flushScheduled = true
        slots[index] = slot
        let incarnation = slot.incarnation
        schedule(Self.analogInterval - elapsed) { [weak self] in
            self?.queue.async { [weak self] in
                guard let self, var slot = slots[index], slot.incarnation == incarnation else { return }
                slot.flushScheduled = false
                slots[index] = slot
                if foreground, slot.phase == .ready, slot.state != slot.sent { sendNow(index) }
            }
        }
    }

    // MARK: The guest's frames

    private func handle(_ frame: GamepadGuestFrame) {
        switch frame {
        case .hello(let version, let count):
            guard !greeted else {
                stream?.drop()
                return
            }
            guard version == GamepadProtocol.version else {
                log("gamepad: the guest speaks version \(version)")
                giveUp()
                return
            }
            greeted = true
            unanswered = [Int](repeating: 0, count: GamepadProtocol.slotCount)
            slotLimit = min(Int(count), GamepadProtocol.slotCount)
            for index in slots.indices { createIfPossible(index) }
        case .created(let index, let result):
            guard greeted else { return protocolBroken() }
            // The reply of a device that is gone does not count; the latest
            // one's does.
            guard unanswered[index] > 0 else { return }
            unanswered[index] -= 1
            guard unanswered[index] == 0 else { return }
            guard var slot = slots[index], slot.phase == .creating else { return }
            guard result == 0 else {
                log("gamepad: the guest could not make a device for \(slot.controller.name): \(result)")
                slot.phase = .failed
                slots[index] = slot
                return
            }
            slot.created = true
            slots[index] = slot
            if slot.started { becomeReady(index) }
        case .event(let index, let event):
            guard greeted else { return protocolBroken() }
            guard unanswered[index] == 0, var slot = slots[index] else { return }
            switch event {
            case .start:
                guard slot.phase == .creating else { return }
                slot.started = true
                slots[index] = slot
                if slot.created { becomeReady(index) }
            case .stop:
                // The driver let go of the device: nothing more is sent to it.
                guard slot.phase == .creating || slot.phase == .ready else { return }
                slot.phase = .failed
                for target in slot.tones.keys { player.stop(target) }
                slot.tones.removeAll()
                slots[index] = slot
            case .open, .close:
                break
            }
        case .output(let index, let reportType, let data):
            guard greeted else { return protocolBroken() }
            guard unanswered[index] == 0, reportType == GamepadReportType.output, let slot = slots[index],
                slot.phase != .absent
            else { return }
            apply(slot.profile.decodeOutput(data), to: index)
        case .getReport(let index, let reportType, let number, let id):
            guard greeted else { return protocolBroken() }
            // A request of a device that is gone, while the slot's new one is
            // made: uhid matches a reply by the device's own counter, so the
            // new device's probe could take the reply meant for the old one.
            guard unanswered[index] == 0 else { return }
            let report = reportType == GamepadReportType.feature
                ? slots[index]?.profile.featureReport(number: number, slot: index) : nil
            stream?.send(
                .getReportReply(slot: index, id: id, error: report == nil ? GamepadErrno.invalid : 0, data: report ?? []))
        case .setReport(let index, _, _, let id, _):
            guard greeted else { return protocolBroken() }
            guard unanswered[index] == 0 else { return }  // See GET_REPORT.
            stream?.send(.setReportReply(slot: index, id: id, error: 0))
        }
    }

    /// The relay spoke before it said hello: it does not speak this protocol.
    private func protocolBroken() {
        stream?.drop()
    }

    // MARK: What the drivers ask

    private func apply(_ effects: GamepadOutputEffects, to index: Int) {
        guard var slot = slots[index] else { return }
        if let light = effects.light {
            slot.light = light
            slots[index] = slot
            indicators.setLight(controller: slot.controller.id, red: light.red, green: light.green, blue: light.blue)
        }
        if let requested = effects.player {
            slot.player = requested
            slots[index] = slot
            applyPlayer(slot)
        }
        if let rumble = effects.rumble, foreground { applyRumble(rumble, to: index) }
    }

    private func applyPlayer(_ slot: Slot) {
        switch slot.player {
        case .set(let index): indicators.setPlayerIndex(controller: slot.controller.id, index: index)
        case .unset: indicators.setPlayerIndex(controller: slot.controller.id, index: nil)
        case nil: break
        }
    }

    private func applyRumble(_ rumble: GamepadOutputEffects.Rumble, to index: Int) {
        guard var slot = slots[index] else { return }
        let now = clock()
        let longest = UInt64(HapticShapes.maximumToneMilliseconds) * 1_000_000
        for tone in GamepadRumble.tones(slot: index, haptics: slot.controller.haptics, rumble: rumble) {
            if tone.intensity <= 0 {
                if slot.tones.removeValue(forKey: tone.target) != nil { player.stop(tone.target) }
            } else {
                if let playing = slot.tones[tone.target], now - playing.began < longest {
                    if playing.intensity == tone.intensity { continue }
                    if player.setToneIntensity(tone.intensity, on: tone.target) {
                        slot.tones[tone.target]?.intensity = tone.intensity
                        continue
                    }
                    // Something else ended the tone: it plays again.
                }
                scoreSequence &+= 1
                let score = HapticScore(
                    target: tone.target, sequence: scoreSequence,
                    events: [
                        .tone(
                            durationMilliseconds: GamepadRumble.toneMilliseconds, intensity: tone.intensity,
                            sharpness: tone.sharpness)
                    ])
                slot.tones[tone.target] = player.play(score) == .played ? Tone(began: now, intensity: tone.intensity) : nil
            }
        }
        slots[index] = slot
    }
}
