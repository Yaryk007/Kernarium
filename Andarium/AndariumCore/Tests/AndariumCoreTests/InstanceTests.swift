// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private func release(
    _ id: String = "rel-b", build: Int = 200, layout: Int = 1, sha: String = String(repeating: "b", count: 64),
    proof: Bool = false
) -> ReleaseIdentity {
    ReleaseIdentity(
        releaseID: id, buildFingerprint: "fp-\(id)", buildNumber: build, securityPatch: "2026-10-01",
        systemDiskSHA256: sha, dataLayout: layout, minimumGiB: 4, defaultGiB: 8, maximumGiB: 64,
        isUnsignedProof: proof
    )
}

private func temporaryDirectory() throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-instance-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}

/// The vectors every implementation of the update rules reproduces
/// (`android/lineage-24/support/instance/update-decisions.json`).
struct DecisionVectorTests {
    private struct Vector: Decodable {
        struct Instance: Decodable {
            var releaseID: String
            var buildNumber: Int
            var systemDiskSHA256: String
            var dataLayout: Int
            var pendingToReleaseID: String?
            enum CodingKeys: String, CodingKey {
                case releaseID = "release_id", buildNumber = "build_number"
                case systemDiskSHA256 = "system_disk_sha256", dataLayout = "data_layout"
                case pendingToReleaseID = "pending_to_release_id"
            }
        }
        struct Release: Decodable {
            var releaseID: String
            var buildNumber: Int
            var systemDiskSHA256: String
            var dataLayout: Int
            enum CodingKeys: String, CodingKey {
                case releaseID = "release_id", buildNumber = "build_number"
                case systemDiskSHA256 = "system_disk_sha256", dataLayout = "data_layout"
            }
        }
        var name: String
        var instance: Instance?
        var dataDisk: Bool
        var snapshot: Bool
        var release: Release
        var flags: [String]
        var expect: String
        enum CodingKeys: String, CodingKey {
            case name, instance, release, flags, expect, snapshot
            case dataDisk = "data_disk"
        }
    }

    /// What a start's `sys.boot_completed` records on the instance.
    private struct Transition: Decodable {
        struct Expectation: Decodable {
            var pendingToReleaseID: String?
            var snapshot: Bool
            enum CodingKeys: String, CodingKey {
                case pendingToReleaseID = "pending_to_release_id", snapshot
            }
        }
        var name: String
        var event: String
        var instance: Vector.Instance?
        var snapshot: Bool
        var release: Vector.Release
        var expect: Expectation?
        var refused: Bool
        enum CodingKeys: String, CodingKey { case name, event, instance, snapshot, release, expect }

        init(from decoder: Decoder) throws {
            let values = try decoder.container(keyedBy: CodingKeys.self)
            name = try values.decode(String.self, forKey: .name)
            event = try values.decode(String.self, forKey: .event)
            instance = try values.decodeIfPresent(Vector.Instance.self, forKey: .instance)
            snapshot = try values.decode(Bool.self, forKey: .snapshot)
            release = try values.decode(Vector.Release.self, forKey: .release)
            // The expectation is the word "refused" or the state afterwards.
            if (try? values.decode(String.self, forKey: .expect)) == "refused" {
                refused = true
                expect = nil
            } else {
                refused = false
                expect = try values.decode(Expectation.self, forKey: .expect)
            }
        }
    }

    private struct Document: Decodable {
        struct Legacy: Decodable {
            var first: Int
            var last: Int
        }
        var legacyBuildNumbers: Legacy
        var cases: [Vector]
        var transitions: [Transition]
        enum CodingKeys: String, CodingKey {
            case legacyBuildNumbers = "legacy_build_numbers", cases, transitions
        }
    }

    private static func document() throws -> Document {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "android/lineage-24/support/instance/update-decisions.json")
        return try JSONDecoder().decode(Document.self, from: Data(contentsOf: url))
    }

    private static func vectors() throws -> [Vector] { try document().cases }

    @Test func everyTransitionIsReproduced() throws {
        let transitions = try Self.document().transitions
        #expect(transitions.count == 5)
        for transition in transitions {
            let root = try temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: root) }
            let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
            if let instance = transition.instance {
                var state = InstanceState(
                    dataLayout: instance.dataLayout, releaseID: instance.releaseID, buildFingerprint: "fp",
                    buildIncremental: String(instance.buildNumber), systemDiskSHA256: instance.systemDiskSHA256,
                    createdWith: "rel-0", lastBooted: "2026-10-02T00:00:00Z"
                )
                if let target = instance.pendingToReleaseID {
                    state.pendingUpdate = InstanceState.Pending(
                        toReleaseID: target, snapshot: "data.img.pre-\(target)",
                        from: InstanceState.Origin(
                            releaseID: "rel-0", buildFingerprint: "fp", buildIncremental: "50",
                            systemDiskSHA256: String(repeating: "0", count: 64), dataLayout: 1,
                            lastBooted: "2026-10-01T00:00:00Z"
                        )
                    )
                }
                try files.write(state)
                try Data("guest data".utf8).write(to: files.data)
            }
            if transition.snapshot {
                try FileManager.default.createDirectory(at: files.directory, withIntermediateDirectories: true)
                try Data("snapshot".utf8).write(to: files.snapshot(for: "rel-b"))
            }
            let target = release(
                transition.release.releaseID, build: transition.release.buildNumber,
                layout: transition.release.dataLayout, sha: transition.release.systemDiskSHA256
            )
            if transition.refused {
                #expect(throws: InstanceError.self, "\(transition.name)") { try files.booted(release: target) }
                continue
            }
            if transition.event == "boot_completed" { try files.booted(release: target) }
            let expected = try #require(transition.expect)
            let after = try #require(try files.readState())
            #expect(after.pendingUpdate?.toReleaseID == expected.pendingToReleaseID, "\(transition.name)")
            #expect(
                FileManager.default.fileExists(atPath: files.snapshot(for: "rel-b").path) == expected.snapshot,
                "\(transition.name)"
            )
            #expect(try Data(contentsOf: files.data) == Data("guest data".utf8), "\(transition.name)")
        }
    }

    @Test func theLegacyRangeIsTheVectors() throws {
        let legacy = try Self.document().legacyBuildNumbers
        #expect(BuildNumber.legacyRange == legacy.first...legacy.last)
        #expect(BuildNumber.legacyRange == 2_026_110_800...2_026_110_899)
    }

    @Test func theOldCounterIsOlderThanEveryOtherNumber() {
        let ascending = [
            2_026_110_800, 2_026_110_801, 2_026_110_805, 2_026_110_817, 2_026_110_899,
            1, 26, 27, 10_000, 2_026_110_799, 2_026_110_900, 9_999_999_999,
        ]
        for (position, number) in ascending.enumerated() {
            #expect(!BuildNumber.isOlder(number, than: number), "\(number)")
            for other in ascending[(position + 1)...] {
                #expect(BuildNumber.isOlder(number, than: other), "\(number) is older than \(other)")
                #expect(!BuildNumber.isOlder(other, than: number), "\(other) is not older than \(number)")
            }
        }
        #expect(ascending.allSatisfy { !BuildNumber.isOlder($0, than: BuildNumber.oldest) })
        #expect(ascending.dropFirst().allSatisfy { BuildNumber.isOlder(BuildNumber.oldest, than: $0) })
    }

    @Test func everyVectorIsReproduced() throws {
        let cases = try Self.vectors()
        #expect(cases.count == 44)
        for vector in cases {
            let state = vector.instance.map { instance in
                var state = InstanceState(
                    dataLayout: instance.dataLayout, releaseID: instance.releaseID, buildFingerprint: "fp",
                    buildIncremental: String(instance.buildNumber), systemDiskSHA256: instance.systemDiskSHA256,
                    createdWith: instance.releaseID, lastBooted: "2026-10-01T00:00:00Z"
                )
                if let target = instance.pendingToReleaseID {
                    state.pendingUpdate = InstanceState.Pending(
                        toReleaseID: target, snapshot: "data.img.pre-\(target)",
                        from: InstanceState.Origin(
                            releaseID: "rel-a", buildFingerprint: "fp", buildIncremental: "100",
                            systemDiskSHA256: String(repeating: "a", count: 64), dataLayout: instance.dataLayout,
                            lastBooted: "2026-09-01T00:00:00Z"
                        )
                    )
                }
                return state
            }
            let flags = InstanceFlags(
                retry: vector.flags.contains("retry"), restoreSnapshot: vector.flags.contains("restore_snapshot"),
                reset: vector.flags.contains("reset")
            )
            let target = release(
                vector.release.releaseID, build: vector.release.buildNumber, layout: vector.release.dataLayout,
                sha: vector.release.systemDiskSHA256
            )
            let decision = InstanceRules.decide(
                state: state, dataDiskExists: vector.dataDisk, snapshotExists: vector.snapshot, release: target, flags: flags
            )
            #expect(decision.action.rawValue == vector.expect, "\(vector.name)")
        }
    }
}

struct InstanceFileTests {
    private func started() throws -> (InstanceFiles, URL, ReleaseIdentity) {
        let root = try temporaryDirectory()
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let first = release("rel-a", build: 100, sha: String(repeating: "a", count: 64))
        let decision = try files.decide(for: first)
        #expect(decision.action == .create)
        try files.apply(decision, release: first)
        try Data("android data".utf8).write(to: files.data)
        return (files, root, first)
    }

    @Test func aNewInstanceRecordsItsReleaseAndAnOldOneBootsAgain() throws {
        let (files, root, first) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let state = try #require(try files.readState())
        #expect(state.releaseID == "rel-a" && state.buildNumber == 100 && state.createdWith == "rel-a")
        #expect(state.lastCompleted == nil && state.pendingUpdate == nil)
        let decision = try files.decide(for: first)
        #expect(decision.action == .boot)
        try files.apply(decision, release: first)
    }

    @Test func theStateIsWrittenWithTheSchemasKeys() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let object = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: files.state)) as? [String: Any])
        #expect(Set(object.keys) == InstanceState.keys)
        #expect(object["schema"] as? Int == 1)
        #expect(object["last_completed"] is NSNull && object["pending_update"] is NSNull)
    }

    @Test func anUpdateSnapshotsTheDataAndCompletingItDeletesTheSnapshot() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        let decision = try files.decide(for: second)
        #expect(decision.action == .update)
        let updated = try files.apply(decision, release: second)
        let snapshot = files.snapshot(for: "rel-b")
        #expect(FileManager.default.fileExists(atPath: snapshot.path))
        #expect(try Data(contentsOf: snapshot) == Data("android data".utf8))
        #expect(updated.pendingUpdate?.toReleaseID == "rel-b" && updated.pendingUpdate?.from.releaseID == "rel-a")
        // Until the update completes, starting is refused.
        #expect(try files.decide(for: second).action == .refusePending)

        let done = try files.complete(release: second)
        #expect(done.pendingUpdate == nil && done.lastCompleted?.releaseID == "rel-b")
        #expect(!FileManager.default.fileExists(atPath: snapshot.path))
        #expect(try files.decide(for: second).action == .boot)
    }

    /// The data of a release of the old counter updates to a release number,
    /// and no way leads back.
    @Test func aReleaseNumberUpdatesTheDataOfTheOldCounterAndNotBack() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let old = release("rel-a", build: 2_026_110_817, sha: String(repeating: "a", count: 64))
        let numbered = release("rel-b", build: 26, sha: String(repeating: "b", count: 64))
        try files.apply(try files.decide(for: old), release: old)
        try Data("android data".utf8).write(to: files.data)
        try files.complete(release: old)

        let update = try files.decide(for: numbered)
        #expect(update.action == .update)
        let updated = try files.apply(update, release: numbered)
        #expect(updated.buildIncremental == "26" && updated.pendingUpdate?.from.buildIncremental == "2026110817")
        try files.complete(release: numbered)
        #expect(try files.decide(for: old).action == .refuseDowngrade)
        #expect(try files.decide(for: release("rel-c", build: 27, sha: String(repeating: "c", count: 64))).action == .update)
    }

    /// The sequence that lost a week of user data: r13 settles, r15 updates and
    /// boots, the host is killed before Android powers off, r15 starts again,
    /// and r16 arrives.
    @Test func anUpdateThatBootedIsNotRolledBackByTheNextRelease() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let r13 = release("r13", build: 13, sha: String(repeating: "a", count: 64))
        let r15 = release("r15", build: 15, sha: String(repeating: "b", count: 64))
        let r16 = release("r16", build: 16, sha: String(repeating: "c", count: 64))
        try files.apply(try files.decide(for: r13), release: r13)
        try Data("data written on r13".utf8).write(to: files.data)
        try files.complete(release: r13)

        let update = try files.decide(for: r15)
        #expect(update.action == .update)
        try files.apply(update, release: r15)
        #expect(FileManager.default.fileExists(atPath: files.snapshot(for: "r15").path))
        // r15 reaches boot_completed: the update is applied.
        let booted = try files.booted(release: r15)
        #expect(booted.pendingUpdate == nil)
        #expect(!FileManager.default.fileExists(atPath: files.snapshot(for: "r15").path))
        try Data("r15 upgraded data and a week of user data".utf8).write(to: files.data)
        // The host dies without a clean power-off; the next start is an
        // ordinary one, not a retry.
        let next = try files.decide(for: r15)
        #expect(next.action == .boot)
        try files.apply(next, release: r15)
        try Data("r15 upgraded data and a month of user data".utf8).write(to: files.data)

        let newer = try files.decide(for: r16)
        #expect(newer.action == .update)
        let updated = try files.apply(newer, release: r16)
        let month = Data("r15 upgraded data and a month of user data".utf8)
        #expect(try Data(contentsOf: files.data) == month)
        #expect(try Data(contentsOf: files.snapshot(for: "r16")) == month)
        #expect(updated.pendingUpdate?.from.releaseID == "r15" && updated.pendingUpdate?.toReleaseID == "r16")
        // The update to r16 is now the pending one, with a snapshot of its own.
        #expect(try files.startPlan(for: r16) == .unfinishedUpdate(to: "r16", canRetry: true, canRestore: true))
    }

    @Test func aFailedUpdateCanBeRetriedOrRestored() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        try files.apply(try files.decide(for: second), release: second)
        try Data("data written by the update".utf8).write(to: files.data)

        let retry = try files.decide(for: second, flags: InstanceFlags(retry: true))
        #expect(retry.action == .retry)
        try files.apply(retry, release: second)
        #expect(try files.readState()?.pendingUpdate != nil)

        let restore = try files.decide(for: second, flags: InstanceFlags(restoreSnapshot: true))
        #expect(restore.action == .restore)
        // The data goes back and nothing else happens.
        let restored = try files.apply(restore, release: second)
        #expect(try Data(contentsOf: files.data) == Data("android data".utf8))
        #expect(restored.releaseID == "rel-a" && restored.pendingUpdate == nil)
        // A normal start then decides afresh: the update again, which is what
        // a restore that went straight on to update would have reached.
        let next = try files.decide(for: second)
        #expect(next.action == .update)
        let again = try files.apply(next, release: second)
        #expect(try Data(contentsOf: files.data) == Data("android data".utf8))
        #expect(again.pendingUpdate?.toReleaseID == "rel-b")
    }

    @Test func aNewerReleaseRestoresAnUnfinishedUpdateBeforeStarting() throws {
        let (files, root, first) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        let newer = release("rel-d", build: 10000, sha: String(repeating: "d", count: 64))
        try files.apply(try files.decide(for: second), release: second)
        try Data("partially upgraded data".utf8).write(to: files.data)

        let decision = try files.decide(for: newer)
        #expect(decision.action == .update)
        let updated = try files.apply(decision, release: newer)
        #expect(try Data(contentsOf: files.data) == Data("android data".utf8))
        #expect(try Data(contentsOf: files.snapshot(for: "rel-d")) == Data("android data".utf8))
        #expect(!FileManager.default.fileExists(atPath: files.snapshot(for: "rel-b").path))
        #expect(updated.releaseID == "rel-d" && updated.pendingUpdate?.from.releaseID == first.releaseID)
        #expect(updated.pendingUpdate?.toReleaseID == "rel-d" && updated.createdWith == first.releaseID)
        try files.complete(release: newer)
        #expect(!FileManager.default.fileExists(atPath: files.snapshot(for: "rel-d").path))
    }

    @Test func aMissingSnapshotLeavesTheUnfinishedUpdatesDataAlone() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        try files.apply(try files.decide(for: second), release: second)
        try Data("partially upgraded data".utf8).write(to: files.data)
        try FileManager.default.removeItem(at: files.snapshot(for: "rel-b"))
        let state = try Data(contentsOf: files.state)
        let newer = release("rel-d", build: 10000, sha: String(repeating: "d", count: 64))
        let decision = try files.decide(for: newer)
        #expect(decision.action == .refuseConflict)
        #expect(throws: InstanceError.self) { try files.apply(decision, release: newer) }
        #expect(try Data(contentsOf: files.state) == state)
        #expect(try Data(contentsOf: files.data) == Data("partially upgraded data".utf8))
    }

    @Test func aRefusalChangesNothing() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let older = release("rel-0", build: 50)
        let decision = try files.decide(for: older)
        #expect(decision.action == .refuseDowngrade)
        let before = try Data(contentsOf: files.state)
        #expect(throws: InstanceError.self) { try files.apply(decision, release: older) }
        #expect(try Data(contentsOf: files.state) == before)
    }

    @Test func resetDeletesTheInstanceAndCreatesItAnew() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        try files.apply(try files.decide(for: second), release: second)
        let reset = try files.decide(for: second, flags: InstanceFlags(reset: true))
        #expect(reset.action == .reset)
        let created = try files.apply(reset, release: second)
        #expect(created.releaseID == "rel-b" && created.createdWith == "rel-b" && created.pendingUpdate == nil)
        #expect(!FileManager.default.fileExists(atPath: files.data.path))
        #expect(!FileManager.default.fileExists(atPath: files.snapshot(for: "rel-b").path))
    }

    /// What the runtime keeps beside the data disk: a synthetic one, since no
    /// real installation's values belong in a test.
    private static let identityContent = Data(
        #"{"ethernet_mac": "02:aa:bb:cc:dd:ee", "schema": 1, "serialno": "0123456789abcdef"}"#.utf8)

    @Test func theInstallationIdentityGoesThroughUpdatesRetriesAndRestoresUntouched() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        try Self.identityContent.write(to: files.identity)
        let manager = FileManager.default
        func unchanged(_ step: String) throws {
            #expect(try Data(contentsOf: files.identity) == Self.identityContent, "\(step)")
        }

        let second = release("rel-b", build: 200)
        try files.apply(try files.decide(for: second), release: second)
        try unchanged("an update")
        let retry = try files.decide(for: second, flags: InstanceFlags(retry: true))
        try files.apply(retry, release: second)
        try unchanged("a retry")
        let restore = try files.decide(for: second, flags: InstanceFlags(restoreSnapshot: true))
        try files.apply(restore, release: second)
        try unchanged("a restore of the data an update started from")
        try files.apply(try files.decide(for: second), release: second)
        try files.complete(release: second)
        try unchanged("a completed update")
        let boot = try files.decide(for: second)
        #expect(boot.action == .boot)
        try files.apply(boot, release: second)
        try unchanged("a boot")
        #expect(manager.fileExists(atPath: files.identity.path))
    }

    @Test func resetAndANewDataDiskEndTheInstallationIdentity() throws {
        let (files, root, first) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let manager = FileManager.default
        let leftover = files.directory.appendingPathComponent(InstanceFiles.identityFile + ".tmp-123")
        try Self.identityContent.write(to: files.identity)
        try Data("half".utf8).write(to: leftover)

        // An erase and the tooling's reset delete the file and what a crash left of a write of it.
        let reset = try files.decide(for: first, flags: InstanceFlags(reset: true))
        #expect(reset.action == .reset)
        try files.apply(reset, release: first)
        #expect(!manager.fileExists(atPath: files.identity.path) && !manager.fileExists(atPath: leftover.path))

        // A data disk that is gone starts a new installation, whatever lies beside it.
        try Self.identityContent.write(to: files.identity)
        try Data("disk".utf8).write(to: files.data)
        try manager.removeItem(at: files.data)
        let created = try files.decide(for: first)
        #expect(created.action == .create)
        try files.apply(created, release: first)
        #expect(!manager.fileExists(atPath: files.identity.path))
    }

    @Test func aReleaseIdentityIsReadFromTheManifestAndTheLayout() throws {
        let manifest = #"{"release_id": "20261102-los24-r5", "android": {"fingerprint": "Andarium/lineage_andarium/andarium:17/a/b:user/release-keys", "build_number": 1793463711, "security_patch": "2026-10-01"}}"#
        let layout = #"{"system_disk": {"sha256": "\#(String(repeating: "c", count: 64))"}, "data_disk": {"layout_version": 1, "capacity_gib": {"minimum": 4, "default": 8, "maximum": 64}}}"#
        let identity = try ReleaseIdentity(manifest: Data(manifest.utf8), layout: Data(layout.utf8))
        #expect(identity.releaseID == "20261102-los24-r5" && identity.buildNumber == 1_793_463_711)
        #expect(identity.dataLayout == 1 && identity.minimumGiB == 4 && identity.defaultGiB == 8 && identity.maximumGiB == 64)
        // A manifest of an earlier schema has no build number.
        let old = #"{"release_id": "20261031-los24-r4", "android": {"fingerprint": "f"}}"#
        #expect(throws: InstanceError.self) { try ReleaseIdentity(manifest: Data(old.utf8), layout: Data(layout.utf8)) }
    }
}

/// An unsigned proof image (a developer build's system, `acceptance`
/// "unsigned-proof") starts only a fresh instance, and an instance it created
/// starts only that proof: a proof never updates data another system wrote, and
/// its data never goes to another system. The refusal is the ordinary
/// mismatch of `StartPlan`; signed releases decide as the vectors say.
struct UnsignedProofInstanceTests {
    private static let proof = "unsigned-proof-p1"

    private func state(
        _ id: String, build: Int, sha: String, pending: String? = nil, proof: Bool = false
    ) -> InstanceState {
        var state = InstanceState(
            dataLayout: 1, releaseID: id, buildFingerprint: "fp", buildIncremental: String(build),
            systemDiskSHA256: sha, createdWith: id, lastBooted: "2026-10-01T00:00:00Z", isUnsignedProof: proof
        )
        if let pending {
            state.pendingUpdate = InstanceState.Pending(
                toReleaseID: pending, snapshot: "data.img.pre-\(pending)",
                from: InstanceState.Origin(
                    releaseID: "rel-0", buildFingerprint: "fp", buildIncremental: "50",
                    systemDiskSHA256: String(repeating: "0", count: 64), dataLayout: 1, lastBooted: "2026-09-01T00:00:00Z"
                )
            )
        }
        return state
    }

    private let sha = String(repeating: "b", count: 64)
    private let otherSHA = String(repeating: "c", count: 64)

    private func decide(
        _ state: InstanceState?, data: Bool = true, snapshot: Bool = false, _ release: ReleaseIdentity,
        flags: InstanceFlags = InstanceFlags()
    ) -> InstanceAction {
        InstanceRules.decide(
            state: state, dataDiskExists: data, snapshotExists: snapshot, release: release, flags: flags
        ).action
    }

    @Test func aFreshInstanceStartsTheProof() {
        let proof = release(Self.proof, build: 300, sha: sha, proof: true)
        #expect(decide(nil, data: false, proof) == .create)
        #expect(InstanceRules.plan(state: nil, dataDiskExists: false, snapshotExists: false, release: proof) == .firstStart)
        // A record with no data behind it holds nothing to keep, whichever system wrote it.
        #expect(decide(state("rel-a", build: 100, sha: otherSHA), data: false, proof) == .create)
        #expect(decide(state("unsigned-proof-p0", build: 250, sha: otherSHA, proof: true), data: false, proof) == .create)
        // Data nothing recorded is refused as for every system.
        #expect(decide(nil, data: true, proof) == .refuseConflict)
    }

    @Test func theInstanceTheProofCreatedStartsTheSameProofAgain() {
        let proof = release(Self.proof, build: 300, sha: sha, proof: true)
        let created = state(Self.proof, build: 300, sha: sha, proof: true)
        #expect(decide(created, proof) == .boot)
        #expect(InstanceRules.plan(state: created, dataDiskExists: true, snapshotExists: false, release: proof) == .normal)
        // One proof ID, two disks is the same conflict as for a release.
        #expect(decide(created, release(Self.proof, build: 300, sha: otherSHA, proof: true)) == .refuseConflict)
    }

    @Test func anInstanceOfAnotherSystemIsRefusedNeverUpdatedToTheProof() {
        let real = state("rel-a", build: 100, sha: otherSHA)
        // Newer, older and equal builds, a newer data layout, and a layout the instance has.
        for (build, layout) in [(300, 1), (50, 1), (100, 1), (300, 2), (100, 2)] {
            let proof = release(Self.proof, build: build, layout: layout, sha: sha, proof: true)
            #expect(decide(real, proof) == .refuseConflict, "build \(build) layout \(layout)")
            #expect(
                InstanceRules.plan(state: real, dataDiskExists: true, snapshotExists: false, release: proof)
                    == .blocked(.mismatch), "build \(build) layout \(layout)")
        }
        // An update that did not finish, with or without its snapshot, and the ways out of it.
        let unfinished = state("rel-b", build: 200, sha: sha, pending: "rel-b")
        let proof = release(Self.proof, build: 300, sha: sha, proof: true)
        for snapshot in [true, false] {
            for flags in [
                InstanceFlags(), InstanceFlags(retry: true), InstanceFlags(restoreSnapshot: true),
            ] {
                #expect(decide(unfinished, snapshot: snapshot, proof, flags: flags) == .refuseConflict)
            }
        }
    }

    @Test func aProofInstanceIsRefusedByEveryOtherSystem() {
        let created = state(Self.proof, build: 300, sha: sha, proof: true)
        let candidates = [
            release("unsigned-proof-p2", build: 400, sha: otherSHA, proof: true),
            release("unsigned-proof-p2", build: 200, sha: otherSHA, proof: true),
            release("rel-c", build: 400, sha: otherSHA),
            release("rel-c", build: 200, sha: otherSHA),
            release("rel-c", build: 300, sha: otherSHA),
            release("rel-c", build: 400, layout: 2, sha: otherSHA),
        ]
        for candidate in candidates {
            #expect(decide(created, candidate) == .refuseConflict, "\(candidate.releaseID) \(candidate.buildNumber)")
            #expect(
                InstanceRules.plan(state: created, dataDiskExists: true, snapshotExists: false, release: candidate)
                    == .blocked(.mismatch))
        }
        // The data is gone: a real release starts a new instance.
        #expect(decide(created, data: false, candidates[2]) == .create)
    }

    @Test func eraseIsTheWayOut() {
        let real = state("rel-a", build: 100, sha: otherSHA)
        let proof = release(Self.proof, build: 300, sha: sha, proof: true)
        #expect(decide(real, proof, flags: InstanceFlags(reset: true)) == .reset)
        let created = state(Self.proof, build: 300, sha: sha, proof: true)
        #expect(decide(created, release("rel-c", build: 400, sha: otherSHA), flags: InstanceFlags(reset: true)) == .reset)
    }

    @Test func aSignedReleaseDecidesAsBefore() {
        // No proof in the instance's record and none in the system: the rules of the vectors.
        let real = state("rel-a", build: 100, sha: otherSHA)
        #expect(decide(real, release("rel-b", build: 200, sha: sha)) == .update)
        #expect(decide(real, release("rel-b", build: 50, sha: sha)) == .refuseDowngrade)
        #expect(decide(real, release("rel-b", build: 100, sha: sha)) == .refuseConflict)
        #expect(decide(real, release("rel-b", build: 200, layout: 2, sha: sha)) == .migrate)
        // A release ID that only contains the words is no proof.
        let named = state("rel-unsigned-proof-a", build: 100, sha: otherSHA)
        #expect(decide(named, release("rel-b", build: 200, sha: sha)) == .update)
    }

    // A signed release and a proof may carry one release ID: the markers are
    // two systems, and the retry and the restore of a pending update never
    // reach the one that is not the system the data belongs to.

    private let sharedID = "20261102-los24-r5"

    @Test func aProofIsRefusedOverASignedUpdateThatSharesItsID() {
        // A signed update to sharedID that did not finish: state and pending name it.
        let unfinished = state(sharedID, build: 200, sha: sha, pending: sharedID)
        let proof = release(sharedID, build: 200, sha: otherSHA, proof: true)
        for snapshot in [true, false] {
            for flags in [InstanceFlags(), InstanceFlags(retry: true), InstanceFlags(restoreSnapshot: true)] {
                #expect(decide(unfinished, snapshot: snapshot, proof, flags: flags) == .refuseConflict, "\(snapshot) \(flags)")
            }
            #expect(
                InstanceRules.plan(state: unfinished, dataDiskExists: true, snapshotExists: snapshot, release: proof)
                    == .blocked(.mismatch))
        }
        // The same ID and the same disk is no different: the marker says it.
        let sameDisk = release(sharedID, build: 200, sha: sha, proof: true)
        #expect(decide(unfinished, sameDisk) == .refuseConflict)
        #expect(decide(unfinished, sameDisk, flags: InstanceFlags(retry: true)) == .refuseConflict)
        // A snapshot with no data disk behind it is data too.
        #expect(decide(unfinished, data: false, snapshot: true, proof, flags: InstanceFlags(retry: true)) == .refuseConflict)
        // A settled signed instance of that ID, too.
        #expect(decide(state(sharedID, build: 200, sha: sha), proof) == .refuseConflict)
        #expect(decide(state(sharedID, build: 200, sha: sha), sameDisk) == .refuseConflict)
        // Erase stays the way out.
        #expect(decide(unfinished, proof, flags: InstanceFlags(reset: true)) == .reset)
    }

    @Test func aSignedReleaseIsRefusedOverTheInstanceOfAProofThatSharesItsID() {
        let created = state(sharedID, build: 200, sha: sha, proof: true)
        for signed in [release(sharedID, build: 200, sha: otherSHA), release(sharedID, build: 200, sha: sha)] {
            #expect(decide(created, signed) == .refuseConflict)
            #expect(
                InstanceRules.plan(state: created, dataDiskExists: true, snapshotExists: false, release: signed)
                    == .blocked(.mismatch))
            #expect(decide(created, signed, flags: InstanceFlags(retry: true)) == .refuseConflict)
            #expect(decide(created, signed, flags: InstanceFlags(restoreSnapshot: true)) == .refuseConflict)
            #expect(decide(created, signed, flags: InstanceFlags(reset: true)) == .reset)
            // The data is gone: a record alone holds nothing to keep.
            #expect(decide(created, data: false, signed) == .create)
        }
    }

    @Test func oneIDWithOneMarkerKeepsEveryRetryAndRestoreDecision() {
        for proofMarker in [false, true] {
            let same = release(sharedID, build: 200, sha: sha, proof: proofMarker)
            let otherDisk = release(sharedID, build: 200, sha: otherSHA, proof: proofMarker)
            let unfinished = state(sharedID, build: 200, sha: sha, pending: sharedID, proof: proofMarker)
            let settled = state(sharedID, build: 200, sha: sha, proof: proofMarker)
            let word: Comment = "marker \(proofMarker)"
            // The update that did not finish: Try Again, restore, and the refusal that offers both.
            #expect(decide(unfinished, snapshot: true, same) == .refusePending, word)
            #expect(decide(unfinished, snapshot: true, same, flags: InstanceFlags(retry: true)) == .retry, word)
            #expect(decide(unfinished, snapshot: true, same, flags: InstanceFlags(restoreSnapshot: true)) == .restore, word)
            #expect(decide(unfinished, snapshot: false, same, flags: InstanceFlags(restoreSnapshot: true)) == .refuseConflict, word)
            #expect(decide(unfinished, snapshot: false, same, flags: InstanceFlags(retry: true)) == .retry, word)
            #expect(decide(unfinished, data: false, same, flags: InstanceFlags(retry: true)) == .refuseConflict, word)
            // A settled instance of that ID: boot, and the two-disks refusal.
            #expect(decide(settled, same) == .boot, word)
            #expect(decide(settled, otherDisk) == .refuseConflict, word)
            #expect(decide(settled, same, flags: InstanceFlags(retry: true)) == .refuseConflict, word)
            #expect(decide(settled, data: false, same) == .create, word)
        }
    }

    @Test func aSharedIDWithTwoMarkersChangesNothingOnTheFiles() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let first = release("rel-a", build: 100, sha: otherSHA)
        let second = release(sharedID, build: 200, sha: sha)
        try files.apply(try files.decide(for: first), release: first)
        try Data("android data".utf8).write(to: files.data)
        try files.complete(release: first)
        try files.apply(try files.decide(for: second), release: second)
        let snapshot = files.snapshot(for: sharedID)
        #expect(FileManager.default.fileExists(atPath: snapshot.path))
        let record = try Data(contentsOf: files.state)

        // The unfinished signed update, a proof of the same ID, and every way a start goes on.
        let proof = release(sharedID, build: 200, sha: otherSHA, proof: true)
        for flags in [InstanceFlags(), InstanceFlags(retry: true), InstanceFlags(restoreSnapshot: true)] {
            let refused = try files.decide(for: proof, flags: flags)
            #expect(refused.action == .refuseConflict)
            #expect(throws: InstanceError.self) { try files.apply(refused, release: proof) }
        }
        #expect(files.startPlan(for: proof) == .blocked(.mismatch))
        #expect(try Data(contentsOf: files.state) == record)
        #expect(try Data(contentsOf: files.data) == Data("android data".utf8))
        #expect(try Data(contentsOf: snapshot) == Data("android data".utf8))
        // The signed release keeps Try Again and the restore of its own update.
        #expect(try files.decide(for: second, flags: InstanceFlags(retry: true)).action == .retry)
        #expect(try files.decide(for: second, flags: InstanceFlags(restoreSnapshot: true)).action == .restore)
    }

    @Test func aRefusedProofChangesNothingAndEraseStartsItFresh() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let first = release("rel-a", build: 100, sha: otherSHA)
        try files.apply(try files.decide(for: first), release: first)
        try Data("android data".utf8).write(to: files.data)

        let proof = release(Self.proof, build: 300, sha: sha, proof: true)
        let refused = try files.decide(for: proof)
        #expect(refused.action == .refuseConflict)
        let before = try Data(contentsOf: files.state)
        #expect(throws: InstanceError.self) { try files.apply(refused, release: proof) }
        #expect(try Data(contentsOf: files.state) == before)
        #expect(try Data(contentsOf: files.data) == Data("android data".utf8))
        #expect(files.startPlan(for: proof) == .blocked(.mismatch))

        // Erase, then the proof starts a fresh instance, and starts it again.
        try files.removeAll()
        try files.apply(try files.decide(for: proof), release: proof)
        try Data("proof data".utf8).write(to: files.data)
        #expect(try files.readState()?.releaseID == Self.proof)
        #expect(try files.decide(for: proof).action == .boot)
        // And a real release is refused by it, the data kept.
        let next = release("rel-c", build: 400, sha: otherSHA)
        #expect(try files.decide(for: next).action == .refuseConflict)
        #expect(try Data(contentsOf: files.data) == Data("proof data".utf8))
    }

    @Test func aReleaseIdentityKnowsAProofFromItsManifestsAcceptanceAlone() throws {
        let layout = #"{"system_disk": {"sha256": "\#(String(repeating: "c", count: 64))"}, "data_disk": {"layout_version": 1, "capacity_gib": {"minimum": 4, "default": 8, "maximum": 64}}}"#
        func identity(_ id: String, acceptance: String?) throws -> ReleaseIdentity {
            let field = acceptance.map { #""acceptance": "\#($0)", "# } ?? ""
            let manifest = #"{\#(field)"release_id": "\#(id)", "android": {"fingerprint": "f", "build_number": 29, "security_patch": "2026-10-01"}}"#
            return try ReleaseIdentity(manifest: Data(manifest.utf8), layout: Data(layout.utf8))
        }
        #expect(try identity(Self.proof, acceptance: "unsigned-proof").isUnsignedProof)
        #expect(try identity("20261102-los24-r5", acceptance: "unsigned-proof").isUnsignedProof)
        #expect(try !identity("20261102-los24-r5", acceptance: "accepted").isUnsignedProof)
        // A manifest of an earlier schema, or a release with no word on it, is no proof.
        #expect(try !identity("20261102-los24-r5", acceptance: nil).isUnsignedProof)
        // The release ID decides nothing: a signed release whose ID starts like a proof's stays usable.
        #expect(try !identity(Self.proof, acceptance: "accepted").isUnsignedProof)
        #expect(try !identity(Self.proof, acceptance: nil).isUnsignedProof)
    }

    @Test func aSignedReleaseNamedLikeAProofDecidesAndRecordsAsASignedOne() throws {
        let named = release("unsigned-proof-demo", build: 100, sha: otherSHA)
        #expect(!named.isUnsignedProof)
        let record = InstanceState(creating: named, at: "2026-10-01T00:00:00Z")
        #expect(!record.isUnsignedProof)
        #expect(decide(record, release("rel-b", build: 200, sha: sha)) == .update)
        #expect(decide(state("rel-a", build: 100, sha: sha), release("unsigned-proof-demo", build: 200, sha: otherSHA)) == .update)
        let object = try #require(
            try JSONSerialization.jsonObject(with: JSONEncoder().encode(record)) as? [String: Any])
        #expect(object["schema"] as? Int == 1 && Set(object.keys) == InstanceState.keys)
    }

    // The record of an instance a proof created: schema 2 with `unsigned_proof`.

    private func encoded(_ state: InstanceState) throws -> [String: Any] {
        try #require(try JSONSerialization.jsonObject(with: JSONEncoder().encode(state)) as? [String: Any])
    }

    @Test func aProofsRecordSaysSoInASchemaPreviousVersionsRefuse() throws {
        let record = InstanceState(creating: release(Self.proof, build: 300, sha: sha, proof: true), at: "2026-10-01T00:00:00Z")
        #expect(record.isUnsignedProof)
        let object = try encoded(record)
        #expect(object["schema"] as? Int == 2 && object["unsigned_proof"] as? Bool == true)
        #expect(Set(object.keys) == InstanceState.keys.union(["unsigned_proof"]))
        // It reads back as a proof, with everything else it holds.
        let again = try JSONDecoder().decode(InstanceState.self, from: JSONEncoder().encode(record))
        #expect(again == record && again.isUnsignedProof)
        // A schema 2 record without the word, with it false, or another schema is no record.
        for bad in [#"{"schema": 2}"#, #"{"schema": 2, "unsigned_proof": false}"#, #"{"schema": 3, "unsigned_proof": true}"#] {
            var broken = object
            let patch = try #require(try JSONSerialization.jsonObject(with: Data(bad.utf8)) as? [String: Any])
            for (key, value) in patch { broken[key] = value }
            if bad == #"{"schema": 2}"# { broken["unsigned_proof"] = nil }
            #expect(throws: InstanceError.self, "\(bad)") {
                try JSONDecoder().decode(InstanceState.self, from: JSONSerialization.data(withJSONObject: broken))
            }
        }
        // A signed record is as before: schema 1, the same keys, no word.
        let signed = try encoded(InstanceState(creating: release("rel-a", build: 100, sha: otherSHA), at: "2026-10-01T00:00:00Z"))
        #expect(signed["schema"] as? Int == 1 && Set(signed.keys) == InstanceState.keys)
    }

    /// The decoder and encoder of `InstanceState` as b91f2b62 has them, the app
    /// version before the proof rule: it knows schema 1 only.
    private struct PreviousInstanceState: Codable {
        var state: InstanceState

        private enum CodingKeys: String, CodingKey {
            case schema
            case dataLayout = "data_layout"
            case releaseID = "release_id"
            case buildFingerprint = "build_fingerprint"
            case buildIncremental = "build_incremental"
            case systemDiskSHA256 = "system_disk_sha256"
            case createdWith = "created_with"
            case lastBooted = "last_booted"
            case lastCompleted = "last_completed"
            case pendingUpdate = "pending_update"
        }

        init(from decoder: Decoder) throws {
            let values = try decoder.container(keyedBy: CodingKeys.self)
            guard try values.decode(Int.self, forKey: .schema) == 1 else {
                throw InstanceError.state("This virtual machine's instance.json is of a schema Andarium does not know.")
            }
            let build = try values.decode(String.self, forKey: .buildIncremental)
            guard build.range(of: "^[1-9][0-9]{0,9}$", options: .regularExpression) != nil else {
                throw InstanceError.state("This virtual machine's instance.json has an invalid build number.")
            }
            state = InstanceState(
                dataLayout: try values.decode(Int.self, forKey: .dataLayout),
                releaseID: try values.decode(String.self, forKey: .releaseID),
                buildFingerprint: try values.decode(String.self, forKey: .buildFingerprint),
                buildIncremental: build,
                systemDiskSHA256: try values.decode(String.self, forKey: .systemDiskSHA256),
                createdWith: try values.decode(String.self, forKey: .createdWith),
                lastBooted: try values.decode(String.self, forKey: .lastBooted),
                lastCompleted: try values.decodeIfPresent(InstanceState.Completed.self, forKey: .lastCompleted),
                pendingUpdate: try values.decodeIfPresent(InstanceState.Pending.self, forKey: .pendingUpdate)
            )
        }

        func encode(to encoder: Encoder) throws {
            var values = encoder.container(keyedBy: CodingKeys.self)
            try values.encode(1, forKey: .schema)
            try values.encode(state.dataLayout, forKey: .dataLayout)
            try values.encode(state.releaseID, forKey: .releaseID)
            try values.encode(state.buildFingerprint, forKey: .buildFingerprint)
            try values.encode(state.buildIncremental, forKey: .buildIncremental)
            try values.encode(state.systemDiskSHA256, forKey: .systemDiskSHA256)
            try values.encode(state.createdWith, forKey: .createdWith)
            try values.encode(state.lastBooted, forKey: .lastBooted)
            if let completed = state.lastCompleted { try values.encode(completed, forKey: .lastCompleted) } else { try values.encodeNil(forKey: .lastCompleted) }
            if let pending = state.pendingUpdate { try values.encode(pending, forKey: .pendingUpdate) } else { try values.encodeNil(forKey: .pendingUpdate) }
        }
    }

    @Test func theVersionBeforeCannotReadAProofsRecordAndSignedRecordsPassBothWays() throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        // A proof's record: the previous version refuses it, so it cannot update that instance.
        let proofRecord = InstanceState(creating: release(Self.proof, build: 1, sha: sha, proof: true), at: "2026-10-01T00:00:00Z")
        #expect(throws: InstanceError.self) {
            try JSONDecoder().decode(PreviousInstanceState.self, from: encoder.encode(proofRecord))
        }
        // A signed record: both versions read the other's bytes, and write the same bytes.
        var signed = InstanceState(creating: release("rel-a", build: 100, sha: otherSHA), at: "2026-10-01T00:00:00Z")
        signed.lastCompleted = InstanceState.Completed(releaseID: "rel-a", buildIncremental: "100", at: "2026-10-02T00:00:00Z")
        let bytes = try encoder.encode(signed)
        let previous = try JSONDecoder().decode(PreviousInstanceState.self, from: bytes)
        #expect(previous.state == signed)
        #expect(try encoder.encode(previous) == bytes)
        #expect(try JSONDecoder().decode(InstanceState.self, from: encoder.encode(previous)) == signed)
    }

    @Test func anInstanceAProofCreatesRecordsItAndKeepsTheWordThroughItsLife() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let proof = release(Self.proof, build: 300, sha: sha, proof: true)
        try files.apply(try files.decide(for: proof), release: proof)
        try Data("proof data".utf8).write(to: files.data)
        #expect(try files.readState()?.isUnsignedProof == true)
        let object = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: files.state)) as? [String: Any])
        #expect(object["schema"] as? Int == 2 && object["unsigned_proof"] as? Bool == true)
        // A start, the first boot and a clean end rewrite the record and keep it.
        try files.apply(try files.decide(for: proof), release: proof)
        try files.booted(release: proof)
        try files.complete(release: proof)
        #expect(try files.readState()?.isUnsignedProof == true)
        // A signed release over it is refused by the word, not by the ID: the ID of the proof says nothing.
        let renamed = release("rel-c", build: 400, sha: otherSHA)
        #expect(try files.decide(for: renamed).action == .refuseConflict)
    }
}

/// Whether a guest's first Android boot has been seen to complete, which the
/// slow-start notice reads to give a first boot its longer time: a persisted
/// fact, since the plan from the files says only that the data disk exists.
struct FirstBootMarkerTests {
    @Test func aGuestsFirstBootIsPendingUntilAndroidBootedOnce() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root)
        let first = release()
        #expect(!files.firstBootPending)
        try files.apply(files.decide(for: first), release: first)
        #expect(files.firstBootPending)
        // The runtime made the data disk and the process ended before the
        // kernel ran: the files say an ordinary start, and the boot is still
        // the first.
        try Data("data".utf8).write(to: files.data)
        let again = try files.decide(for: first)
        #expect(again.action == .boot && files.startPlan(for: first) == .normal)
        try files.apply(again, release: first)
        #expect(files.firstBootPending)
        // Android booted.
        try files.booted(release: first)
        #expect(!files.firstBootPending)
        let next = try files.decide(for: first)
        try files.apply(next, release: first)
        #expect(!files.firstBootPending)
    }

    @Test func aCleanEndOfTheFirstRunSaysItBootedToo() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root)
        let first = release()
        try files.apply(files.decide(for: first), release: first)
        try Data("data".utf8).write(to: files.data)
        try files.complete(release: first)
        #expect(!files.firstBootPending)
    }

    @Test func eraseAndResetGiveTheNextGuestItsOwnFirstBoot() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root)
        let first = release()
        try files.apply(files.decide(for: first), release: first)
        try Data("data".utf8).write(to: files.data)
        try files.booted(release: first)
        #expect(!files.firstBootPending)
        // A reset makes a new instance: its first boot is pending.
        let reset = try files.decide(for: first, flags: InstanceFlags(reset: true))
        try files.apply(reset, release: first)
        #expect(files.firstBootPending)
        // Erasing takes the marker with the rest.
        try files.removeAll()
        #expect(!files.firstBootPending)
    }

    @Test func aGuestAnEarlierVersionMadeIsPastItsFirstBoot() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root)
        let first = release()
        try files.apply(files.decide(for: first), release: first)
        try FileManager.default.removeItem(at: files.firstBoot)
        try Data("data".utf8).write(to: files.data)
        #expect(!files.firstBootPending)
        try files.apply(files.decide(for: first), release: first)
        #expect(!files.firstBootPending)
    }
}
