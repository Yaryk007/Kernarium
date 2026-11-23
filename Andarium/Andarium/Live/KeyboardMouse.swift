// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import GameController
import GuestHostCore
import UIKit

// MARK: The runtime's end

/// The guest's hardware keyboard and mouse as the runtime offers them
/// (`kernarium_runtime_set_input_connected` and the entries after it). A fake
/// stands in for the runtime in the tests.
@MainActor
protocol HardwareInputRuntime: AnyObject {
    /// Plugs or unplugs the keyboard or the mouse; true when the runtime took
    /// the request, which it does not between runs.
    func setConnected(_ device: HardwareInputDevice, _ connected: Bool) -> Bool
    func sendKey(usage: UInt32, pressed: Bool)
    func sendPointer(_ report: PointerReport)
    func sendMouse(dx: Int32, dy: Int32, buttons: UInt32, wheel: Int32, horizontalWheel: Int32)
    /// What Android tells the host: the keyboard's lights and a capture of the
    /// pointer. The handler is called on the main actor.
    func setStatusHandler(_ handler: (@MainActor (HardwareInputStatus, Bool) -> Void)?)
}

enum HardwareInputDevice: Equatable, Sendable {
    case keyboard, mouse
}

enum HardwareInputStatus: Equatable, Sendable {
    case numLock, capsLock, scrollLock, pointerCapture
    /// The guest registered its keyboard, or its pointer (the mouse and the
    /// absolute pointer both), or lost it: the guest has the device only from
    /// then on, and a kernel without the hot-plug topic never says it.
    case keyboardRegistered, pointerRegistered
}

/// The loader's entries, the runtime of the device application.
@MainActor
final class LoaderHardwareInputRuntime: HardwareInputRuntime {
    func setConnected(_ device: HardwareInputDevice, _ connected: Bool) -> Bool {
        let number = switch device {
        case .keyboard: Int32(KERNARIUM_RUNTIME_INPUT_KEYBOARD)
        case .mouse: Int32(KERNARIUM_RUNTIME_INPUT_MOUSE)
        }
        return kernarium_runtime_loader_set_input_connected(number, connected) == 0
    }

    func sendKey(usage: UInt32, pressed: Bool) {
        _ = kernarium_runtime_loader_send_keyboard_usage(usage, pressed)
    }

    func sendPointer(_ report: PointerReport) {
        _ = kernarium_runtime_loader_send_pointer(report.x, report.y, report.buttons, report.inRange)
    }

    func sendMouse(dx: Int32, dy: Int32, buttons: UInt32, wheel: Int32, horizontalWheel: Int32) {
        _ = kernarium_runtime_loader_send_mouse(dx, dy, buttons, wheel, horizontalWheel)
    }

    func setStatusHandler(_ handler: (@MainActor (HardwareInputStatus, Bool) -> Void)?) {
        guard let handler else {
            kernarium_runtime_loader_set_input_status_handler(nil)
            return
        }
        // The loader delivers on the main queue, in the order the runtime
        // published.
        kernarium_runtime_loader_set_input_status_handler { status, on in
            let known: HardwareInputStatus
            switch Int(status) {
            case KERNARIUM_RUNTIME_INPUT_STATUS_NUM_LOCK: known = .numLock
            case KERNARIUM_RUNTIME_INPUT_STATUS_CAPS_LOCK: known = .capsLock
            case KERNARIUM_RUNTIME_INPUT_STATUS_SCROLL_LOCK: known = .scrollLock
            case KERNARIUM_RUNTIME_INPUT_STATUS_POINTER_CAPTURE: known = .pointerCapture
            case KERNARIUM_RUNTIME_INPUT_STATUS_KEYBOARD_REGISTERED: known = .keyboardRegistered
            case KERNARIUM_RUNTIME_INPUT_STATUS_POINTER_REGISTERED: known = .pointerRegistered
            default: return
            }
            MainActor.assumeIsolated { handler(known, on) }
        }
    }
}

// MARK: iOS's end

/// Which keyboards and mice iOS says are attached.
@MainActor
protocol HardwarePresence: AnyObject {
    var keyboardConnected: Bool { get }
    var mouseConnected: Bool { get }
    /// Called when either changes.
    var changed: (@MainActor () -> Void)? { get set }
    func start()
    func stop()
}

/// What the system's mouse reports raw, under the pointer lock and when the
/// lock is refused: motion in units of the mouse, the buttons that are down
/// and the scroll.
enum RawMouseEvent: Equatable {
    case moved(dx: Double, dy: Double)
    case buttons(UInt32)
    case scrolled(x: Double, y: Double)
}

@MainActor
protocol RawMouseSource: AnyObject {
    func startRaw(_ handler: @escaping @MainActor (RawMouseEvent) -> Void)
    func stopRaw()
}

/// Game Controller's keyboard and mouse. Its mouse is the one that reports
/// raw motion, unaffected by the pointer speed the person chose and still
/// reported while the pointer is locked.
@MainActor
final class GameControllerHardware: HardwarePresence, RawMouseSource {
    var changed: (@MainActor () -> Void)?
    private var observers: [NSObjectProtocol] = []
    private var rawHandler: (@MainActor (RawMouseEvent) -> Void)?
    private var buttons: UInt32 = 0

    var keyboardConnected: Bool { GCKeyboard.coalesced != nil }
    var mouseConnected: Bool { !GCMouse.mice().isEmpty }

    func start() {
        guard observers.isEmpty else { return }
        let names: [Notification.Name] = [
            .GCKeyboardDidConnect, .GCKeyboardDidDisconnect, .GCMouseDidConnect, .GCMouseDidDisconnect,
        ]
        observers = names.map { name in
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    if name == .GCMouseDidConnect, let mouse = note.object as? GCMouse { self.attach(mouse) }
                    self.changed?()
                }
            }
        }
    }

    func stop() {
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers = []
    }

    func startRaw(_ handler: @escaping @MainActor (RawMouseEvent) -> Void) {
        rawHandler = handler
        buttons = 0
        for mouse in GCMouse.mice() { attach(mouse) }
    }

    func stopRaw() {
        rawHandler = nil
        for mouse in GCMouse.mice() { detach(mouse) }
    }

    private func attach(_ mouse: GCMouse) {
        guard rawHandler != nil, let input = mouse.mouseInput else { return }
        input.mouseMovedHandler = { [weak self] _, dx, dy in
            MainActor.assumeIsolated { self?.rawHandler?(.moved(dx: Double(dx), dy: Double(dy))) }
        }
        input.scroll.valueChangedHandler = { [weak self] _, x, y in
            MainActor.assumeIsolated { self?.rawHandler?(.scrolled(x: Double(x), y: Double(y))) }
        }
        bind(input.leftButton, HardwareButtons.primary)
        if let right = input.rightButton { bind(right, HardwareButtons.secondary) }
        if let middle = input.middleButton { bind(middle, HardwareButtons.middle) }
        // The extra buttons are Back and then Forward, in the order the
        // system lists them.
        let extra = [HardwareButtons.back, HardwareButtons.forward]
        for (button, bit) in zip(input.auxiliaryButtons ?? [], extra) { bind(button, bit) }
    }

    private func detach(_ mouse: GCMouse) {
        guard let input = mouse.mouseInput else { return }
        input.mouseMovedHandler = nil
        input.scroll.valueChangedHandler = nil
        input.leftButton.pressedChangedHandler = nil
        input.rightButton?.pressedChangedHandler = nil
        input.middleButton?.pressedChangedHandler = nil
        for button in input.auxiliaryButtons ?? [] { button.pressedChangedHandler = nil }
    }

    private func bind(_ button: GCControllerButtonInput, _ bit: UInt32) {
        button.pressedChangedHandler = { [weak self] _, _, pressed in
            MainActor.assumeIsolated {
                guard let self else { return }
                if pressed { self.buttons |= bit } else { self.buttons &= ~bit }
                self.rawHandler?(.buttons(self.buttons))
            }
        }
    }
}

// MARK: The session

/// The guest's hardware keyboard and mouse while a run goes on.
///
/// The guest has them from boot, unplugged; this plugs them while iOS has the
/// hardware (`HardwareInputPolicy`), sends the keys the guest view takes as
/// positions and the system pointer as the guest's mouse, and answers an
/// Android application's capture of the pointer with the pointer lock and the
/// mouse's raw motion (`PointerCapture`). Leaving the front lifts every key
/// and button, ends the pointer's contact and drops the lock; the devices stay
/// plugged, as a keyboard stays paired.
@MainActor
final class KeyboardMouse: GuestHardwareSink {
    /// What the guest view hands its events to.
    let input: GuestHardwareInput

    private let runtime: any HardwareInputRuntime
    private let presence: any HardwarePresence
    private let rawMouse: (any RawMouseSource)?
    private let clock: () -> Double
    private let wantsLock: (Bool) -> Void
    private var policy = HardwareInputPolicy()
    private var capture = PointerCapture()
    private var motion = RelativeMotionAccumulator()
    private var rawScroll = ScrollAccumulator(pointsPerDetent: GuestHardwareInput.continuousPointsPerDetent)
    private var rawButtons: UInt32 = 0
    private var keysDown = Set<UInt32>()
    /// What the runtime has been asked and took.
    private var keyboardPlugged = false
    private var mousePlugged = false
    /// What the guest says it has registered: the system pointer is its mouse
    /// only once it has.
    private var pointerRegistered = false
    private var running = false
    private var keyboardWasConnected = false
    private var timer: Task<Void, Never>?

    /// `wantsLock` is told whether the pointer lock is wanted whenever that
    /// changes.
    init(
        runtime: (any HardwareInputRuntime)? = nil,
        presence: any HardwarePresence,
        rawMouse: (any RawMouseSource)?,
        supportsMouse: Bool,
        clock: @escaping () -> Double = { MonotonicClock.continuousSeconds() },
        wantsLock: @escaping (Bool) -> Void = { _ in }
    ) {
        self.runtime = runtime ?? LoaderHardwareInputRuntime()
        self.presence = presence
        self.rawMouse = rawMouse
        self.clock = clock
        self.wantsLock = wantsLock
        input = GuestHardwareInput(clock: clock)
        policy.supportsMouse = supportsMouse
        input.sink = self
        presence.changed = { [weak self] in self?.presenceChanged() }
        presence.start()
        self.runtime.setStatusHandler { [weak self] status, on in self?.androidSaid(status, on) }
    }

    /// The keyboard and mouse of the device application: Game Controller's.
    static func live(supportsMouse: Bool, wantsLock: @escaping (Bool) -> Void) -> KeyboardMouse {
        let hardware = GameControllerHardware()
        return KeyboardMouse(presence: hardware, rawMouse: hardware, supportsMouse: supportsMouse, wantsLock: wantsLock)
    }

    /// Lets go of iOS and the runtime. A test that builds sessions ends them
    /// with it; the application's lasts the process.
    func shutDown() {
        timer?.cancel()
        timer = nil
        runtime.setStatusHandler(nil)
        presence.changed = nil
        presence.stop()
        stopRawMouse()
    }

    // MARK: What LiveRuntime says

    /// The run has started and its devices exist (`running`), and the guest is
    /// in front and not held still (`foreground`). A sheet or an alert over
    /// it (`overlaid`) takes the keys, and the guest's capture of the pointer
    /// waits until it goes.
    func update(running: Bool, foreground: Bool, overlaid: Bool = false) {
        let inFront = running && foreground
        if self.running != running {
            self.running = running
            policy.running = running
            if running {
                presenceChanged()
            } else {
                // The runtime forgot what it was asked at the run's end, and
                // what the guest had registered went with the guest.
                keyboardPlugged = false
                mousePlugged = false
                pointerRegistered = false
                keysDown.removeAll()
                capture.reset()
                stopRawMouse()
                input.capturing = false
                input.routesPointer = false
                input.watchesPointer = false
                wantsLock(false)
            }
        }
        input.isActive = inFront
        applyCapture(capture.inFront(inFront && !overlaid))
        evaluate()
    }

    /// The guest restarted itself: the keys it was sent are gone with it, its
    /// Caps Lock starts off, and so do what it registered and what an
    /// application had captured. The runtime plugs the devices into the next
    /// boot as they were.
    func guestRestarted() {
        input.releaseAll()
        keysDown.removeAll()
        input.keyboardPlugged()
        pointerRegistered = false
        applyCapture(capture.androidCaptured(false))
        evaluate()
    }

    /// The person turned "Use the pointer as touch" on or off.
    func setPointerAsTouch(_ on: Bool) {
        policy.pointerAsTouch = on
        evaluate()
    }

    /// The scene's pointer lock was granted or released.
    func pointerLockChanged(_ locked: Bool) {
        capture.lockHeld(locked)
        updateRawMouse()
    }

    // MARK: Plugging

    private func presenceChanged() {
        let now = clock()
        let keyboard = presence.keyboardConnected
        // A keyboard that goes takes the release of its keys with it: UIKit
        // sends none.
        if keyboardWasConnected, !keyboard { input.releaseKeys() }
        keyboardWasConnected = keyboard
        policy.keyboardReported(keyboard, at: now)
        policy.mouseReported(presence.mouseConnected, at: now)
        evaluate()
    }

    /// Makes the guest's devices what the policy says, and arranges to ask
    /// again when its plan changes by the clock.
    func evaluate() {
        // Releasing what is held on an unplug reaches the sink, which asks
        // again: that is the same pass.
        guard !evaluating else {
            evaluateAgain = true
            return
        }
        evaluating = true
        defer { evaluating = false }
        repeat {
            evaluateAgain = false
            reconcile()
        } while evaluateAgain
    }

    private var evaluating = false
    private var evaluateAgain = false

    private func reconcile() {
        let plan = policy.plan(at: clock())
        if plan.keyboardPlugged != keyboardPlugged {
            if !plan.keyboardPlugged { input.releaseKeys() }
            if runtime.setConnected(.keyboard, plan.keyboardPlugged) {
                keyboardPlugged = plan.keyboardPlugged
                if keyboardPlugged { input.keyboardPlugged() } else { keysDown.removeAll() }
            }
        }
        if plan.mousePlugged != mousePlugged {
            if !plan.mousePlugged { input.releasePointer() }
            if runtime.setConnected(.mouse, plan.mousePlugged) {
                mousePlugged = plan.mousePlugged
                applyCapture(capture.mousePlugged(mousePlugged))
            }
        }
        // The system pointer is the guest's mouse once the guest has
        // registered it. Until then, and on a kernel that never does, its
        // clicks are touches as they are with no mouse, and what the person
        // does with it only plugs the mouse.
        let routes = plan.routesPointer && mousePlugged && pointerRegistered
        if routes {
            // Routing first: the position the watching kept goes to the guest.
            input.routesPointer = true
            input.watchesPointer = false
        } else {
            input.watchesPointer = plan.routesPointer
            input.routesPointer = false
        }
        schedule(plan.nextChange)
    }

    private func schedule(_ time: Double?) {
        timer?.cancel()
        timer = nil
        guard let time else { return }
        let delay = max(time - clock(), 0) + 0.05
        timer = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled else { return }
            self?.evaluate()
        }
    }

    // MARK: The events of the guest view

    func hardwareKeyboardUsed() {
        policy.keyDown(at: clock())
        // The key that plugs the keyboard is the first of its keys: the
        // runtime holds what comes before the guest has registered the device.
        evaluate()
    }

    func hardwareKey(usage: UInt32, pressed: Bool) {
        if pressed { keysDown.insert(usage) } else { keysDown.remove(usage) }
        policy.keysHeld(!keysDown.isEmpty)
        guard keyboardPlugged else { return }
        runtime.sendKey(usage: usage, pressed: pressed)
        // A key that comes up may let an unplug happen.
        if !pressed { evaluate() }
    }

    func hardwarePointerUsed() {
        policy.pointerUsed()
        evaluate()
    }

    func hardwarePointer(_ report: PointerReport) {
        capture.pointerOverDisplay(input.pointerIsOverDisplay)
        guard mousePlugged else { return }
        runtime.sendPointer(report)
    }

    func hardwareMouse(dx: Int32, dy: Int32, buttons: UInt32, wheel: Int32, horizontalWheel: Int32) {
        guard mousePlugged else { return }
        runtime.sendMouse(dx: dx, dy: dy, buttons: buttons, wheel: wheel, horizontalWheel: horizontalWheel)
    }

    // MARK: What Android says

    private func androidSaid(_ status: HardwareInputStatus, _ on: Bool) {
        // Nothing the guest says outlives its run.
        guard running else { return }
        switch status {
        case .capsLock:
            input.androidCapsLock(lit: on)
        case .pointerCapture:
            applyCapture(capture.androidCaptured(on))
        case .pointerRegistered:
            pointerRegistered = on
            evaluate()
        case .keyboardRegistered:
            // Keys wait in the runtime until the keyboard is registered, so
            // there is nothing for the application to hold back.
            break
        case .numLock, .scrollLock:
            break
        }
    }

    // MARK: Capture

    private func applyCapture(_ change: PointerCapture.Change) {
        switch change {
        case .began:
            // The system's pointer is no longer the guest's: its contact ends
            // and the raw mouse takes over.
            input.capturing = true
            motion.reset()
            rawScroll.reset()
            rawButtons = 0
        case .ended:
            input.capturing = false
            if rawButtons != 0 {
                rawButtons = 0
                if mousePlugged { runtime.sendMouse(dx: 0, dy: 0, buttons: 0, wheel: 0, horizontalWheel: 0) }
            }
        case .none:
            break
        }
        wantsLock(capture.wantsLock)
        updateRawMouse()
    }

    private var rawMouseStarted = false

    private func updateRawMouse() {
        guard let rawMouse else { return }
        if capture.isCapturing, !rawMouseStarted {
            rawMouseStarted = true
            rawMouse.startRaw { [weak self] event in self?.raw(event) }
        } else if !capture.isCapturing, rawMouseStarted {
            stopRawMouse()
        }
    }

    private func stopRawMouse() {
        guard rawMouseStarted else { return }
        rawMouseStarted = false
        rawMouse?.stopRaw()
    }

    /// The raw mouse's events, forwarded while the guest has the pointer.
    func raw(_ event: RawMouseEvent) {
        capture.pointerOverDisplay(input.pointerIsOverDisplay)
        guard mousePlugged, capture.forwardsRawMotion else { return }
        switch event {
        case let .moved(dx, dy):
            let step = motion.add(gameControllerDeltaX: dx, deltaY: dy)
            guard step.dx != 0 || step.dy != 0 else { return }
            runtime.sendMouse(dx: step.dx, dy: step.dy, buttons: rawButtons, wheel: 0, horizontalWheel: 0)
        case let .buttons(mask):
            rawButtons = mask
            runtime.sendMouse(dx: 0, dy: 0, buttons: mask, wheel: 0, horizontalWheel: 0)
        case let .scrolled(x, y):
            // Game Controller reports a horizontal scroll with the sign of the
            // wheel, which UIKit's translation does not have.
            let step = rawScroll.add(translationX: -x, translationY: y)
            guard step.wheel != 0 || step.horizontalWheel != 0 else { return }
            runtime.sendMouse(
                dx: 0, dy: 0, buttons: rawButtons, wheel: step.wheel, horizontalWheel: step.horizontalWheel)
        }
    }
}

// MARK: A recording runtime

/// What the guest's keyboard and mouse were sent, for the tests and for the
/// UI tests' probe launch.
enum HardwareInputEvent: Equatable {
    case connect(HardwareInputDevice, Bool)
    case key(usage: UInt32, pressed: Bool)
    case pointer(PointerReport)
    case mouse(dx: Int32, dy: Int32, buttons: UInt32, wheel: Int32, horizontalWheel: Int32)

    /// One line of the probe's log.
    var line: String {
        switch self {
        case let .connect(device, connected):
            "connect \(device == .keyboard ? "keyboard" : "mouse") \(connected ? 1 : 0)"
        case let .key(usage, pressed):
            "key 0x\(String(usage, radix: 16)) \(pressed ? "down" : "up")"
        case let .pointer(report):
            "pointer \(report.x) \(report.y) b\(report.buttons) \(report.inRange ? "in" : "out")"
        case let .mouse(dx, dy, buttons, wheel, horizontalWheel):
            "mouse \(dx) \(dy) b\(buttons) w\(wheel) h\(horizontalWheel)"
        }
    }
}

/// A runtime that takes what it is sent and keeps it. It refuses plug requests
/// while `accepts` is false, as the runtime does between runs, and tells
/// Android's side what a test makes the guest say. With `registers` set it
/// plays a guest that registers each device it is plugged and unregisters it
/// when it is unplugged; without it nothing is registered until a test says so.
@MainActor
final class RecordingHardwareInputRuntime: HardwareInputRuntime {
    private(set) var events: [HardwareInputEvent] = []
    var accepts = true
    var registers = false
    var onEvent: ((HardwareInputEvent) -> Void)?
    private var statusHandler: (@MainActor (HardwareInputStatus, Bool) -> Void)?

    init(onEvent: ((HardwareInputEvent) -> Void)? = nil) {
        self.onEvent = onEvent
    }

    private func record(_ event: HardwareInputEvent) {
        events.append(event)
        onEvent?(event)
    }

    func setConnected(_ device: HardwareInputDevice, _ connected: Bool) -> Bool {
        guard accepts else { return false }
        record(.connect(device, connected))
        if registers {
            // The guest's driver answers after the plug, not within it.
            let status: HardwareInputStatus = device == .keyboard ? .keyboardRegistered : .pointerRegistered
            Task { @MainActor [weak self] in self?.androidSays(status, connected) }
        }
        return true
    }

    func sendKey(usage: UInt32, pressed: Bool) {
        record(.key(usage: usage, pressed: pressed))
    }

    func sendPointer(_ report: PointerReport) {
        record(.pointer(report))
    }

    func sendMouse(dx: Int32, dy: Int32, buttons: UInt32, wheel: Int32, horizontalWheel: Int32) {
        record(.mouse(dx: dx, dy: dy, buttons: buttons, wheel: wheel, horizontalWheel: horizontalWheel))
    }

    func setStatusHandler(_ handler: (@MainActor (HardwareInputStatus, Bool) -> Void)?) {
        statusHandler = handler
    }

    /// Android says something, as the runtime would deliver it.
    func androidSays(_ status: HardwareInputStatus, _ on: Bool) {
        statusHandler?(status, on)
    }
}

extension KeyboardMouse {
    /// The keyboard and mouse of a UI test's launch: iOS's own hardware, the
    /// guest's end a log (`InputProbe`).
    static func probe(
        _ probe: InputProbe, supportsMouse: Bool, guestRegisters: Bool, wantsLock: @escaping (Bool) -> Void
    ) -> KeyboardMouse {
        let hardware = GameControllerHardware()
        let runtime = RecordingHardwareInputRuntime { probe.record($0.line) }
        runtime.registers = guestRegisters
        return KeyboardMouse(
            runtime: runtime, presence: hardware, rawMouse: hardware, supportsMouse: supportsMouse,
            wantsLock: wantsLock)
    }
}
