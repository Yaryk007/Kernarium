// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Why a start was ended for the size of the guest's memory. The runtime
/// admits the sizes from its minimum through this host's ceiling in its steps
/// (`kernarium_runtime_loader_guest_memory_admitted`) and the settings offer no
/// others, so this is said only where something else stated a size: a launch's
/// environment, or a stored size that nothing has brought down yet.
public enum MemoryRefusal {
    /// The reason for a refused size, in the figures this host has: `ceilingMiB`
    /// is the most it admits (`kernarium_runtime_loader_guest_memory_ceiling_mib`),
    /// zero where the process has no room for even the smallest virtual machine.
    /// It reads as the end of "it could not start (...)".
    public static func reason(memoryMiB: Int, ceilingMiB: Int) -> String {
        guard ceilingMiB > 0 else {
            return "this device has no room for the virtual machine\u{2019}s memory"
        }
        let size = Formats.size(mib: memoryMiB)
        guard memoryMiB > ceilingMiB else {
            return "\(size) of memory is not a size the virtual machine can start with"
        }
        return "\(size) of memory is more than this device can give the virtual machine "
            + "(at most \(Formats.size(mib: ceilingMiB)))"
    }
}
