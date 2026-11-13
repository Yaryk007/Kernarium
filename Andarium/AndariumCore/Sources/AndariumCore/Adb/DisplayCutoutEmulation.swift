// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The experimental notch: Android is told its display has a cutout at the
/// top, so apps keep their content clear of the Dynamic Island, and nothing is
/// drawn there. Android's own cutout emulation (Developer options, Display
/// cutout), which the system image carries as framework overlays, supplies
/// the cutout: its tall one, a notch 96 dp wide and 48 dp deep with a 48 dp
/// status bar, covers the island, since a guest dp is an iOS point. The
/// emulation also paints the cutout black, so the system image carries a
/// second overlay above it that turns the fill off. The shell may switch both,
/// so the running guest takes the change without a restart.
public enum DisplayCutoutEmulation {
    /// The emulation overlay that supplies the cutout.
    public static let overlay = "com.android.internal.display.cutout.emulation.tall"
    /// The system image's overlay that keeps the cutout unpainted.
    public static let clearOverlay = "android.kernarium.overlay.cutoutclear"

    /// What to run in the guest's shell. On enables the tall cutout and turns
    /// every other cutout emulation off, but only after the image's clear
    /// overlay is enabled. A failure disables both, so an older image can
    /// never be left with a black cutout. Off turns both off, so a cutout the
    /// person chose in Developer options stays.
    public static func command(on: Bool) -> String {
        if on {
            let enable = [
                "cmd overlay enable --user 0 \(clearOverlay)",
                "cmd overlay set-priority --user 0 \(clearOverlay) highest",
                "cmd overlay enable-exclusive --user 0 --category \(overlay)",
                "cmd overlay set-priority --user 0 \(clearOverlay) highest",
            ].joined(separator: " && ")
            return "{ \(enable); } || { " + disableCommand + "; exit 1; }"
        }
        return disableCommand
    }

    private static var disableCommand: String {
        "cmd overlay disable --user 0 \(overlay) >/dev/null 2>&1; "
            + "cmd overlay disable --user 0 \(clearOverlay) >/dev/null 2>&1; true"
    }

    /// Whether the command did what it was asked.
    public static func succeeded(_ result: ShellResult) -> Bool {
        result.exitCode == 0
    }
}
