// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The runtime's network slot as Andarium reads it. The slot text is the
/// golden fixture the runtime's own test checks its writer against
/// (`tests/unit/andarium/fixtures/network-slot-golden.txt`), so the two sides
/// agree on the format from one file.
struct NetworkRecordTests {
    static func golden() throws -> String {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/network-slot-golden.txt")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// A whole layout 5 record: the memory slot, a stall slot never written,
    /// the network slot padded to 512 bytes, the state.
    static func record(
        network slot: String, stalls: String = "",
        state: String = "state running\nend running\nstatus 0\npid 77\n"
    ) -> String {
        var memory = "uml-session 5\nmemory at 1794114290 footprint 2147483648 peak 2415919104 available 900000000\n"
        memory += String(repeating: " ", count: 4095 - memory.utf8.count) + "\n"
        return memory + padded(stalls, to: 4096) + padded(slot, to: 512) + state
    }

    /// `text` and blanks up to `size` bytes, the last of them a newline.
    static func padded(_ text: String, to size: Int) -> String {
        text + String(repeating: " ", count: size - 1 - text.utf8.count) + "\n"
    }

    @Test func theGoldenSlotParses() throws {
        let record = try #require(NetworkRecord(text: try Self.golden()))
        #expect(record.dns == .init(answered: 5, policyDenied: 3, timedOut: 2, failed: 1))
        #expect(record.dns?.total == 11)
        #expect(record.dhcp == .init(discovers: 1, requests: 2, acks: 1))
        #expect(record.tcp == .init(attempts: 12, established: 4, unreachable: 2, reset: 1))
        #expect(record.tcp?.unanswered == 5)
        #expect(record.noticeDenials == 2 && record.noticeNow == "wifi")
    }

    @Test func aLayout5RecordCarriesTheSlot() throws {
        let session = try #require(SessionRecord(text: Self.record(network: try Self.golden())))
        #expect(session.processID == 77 && session.running)
        #expect(session.network?.tcp?.attempts == 12)
        // The stall slot was never written, and the network slot does not
        // pass for it.
        #expect(session.stalls == nil)
        // With the stall slot written, both are read.
        let both = try #require(
            SessionRecord(text: Self.record(network: try Self.golden(), stalls: try StallRecordTests.golden())))
        #expect(both.stalls?.total == 37 && both.network?.dns?.policyDenied == 3)
    }

    @Test func theEarlierLayoutsHaveNoNetworkSlot() throws {
        // A layout 4 record, even with a network-looking line, has none: the
        // runtime that wrote it did not know the slot.
        let four = StallRecordTests.record(
            stalls: try StallRecordTests.golden() + String(repeating: " ", count: 100) + "\n",
            state: "state running\nend running\nstatus 0\npid 77\n")
        let session = try #require(SessionRecord(text: four + "network 1\nnet-notice denials 1 now wifi\n"))
        #expect(session.network == nil)
        for format in 1...3 {
            let text = "uml-session \(format)\nmemory at 1 footprint 2 peak 3\nstate ended\nend powered-off\nnetwork 1\n"
                + "net-notice denials 1 now wifi\n"
            #expect(SessionRecord(text: text)?.network == nil, "layout \(format)")
        }
    }

    @Test func aGuestWithoutANetworkHasTheNoticeAndNoCounts() throws {
        let record = try #require(NetworkRecord(text: "network 1\nnet-notice denials 1 now cellular\n"))
        #expect(record.dns == nil && record.dhcp == nil && record.tcp == nil)
        #expect(record.noticeDenials == 1 && record.noticeNow == "cellular")
        let words = record.report()
        #expect(words.contains("DNS: not counted.") && words.contains("DHCP: not counted.")
            && words.contains("TCP: not counted."))
        #expect(words.contains("Denials of the network by iOS: 1 began during the session, each raising the notice; "
            + "the cellular denial in force at the last write."))
    }

    @Test func aSlotOfAVersionOrShapeThisReaderDoesNotKnowIsNoRecord() {
        #expect(NetworkRecord(text: "") == nil)
        #expect(NetworkRecord(text: "network 2\nnet-notice denials 1 now wifi\n") == nil)
        #expect(NetworkRecord(text: "net-notice denials 1 now wifi\n") == nil)
        // The notice line is the slot's one constant line.
        #expect(NetworkRecord(text: "network 1\nnet-dns answered 1 policy-denied 0 timed-out 0 failed 0\n") == nil)
        #expect(NetworkRecord(text: "network 1\nnet-notice denials x now wifi\n") == nil)
    }

    @Test func aLineWithABadNumberIsDroppedAndTheRestIsKept() throws {
        let text = """
            network 1
            net-dns answered -1 policy-denied 0 timed-out 0 failed 0
            net-dhcp discovers 1 requests 1 acks 99999999999999999999999
            net-tcp attempts 3 established 1 unreachable 1 reset 0
            net-notice denials 0 now none

            """
        let record = try #require(NetworkRecord(text: text))
        #expect(record.dns == nil && record.dhcp == nil)
        #expect(record.tcp == .init(attempts: 3, established: 1, unreachable: 1, reset: 0))
    }

    @Test func aLaterRuntimeMayAddFieldsAndLines() throws {
        let text = """
            network 1
            net-dns answered 5 policy-denied 3 timed-out 2 failed 1 cancelled 7
            net-quic attempts 4
            net-dhcp requests 2 discovers 1 acks 1
            net-tcp attempts 12 established 4 unreachable 2 reset 1 blocked 9
            net-notice denials 2 now wifi since 1794114290
            """
        let record = try #require(NetworkRecord(text: text))
        #expect(record.dns == .init(answered: 5, policyDenied: 3, timedOut: 2, failed: 1))
        #expect(record.dhcp == .init(discovers: 1, requests: 2, acks: 1))
        #expect(record.tcp?.attempts == 12 && record.noticeNow == "wifi")
    }

    @Test func unansweredNeverGoesNegative() {
        // A reading taken between the two counters of a reply can show more
        // answers than attempts.
        #expect(NetworkRecord.TCP(attempts: 1, established: 2, unreachable: 0, reset: 1).unanswered == 0)
        #expect(NetworkRecord.TCP(attempts: 0, established: 0, unreachable: 0, reset: 0).unanswered == 0)
    }

    @Test func theReportIsWordsWithNumbersOnly() throws {
        let record = try #require(NetworkRecord(text: try Self.golden()))
        let words = record.report()
        #expect(words.contains("DNS: 11 queries to the system's resolver: 5 answered"))
        #expect(words.contains("3 refused by the system, 2 timed out, 1 failed otherwise."))
        #expect(words.contains("DHCP: the guest sent 1 discover and 2 request messages; 1 leases were acknowledged."))
        #expect(words.contains("TCP: 12 outbound connection attempts: 4 connected, 2 unreachable (connect() failed at once"))
        #expect(words.contains("1 reset (refused, timed out on the host after 75 seconds, or failed after it started), "
            + "5 with no answer (under way, given up by the guest, or pushed out of the table of waiting attempts)."))
        #expect(words.contains("Denials of the network by iOS: 2 began during the session, each raising the notice; "
            + "the wifi denial in force at the last write."))
        let none = try #require(NetworkRecord(text: "network 1\nnet-notice denials 0 now none\n"))
        #expect(none.report().contains("Denials of the network by iOS: 0; no denial in force."))
    }

    @Test func aHostileRecordOfValidLinesStaysSmall() throws {
        var text = "network 1\n"
        while text.utf8.count < SessionRecord.maximumBytes - 200 {
            text += "net-dns answered 18446744073709551615 policy-denied 18446744073709551615 "
                + "timed-out 18446744073709551615 failed 18446744073709551615\n"
        }
        text += "net-notice denials 2147483647 now wifi\n"
        let record = try #require(NetworkRecord(text: text))
        // The total wraps rather than traps.
        #expect(record.dns?.total != nil)
        #expect(record.report().utf8.count < 1024)
    }

    // MARK: the bug report

    private func scratch() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("network-record-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    @Test func theNetworkIsAnEntryOfItsOwnWhenARecordHasIt() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try Self.record(network: try Self.golden(), state: "state ended\nend powered-off\nstatus 0\npid 31\n")
            .write(
                to: directory.appendingPathComponent(BugReportAttachments.previousSessionRecord),
                atomically: true, encoding: .utf8)
        try Self.record(network: "network 1\nnet-notice denials 0 now none\n")
            .write(
                to: directory.appendingPathComponent(BugReportAttachments.sessionRecord),
                atomically: true, encoding: .utf8)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.map(\.name) == [
            "andarium/session-previous.txt", "andarium/session-current.txt", "andarium/network.txt",
        ])
        let words = String(decoding: entries[2].data, as: UTF8.self)
        #expect(words.contains("== The previous session (process 31) =="))
        #expect(words.contains("3 refused by the system"))
        #expect(words.contains("== The current session (process 77) =="))
        #expect(words.contains("Denials of the network by iOS: 0; no denial in force."))
        // The raw record carries the same numbers.
        #expect(String(decoding: entries[0].data, as: UTF8.self).contains("net-dns answered 5 policy-denied 3"))
    }

    @Test func theReportsInWordsFollowTheLatestRunWhileTheNextHasWrittenNone() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try Self.record(network: try Self.golden(), state: "state ended\nend powered-off\nstatus 0\npid 31\n")
            .write(
                to: directory.appendingPathComponent(BugReportAttachments.previousSessionRecord),
                atomically: true, encoding: .utf8)
        // The run that ended and was retired, the next not begun: no current record.
        try Self.record(network: "network 1\nnet-notice denials 0 now none\n")
            .write(
                to: directory.appendingPathComponent(BugReportAttachments.earlierRunRecord),
                atomically: true, encoding: .utf8)
        let entries = BugReportAttachments.entries(from: directory, containers: [], includingEarlierRun: true)
        #expect(entries.map(\.name) == [
            "andarium/session-previous.txt", "andarium/session-earlier-run.txt", "andarium/network.txt",
        ])
        let words = String(decoding: entries[2].data, as: UTF8.self)
        #expect(words.contains("== The previous session (process 31) =="))
        #expect(words.contains("== The current session (process 77) =="))
    }

    @Test func afterALaunchTheReportsInWordsDescribeTheProcessBeforeAndNoRunOfThisOne() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        // Two processes back, kept at the launch before.
        try Self.record(network: try Self.golden(), state: "state ended\nend powered-off\nstatus 0\npid 31\n")
            .write(
                to: directory.appendingPathComponent(BugReportAttachments.previousSessionRecord),
                atomically: true, encoding: .utf8)
        // The process before ended with a failed start whose pause was saved: its record was moved aside.
        try Self.record(
            network: "network 1\nnet-notice denials 0 now none\n",
            state: "state ended\nend guest-panic\nstatus 0\npid 50\n")
            .write(
                to: directory.appendingPathComponent(BugReportAttachments.earlierRunRecord),
                atomically: true, encoding: .utf8)
        PreviousSession.settleEarlierRun(in: directory)
        let entries = BugReportAttachments.entries(from: directory, containers: [], includingEarlierRun: true)
        #expect(entries.map(\.name) == ["andarium/session-previous.txt", "andarium/network.txt"])
        let words = String(decoding: entries[1].data, as: UTF8.self)
        #expect(words.contains("== The previous session (process 50) =="))
        #expect(!words.contains("process 31"))
        #expect(words.contains("== The current session ==") && words.contains("There is no record."))
    }

    @Test func aRuntimeBeforeLayout5HasNoNetworkToReport() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try "uml-session 3\nstate running\nend running\n".write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord),
            atomically: true, encoding: .utf8)
        #expect(BugReportAttachments.entries(from: directory, containers: []).map(\.name)
            == ["andarium/session-current.txt"])
        let session = SessionRecord(text: "uml-session 4\nstate running\nend running\n")
        #expect(BugReportAttachments.network(previous: nil, current: session) == nil)
        let five = SessionRecord(text: "uml-session 5\nstate running\nend running\nstatus 0\npid 3\n")
        let words = BugReportAttachments.network(previous: nil, current: five)
        #expect(words == nil, "a layout 5 record whose slot is blank has no network slot to report")
    }

    @Test func aLongerRecordThanTheRuntimeWritesIsStillNamedAtTheLimit() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try (Self.record(network: try Self.golden()) + String(repeating: "x", count: 4 << 20)).write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord),
            atomically: true, encoding: .utf8)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        let note = String(decoding: try #require(entries.first).data, as: UTF8.self)
        #expect(note.contains("at most 19200"))
        #expect(entries.map(\.name) == ["andarium/session-current.txt"])
    }
}
