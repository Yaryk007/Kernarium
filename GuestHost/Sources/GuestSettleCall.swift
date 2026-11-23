// SPDX-License-Identifier: GPL-2.0-or-later

import GuestHostCore

/// The runtime's calls that write the guest's storage back, with what each
/// reports about the guest (`kernarium_runtime_settle`). A call that does not
/// fill the report leaves it "not reported", which no application calls
/// settled. They run on the callers' queues, never on the main actor.
nonisolated extension GuestSettle {
    static func sync() -> GuestSettle {
        report { kernarium_runtime_loader_sync($0) }
    }

    static func quiesce() -> GuestSettle {
        report { kernarium_runtime_loader_quiesce($0) }
    }

    /// Like `quiesce()`, with the guest's RAM kept in the process.
    static func quiesceResident() -> GuestSettle {
        report { kernarium_runtime_loader_quiesce_resident($0) }
    }

    private static func report(
        _ call: (UnsafeMutablePointer<kernarium_runtime_settle>) -> Int32
    ) -> GuestSettle {
        var report = kernarium_runtime_settle(
            struct_size: UInt32(MemoryLayout<kernarium_runtime_settle>.size),
            reserved: 0,
            guest_storage: UInt32(KERNARIUM_RUNTIME_GUEST_STORAGE_NOT_REPORTED),
            guest_suspension: UInt32(KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_ASKED)
        )
        let unflushed = call(&report)
        return GuestSettle(
            unflushed: unflushed,
            rawStorage: report.guest_storage,
            rawSuspension: report.guest_suspension
        )
    }
}
