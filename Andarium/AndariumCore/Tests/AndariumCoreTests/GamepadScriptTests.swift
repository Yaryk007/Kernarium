// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

/// The scripts the probe tool plays: that each plays against a session in
/// order, and that the built-in ones name what they promise.
struct GamepadScriptTests {
    private final class Sleeps: @unchecked Sendable {
        private let lock = NSLock()
        private var list: [Int] = []
        var values: [Int] { lock.withLock { list } }
        func add(_ value: Int) { lock.withLock { list.append(value) } }
    }

    private func session(_ guest: GamepadFakeGuest) -> GamepadSession {
        GamepadSession(
            opener: guest, player: GamepadFakePlayer(), indicators: GamepadFakeIndicators(),
            retryDelay: .milliseconds(10), schedule: { _, _ in }, wake: {})
    }

    @Test func everyNamedScriptExists() {
        for name in GamepadScript.names { #expect(GamepadScript.named(name) != nil, "\(name)") }
        #expect(GamepadScript.named("nonsense") == nil)
    }

    @Test func theIdentityScriptMakesOneDevicePerFamilyInSlotOrder() {
        let guest = GamepadFakeGuest()
        let session = session(guest)
        session.update(running: true, booted: false, foreground: true)
        #expect(gamepadEventually { guest.connections == 1 })
        guest.send(.hello(version: 1, slotCount: 8))
        #expect(gamepadEventually { session.isConnected })
        let sleeps = Sleeps()
        GamepadScript.identity.play(on: session) { sleeps.add($0) }
        let names = (0..<4).compactMap { _ -> String? in
            if case .create(let create)? = guest.nextFrame() { create.name } else { nil }
        }
        #expect(names == ["DualSense Wireless Controller", "Wireless Controller", "Xbox Wireless Controller", "Probe Pad"])
        #expect(sleeps.values == [500])
    }

    @Test func theButtonsScriptPressesEveryButtonOnEveryController() {
        let steps = GamepadScript.buttons.steps
        let pressed = steps.compactMap { step -> GamepadButtons? in
            if case .submit(1, let state) = step, !state.buttons.isEmpty { state.buttons } else { nil }
        }
        // The thirteen standard buttons one at a time, and Record.
        #expect(pressed.count == 14)
        #expect(pressed.reduce(GamepadButtons()) { $0.union($1) } == GamepadButtons.standard.union(.record))
        for id in 1...4 {
            let count = steps.filter { if case .submit(id, _) = $0 { true } else { false } }.count
            #expect(count == 2 * (14 + 4 + 10), "controller \(id)")
        }
    }

    @Test func theHotPlugScriptDisconnectsWhatItConnected() {
        var connected = Set<Int>()
        for step in GamepadScript.hotPlug.steps {
            switch step {
            case .connect(let controller, _): connected.insert(controller.id)
            case .disconnect(let id): #expect(connected.remove(id) != nil)
            default: break
            }
        }
        #expect(connected == [2])
    }
}
