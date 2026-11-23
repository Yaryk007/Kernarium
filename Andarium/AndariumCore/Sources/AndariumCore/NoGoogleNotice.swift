// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The notice that the virtual machine's Android has no Google apps, on
/// purpose: a panel on the boot screen.
///
/// The panel belongs to a new installation's first Android boot, and nothing
/// is kept for it: a first start (a new virtual machine and one after Erase
/// alike) and the retry of one that ended before Android booted
/// (`InstanceFiles.firstBootPending`, the same condition as `SlowStart`'s).
/// A start that restarts the running guest, boots recovery or is any other
/// normal or update start never shows the panel.
public enum NoGoogleNotice {
    /// Whether the boot screen of a start of this kind carries the panel.
    /// `firstBootPending` is the guest's record when the start began.
    public static func showsOnBoot(_ kind: BootKind, firstBootPending: Bool) -> Bool {
        kind == .firstStart || (kind == .normal && firstBootPending)
    }
}
