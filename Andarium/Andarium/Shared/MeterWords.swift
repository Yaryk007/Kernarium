// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// How a part of a whole is written. English says "of" in all of them; the
/// languages differ, so each readout has its own key and its own comment.
enum MeterWords {
    /// A meter or a progress readout: 1.4 GiB of 2.5 GiB, written
    /// "1.4 GiB / 2.5 GiB" in Chinese and Japanese.
    static func of(_ part: String, _ whole: String) -> String {
        String(
            localized: "\(part) of \(whole)",
            comment: "A meter or progress readout, the part first: the memory or graphics memory in use of its limit, the bytes moved of a file's size. Written 1.4 GiB / 2.5 GiB. The storage rows use another key.")
    }

    /// The storage rows of Home, the first start and the resize sheet, in the
    /// form iOS uses for storage in each language.
    static func storage(_ used: String, of capacity: String) -> String {
        String(
            localized: "storage.of", defaultValue: "\(used) of \(capacity)",
            comment: "The storage row of Home and of the first start: the space the virtual machine's data takes of its capacity, as in 5.4 GiB of 16 GiB. Follows iOS's own form for storage in use.")
    }

    /// What VoiceOver says for a part of a whole.
    static func spoken(_ part: String, of whole: String) -> String {
        String(
            localized: "spoken.of", defaultValue: "\(part) of \(whole)",
            comment: "What VoiceOver says for a part of a whole, as memory 1.4 GiB of 2.5 GiB; the meters on screen use another key.")
    }
}
