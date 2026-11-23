// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreHaptics
import Foundation
@preconcurrency import GameController
import SwiftUI
import UIKit

/// The connection to the guest's gamepad relay: vsock 5558, with the same
/// bound on one attempt as motion's, haptics' and thermal's.
nonisolated struct GamepadConnector: GuestStreamOpener {
    func openStream() throws -> Int32 {
        let descriptor = kernarium_runtime_loader_connect_vsock(GamepadProtocol.port, 250)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -descriptor) ?? .EIO) }
        return descriptor
    }
}

/// What the game controllers say about themselves, in the development builds only.
nonisolated enum GamepadDiagnostics {
    static func note(_ text: @autoclosure () -> String) {
        print("Andarium: \(text())")
    }
}

// MARK: From GameController to the neutral state

/// What iOS says about one controller's elements at a moment, as plain
/// values. The buffered input states of iOS 17 and the profile of a
/// controller (the one a snapshot controller has too) both read into it, and
/// `GamepadInputMapper` turns it into the state the guest is sent.
nonisolated struct GamepadReading: Equatable, Sendable {
    struct Button: Equatable, Sendable {
        var pressed = false
        var value: Float = 0
    }

    /// The buttons iOS names, each with the name it keys them by.
    enum Element: CaseIterable, Sendable {
        case a, b, x, y
        case leftShoulder, rightShoulder, leftTrigger, rightTrigger
        case home, menu, options, share
        case leftThumbstickButton, rightThumbstickButton

        var name: GCButtonElementName {
            switch self {
            case .a: .a
            case .b: .b
            case .x: .x
            case .y: .y
            case .leftShoulder: .leftShoulder
            case .rightShoulder: .rightShoulder
            case .leftTrigger: .leftTrigger
            case .rightTrigger: .rightTrigger
            case .home: .home
            case .menu: .menu
            case .options: .options
            case .share: .share
            case .leftThumbstickButton: .leftThumbstickButton
            case .rightThumbstickButton: .rightThumbstickButton
            }
        }
    }

    var buttons: [Element: Button] = [:]
    var leftStick = GamepadStick.centered
    var rightStick = GamepadStick.centered
    var directions: GamepadDirections = []

    init(
        buttons: [Element: Button] = [:], leftStick: GamepadStick = .centered, rightStick: GamepadStick = .centered,
        directions: GamepadDirections = []
    ) {
        self.buttons = buttons
        self.leftStick = leftStick
        self.rightStick = rightStick
        self.directions = directions
    }

    /// One buffered input state of iOS 17's `GCController.input`.
    init(state: any GCDevicePhysicalInputState) {
        for element in Element.allCases {
            guard let button = state.buttons[element.name] else { continue }
            buttons[element] = Button(pressed: button.pressedInput.isPressed, value: button.pressedInput.value)
        }
        if let stick = state.dpads[.leftThumbstick] { leftStick = GamepadStick(x: stick.xAxis.value, y: stick.yAxis.value) }
        if let stick = state.dpads[.rightThumbstick] { rightStick = GamepadStick(x: stick.xAxis.value, y: stick.yAxis.value) }
        if let pad = state.dpads[.directionPad] {
            if pad.up.isPressed { directions.insert(.up) }
            if pad.down.isPressed { directions.insert(.down) }
            if pad.left.isPressed { directions.insert(.left) }
            if pad.right.isPressed { directions.insert(.right) }
        }
    }

    /// The elements as a controller's physical input profile holds them now.
    init(profile: GCPhysicalInputProfile) {
        for element in Element.allCases {
            guard let button = profile.buttons[element.name.rawValue] else { continue }
            buttons[element] = Button(pressed: button.isPressed, value: button.value)
        }
        if let stick = profile.dpads[GCDirectionPadElementName.leftThumbstick.rawValue] {
            leftStick = GamepadStick(x: stick.xAxis.value, y: stick.yAxis.value)
        }
        if let stick = profile.dpads[GCDirectionPadElementName.rightThumbstick.rawValue] {
            rightStick = GamepadStick(x: stick.xAxis.value, y: stick.yAxis.value)
        }
        if let pad = profile.dpads[GCDirectionPadElementName.directionPad.rawValue] {
            if pad.up.isPressed { directions.insert(.up) }
            if pad.down.isPressed { directions.insert(.down) }
            if pad.left.isPressed { directions.insert(.left) }
            if pad.right.isPressed { directions.insert(.right) }
        }
    }
}

/// The rules that turn what iOS reports into what the guest is told, each
/// written out element by element.
nonisolated enum GamepadInputMapper {
    /// The buttons and values of a reading as the neutral state. iOS names the
    /// face buttons by position (A south, B east, X west, Y north, whatever the
    /// controller prints on them), and applies the person's remapping from
    /// Settings before the application sees an element.
    static func state(_ reading: GamepadReading) -> GamepadState {
        func down(_ element: GamepadReading.Element) -> Bool { reading.buttons[element]?.pressed == true }
        var buttons: GamepadButtons = []
        if down(.a) { buttons.insert(.south) }
        if down(.b) { buttons.insert(.east) }
        if down(.x) { buttons.insert(.west) }
        if down(.y) { buttons.insert(.north) }
        if down(.leftShoulder) { buttons.insert(.leftShoulder) }
        if down(.rightShoulder) { buttons.insert(.rightShoulder) }
        if down(.leftTrigger) { buttons.insert(.leftTrigger) }
        if down(.rightTrigger) { buttons.insert(.rightTrigger) }
        // Options on iOS is the Create, Share or View button, and Menu is
        // PlayStation's Options and Xbox's Menu.
        if down(.options) { buttons.insert(.select) }
        if down(.menu) { buttons.insert(.start) }
        if down(.home) { buttons.insert(.mode) }
        // Share on iOS is the Xbox controller's own button, which Android
        // reads as Record.
        if down(.share) { buttons.insert(.record) }
        if down(.leftThumbstickButton) { buttons.insert(.leftThumbstick) }
        if down(.rightThumbstickButton) { buttons.insert(.rightThumbstick) }
        return GamepadState(
            buttons: buttons, directions: reading.directions, leftStick: reading.leftStick,
            rightStick: reading.rightStick, leftTrigger: reading.buttons[.leftTrigger]?.value ?? 0,
            rightTrigger: reading.buttons[.rightTrigger]?.value ?? 0)
    }

    /// Which family of guest device a controller is: the three PlayStation
    /// ones and the Xbox one have their own, and everything else (MFi, Switch,
    /// arcade sticks, other HID gamepads) is a plain HID gamepad. iOS reports
    /// the DualSense Edge as a DualSense, which only its name tells apart.
    static func family(productCategory: String, name: String) -> GamepadFamily {
        switch productCategory {
        case GCProductCategoryDualSense: name.localizedCaseInsensitiveContains("edge") ? .dualSenseEdge : .dualSense
        case GCProductCategoryDualShock4: .dualShock4
        case GCProductCategoryXboxOne: .xbox
        default: .generic
        }
    }

    /// What a generic device lists: the elements the controller has, so that
    /// Android's answer to "has this key" is true.
    static func elements(of profile: GCPhysicalInputProfile) -> GamepadElements {
        func has(_ element: GamepadReading.Element) -> Bool { profile.buttons[element.name.rawValue] != nil }
        var buttons: GamepadButtons = []
        let present: [(GamepadReading.Element, GamepadButtons)] = [
            (.a, .south), (.b, .east), (.x, .west), (.y, .north), (.leftShoulder, .leftShoulder),
            (.rightShoulder, .rightShoulder), (.leftTrigger, .leftTrigger), (.rightTrigger, .rightTrigger),
            (.options, .select), (.menu, .start), (.home, .mode), (.share, .record),
            (.leftThumbstickButton, .leftThumbstick), (.rightThumbstickButton, .rightThumbstick),
        ]
        for (element, button) in present where has(element) { buttons.insert(button) }
        func has(_ name: GCDirectionPadElementName) -> Bool { profile.dpads[name.rawValue] != nil }
        return GamepadElements(
            buttons: buttons, leftStick: has(.leftThumbstick), rightStick: has(.rightThumbstick),
            triggers: profile.buttons[GCButtonElementName.leftTrigger.rawValue]?.isAnalog == true,
            directionPad: has(.directionPad))
    }

    /// Whether and how a controller can rumble: handles that play the two
    /// motors, one engine for the whole controller, or nothing.
    static func haptics(_ supported: Set<GCHapticsLocality>?) -> GamepadHaptics {
        guard let supported else { return .none }
        return supported.contains(.leftHandle) && supported.contains(.rightHandle) ? .handles : .single
    }

    static func battery(_ battery: GCDeviceBattery?) -> GamepadBattery {
        guard let battery else { return .none }
        let state: GamepadBattery.State =
            switch battery.batteryState {
            case .charging: .charging
            case .full: .full
            case .discharging: .discharging
            default: .unknown
            }
        return GamepadBattery(level: battery.batteryLevel, state: state)
    }
}

// MARK: Rumble engines

/// Where the haptic engine of a game controller's actuator comes from: the
/// controller that holds the target's slot, and the locality of the target.
/// A controller with no such actuator has no engine, and its rumble is dropped.
nonisolated final class GameControllerEngines: HapticEngineSource, @unchecked Sendable {
    private let lock = NSLock()
    private var resolver: (@Sendable (Int) -> GCController?)?

    /// The controller that holds a slot, set by `GameControllers`.
    func setResolver(_ resolver: @escaping @Sendable (Int) -> GCController?) {
        lock.withLock { self.resolver = resolver }
    }

    func engine(for target: HapticTarget) throws -> CHHapticEngine {
        guard let (slot, locality) = target.controller, let resolve = lock.withLock({ resolver }),
            let haptics = resolve(slot)?.haptics, let engine = haptics.createEngine(withLocality: Self.locality(locality))
        else { throw CHHapticError(.notSupported) }
        // As the device's own engine (`DeviceHapticEngines`): haptics only, and
        // idle between tones.
        engine.playsHapticsOnly = true
        engine.isAutoShutdownEnabled = true
        return engine
    }

    static func locality(_ locality: HapticTarget.ControllerLocality) -> GCHapticsLocality {
        switch locality {
        case .standard: .default
        case .leftHandle: .leftHandle
        case .rightHandle: .rightHandle
        }
    }
}

/// The device's own engine for its own target, and a game controller's for
/// the targets of its actuators.
nonisolated struct HapticEngineSources: HapticEngineSource {
    var device: any HapticEngineSource
    var controllers: any HapticEngineSource

    func engine(for target: HapticTarget) throws -> CHHapticEngine {
        target.controller == nil ? try device.engine(for: target) : try controllers.engine(for: target)
    }
}

// MARK: The controllers

/// The controllers the application holds and the numbers it gave them, which
/// the session, the rumble engines and the indicators all look controllers up
/// by.
nonisolated final class ControllerTable: @unchecked Sendable {
    final class Entry: @unchecked Sendable {
        let id: Int
        let controller: GCController
        init(id: Int, controller: GCController) {
            self.id = id
            self.controller = controller
        }
    }

    private let lock = NSLock()
    private var entries: [ObjectIdentifier: Entry] = [:]
    private var nextID = 1

    /// A new number for a controller the table does not hold yet.
    func add(_ controller: GCController) -> Entry? {
        lock.withLock {
            guard entries[ObjectIdentifier(controller)] == nil else { return nil }
            defer { nextID += 1 }
            let entry = Entry(id: nextID, controller: controller)
            entries[ObjectIdentifier(controller)] = entry
            return entry
        }
    }

    func remove(_ controller: GCController) -> Entry? {
        lock.withLock { entries.removeValue(forKey: ObjectIdentifier(controller)) }
    }

    func controller(id: Int) -> GCController? {
        lock.withLock { entries.values.first { $0.id == id }?.controller }
    }

    var all: [Entry] { lock.withLock { Array(entries.values) } }
    var isEmpty: Bool { lock.withLock { entries.isEmpty } }
}

/// What the guest's drivers ask of a controller besides rumble, applied to the
/// controller that has the number.
nonisolated final class GameControllerIndicators: GamepadIndicators, @unchecked Sendable {
    private let table: ControllerTable

    init(table: ControllerTable) { self.table = table }

    // Both are set on the main queue: iOS observes a controller's player
    // index there, and the light and the number then go through one place.
    func setLight(controller id: Int, red: UInt8, green: UInt8, blue: UInt8) {
        guard let controller = table.controller(id: id) else { return }
        let color = GCColor(red: Float(red) / 255, green: Float(green) / 255, blue: Float(blue) / 255)
        DispatchQueue.main.async { controller.light?.color = color }
    }

    func setPlayerIndex(controller id: Int, index: Int?) {
        guard let controller = table.controller(id: id) else { return }
        let player: GCControllerPlayerIndex =
            switch index {
            case 0: .index1
            case 1: .index2
            case 2: .index3
            case 3: .index4
            default: .indexUnset
            }
        DispatchQueue.main.async { controller.playerIndex = player }
    }
}

/// The game controllers iOS has connected, given to the guest's gamepad relay
/// (`GamepadSession`). Controllers are looked for only while a normal run
/// goes on: it listens for connects and disconnects, takes the ones already
/// connected, reads each one's buffered input states and battery, and applies
/// the light and player number the guest's drivers ask for. Nothing is
/// scanned for or paired; iOS owns the controllers. With no controller
/// nothing is sent and nothing can fail.
///
/// Home, Menu, Options and Share are taken from iOS's gestures, so they reach
/// the guest as buttons (iOS decides in its own settings whether it lets an
/// application have the Home button), and are given back when the controller
/// is let go.
nonisolated final class GameControllers: @unchecked Sendable {
    let session: GamepadSession
    let table = ControllerTable()
    private let center: NotificationCenter
    private let discover: @Sendable () -> [GCController]
    private let include: @Sendable (GCController) -> Bool
    private let readBattery: @Sendable (GCController) -> GamepadBattery
    private let batteryInterval: DispatchTimeInterval
    private let readState: @Sendable (GCControllerLiveInput) -> GamepadState?
    private let lock = NSLock()
    private var running = false
    private var foreground = false
    /// Counts the times the application has gone to the back or come to the
    /// front, under `lock`, together with the session being told: a callback
    /// of the controller queue that began in an earlier count reads states
    /// from before that transition, and `drain` does not pass them on.
    private var epoch: UInt64 = 0
    private var observers: [NSObjectProtocol] = []
    private var batteryTimer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "andarium-game-controllers", qos: .userInteractive)

    /// `center` and `discover` are where connects are announced and the
    /// connected controllers listed, `include` which of them are taken,
    /// `readBattery` how a battery is read and `batteryInterval` how often,
    /// and `readState` how the next buffered input state of a controller is
    /// read (nil when none is left): a test replaces them, and the application
    /// leaves the defaults.
    init(
        player: any HapticPlayer, engines: GameControllerEngines, opener: any GuestStreamOpener = GamepadConnector(),
        center: NotificationCenter = .default,
        discover: @escaping @Sendable () -> [GCController] = { GCController.controllers() },
        include: @escaping @Sendable (GCController) -> Bool = { _ in true },
        readBattery: @escaping @Sendable (GCController) -> GamepadBattery = { GamepadInputMapper.battery($0.battery) },
        batteryInterval: DispatchTimeInterval = .seconds(30),
        readState: @escaping @Sendable (GCControllerLiveInput) -> GamepadState? = { live in
            live.nextInputState().map { GamepadInputMapper.state(GamepadReading(state: $0)) }
        },
        retryDelay: DispatchTimeInterval = .seconds(1),
        wake: @escaping @Sendable () -> Void = { kernarium_runtime_loader_wake_for_input() }
    ) {
        self.center = center
        self.discover = discover
        self.include = include
        self.readBattery = readBattery
        self.batteryInterval = batteryInterval
        self.readState = readState
        let table = table
        let session = GamepadSession(
            opener: opener, player: player, indicators: GameControllerIndicators(table: table), retryDelay: retryDelay,
            wake: wake, log: { GamepadDiagnostics.note($0) })
        self.session = session
        // The engine source is asked from the session's own queue, while the
        // session may be playing: both tables are read from any thread.
        engines.setResolver { slot in session.controller(inSlot: slot).flatMap { table.controller(id: $0) } }
    }

    deinit {
        for observer in observers { center.removeObserver(observer) }
        batteryTimer?.cancel()
    }

    // MARK: What is wanted

    /// What is wanted now, as `GamepadSession.update` takes it. Controllers
    /// are looked for from the moment a run begins until it ends. Leaving the
    /// front lets every controller go, and returning to it shows the guest
    /// nothing held. iOS delivers no input to an application that is not in
    /// front, and until a controller reports again both its snapshot
    /// (`capture`) and its element values still say what they said before the
    /// stretch: a button let go of meanwhile reads as held, with no report to
    /// correct it. What the controller holds reaches the guest with its next
    /// report (`drain`), which carries every element.
    func update(running: Bool, booted: Bool, foreground: Bool) {
        let change = lock.withLock { () -> Bool? in
            let change: Bool? = running != self.running ? running : nil
            self.running = running
            return change
        }
        if change == false { stopLooking() }
        // `drain` passes a state on under this lock too, so a state is queued
        // for the session either before this transition, to be let go of by it,
        // or after it, and then its callback began after it.
        lock.withLock {
            if foreground != self.foreground {
                self.foreground = foreground
                epoch &+= 1
            }
            session.update(running: running, booted: booted, foreground: foreground)
        }
        if change == true { startLooking() }
    }

    /// The guest restarted itself: the controllers stay, and the relay is
    /// connected to again.
    func restart() { session.restart() }

    // MARK: Looking

    private func startLooking() {
        let connect = center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
            if let controller = note.object as? GCController { self?.attach(controller) }
        }
        let disconnect = center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] note in
            if let controller = note.object as? GCController { self?.detach(controller) }
        }
        lock.withLock { observers = [connect, disconnect] }
        for controller in discover() { attach(controller) }
    }

    private func stopLooking() {
        let removed = lock.withLock { () -> [NSObjectProtocol] in
            defer { observers = [] }
            return observers
        }
        for observer in removed { center.removeObserver(observer) }
        for entry in table.all { detach(entry.controller) }
    }

    /// Takes a controller: its gestures, its input, its battery, and the
    /// guest's device for it.
    func attach(_ controller: GCController) {
        guard lock.withLock({ running }), include(controller) else { return }
        guard let pad = controller.extendedGamepad else {
            GamepadDiagnostics.note("a controller with no extended gamepad profile is left out")
            return
        }
        guard let entry = table.add(controller) else { return }

        // The buttons iOS keeps for its own gestures come to the guest.
        Self.setSystemGestures(.disabled, on: controller)

        let name = controller.vendorName ?? ""
        let family = GamepadInputMapper.family(productCategory: controller.productCategory, name: name)
        let description = GamepadController(
            id: entry.id, family: family, name: name, elements: GamepadInputMapper.elements(of: controller.physicalInputProfile),
            haptics: GamepadInputMapper.haptics(controller.haptics?.supportedLocalities))
        let live = controller.input
        live.queue = queue
        live.inputStateQueueDepth = 20
        session.connect(description, state: GamepadInputMapper.state(GamepadReading(profile: pad)))
        session.updateBattery(readBattery(controller), for: entry.id)
        let id = entry.id
        // The input object cannot be referenced weakly: the handler takes it
        // from its own argument.
        live.inputStateAvailableHandler = { [weak self] input in
            guard let self, let live = input as? GCControllerLiveInput else { return }
            drain(live, id: id)
        }
        GamepadDiagnostics.note("controller \(id) connected: \(name), \(family)")
        scheduleBatteryReads()
    }

    /// A controller is gone: its device, its input handler and its slot.
    func detach(_ controller: GCController) {
        guard let entry = table.remove(controller) else { return }
        controller.input.inputStateAvailableHandler = nil
        // The buttons go back to iOS's gestures: another application may have
        // the controller next, and so may Andarium's own screens.
        Self.setSystemGestures(.enabled, on: controller)
        session.disconnect(id: entry.id)
        GamepadDiagnostics.note("controller \(entry.id) disconnected")
        if table.isEmpty { cancelBatteryReads() }
    }

    /// What iOS does with the buttons it keeps for its own gestures on a
    /// controller: Home, Menu, Options and Share.
    private static func setSystemGestures(_ state: GCControllerElement.SystemGestureState, on controller: GCController) {
        for element in [GamepadReading.Element.home, .menu, .options, .share] {
            controller.physicalInputProfile.buttons[element.name.rawValue]?.preferredSystemGestureState = state
        }
    }

    /// Reads every buffered state in order, so that no tap between two
    /// callbacks is lost; the session sends what changes a button at once. The
    /// callback runs on the controller queue, apart from the application's
    /// going to the back and coming to the front, so a state may be read
    /// before one and reach this point after it: it is the controller's state
    /// from before, which the guest has been told to let go of, and so is each
    /// state the callback reads after it. They are read and dropped, so that
    /// none is left for a later callback, and the controller's next report is
    /// its state. The only old state that can still be taken as new is one
    /// waiting in iOS's queue at a callback that first runs after the return.
    private func drain(_ live: GCControllerLiveInput, id: Int) {
        let read = lock.withLock { epoch }
        while let state = readState(live) {
            lock.withLock {
                guard epoch == read else { return }
                session.submit(state, for: id)
            }
        }
    }

    // MARK: Battery

    private func scheduleBatteryReads() {
        lock.withLock {
            guard batteryTimer == nil else { return }
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + batteryInterval, repeating: batteryInterval)
            timer.setEventHandler { [weak self] in self?.readBatteries() }
            batteryTimer = timer
            timer.resume()
        }
    }

    private func cancelBatteryReads() {
        let timer = lock.withLock { () -> DispatchSourceTimer? in
            defer { batteryTimer = nil }
            return batteryTimer
        }
        timer?.cancel()
    }

    /// Reads every controller's battery and tells the session.
    func readBatteries() {
        for entry in table.all {
            session.updateBattery(readBattery(entry.controller), for: entry.id)
        }
    }
}

// MARK: The session's view

/// Tells UIKit that the session's view takes game controller events through
/// the Game Controller framework only, so that iOS does not also turn them
/// into focus and press events in Andarium's own screens.
struct GameControllerEventInteraction: UIViewRepresentable {
    /// A view that carries the interaction.
    static func makeView() -> UIView {
        let view = UIView()
        let interaction = GCEventInteraction()
        interaction.handledEventTypes = .gamepad
        view.addInteraction(interaction)
        return view
    }

    func makeUIView(context: Context) -> UIView { Self.makeView() }

    func updateUIView(_ view: UIView, context: Context) {}
}
