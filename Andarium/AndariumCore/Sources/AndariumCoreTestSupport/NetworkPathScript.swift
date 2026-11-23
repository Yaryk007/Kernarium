// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore

/// A network path source that reports what a test sends, and counts its
/// starts and stops. It is simulated input: the unit tests and the
/// application's hosted tests use it, and the application does not link this
/// target.
@MainActor
public final class ScriptedNetworkPathSource: NetworkPathSource {
    public private(set) var starts = 0
    public private(set) var stops = 0
    /// What the watcher gave the last start; nil while stopped.
    public private(set) var deliver: (@MainActor (NetworkPathReport) -> Void)?

    public init() {}

    public func start(_ deliver: @escaping @MainActor (NetworkPathReport) -> Void) {
        starts += 1
        self.deliver = deliver
    }

    public func stop() {
        stops += 1
        deliver = nil
    }

    /// Reports a path to the watcher, as the system would; nothing while stopped.
    public func send(_ report: NetworkPathReport) { deliver?(report) }

    public var isStarted: Bool { deliver != nil }
}
