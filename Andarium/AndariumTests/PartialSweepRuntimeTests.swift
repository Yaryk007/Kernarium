// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// How the runtime keeps the record of the partial files its guest may hold
/// (`PartialSweepLedger`) and the mark of the run (`GuestRunMark`): the
/// mark is renewed when the guest restarts inside the run, and a sweep reads
/// the one current when it runs. The guest's own end of it is
/// `GuestFilesGuestTests`.
@Suite(.serialized)
@MainActor
struct PartialSweepRuntimeTests {
    private struct Walk: Error {}

    /// The marks a sweep spared, from the thread that walks.
    private final class Spared: @unchecked Sendable {
        private let lock = NSLock()
        private var marks: [GuestRunMark] = []
        func note(_ mark: GuestRunMark) { lock.withLock { marks.append(mark) } }
        var all: [GuestRunMark] { lock.withLock { marks } }
    }

    /// A runtime over an empty guest directory, which a sweep's record can be
    /// read from. The runtime holds its model weakly, so the model is returned
    /// for the test to keep.
    private func makeRuntime() throws -> (runtime: LiveRuntime, model: AppModel, directory: URL) {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let release = try SystemFolder.release(in: systemDirectory)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: release.identity, releaseDirectory: systemDirectory
        )
        let model = AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: storage.createRequirementBytes
        )
        let runtime = LiveRuntime(model: model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        return (runtime, model, directory)
    }

    @Test func aRestartInsideTheRunRenewsTheMarkAndTheSweepSparesTheOneCurrentWhenItRuns() async throws {
        let (runtime, model, directory) = try makeRuntime()
        defer {
            withExtendedLifetime(model) {}
            try? FileManager.default.removeItem(at: directory)
        }
        let spared = Spared()

        let first = runtime.runMark
        _ = try await runtime.sweepPartialFiles { guest in
            spared.note(guest.run)
            return 0
        }
        runtime.guestRestarted(.began(restart: 1))
        let second = runtime.runMark
        #expect(second != first)
        _ = try await runtime.sweepPartialFiles { guest in
            spared.note(guest.run)
            return 0
        }
        #expect(spared.all == [first, second])
        // The operations the file browser reads for each import carry the
        // renewed mark too.
        #expect(runtime.runMark == second)
    }

    @Test func anImportCutOffByARestartIsSweptByTheNextBootOfTheSameRun() async throws {
        let (runtime, model, directory) = try makeRuntime()
        defer {
            withExtendedLifetime(model) {}
            try? FileManager.default.removeItem(at: directory)
        }
        try PartialSweepLedger.markSettled(in: directory)

        // An import of this run is cut off by the restart; its own removal
        // cannot be made (the guest is gone), so the record is owed.
        let cutOff = runtime.runMark
        try runtime.partials.importBegan(mark: cutOff)
        runtime.partials.importEnded(mark: cutOff, clean: false)
        runtime.guestRestarted(.began(restart: 1))
        #expect(runtime.partials.isOwed)

        // The next boot's sweep spares the new mark only, so it takes the
        // cut-off import's file, and the record is settled.
        _ = try await runtime.sweepPartialFiles { guest in
            #expect(guest.run != cutOff)
            return 1
        }
        #expect(!runtime.partials.isOwed)
    }

    @Test func aSweepThatFailsLeavesTheRecordOwedAndOneThatFinishesSettlesIt() async throws {
        let (runtime, model, directory) = try makeRuntime()
        defer {
            withExtendedLifetime(model) {}
            try? FileManager.default.removeItem(at: directory)
        }
        // A guest with no record owes the walk.
        #expect(runtime.partials.isOwed)
        await #expect(throws: Walk.self) {
            _ = try await runtime.sweepPartialFiles { _ in throw Walk() }
        }
        #expect(runtime.partials.isOwed)
        #expect(PartialSweepLedger(directory: directory).isOwed)

        _ = try await runtime.sweepPartialFiles { _ in 2 }
        #expect(!runtime.partials.isOwed)
        #expect(!PartialSweepLedger(directory: directory).isOwed)
    }
}
