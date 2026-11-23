// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The session record's network slot on a real guest. The guest reserves its
/// management address with one DHCPREQUEST when it boots
/// (`kernarium_net_setup`), and the runtime, which rewrites the record every
/// two seconds from the counts of the user network's provider, must show that
/// exchange: the test fails when the record has no network slot or no lease in
/// it, which is what a runtime that wires no reader writes. What else the
/// guest does on the network depends on the image and the host (a Wi-Fi that
/// joins takes a lease of its own, a DISCOVER; Android's checks make DNS
/// queries and connections), so the test prints the slot before and after a
/// connection from the guest's shell and asserts nothing about them. Runs alone
/// with ANDARIUM_NETWORK_RECORD_TESTS=1 and
/// ANDARIUM_GUEST_INSTANCE=network-record, on a Mac, an iPad or the Simulator,
/// with an isolated guest. The test installs nothing and deletes nothing:
/// erase the guest with an ANDARIUM_ERASE=1 launch (README).
///
/// The test prints the slot, which holds numbers only, on NETWORK_RECORD lines.
@MainActor
struct NetworkRecordGuestTests {
    private static let name = "network-record"

    /// The slot as the record on disk has it now.
    private func slot() -> (record: NetworkRecord?, lines: String) {
        let url = AppEntry.guestDirectory(instance: Self.name).appendingPathComponent(BugReportAttachments.sessionRecord)
        let text = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
        let lines = text.split(separator: "\n").filter { $0.hasPrefix("network ") || $0.hasPrefix("net-") }
        return (NetworkRecord(text: text), lines.joined(separator: " | "))
    }

    /// Waits, up to a bound, for the slot to satisfy `condition`.
    private func eventually(
        seconds: Int, _ condition: (NetworkRecord) -> Bool
    ) async throws -> NetworkRecord? {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while ContinuousClock.now < deadline {
            if let record = slot().record, condition(record) { return record }
            try await Task.sleep(for: .seconds(2))
        }
        return slot().record.flatMap { condition($0) ? $0 : nil }
    }

    @Test(.timeLimit(.minutes(25)))
    func aBootedGuestsLeaseIsInTheSessionRecord() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_NETWORK_RECORD_TESTS"] == "1" else { return }
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == Self.name)
        try await withAndroidGuest { model, operations in
            // The reservation was made early in the boot.
            let leased = try await eventually(seconds: 60) { ($0.dhcp?.acks ?? 0) >= 1 }
            print("NETWORK_RECORD: after boot: \(slot().lines)")
            let lease = try #require(leased?.dhcp, "the record shows no lease: \(slot().lines)")
            #expect(lease.requests >= 1 && lease.acks >= 1)
            let found = try #require(slot().record)
            #expect(found.dns != nil && found.tcp != nil)
            // The bug report reads the same numbers.
            #expect(found.report().contains("DHCP: the guest sent"))

            // What the guest's network is and does, for the reader of the log.
            for command in [
                "ip -4 addr show", "ip link show | grep -c wlan0", "cmd wifi status | head -n 4",
                "toybox nc -s 10.0.2.15 -w 3 10.0.2.2 9 </dev/null 2>&1; echo nc=$?",
            ] {
                let result = try? await operations.shell(command, limit: .seconds(30))
                print("NETWORK_RECORD: \(command): \(result?.text.replacingOccurrences(of: "\n", with: " / ") ?? "-")")
            }
            try await Task.sleep(for: .seconds(6))
            print("NETWORK_RECORD: later: \(slot().lines)")
            let alive = try await operations.shell("getprop sys.boot_completed", limit: .seconds(30))
            #expect(alive.text.trimmingCharacters(in: .whitespacesAndNewlines) == "1")
        }
    }
}
