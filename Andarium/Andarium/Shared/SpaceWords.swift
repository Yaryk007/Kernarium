// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The words of a start the device has too little space for, in the person's
/// language. `StartSpace` (GuestHostCore) says them in English for Kernarium,
/// which is not localized; the figures come from it.
enum StartSpaceWords {
    static let title = String(localized: "Not enough free space")

    static func message(neededBytes: UInt64, freeBytes: UInt64, device: String) -> String {
        String(
            localized: "The virtual machine needs \(Formats.size(bytes: neededBytes)) of free space to start, for the data it writes. This \(device) has \(Formats.size(bytes: freeBytes)) free.")
    }
}

/// What the screen over a guest paused for want of space says, as
/// `SpacePause` says it in English: the figures and the stage are its, the
/// sentences are one string each so a language can order them as it needs. A
/// test asks for a language by `locale`.
enum SpacePauseWords {
    static func title(_ pause: SpacePause, locale: Locale = .current) -> String {
        let guest = pause.guest
        switch pause.stage {
        case .pausing: return String(localized: LocalizedStringResource("Pausing \(guest)", locale: locale))
        case .paused: return String(localized: LocalizedStringResource("\(guest) is paused", locale: locale))
        case .notPaused: return String(localized: LocalizedStringResource("\(guest) is still running", locale: locale))
        }
    }

    static func detail(_ pause: SpacePause, locale: Locale = .current) -> String {
        // Shut down that only stops the run is Kernarium's.
        guard pause.ending == .powerOff else { return pause.detail }
        let guest = pause.guest
        let needed = Formats.size(bytes: RunSpace.resumeAtBytes)
        let free = Formats.size(bytes: pause.freeBytes)
        if pause.working == .shuttingDown {
            return String(localized: LocalizedStringResource("\(guest) is saving what it has and shutting down.", locale: locale))
        }
        switch pause.stage {
        case .pausing:
            return String(localized: LocalizedStringResource("This device has \(free) of free space left. \(guest) is saving what it has to storage.", locale: locale))
        case .paused:
            if pause.stillShort {
                return String(localized: LocalizedStringResource("Only \(free) is free. Free up more space, then resume. \(guest) needs \(needed) free to continue.", locale: locale))
            }
            return String(localized: LocalizedStringResource("This device has \(free) of free space left, too little for \(guest) to keep writing. Free up space, then resume. \(guest) needs \(needed) free to continue.", locale: locale))
        case .notPaused:
            if pause.stillShort {
                return String(localized: LocalizedStringResource("Only \(free) is free, and \(guest) is still running. Shut it down before the space runs out, or free up more space and check again. \(guest) needs \(needed) free to continue.", locale: locale))
            }
            return String(localized: LocalizedStringResource("This device has \(free) of free space left and \(guest) could not be paused. Shut it down before the space runs out, or free up space and check again.", locale: locale))
        }
    }

    static let resuming = String(localized: "Resuming\u{2026}")
    static let shuttingDown = String(localized: "Shutting down\u{2026}")
    static let pausing = String(localized: "Pausing\u{2026}")
}
