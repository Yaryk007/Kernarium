// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The thermal wire against the golden frames that the guest's HAL tests
/// against too.
struct ThermalProtocolTests {
    @Test func theHostEncodesTheGoldenFrames() throws {
        let frames: [(String, AndroidThermalSeverity)] = [
            ("none", .none), ("light", .light), ("moderate", .moderate), ("severe", .severe),
            ("critical", .critical),
        ]
        for (name, severity) in frames {
            let golden = try ThermalFixture.bytes(name)
            #expect(Array(ThermalProtocol.record(severity)) == golden, Comment(rawValue: name))
        }
    }

    @Test func theHostSendsNoFrameTheHALRefuses() throws {
        let refused = try ThermalFixture.frames().filter { $0.name.hasPrefix("reject-") }
        #expect(refused.count == 4)
        for severity in AndroidThermalSeverity.allCases {
            let record = Array(ThermalProtocol.record(severity))
            #expect(record.count == ThermalProtocol.recordSize)
            #expect(!refused.contains { $0.bytes == record }, "\(severity) is a refused frame")
        }
        for state in HostThermalState.allCases {
            #expect(AndroidThermalSeverity(rawValue: state.severity.rawValue) != nil)
            #expect(state.severity.rawValue <= 4)
        }
    }

    @Test func theDecoderTakesTheGoldenFramesAndRefusesTheRejectedOnes() throws {
        #expect(try ThermalProtocol.severity(of: ThermalFixture.bytes("none")) == .none)
        #expect(try ThermalProtocol.severity(of: ThermalFixture.bytes("light")) == .light)
        #expect(try ThermalProtocol.severity(of: ThermalFixture.bytes("moderate")) == .moderate)
        #expect(try ThermalProtocol.severity(of: ThermalFixture.bytes("severe")) == .severe)
        #expect(try ThermalProtocol.severity(of: ThermalFixture.bytes("critical")) == .critical)
        #expect(throws: ThermalProtocol.Failure.severityAboveCritical(5)) {
            try ThermalProtocol.severity(of: ThermalFixture.bytes("reject-emergency"))
        }
        #expect(throws: ThermalProtocol.Failure.severityAboveCritical(6)) {
            try ThermalProtocol.severity(of: ThermalFixture.bytes("reject-shutdown"))
        }
        #expect(throws: ThermalProtocol.Failure.badMagic) {
            try ThermalProtocol.severity(of: ThermalFixture.bytes("reject-magic"))
        }
        #expect(throws: ThermalProtocol.Failure.badReserved) {
            try ThermalProtocol.severity(of: ThermalFixture.bytes("reject-reserved"))
        }
        #expect(throws: ThermalProtocol.Failure.badLength(7)) {
            try ThermalProtocol.severity(of: Array(ThermalFixture.bytes("none").dropLast()))
        }
    }

    @Test func everySeverityRoundTrips() throws {
        for severity in AndroidThermalSeverity.allCases {
            #expect(try ThermalProtocol.severity(of: ThermalProtocol.record(severity)) == severity)
        }
    }

    @Test func theThermalStreamIsOnPort5559() {
        #expect(ThermalProtocol.port == 5559)
        #expect(String(decoding: ThermalProtocol.magic, as: UTF8.self) == "ATH1")
    }
}
