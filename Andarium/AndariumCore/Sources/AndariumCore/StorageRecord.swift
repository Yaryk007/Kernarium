// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// How often the guest's storage was settled during a session and how often
/// the guest did not confirm it, as the runtime wrote it into the session
/// record's storage slot (darwin/runtime/session_record.hpp).
///
/// A count and the last settle's call, outcome and time: words and numbers
/// only, never a path. The slot is blank until the first settle, and a blank
/// slot is no claim: the session never settled, or it did and nothing says so.
public struct StorageRecord: Equatable, Sendable {
    /// The last settle: the call (`sync`, `quiesce` or `quiesce-resident`),
    /// what the guest confirmed (`confirmed`, `no-guest`, `unconfirmed`,
    /// `refused` or `unreachable`), whether the call put it to sleep
    /// (`not-asked`, `suspended` or `not-suspended`) and when. The words are
    /// the runtime's own, kept as they were written.
    public struct Last: Equatable, Sendable {
        public var call: String
        public var storage: String
        public var suspension: String
        public var at: Date

        /// Whether the guest's storage counted as settled: the guest
        /// confirmed it, or none ran.
        public var settled: Bool { storage == "confirmed" || storage == "no-guest" }
    }

    /// The settles of the session, and those that left the storage unsettled.
    public var settles: Int
    public var unsettled: Int
    public var last: Last?

    /// Reads the storage slot out of a session record's text (or the slot
    /// alone); nil when the slot is blank, as in a session that never settled
    /// the guest and in the records of the earlier layouts, or of a version
    /// this reader does not know.
    public init?(text: String) {
        var header = false
        var settles: Int?
        var unsettled: Int?
        var last: Last?
        // The kernel's quoted output is not the record's own lines.
        let own = text.components(separatedBy: "\nkernel-output\n")[0]
        for line in own.split(separator: "\n", omittingEmptySubsequences: true) {
            let words = line.split(separator: " ", omittingEmptySubsequences: true)
            guard let first = words.first else { continue }
            switch first {
            case "storage":
                header = words.count >= 2 && words[1] == "1"
            case "settles":
                // settles N unsettled M
                if words.count >= 2 { settles = Int(words[1]) }
                if let at = words.firstIndex(of: "unsettled"), at + 1 < words.count {
                    unsettled = Int(words[at + 1])
                }
            case "last":
                // last CALL STORAGE SUSPENSION at T
                if words.count >= 6, words[4] == "at", let time = TimeInterval(words[5]) {
                    last = Last(
                        call: String(words[1]), storage: String(words[2]), suspension: String(words[3]),
                        at: Date(timeIntervalSince1970: time))
                }
            default:
                break
            }
        }
        guard header, let settles, let unsettled else { return nil }
        self.settles = settles
        self.unsettled = unsettled
        self.last = last
    }
}
