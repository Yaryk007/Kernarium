// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The experimental notch: Android's own display cutout emulation (Developer
/// options, Display cutout), which the system image carries as overlays of the
/// framework. Its tall cutout, a notch 96 dp wide and 48 dp deep with a 48 dp
/// status bar, covers the Dynamic Island when the guest draws under it, since
/// a guest dp is an iOS point. The shell may switch the overlays, so the
/// running guest takes the change without a restart.
public enum DisplayCutoutEmulation {
    /// The overlay Andarium switches.
    public static let overlay = "com.android.internal.display.cutout.emulation.tall"

    /// What to run in the guest's shell: on enables the overlay and turns
    /// every other cutout emulation off; off turns only this one off, so a
    /// cutout the person chose in Developer options stays.
    public static func command(on: Bool) -> String {
        on ? "cmd overlay enable-exclusive --user 0 --category \(overlay)"
            : "cmd overlay disable --user 0 \(overlay)"
    }

    /// Whether the command did what it was asked.
    public static func succeeded(_ result: ShellResult) -> Bool {
        result.exitCode == 0
    }
}
