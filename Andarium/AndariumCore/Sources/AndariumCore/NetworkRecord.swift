// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// What the guest's user network counted during a session, and whether the
/// application told the person that iOS denies it the network, as the runtime
/// wrote them into the session record's network slot
/// (darwin/runtime/session_record.hpp).
///
/// Every number is a count; the record holds no address, port, host name or
/// name that was queried. A line this reader does not know is skipped and a
/// line with more fields than it knows is read by field name, so a later
/// runtime can add fields.
public struct NetworkRecord: Equatable, Sendable {
    /// Queries to the system's resolver, by how they ended.
    public struct DNS: Equatable, Sendable {
        /// A missing name or a name without a record of the type is an answer,
        /// unless the host had no network path: then it counts as failed.
        public var answered: UInt64
        /// The system refused the query: the mark of a network iOS denies.
        public var policyDenied: UInt64
        public var timedOut: UInt64
        public var failed: UInt64

        public var total: UInt64 { answered &+ policyDenied &+ timedOut &+ failed }
    }

    /// The guest's DHCP messages and the lease acknowledgements that answered.
    public struct DHCP: Equatable, Sendable {
        public var discovers: UInt64
        public var requests: UInt64
        public var acks: UInt64
    }

    /// The guest's attempts to open a TCP connection to an outside address,
    /// by what the guest was told. DNS over TCP to the name server or the
    /// gateway is answered by the user network itself and is not one.
    public struct TCP: Equatable, Sendable {
        public var attempts: UInt64
        /// A SYN-ACK.
        public var established: UInt64
        /// An ICMP net or host unreachable: the host's connect() failed at
        /// once with anything but a refusal (no route, for one).
        public var unreachable: UInt64
        /// A reset: the connection was refused, or the host's connect timed
        /// out after 75 seconds, or it failed after it had started.
        public var reset: UInt64

        /// Attempts with no answer when the slot was written: still under
        /// way, given up by the guest, or pushed out of the table of waiting
        /// attempts when it was full.
        public var unanswered: UInt64 {
            let answered = established &+ unreachable &+ reset
            return attempts > answered ? attempts - answered : 0
        }
    }

    /// DNS, DHCP and TCP counts; nil in a slot written while the runtime had
    /// no network to count (before the user network exists).
    public var dns: DNS?
    public var dhcp: DHCP?
    public var tcp: TCP?
    /// How many denials of the network began during the session, each one
    /// raising the application's notice (which the person may have dismissed
    /// or not seen); one already in force when the session began counts once.
    public var noticeDenials: Int
    /// The denial the notice was up for when the slot was written, in the
    /// runtime's word: none, cellular or wifi.
    public var noticeNow: String

    /// Reads the network slot out of a session record's text (or the slot
    /// alone); nil when the text has none, as the records of the earlier
    /// layouts, or one of a version this reader does not know.
    public init?(text: String) {
        var header = false
        var dns: DNS?
        var dhcp: DHCP?
        var tcp: TCP?
        var denials: Int?
        var now: String?
        // The kernel's quoted output is not the record's own lines.
        let own = text.components(separatedBy: "\nkernel-output\n")[0]
        for line in own.split(separator: "\n", omittingEmptySubsequences: true) {
            let words = line.split(separator: " ", omittingEmptySubsequences: true)
            guard let first = words.first else { continue }
            switch first {
            case "network":
                header = words.count >= 2 && words[1] == "1"
            case "net-dns":
                let values = Self.values(words)
                if let answered = values["answered"], let denied = values["policy-denied"],
                    let timedOut = values["timed-out"], let failed = values["failed"]
                {
                    dns = DNS(answered: answered, policyDenied: denied, timedOut: timedOut, failed: failed)
                }
            case "net-dhcp":
                let values = Self.values(words)
                if let discovers = values["discovers"], let requests = values["requests"],
                    let acks = values["acks"]
                {
                    dhcp = DHCP(discovers: discovers, requests: requests, acks: acks)
                }
            case "net-tcp":
                let values = Self.values(words)
                if let attempts = values["attempts"], let established = values["established"],
                    let unreachable = values["unreachable"], let reset = values["reset"]
                {
                    tcp = TCP(
                        attempts: attempts, established: established, unreachable: unreachable,
                        reset: reset)
                }
            case "net-notice":
                // net-notice denials N now WORD
                if let at = words.firstIndex(of: "denials"), at + 1 < words.count, let count = Int(words[at + 1]) {
                    denials = count
                }
                if let at = words.firstIndex(of: "now"), at + 1 < words.count { now = String(words[at + 1]) }
            default:
                break
            }
        }
        guard header, let denials, let now else { return nil }
        self.dns = dns
        self.dhcp = dhcp
        self.tcp = tcp
        noticeDenials = denials
        noticeNow = now
    }

    /// The `name value` pairs after the line's first word.
    private static func values(_ words: [Substring]) -> [String: UInt64] {
        var values: [String: UInt64] = [:]
        var index = 1
        while index + 1 < words.count {
            if let value = UInt64(words[index + 1]) { values[String(words[index])] = value }
            index += 2
        }
        return values
    }

    /// The record in words, for the bug report.
    public func report() -> String {
        var lines: [String] = []
        if let dns {
            lines.append(
                "DNS: \(dns.total) queries to the system's resolver: \(dns.answered) answered (a missing name is an "
                    + "answer, unless the host had no network path: then it failed), "
                    + "\(dns.policyDenied) refused by the system, \(dns.timedOut) timed out, "
                    + "\(dns.failed) failed otherwise.")
        } else {
            lines.append("DNS: not counted.")
        }
        if let dhcp {
            lines.append(
                "DHCP: the guest sent \(dhcp.discovers) discover and \(dhcp.requests) request messages; "
                    + "\(dhcp.acks) leases were acknowledged.")
        } else {
            lines.append("DHCP: not counted.")
        }
        if let tcp {
            lines.append(
                "TCP: \(tcp.attempts) outbound connection attempts: \(tcp.established) connected, "
                    + "\(tcp.unreachable) unreachable (connect() failed at once, but not with a refusal), "
                    + "\(tcp.reset) reset (refused, timed out on the host after 75 seconds, or failed after it "
                    + "started), \(tcp.unanswered) with no answer (under way, given up by the guest, or pushed "
                    + "out of the table of waiting attempts).")
        } else {
            lines.append("TCP: not counted.")
        }
        let now = noticeNow == "none" ? "no denial in force" : "the \(noticeNow) denial in force"
        lines.append(
            noticeDenials == 0
                ? "Denials of the network by iOS: 0; \(now)."
                : "Denials of the network by iOS: \(noticeDenials) began during the session, each raising the "
                    + "notice; \(now) at the last write.")
        return lines.joined(separator: "\n")
    }
}
