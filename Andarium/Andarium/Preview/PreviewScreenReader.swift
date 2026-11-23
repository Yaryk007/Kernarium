// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The preview's mock guest screen reader: the system carries it, it is off
/// until a switch turns it on, and its setting outlives a run as a guest's
/// does. `fault` makes a read or a change not work, for the states a person
/// sees of one that does not. The application has no such thing
/// (`AppModel.screenReaderStandIn` is nil there).
@MainActor
final class PreviewScreenReader: ScreenReaderStandIn {
    enum Fault { case unreadable, refusesChange, slow, transfer, pending }

    var state = ScreenReaderState(installedService: ScreenReader.services[0], enabledServices: [])
    var fault: Fault?

    var holdsChannel: Bool { fault == .transfer }

    func read() async -> ScreenReaderState? {
        if fault == .pending { try? await Task.sleep(for: .seconds(600)) }
        return fault == .unreadable ? nil : state
    }

    func change(_ on: Bool, from state: ScreenReaderState) async throws -> ScreenReaderState {
        switch fault {
        case .slow: try await Task.sleep(for: .seconds(600))
        case .refusesChange: throw AdbFailure(.io(EIO), activity: .screenReader)
        case .unreadable, .transfer, .pending, nil: break
        }
        let after = ScreenReader.state(afterTurningOn: on, state)
        self.state = after
        return after
    }
}
