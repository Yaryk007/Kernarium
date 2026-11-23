// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The connection to the guest's text message service: vsock 5560, the one
/// port this connector names. The service takes a request only from the host,
/// so no other connector of the application reaches it
/// (`GuestSmsChannel`, android/lineage-24/overlay/sms).
nonisolated struct TextMessageConnector: GuestSmsConnector {
    static let port: UInt32 = GuestSmsChannel.port

    /// `connect` takes the port and the timeout in milliseconds, as the
    /// runtime's does; a test names a recording one.
    var connect: @Sendable (UInt32, UInt32) -> Int32 = { port, timeout in
        kernarium_runtime_loader_connect_vsock(port, timeout)
    }

    init() {}

    init(connect: @escaping @Sendable (UInt32, UInt32) -> Int32) {
        self.connect = connect
    }

    func open(timeoutMilliseconds: UInt32) throws -> Int32 {
        let result = connect(Self.port, timeoutMilliseconds)
        guard result >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -result) ?? .EIO) }
        return result
    }
}
