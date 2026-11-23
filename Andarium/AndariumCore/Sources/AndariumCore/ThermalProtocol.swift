// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The private thermal protocol on vsock 5559, from the host to the guest's
/// thermal HAL (which listens and takes the connection of host CID 2, a later
/// one replacing an earlier one). The host only writes. A record is 8 bytes:
/// the magic `ATH1`, one byte with Android's severity (0 none, 1 light, 2
/// moderate, 3 severe, 4 critical) and three reserved bytes, all 0. The host
/// sends one record when it connects and one at each change of the severity.
///
/// The HAL closes the connection on a record that breaks the format (a bad
/// magic, a severity above 4, a reserved byte that is not 0) and keeps the
/// severity it had. The golden frames the guest's test shares are in
/// `tests/unit/andarium/fixtures`.
public enum ThermalProtocol {
    public static let port: UInt32 = 5559
    public static let magic: [UInt8] = [0x41, 0x54, 0x48, 0x31]
    public static let recordSize = 8

    /// Why bytes are not a record the HAL accepts.
    public enum Failure: Error, Equatable, Sendable {
        case badLength(Int)
        case badMagic
        /// Android's emergency and shutdown (5 and 6) and anything above are
        /// not for the host to send.
        case severityAboveCritical(UInt8)
        case badReserved
    }

    /// The record that tells the guest `severity`.
    public static func record(_ severity: AndroidThermalSeverity) -> Data {
        Data(magic + [severity.rawValue, 0, 0, 0])
    }

    /// The severity a record states, or why the HAL refuses it.
    public static func severity(of record: some Collection<UInt8>) throws -> AndroidThermalSeverity {
        let bytes = Array(record)
        guard bytes.count == recordSize else { throw Failure.badLength(bytes.count) }
        guard Array(bytes[0..<4]) == magic else { throw Failure.badMagic }
        guard let severity = AndroidThermalSeverity(rawValue: bytes[4]) else {
            throw Failure.severityAboveCritical(bytes[4])
        }
        guard bytes[5] == 0, bytes[6] == 0, bytes[7] == 0 else { throw Failure.badReserved }
        return severity
    }
}
