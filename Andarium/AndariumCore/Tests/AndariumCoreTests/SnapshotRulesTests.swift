// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private func state(
    release: ReleaseIdentity = snapshotRelease(), booted: String = "2026-11-10T10:00:00Z",
    completed: String? = "2026-11-10T10:05:00Z", completedRelease: String? = nil, pending: InstanceState.Pending? = nil
) -> InstanceState {
    var result = InstanceState(creating: release, at: booted)
    if let completed {
        result.lastCompleted = InstanceState.Completed(
            releaseID: completedRelease ?? release.releaseID, buildIncremental: String(release.buildNumber), at: completed)
    }
    result.pendingUpdate = pending
    return result
}

private let older = snapshotRelease("20261031-los24-r4", build: 100)
private let newer = snapshotRelease("20261102-los24-r5", build: 200, sha: String(repeating: "b", count: 64))

struct SnapshotRulesTests {
    // MARK: How the data was left

    @Test func aDiskIsShutDownWhenTheLastStartCompletedAfterItBegan() {
        #expect(SnapshotRules.wasShutDown(state()))
        #expect(SnapshotRules.wasShutDown(state(completed: "2026-11-10T10:00:01Z")))
    }

    @Test func aStartInTheSameSecondAsTheLastCompletionIsNotShutDown() {
        // The clock has whole seconds: a start that follows a completion at once
        // cannot be told from the completion of that start.
        #expect(!SnapshotRules.wasShutDown(state(completed: "2026-11-10T10:00:00Z")))
    }

    @Test(arguments: [
        ("no completion at all", nil as String?, nil as String?),
        ("a completion before the last start", "2026-11-10T09:59:59Z", nil),
        ("a completion of another release", "2026-11-10T10:05:00Z", "20261001-los24-r1"),
        ("a completion that cannot be read", "yesterday", nil),
    ])
    func aDiskIsNotShutDownOtherwise(_ label: String, completed: String?, completedRelease: String?) {
        #expect(!SnapshotRules.wasShutDown(state(completed: completed, completedRelease: completedRelease)), "\(label)")
    }

    @Test func aStartThatFollowsAShutDownIsNotOne() {
        // What a boot writes (`lastBooted`) is later than the earlier completion.
        #expect(!SnapshotRules.wasShutDown(state(booted: "2026-11-10T11:00:00Z", completed: "2026-11-10T10:05:00Z")))
    }

    // MARK: Create

    @Test func createIsAllowedOnAShutDownDiskOfThisRelease() {
        #expect(SnapshotRules.createRefusal(state: state(), dataDiskExists: true, release: older) == nil)
    }

    @Test func createRefusesWhatIsNotAShutDownDiskOfAKnownRecord() {
        #expect(SnapshotRules.createRefusal(state: state(), dataDiskExists: false, release: older) == .noData)
        #expect(SnapshotRules.createRefusal(state: nil, dataDiskExists: true, release: older) == .mismatch)
        #expect(SnapshotRules.createRefusal(state: state(completed: nil), dataDiskExists: true, release: older) == .notShutDown)
        // Another system disk under the same release.
        let other = snapshotRelease(older.releaseID, build: older.buildNumber, sha: String(repeating: "c", count: 64))
        #expect(SnapshotRules.createRefusal(state: state(), dataDiskExists: true, release: other) == .mismatch)
    }

    @Test func createRefusesAnUnfinishedUpdateWhateverTheRelease() {
        let origin = InstanceState.Origin(
            releaseID: older.releaseID, buildFingerprint: older.buildFingerprint, buildIncremental: "100",
            systemDiskSHA256: older.systemDiskSHA256, dataLayout: 1, lastBooted: "2026-11-10T10:00:00Z")
        let pending = InstanceState.Pending(toReleaseID: newer.releaseID, snapshot: "data.img.pre-\(newer.releaseID)", from: origin)
        let updating = state(release: newer, pending: pending)
        for release in [older, newer] {
            #expect(SnapshotRules.createRefusal(state: updating, dataDiskExists: true, release: release) == .unfinishedUpdate)
        }
    }

    @Test func createIsAllowedWhileAnUpdateWaitsAndWhenTheDataIsIntactButBlocked() {
        // A newer Android waits to update the data: the snapshot is what a
        // careful person takes first.
        #expect(SnapshotRules.createRefusal(state: state(release: older), dataDiskExists: true, release: newer) == nil)
        // An app older than the data, and an app whose data layout cannot take the
        // data forward: the data is intact, and a newer app restores it.
        #expect(SnapshotRules.createRefusal(state: state(release: newer), dataDiskExists: true, release: older) == nil)
        let layout2 = snapshotRelease("20261103-los24-r6", build: 300, sha: String(repeating: "d", count: 64), layout: 2)
        #expect(SnapshotRules.createRefusal(state: state(release: older), dataDiskExists: true, release: layout2) == nil)
        #expect(SnapshotRules.createRefusal(state: state(release: layout2), dataDiskExists: true, release: older) == nil)
    }

    // MARK: Restore

    @Test func aSnapshotOfThisAndroidRestoresAsItIs() {
        #expect(SnapshotRules.restorePlan(of: state(release: older), diskGiB: 8, release: older) == .asIs)
    }

    @Test func aSnapshotOfAnOlderAndroidRestoresAndUpdatesAtTheNextStart() {
        #expect(
            SnapshotRules.restorePlan(of: state(release: older), diskGiB: 8, release: newer)
                == .updatesAtNextStart(from: older.releaseID, to: newer.releaseID))
    }

    @Test func aSnapshotThisAppCannotOpenIsRefusedWithTheReason() {
        #expect(SnapshotRules.restorePlan(of: state(release: newer), diskGiB: 8, release: older) == .refused(.newerAndroid))
        let layout2 = snapshotRelease("20261103-los24-r6", build: 300, sha: String(repeating: "d", count: 64), layout: 2)
        // The app's layout is newer than the snapshot's and there is no migration.
        #expect(SnapshotRules.restorePlan(of: state(release: older), diskGiB: 8, release: layout2) == .refused(.olderDataLayout))
        // The snapshot's layout is newer than the app's: an older app.
        #expect(SnapshotRules.restorePlan(of: state(release: layout2), diskGiB: 8, release: older) == .refused(.newerAndroid))
        let other = snapshotRelease(older.releaseID, build: older.buildNumber, sha: String(repeating: "c", count: 64))
        #expect(SnapshotRules.restorePlan(of: state(release: older), diskGiB: 8, release: other) == .refused(.differentSystem))
    }

    @Test func aSnapshotOfTheOldCounterUpdatesToAReleaseNumberAndNotBack() {
        let counted = snapshotRelease("20261110-los24-r22", build: 2_026_110_817)
        let numbered = snapshotRelease("20261114-los24-r26", build: 26, sha: String(repeating: "b", count: 64))
        #expect(
            SnapshotRules.restorePlan(of: state(release: counted), diskGiB: 8, release: numbered)
                == .updatesAtNextStart(from: counted.releaseID, to: numbered.releaseID))
        #expect(SnapshotRules.restorePlan(of: state(release: numbered), diskGiB: 8, release: counted) == .refused(.newerAndroid))
    }

    @Test func aDiskSizeTheReleaseDoesNotAcceptIsRefused() {
        let release = snapshotRelease(minimumGiB: 4, maximumGiB: 16)
        #expect(SnapshotRules.restorePlan(of: state(release: release), diskGiB: 16, release: release) == .asIs)
        #expect(SnapshotRules.restorePlan(of: state(release: release), diskGiB: 4, release: release) == .asIs)
        #expect(SnapshotRules.restorePlan(of: state(release: release), diskGiB: 32, release: release) == .refused(.diskSizeNotAccepted(gib: 32)))
        #expect(SnapshotRules.restorePlan(of: state(release: release), diskGiB: 2, release: release) == .refused(.diskSizeNotAccepted(gib: 2)))
        // The size of an update's target counts as well.
        let target = snapshotRelease(newer.releaseID, build: 200, sha: newer.systemDiskSHA256, minimumGiB: 4, maximumGiB: 16)
        #expect(SnapshotRules.restorePlan(of: state(release: older), diskGiB: 32, release: target) == .refused(.diskSizeNotAccepted(gib: 32)))
    }

    @Test func aRecordThatNamesAnUnfinishedUpdateIsDamaged() {
        let origin = InstanceState.Origin(
            releaseID: older.releaseID, buildFingerprint: older.buildFingerprint, buildIncremental: "100",
            systemDiskSHA256: older.systemDiskSHA256, dataLayout: 1, lastBooted: "2026-11-10T10:00:00Z")
        let pending = InstanceState.Pending(toReleaseID: newer.releaseID, snapshot: "data.img.pre-x", from: origin)
        #expect(SnapshotRules.restorePlan(of: state(release: newer, pending: pending), diskGiB: 8, release: newer) == .refused(.incomplete))
    }

    // MARK: Names, gate

    @Test func aNameIsTrimmedAndBounded() {
        #expect(SnapshotRules.normalizedName("  Before the update \n") == "Before the update")
        #expect(SnapshotRules.normalizedName("   ") == "")
        #expect(SnapshotRules.normalizedName(String(repeating: "x", count: SnapshotRules.maximumNameLength)) != nil)
        #expect(SnapshotRules.normalizedName(String(repeating: "x", count: SnapshotRules.maximumNameLength + 1)) == nil)
        #expect(SnapshotRules.normalizedName("a\u{0}b") == nil)
        #expect(SnapshotRules.normalizedName("a\tb") == nil)
        #expect(SnapshotRules.normalizedName("Pre-update \u{1F4F1}") == "Pre-update \u{1F4F1}")
    }

    @Test func aGuestIsStoppedOnlyAtHomeWithNothingRunningOrUnderWay() {
        #expect(SnapshotRules.gate(phase: .home, runtimeActive: false, operationUnderWay: false) == nil)
        for phase: Phase in [.booting(.normal), .booting(.restart), .running(.normal), .running(.recovery), .failed] {
            #expect(SnapshotRules.gate(phase: phase, runtimeActive: false, operationUnderWay: false) == .guestNotStopped)
        }
        // The runtime holds the disk before the phase says so, and after a run.
        #expect(SnapshotRules.gate(phase: .home, runtimeActive: true, operationUnderWay: false) == .guestNotStopped)
        #expect(SnapshotRules.gate(phase: .home, runtimeActive: false, operationUnderWay: true) == .busy)
        // A guest that runs is the stronger reason.
        #expect(SnapshotRules.gate(phase: .running(.normal), runtimeActive: true, operationUnderWay: true) == .guestNotStopped)
    }

    // MARK: Records

    @Test func aSnapshotRecordKeepsTheSchemaAndRoundTrips() throws {
        let record = SnapshotRecord(
            name: "Before r5", created: Date(timeIntervalSince1970: 1_793_000_000.25), releaseID: older.releaseID,
            buildIncremental: "100", diskGiB: 8)
        let bytes = try SnapshotStore.encoded(record)
        let object = try #require(try JSONSerialization.jsonObject(with: bytes) as? [String: Any])
        #expect(Set(object.keys) == ["schema", "name", "created", "release_id", "build_incremental", "disk_gib"])
        #expect(object["schema"] as? Int == 1)
        let decoded = try JSONDecoder().decode(SnapshotRecord.self, from: bytes)
        #expect(decoded == record)
        #expect(decoded.buildNumber == 100)
    }

    @Test func aSnapshotRecordOfAnotherSchemaOrWithoutATimeIsRefused() throws {
        func decode(_ text: String) throws -> SnapshotRecord {
            try JSONDecoder().decode(SnapshotRecord.self, from: Data(text.utf8))
        }
        let good = #"{"schema": 1, "name": "", "created": "2026-11-10T10:00:00Z", "release_id": "r", "build_incremental": "1", "disk_gib": 4}"#
        #expect(throws: Never.self) { try decode(good) }
        #expect(throws: InstanceError.self) { try decode(good.replacingOccurrences(of: #""schema": 1"#, with: #""schema": 2"#)) }
        #expect(throws: InstanceError.self) { try decode(good.replacingOccurrences(of: "2026-11-10T10:00:00Z", with: "later")) }
        #expect(throws: InstanceError.self) { try decode(good.replacingOccurrences(of: #""disk_gib": 4"#, with: #""disk_gib": 0"#)) }
    }

    @Test func anIdentityIsALowercaseUUIDSoNoNameReachesOutsideTheStore() {
        #expect(SnapshotID(SnapshotID().rawValue) != nil)
        for text in ["", "..", "../x", "ABCDEF01-0000-0000-0000-000000000000", ".new-0f0f0f0f-0000-0000-0000-000000000000",
            "0f0f0f0f-0000-0000-0000-00000000000g", "0f0f0f0f-0000-0000-0000-0000000000001"] {
            #expect(SnapshotID(text) == nil, "\(text)")
        }
        #expect(throws: DecodingError.self) { try JSONDecoder().decode(SnapshotID.self, from: Data(#""../x""#.utf8)) }
    }

    @Test func eachStateOfTheRuntimeKeepsTheDiskFromAnOperation() {
        #expect(!SnapshotRules.runtimeActive(running: false, pending: false, launching: false, preparing: false))
        #expect(SnapshotRules.runtimeActive(running: true, pending: false, launching: false, preparing: false))
        #expect(SnapshotRules.runtimeActive(running: false, pending: true, launching: false, preparing: false))
        #expect(SnapshotRules.runtimeActive(running: false, pending: false, launching: true, preparing: false))
        #expect(SnapshotRules.runtimeActive(running: false, pending: false, launching: false, preparing: true))
        // And with it the gate says the guest is not stopped, on Home or not.
        #expect(SnapshotRules.gate(phase: .home, runtimeActive: true, operationUnderWay: false) == .guestNotStopped)
    }

    @Test func aResultCarriesARefusalAsAValueAndAnythingElseAsAFailure() {
        struct Broken: Error {}
        #expect(SnapshotResult<Int>(catching: { 7 }).value == 7)
        #expect(SnapshotResult<Int>(catching: { throw SnapshotRefusal.notShutDown }).refusal == .notShutDown)
        // A restore that took effect and did not finish says so as a value.
        let unfinished = SnapshotRestoreUnfinished(cause: Broken())
        #expect(SnapshotResult<Int>(catching: { throw unfinished }).refusal == .restoreUnfinished)
        if case .failed(let error) = SnapshotResult<Int>(catching: { throw Broken() }) {
            #expect(error is Broken)
        } else {
            Issue.record("a failure was expected")
        }
    }
}
