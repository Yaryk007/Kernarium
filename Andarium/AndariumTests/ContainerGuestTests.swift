// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Darwin
import Foundation
import Testing
@testable import Andarium

/// Install APK of a file that holds an app in parts, against a real Android
/// guest. The suite is opt-in. It reaches the guest's adbd over a loopback TCP
/// port (a launcher on the Mac bridges the port to the guest's adbd), and the
/// APKs it installs are synthesized ones that a harness outside the repository
/// writes into a folder:
///
///     TEST_RUNNER_ANDARIUM_CONTAINER_GUEST_PORT=<port of the guest's adbd>
///     TEST_RUNNER_ANDARIUM_CONTAINER_GUEST_FIXTURES=<folder with the APKs>
///
/// The folder holds `base.apk`, `config.xhdpi.apk`, `config.arm64_v8a.apk`,
/// `config.armeabi_v7a.apk`, `config.x86_64.apk` (one app, one signature),
/// `old-base.apk` and `old-config.xhdpi.apk` (the same package built for API
/// level 21) and `foreign-config.fr.apk` (a split of that package signed by
/// another key), and for the checks of what a review found, `review-own.apk`
/// (a signed APK of org.example.reviewown), `review-bad.apk` (an APK of
/// org.example.reviewbad whose manifest makes the package manager's refusal
/// hold a line that says `Success`) and `review-nested.apk` (a signed APK of
/// org.example.reviewvictim that holds, as an asset, an APK of reviewown).
/// Every test leaves the guest as it found it, except the one
/// that takes real files (`ANDARIUM_CONTAINER_GUEST_SAMPLES`, below).
@Suite(.serialized)
struct ContainerGuestTests {
    private static let environment = ProcessInfo.processInfo.environment
    private static let port = environment["ANDARIUM_CONTAINER_GUEST_PORT"].flatMap { UInt16($0) }
    private static let fixtures = environment["ANDARIUM_CONTAINER_GUEST_FIXTURES"]
    private static let enabled = port != nil && fixtures != nil
    private static let package = "org.example.andarium.splitprobe"
    private static let obbFolder = "/storage/emulated/0/Android/obb/" + package

    private func apk(_ name: String) throws -> Data {
        try Data(contentsOf: URL(fileURLWithPath: Self.fixtures!).appendingPathComponent(name))
    }

    private func operations() throws -> AdbGuestOperations {
        AdbGuestOperations(
            identity: try throwawayIdentity(), clock: GuestRunClock(), device: "Mac",
            connector: LoopbackAdbConnector(port: Self.port!), hostFree: { .max }, run: GuestRunMark.random())
    }

    private func shell(_ operations: AdbGuestOperations, _ command: String) async throws -> String {
        try await operations.shell(command, limit: .seconds(60)).text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// The installed files of the app, by name, in `pm path` order; empty when it is not installed.
    private func installedFiles(_ operations: AdbGuestOperations) async throws -> [String] {
        try await shell(operations, "pm path \(Self.package)").split(whereSeparator: \.isNewline).map {
            String($0.split(separator: "/").last ?? "")
        }
    }

    /// The free space of the data partition, in KiB.
    private func freeKiB(_ operations: AdbGuestOperations) async throws -> Int {
        Int(try await shell(operations, "df -Pk /data | tail -n 1 | tr -s ' ' | cut -d' ' -f4")) ?? -1
    }

    private func activeSessions(_ operations: AdbGuestOperations) async throws -> Int {
        Int(try await shell(operations, "dumpsys package | grep -c 'Active Session'")) ?? -1
    }

    private func clean(_ operations: AdbGuestOperations) async {
        _ = try? await shell(operations, "pm uninstall \(Self.package) >/dev/null 2>&1; rm -rf '\(Self.obbFolder)'; true")
    }

    private func write(_ entries: [ContainerEntry], _ name: String) throws -> URL {
        try ContainerFixture.write(entries, name: name)
    }

    private func digest(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    // MARK: A plain APK and every kind of container

    @Test(.timeLimit(.minutes(10))) func aPlainApkInstallsAsItAlwaysDid() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-Probe.apk")
        try apk("base.apk").write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await operations.installApk(at: url, update: { _ in }) == .installed)
        #expect(try await installedFiles(operations) == ["base.apk"])
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(20))) func everyKindOfContainerInstallsTheSplitsThatFitTheGuest() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        let abis = try await shell(operations, "getprop ro.product.cpu.abilist")
        let first = abis.split(separator: ",").first.map(String.init) ?? ""
        let known = ["arm64-v8a": "arm64_v8a", "armeabi-v7a": "armeabi_v7a", "x86_64": "x86_64"]
        let config = try #require(known[first], "unexpected processor list \(abis)")
        let wrong = known.values.filter { $0 != config }
        let base = try apk("base.apk"), density = try apk("config.xhdpi.apk")
        let arm64 = try apk("config.arm64_v8a.apk"), arm32 = try apk("config.armeabi_v7a.apk"), x86 = try apk("config.x86_64.apk")
        let kinds: [(String, [ContainerEntry])] = [
            ("bundletool.apks", [
                ContainerEntry("toc.pb", BundleToc(variants: [BundleToc.Variant(apks: [
                    .master("splits/base-master.apk"), .split("splits/base-xhdpi.apk", "config.xhdpi"),
                    .split("splits/base-arm64_v8a.apk", "config.arm64_v8a", abi: 3),
                    .split("splits/base-armeabi_v7a.apk", "config.armeabi_v7a", abi: 2),
                    .split("splits/base-x86_64.apk", "config.x86_64", abi: 5),
                ])]).build()),
                ContainerEntry("splits/base-master.apk", base),
                ContainerEntry("splits/base-xhdpi.apk", density), ContainerEntry("splits/base-arm64_v8a.apk", arm64),
                ContainerEntry("splits/base-armeabi_v7a.apk", arm32), ContainerEntry("splits/base-x86_64.apk", x86),
            ]),
            ("installer.apks", [
                ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", base),
                ContainerEntry("split_config.xhdpi.apk", density), ContainerEntry("split_config.arm64_v8a.apk", arm64),
                ContainerEntry("split_config.armeabi_v7a.apk", arm32), ContainerEntry("split_config.x86_64.apk", x86),
            ]),
            ("game.xapk", [
                ContainerEntry("icon.png", Data([9])),
                ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"\#(Self.package)","split_apks":[{"file":"base.apk","id":"base"},{"file":"config.xhdpi.apk","id":"config.xhdpi"},{"file":"config.arm64_v8a.apk","id":"config.arm64_v8a"},{"file":"config.armeabi_v7a.apk","id":"config.armeabi_v7a"},{"file":"config.x86_64.apk","id":"config.x86_64"}]}"#.utf8)),
                ContainerEntry("base.apk", base), ContainerEntry("config.xhdpi.apk", density),
                ContainerEntry("config.arm64_v8a.apk", arm64), ContainerEntry("config.armeabi_v7a.apk", arm32),
                ContainerEntry("config.x86_64.apk", x86),
            ]),
            ("viewer.apkm", [
                ContainerEntry("info.json", Data("{}".utf8)), ContainerEntry("base.apk", base),
                ContainerEntry("split_config.xhdpi.apk", density), ContainerEntry("split_config.arm64_v8a.apk", arm64),
                ContainerEntry("split_config.armeabi_v7a.apk", arm32), ContainerEntry("split_config.x86_64.apk", x86),
            ]),
        ]
        for (name, entries) in kinds {
            await clean(operations)
            let url = try write(entries, name)
            defer { try? FileManager.default.removeItem(at: url) }
            let log = UpdateLog()
            let outcome = try await operations.installApk(at: url, update: { log.add($0) })
            #expect(outcome == .installed, "\(name)")
            // The base, the density split and the split of the guest's own processor went in together.
            let files = try await installedFiles(operations)
            #expect(files.count == 3 && files.contains("base.apk"), "\(name): \(files)")
            #expect(files.contains { $0.contains("xhdpi") } && files.contains { $0.contains(config) }, "\(name): \(files)")
            for other in wrong { #expect(!files.contains { $0.contains(other) }, "\(name): \(files)") }
            #expect(try await activeSessions(operations) == 0, "\(name)")
            #expect(log.details.last == "The virtual machine is installing the app\u{2026}", "\(name)")
        }
        await clean(operations)
    }

    // MARK: Failures

    @Test(.timeLimit(.minutes(10))) func aSplitSignedByAnotherKeyIsTheGuestsReasonAndLeavesNoSession() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let url = try write([
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("base.apk")),
            ContainerEntry("split_config.fr.apk", try apk("foreign-config.fr.apk")),
        ], "foreign.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        do {
            _ = try await operations.installApk(at: url, update: { _ in })
            Issue.record("installed")
        } catch let failure as AdbFailure {
            #expect(!failure.message.isEmpty)
            print("CONTAINER_GUEST foreign-signature message=\(failure.message)")
        }
        #expect(try await installedFiles(operations).isEmpty)
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(10))) func aDamagedPartAbandonsTheSession() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        var density = ContainerEntry("split_config.xhdpi.apk", try apk("config.xhdpi.apk"))
        density.crc = 0x1357_9BDF
        let url = try write([
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("base.apk")), density,
        ], "damaged.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        do {
            _ = try await operations.installApk(at: url, update: { _ in })
            Issue.record("installed")
        } catch let failure as AdbFailure {
            #expect(failure.message == InstallRefusal.damaged.message)
        }
        #expect(try await installedFiles(operations).isEmpty)
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(10))) func aCancelPartWayThroughAWriteLeavesNoSessionAndNoStagedData() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        // A split of 400 MiB (a hole in the file) after the base: the cancel lands inside its write.
        let url = try write([
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("base.apk")),
            ContainerEntry("split_config.xhdpi.apk", zeros: 400 << 20),
        ], "big.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let freeBefore = try await freeKiB(operations)
        let box = TaskBox()
        let task = Task {
            try await operations.installApk(at: url, update: { update in
                if let progress = update.progress, progress > 0.25 { box.cancel() }
            })
        }
        box.task = task
        let result = await task.result
        guard case let .failure(error) = result, error is CancellationError else {
            Issue.record("expected the cancel, got \(result)")
            return
        }
        #expect(try await installedFiles(operations).isEmpty)
        #expect(try await activeSessions(operations) == 0)
        // Nothing of the session's staging is left on the data partition: the
        // 400 MiB the cancelled write had staged is free again (within the
        // noise of the system's own writes).
        var freeAfter = try await freeKiB(operations)
        for _ in 0..<30 where freeAfter + 8192 < freeBefore {
            try await Task.sleep(for: .milliseconds(500))
            freeAfter = try await freeKiB(operations)
        }
        print("CONTAINER_GUEST cancel free-before-kib=\(freeBefore) free-after-kib=\(freeAfter)")
        #expect(freeAfter + 8192 >= freeBefore)
        await clean(operations)
    }

    // MARK: Apps built for an older Android

    @Test(.timeLimit(.minutes(10))) func anOlderAppIsInstalledThroughANewSessionAfterTheQuestion() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let url = try write([
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("old-base.apk")),
            ContainerEntry("split_config.xhdpi.apk", try apk("old-config.xhdpi.apk")),
        ], "old.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        do {
            _ = try await operations.installApk(at: url, update: { _ in })
            Issue.record("installed without the question")
        } catch let refused as LegacyApkRefused {
            #expect(refused.target == LegacyTarget(appLevel: 21, requiredLevel: 24))
        }
        #expect(try await installedFiles(operations).isEmpty)
        #expect(try await activeSessions(operations) == 0)
        #expect(try await operations.installApk(at: url, bypassLowTargetSdkBlock: true, update: { _ in }) == .installed)
        #expect(try await installedFiles(operations).count == 2)
        #expect(try await shell(operations, "dumpsys package \(Self.package) | grep -m1 -o 'targetSdk=[0-9]*'") == "targetSdk=21")
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    // MARK: Data

    @Test(.timeLimit(.minutes(15))) func anXapkPlacesItsObbFileAndTheAppReadsNoPartialOne() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let obb = incompressible(5 << 20)
        let name = "main.1.\(Self.package).obb"
        let path = "Android/obb/\(Self.package)/\(name)"
        let url = try write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"\#(Self.package)","split_apks":[{"file":"base.apk","id":"base"},{"file":"config.xhdpi.apk","id":"config.xhdpi"}],"expansions":[{"file":"\#(path)","install_location":"EXTERNAL_STORAGE","install_path":"\#(path)"}]}"#.utf8)),
            ContainerEntry("base.apk", try apk("base.apk")), ContainerEntry("config.xhdpi.apk", try apk("config.xhdpi.apk")),
            ContainerEntry(path, obb),
        ], "game.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        let partials = try Self.settledLedger()
        #expect(try await operations.installApk(at: url, partials: partials.ledger, update: { _ in }) == .installed)
        defer { try? FileManager.default.removeItem(at: partials.directory) }
        #expect(try await installedFiles(operations).count == 2)
        let listing = try await shell(operations, "ls -A '\(Self.obbFolder)'")
        #expect(listing == name, "\(listing)")
        #expect(try await shell(operations, "sha256sum '\(Self.obbFolder)/\(name)'").hasPrefix(digest(obb)))
        #expect(try await shell(operations, "stat -c %s '\(Self.obbFolder)/\(name)'") == String(obb.count))
        #expect(!partials.ledger.isOwed)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(15))) func aCancelWhileTheDataIsCopiedLeavesTheAppInstalledAndNoPartialFile() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let name = "main.1.\(Self.package).obb"
        let path = "Android/obb/\(Self.package)/\(name)"
        let url = try write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"\#(Self.package)","split_apks":[{"file":"base.apk","id":"base"}],"expansions":[{"file":"\#(path)","install_location":"EXTERNAL_STORAGE","install_path":"\#(path)"}]}"#.utf8)),
            ContainerEntry("base.apk", try apk("base.apk")), ContainerEntry(path, zeros: 600 << 20),
        ], "bigdata.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        let partials = try Self.settledLedger()
        defer { try? FileManager.default.removeItem(at: partials.directory) }
        let box = TaskBox()
        let copying = Flag()
        let task = Task {
            try await operations.installApk(at: url, partials: partials.ledger, update: { update in
                if update.cancellable == true { copying.raise() }
                if copying.isRaised, let progress = update.progress, progress > 0.3 { box.cancel() }
            })
        }
        box.task = task
        let result = await task.result
        guard case let .success(outcome) = result else {
            Issue.record("the cancel of the copy ends in the installed app, got \(result)")
            return
        }
        #expect(outcome == .installedWithoutData)
        #expect(try await installedFiles(operations) == ["base.apk"])
        let listing = try await shell(operations, "ls -A '\(Self.obbFolder)' 2>/dev/null")
        print("CONTAINER_GUEST cancel-data folder-listing=[\(listing)]")
        #expect(!listing.contains(".andarium-") && !listing.contains(name))
        #expect(!partials.ledger.isOwed)
        await clean(operations)
    }

    // MARK: Found by the review of the first round, and the variants of a bundle

    private static let own = "org.example.reviewown"
    private static let victim = "org.example.reviewvictim"
    private static let badPackage = "org.example.reviewbad"
    private static let victimFolder = "/storage/emulated/0/Android/obb/" + victim

    private func installFails(_ operations: AdbGuestOperations, _ url: URL, _ what: String) async throws {
        do {
            let outcome = try await operations.installApk(at: url, update: { _ in })
            Issue.record("\(what): installed (\(outcome))")
        } catch {
            #expect(error is AdbFailure, "\(what): \(error)")
        }
    }

    @Test(.timeLimit(.minutes(10))) func anXapkCannotChooseAnotherAppsObbFolder() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        let path = "Android/obb/\(Self.victim)/main.1.\(Self.victim).obb"
        _ = try await shell(operations, "pm uninstall \(Self.own) >/dev/null 2>&1; mkdir -p '\(Self.victimFolder)' && printf original > '\(Self.victimFolder)/main.1.\(Self.victim).obb'")
        // The file says it is the victim's, and carries another app's APK.
        let url = try write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"\#(Self.victim)","split_apks":[{"file":"base.apk","id":"base"}],"expansions":[{"file":"\#(path)","install_location":"EXTERNAL_STORAGE","install_path":"\#(path)"}]}"#.utf8)),
            ContainerEntry("base.apk", try apk("review-own.apk")), ContainerEntry(path, Data("replaced".utf8)),
        ], "claim.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        try await installFails(operations, url, "an APK that is not the package the file names")
        #expect(try await shell(operations, "cat '\(Self.victimFolder)/main.1.\(Self.victim).obb'") == "original")
        #expect(try await shell(operations, "pm path \(Self.own)").isEmpty)
        #expect(try await activeSessions(operations) == 0)
        _ = try await shell(operations, "rm -rf '\(Self.victimFolder)'; true")
    }

    @Test(.timeLimit(.minutes(10))) func anApkmCannotChooseAnotherAppsObbFolderEither() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        _ = try await shell(operations, "mkdir -p '\(Self.victimFolder)' && printf original > '\(Self.victimFolder)/main.1.\(Self.victim).obb'")
        let url = try write([
            ContainerEntry("info.json", Data(#"{"apkm_version":5,"pname":"\#(Self.victim)","obb_files":["main.2.\#(Self.victim).obb"]}"#.utf8)),
            ContainerEntry("base.apk", try apk("base.apk")), ContainerEntry("main.2.\(Self.victim).obb", Data("replaced".utf8)),
        ], "claim.apkm")
        defer { try? FileManager.default.removeItem(at: url) }
        try await installFails(operations, url, "an APK that is not the package the file names")
        #expect(try await shell(operations, "ls -A '\(Self.victimFolder)'") == "main.1.\(Self.victim).obb")
        #expect(try await installedFiles(operations).isEmpty)
        _ = try await shell(operations, "rm -rf '\(Self.victimFolder)'; true")
    }

    @Test(.timeLimit(.minutes(10))) func aRefusalWhoseDiagnosticSaysSuccessInstallsNothingAndCopiesNothing() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        let folder = "/storage/emulated/0/Android/obb/" + Self.badPackage
        let path = "Android/obb/\(Self.badPackage)/failure.obb"
        _ = try await shell(operations, "rm -rf '\(folder)'; true")
        let url = try write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"\#(Self.badPackage)","split_apks":[{"file":"base.apk","id":"base"}],"expansions":[{"file":"\#(path)","install_location":"EXTERNAL_STORAGE","install_path":"\#(path)"}]}"#.utf8)),
            ContainerEntry("base.apk", try apk("review-bad.apk")), ContainerEntry(path, Data("copied despite refusal".utf8)),
        ], "refused.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        try await installFails(operations, url, "a refused APK")
        #expect(try await shell(operations, "pm path \(Self.badPackage)").isEmpty)
        #expect(try await shell(operations, "ls -A '\(folder)' 2>/dev/null").isEmpty)
        #expect(try await activeSessions(operations) == 0)
        _ = try await shell(operations, "rm -rf '\(folder)'; true")
    }

    @Test(.timeLimit(.minutes(10))) func aCancelBetweenTheLastWriteAndTheCommitLeavesNoSession() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        _ = try await shell(operations, "pm uninstall \(Self.own) >/dev/null 2>&1; true")
        let before = try await activeSessions(operations)
        let url = try write([ContainerEntry("base.apk", try apk("review-own.apk"))], "cancel.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let box = TaskBox()
        let task = Task {
            try await operations.installApk(at: url, update: { update in
                // The Cancel lands as the install announces the commit it can no longer stop.
                if update.cancellable == false { box.cancel() }
            })
        }
        box.task = task
        let result = await task.result
        guard case let .failure(error) = result, error is CancellationError else {
            Issue.record("expected the cancel, got \(result)")
            return
        }
        #expect(try await activeSessions(operations) == before)
        #expect(try await shell(operations, "pm path \(Self.own)").isEmpty)
    }

    @Test(.timeLimit(.minutes(10))) func anApkThatHoldsAnApkAsAnAssetInstallsAsItself() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        _ = try await shell(operations, "pm uninstall \(Self.own) >/dev/null 2>&1; pm uninstall \(Self.victim) >/dev/null 2>&1; true")
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-renamed.apks")
        try apk("review-nested.apk").write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await operations.installApk(at: url, update: { _ in }) == .installed)
        #expect(try await shell(operations, "pm path \(Self.victim)").hasPrefix("package:"))
        #expect(try await shell(operations, "pm path \(Self.own)").isEmpty)
        _ = try await shell(operations, "pm uninstall \(Self.victim) >/dev/null 2>&1; true")
    }

    @Test(.timeLimit(.minutes(15))) func anApkmPlacesTheDataItsInfoListsForTheInstalledPackage() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let obb = incompressible(3 << 20)
        let name = "main.1.\(Self.package).obb"
        let url = try write([
            ContainerEntry("info.json", Data(#"{"apkm_version":5,"pname":"\#(Self.package)","obb_files":["\#(name)"]}"#.utf8)),
            ContainerEntry("base.apk", try apk("base.apk")), ContainerEntry("split_config.xhdpi.apk", try apk("config.xhdpi.apk")),
            ContainerEntry(name, obb),
        ], "data.apkm")
        defer { try? FileManager.default.removeItem(at: url) }
        let partials = try Self.settledLedger()
        defer { try? FileManager.default.removeItem(at: partials.directory) }
        #expect(try await operations.installApk(at: url, partials: partials.ledger, update: { _ in }) == .installed)
        #expect(try await installedFiles(operations).count == 2)
        #expect(try await shell(operations, "ls -A '\(Self.obbFolder)'") == name)
        #expect(try await shell(operations, "sha256sum '\(Self.obbFolder)/\(name)'").hasPrefix(digest(obb)))
        #expect(try await shell(operations, "stat -c %s '\(Self.obbFolder)/\(name)'") == String(obb.count))
        #expect(!partials.ledger.isOwed)
        await clean(operations)
    }

    /// One bundle of three variants (API 21, 23 and 31), each with a master split of its own and the
    /// processor splits shared by the two newer ones, as a real bundletool file is.
    private func variantBundle() throws -> [ContainerEntry] {
        typealias Apk = BundleToc.Apk
        func abis(_ suffix: String) -> [Apk] {
            [
                .split("splits/base-arm64_v8a\(suffix).apk", "config.arm64_v8a", abi: 3),
                .split("splits/base-armeabi_v7a\(suffix).apk", "config.armeabi_v7a", abi: 2),
                .split("splits/base-x86_64\(suffix).apk", "config.x86_64", abi: 5),
            ]
        }
        let density: Apk = .split("splits/base-xhdpi.apk", "config.xhdpi")
        let toc = BundleToc(variants: [
            BundleToc.Variant(minSdk: 21, apks: [.master("splits/base-master.apk")] + abis("") + [density]),
            BundleToc.Variant(minSdk: 23, apks: [.master("splits/base-master_2.apk")] + abis("_2") + [density]),
            BundleToc.Variant(minSdk: 31, apks: [.master("splits/base-master_3.apk")] + abis("_2") + [density]),
        ])
        let base = try apk("base.apk"), arm64 = try apk("config.arm64_v8a.apk"), arm32 = try apk("config.armeabi_v7a.apk")
        let x86 = try apk("config.x86_64.apk"), density2 = try apk("config.xhdpi.apk")
        return [
            ContainerEntry("toc.pb", toc.build()),
            ContainerEntry("splits/base-master.apk", base), ContainerEntry("splits/base-master_2.apk", base), ContainerEntry("splits/base-master_3.apk", base),
            ContainerEntry("splits/base-arm64_v8a.apk", arm64), ContainerEntry("splits/base-arm64_v8a_2.apk", arm64),
            ContainerEntry("splits/base-armeabi_v7a.apk", arm32), ContainerEntry("splits/base-armeabi_v7a_2.apk", arm32),
            ContainerEntry("splits/base-x86_64.apk", x86), ContainerEntry("splits/base-x86_64_2.apk", x86),
            ContainerEntry("splits/base-xhdpi.apk", density2),
        ]
    }

    @Test(.timeLimit(.minutes(10))) func aBundleOfSeveralVariantsInstallsOneOfThem() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let url = try write(try variantBundle(), "variants.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await operations.installApk(at: url, update: { _ in }) == .installed)
        // The base, the density split and the split of the guest's processor, once each.
        let files = try await installedFiles(operations)
        #expect(files.count == 3 && files.contains("base.apk"), "\(files)")
        #expect(files.contains { $0.contains("xhdpi") } && files.contains { $0.contains("arm64") }, "\(files)")
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(10))) func aUniversalBundleInstallsAsOneApk() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let toc = BundleToc(variants: [BundleToc.Variant(minSdk: nil, apks: [.standalone("universal.apk")])])
        let url = try write([ContainerEntry("toc.pb", toc.build()), ContainerEntry("universal.apk", try apk("base.apk"))], "universal.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await operations.installApk(at: url, update: { _ in }) == .installed)
        #expect(try await installedFiles(operations) == ["base.apk"])
        await clean(operations)
    }

    /// The pack of a bundle: this test's fixtures stand in for the asset slices (split APKs of the same
    /// app that the package manager takes as splits), and the table, written from bundletool's
    /// published `BuildApksResult`, says which are install-time and which come later.
    @Test(.timeLimit(.minutes(10))) func anInstallTimePacksApkLandsInTheSessionAndALaterOnesDoesNot() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let toc = BundleToc(
            variants: [BundleToc.Variant(minSdk: 21, apks: [.master("splits/base-master.apk")])],
            assetModules: [
                .installTime("levels", [.slice("asset-slices/levels-master.apk", "config.xhdpi", master: true)]),
                .fastFollow("later", [.slice("asset-slices/later-master.apk", "config.x86_64", master: true)]),
            ])
        let url = try write([
            ContainerEntry("toc.pb", toc.build()), ContainerEntry("splits/base-master.apk", try apk("base.apk")),
            ContainerEntry("asset-slices/levels-master.apk", try apk("config.xhdpi.apk")),
            ContainerEntry("asset-slices/later-master.apk", try apk("config.x86_64.apk")),
        ], "packs.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        // The fast-follow pack is left out, so the result is not the plain one.
        #expect(try await operations.installApk(at: url, update: { _ in }) == .installedDataNotPlaced)
        let files = try await installedFiles(operations)
        #expect(files.count == 2 && files.contains("base.apk"), "\(files)")
        #expect(files.contains { $0.contains("xhdpi") } && !files.contains { $0.contains("x86_64") }, "\(files)")
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(10))) func anInstallTimeSliceForAnotherProcessorOfTheGuestIsInstalledWithTheApp() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        // The app has an arm64 split; the install-time slice is for armeabi-v7a, which the guest has too.
        let toc = BundleToc(
            variants: [BundleToc.Variant(minSdk: 21, apks: [
                .master("splits/base-master.apk"), .split("splits/base-arm64_v8a.apk", "config.arm64_v8a", abi: 3),
            ])],
            assetModules: [.installTime("levels", [.slice("asset-slices/levels-master.apk", "config.armeabi_v7a", master: true, abi: 2)])])
        let url = try write([
            ContainerEntry("toc.pb", toc.build()), ContainerEntry("splits/base-master.apk", try apk("base.apk")),
            ContainerEntry("splits/base-arm64_v8a.apk", try apk("config.arm64_v8a.apk")),
            ContainerEntry("asset-slices/levels-master.apk", try apk("config.armeabi_v7a.apk")),
        ], "slice-processor.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await operations.installApk(at: url, update: { _ in }) == .installed)
        let files = try await installedFiles(operations)
        #expect(files.count == 3 && files.contains("base.apk"), "\(files)")
        #expect(files.contains { $0.contains("armeabi_v7a") } && files.contains { $0.contains("arm64_v8a") }, "\(files)")
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    // MARK: A container of more than 4 GiB

    @Test(.timeLimit(.minutes(30))) func aContainerOfMoreThanFourGibInstallsItsApksFromPastTheFourGibMark() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        // Two unused entries of 2.2 GiB (holes) come first, so the APKs and the
        // directory are past what 32-bit offsets hold.
        let pad: UInt64 = 22 << 27
        let url = try write([
            ContainerEntry("docs/pad1.bin", zeros: pad), ContainerEntry("docs/pad2.bin", zeros: pad),
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("base.apk")),
            ContainerEntry("split_config.xhdpi.apk", try apk("config.xhdpi.apk")),
        ], "huge.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let size = try #require(try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? UInt64)
        #expect(size > 4 << 30)
        let footprint = FootprintPeak()
        let before = FootprintPeak.current(relieving: true)
        #expect(try await operations.installApk(at: url, update: { _ in footprint.sample() }) == .installed)
        #expect(try await installedFiles(operations).count == 2)
        let growth = footprint.growth(since: before)
        print("CONTAINER_GUEST zip64 file=\(size) footprint-growth=\(growth.footprint) heap-growth=\(growth.heap)")
        #expect(growth.footprint < LargeTransferFootprintTests.allowedGrowth)
        await clean(operations)
    }

    // MARK: Files a person supplies

    /// With `ANDARIUM_CONTAINER_GUEST_SAMPLES` naming a folder of folders `1`,
    /// `2`, ... that each hold one container file, each file goes through the
    /// reader and then the install, and lines say what happened: how the file
    /// is laid out, what the install told the overlay, and what the guest then
    /// has, down to the size and SHA-256 of each data file against its entry.
    /// The lines name neither the file nor the app: the package is called by
    /// the file's number. `ANDARIUM_CONTAINER_GUEST_CANCEL_COPY` lists the
    /// numbers whose data copy is first cancelled halfway, on a guest that has
    /// not seen the app, so that the next install of the file is the retry
    /// the result text promises. The apps are not started, and are left
    /// installed.
    @Test(.timeLimit(.minutes(120))) func filesAPersonSuppliesGoThroughTheReaderAndIntoTheGuest() async throws {
        guard Self.enabled, let folder = Self.environment["ANDARIUM_CONTAINER_GUEST_SAMPLES"] else { return }
        let commands = SessionCommands()
        let operations = AdbGuestOperations(
            identity: try throwawayIdentity(), clock: GuestRunClock(), device: "Mac",
            connector: RecordingAdbConnector(base: LoopbackAdbConnector(port: Self.port!), commands: commands), hostFree: { .max },
            run: GuestRunMark.random())
        let cancelCopy = Set((Self.environment["ANDARIUM_CONTAINER_GUEST_CANCEL_COPY"] ?? "").split(separator: ",").compactMap { Int($0) })
        let abis = try await shell(operations, "getprop ro.product.cpu.abilist").split(separator: ",").map(String.init)
        let sdk = Int(try await shell(operations, "getprop ro.build.version.sdk"))
        print("REAL_SAMPLE guest-abis=\(abis) sdk=\(sdk.map(String.init) ?? "?")")
        func packages() async throws -> Set<String> {
            Set(try await shell(operations, "pm list packages -3").split(whereSeparator: \.isNewline).map { String($0.dropFirst("package:".count)) })
        }
        if Self.environment["ANDARIUM_CONTAINER_GUEST_START_CLEAN"] != nil {
            // A guest with no app of anyone's: the packages the guest was given, and the OBB folders they left.
            let removed = try await packages()
            for package in removed { _ = try await shell(operations, "pm uninstall \(package) >/dev/null 2>&1; rm -rf '/storage/emulated/0/Android/obb/\(package)'; true") }
            print("REAL_SAMPLE start-clean removed-packages=\(removed.count) remaining=\(try await packages().count)")
        }
        var known = try await packages()
        let numbers = ((try? FileManager.default.contentsOfDirectory(atPath: folder)) ?? []).compactMap(Int.init).sorted()
        for number in numbers {
            let directory = URL(fileURLWithPath: folder).appendingPathComponent(String(number))
            guard let url = ((try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? []).first
            else { continue }
            var row = "REAL_SAMPLE \(number) kind=\(url.pathExtension.lowercased())"
            var chosen: ApkContainerPlan?
            // The reader alone.
            do {
                let source = try ZipFileSource(contentsOf: url)
                row += " bytes=\(source.size) zip=readable"
                switch try ApkContainer.inspect(source, fileName: url.lastPathComponent) {
                case .singleApk:
                    row += " layout=single-apk"
                case let .parts(plan):
                    let listed = plan.variants.isEmpty ? plan.candidates : plan.variants.flatMap { $0.apks.map(\.part) }
                    let sizes = Dictionary(listed.map { ($0.entry, $0.bytes) }, uniquingKeysWith: { first, _ in first })
                    row += " layout=\(plan.kind) parts-in-file=\(sizes.count) apk-bytes=\(sizes.values.reduce(0, +))"
                    row += " variants=\(plan.variants.map { "n\($0.number):sdk\($0.targeting.sdk)alt\($0.targeting.sdkAlternatives):abi\($0.targeting.abis)alt\($0.targeting.abiAlternatives)\($0.targeting.other || $0.targeting.unknownAbi ? ":other" : "")" })"
                    row += " data-files=\(plan.data.count) unplaced-data=\(plan.unplacedData) package-from-container=\(plan.packageName != nil)"
                    if let selected = try? plan.selecting(guestAbis: abis, guestSdk: sdk) {
                        chosen = selected
                        let kinds = selected.candidates.compactMap(\.abi)
                        row += " parts-chosen=\(selected.candidates.count) chosen-bytes=\(selected.apkBytes) chosen-processor-splits=\(kinds)"
                        row += " processor-splits-in-file=\(Set(listed.compactMap(\.abi)).sorted())"
                    } else {
                        row += " chosen=none"
                    }
                    print("REAL_SAMPLE \(number) facts \(try Self.facts(of: ZipArchive(source: source), plan: plan))")
                }
            } catch let refusal as InstallRefusal {
                row += " reader-refused=\(refusal.message)"
            } catch {
                row += " reader-error=\(error)"
            }
            print(row)
            var installedPackage: String?
            // The extra run: the copy of the data cancelled halfway.
            if cancelCopy.contains(number), let chosen, !chosen.data.isEmpty {
                // The app, if an earlier file of the folder is the same app, goes first: this run is on a guest that has not seen it.
                if let package = chosen.packageName, known.contains(package) {
                    _ = try await shell(operations, "pm uninstall \(package) >/dev/null 2>&1; rm -rf '/storage/emulated/0/Android/obb/\(package)'; true")
                    known = try await packages()
                    print("REAL_SAMPLE \(number) pre-clean removed an earlier file's copy of the same app")
                }
                let log = UpdateLog()
                let box = TaskBox()
                let partials = try Self.settledLedger()
                defer { try? FileManager.default.removeItem(at: partials.directory) }
                let task = Task {
                    try await operations.installApk(at: url, partials: partials.ledger, update: { update in
                        log.add(update)
                        if let progress = update.progress, progress >= 0.6 { box.cancel() }
                    })
                }
                box.task = task
                let result = await task.result
                var line = "REAL_SAMPLE \(number) cancel-mid-copy"
                if case let .success(outcome) = result { line += " result=\(Self.word(outcome)) ui=\"\(Self.text(outcome))\"" } else { line += " result=\(result)" }
                line += " last-progress=\(String(format: "%.2f", log.progress.last ?? -1)) ledger-owed=\(partials.ledger.isOwed)"
                print(line)
                let now = try await packages()
                installedPackage = now.subtracting(known).first ?? chosen.packageName
                known = now
                if let package = installedPackage {
                    let listing = try await shell(operations, "ls -A '/storage/emulated/0/Android/obb/\(package)' 2>/dev/null")
                    let paths = try await shell(operations, "pm path \(package)").split(whereSeparator: \.isNewline).count
                    print("REAL_SAMPLE \(number) after-cancel installed-apk-files=\(paths) obb-folder-entries=\(listing.split(whereSeparator: \.isNewline).count) partial-left=\(listing.contains(".andarium-")) active-sessions=\(try await activeSessions(operations))")
                }
            }
            // The install, as Settings runs it.
            commands.clear()
            let footprint = FootprintPeak()
            let before = FootprintPeak.current(relieving: true)
            let started = ContinuousClock.now
            let log = UpdateLog()
            var line = "REAL_SAMPLE \(number) install"
            let partials = try Self.settledLedger()
            defer { try? FileManager.default.removeItem(at: partials.directory) }
            do {
                let outcome: ApkInstallOutcome
                do {
                    outcome = try await operations.installApk(at: url, partials: partials.ledger, update: { footprint.sample(); log.add($0) })
                } catch let refused as LegacyApkRefused {
                    line += " older-app-question(app-level=\(refused.target.appLevel.map(String.init) ?? "?"), required=\(refused.target.requiredLevel.map(String.init) ?? "?"))-then"
                    outcome = try await operations.installApk(at: url, bypassLowTargetSdkBlock: true, partials: partials.ledger, update: { footprint.sample(); log.add($0) })
                }
                line += " result=\(Self.word(outcome)) ui=\"\(Self.text(outcome))\""
            } catch let failure as AdbFailure {
                line += " result=refused ui=\"\(failure.message)\""
            } catch {
                line += " result=error \(error)"
            }
            let elapsed = ContinuousClock.now - started
            let growth = footprint.growth(since: before)
            line += " seconds=\(Int(elapsed.components.seconds)) footprint-growth=\(growth.footprint) heap-growth=\(growth.heap)"
            print(line)
            // What the overlay was told: progress, the Cancel button and the counted lines.
            let updates = log.all
            let progress = log.progress
            var cancellable: [String] = []
            var latest = 0.0
            for update in updates {
                if let value = update.progress { latest = value }
                if let flag = update.cancellable { cancellable.append("\(flag)@\(String(format: "%.2f", latest))") }
            }
            // The lines the overlay showed, with their numbers as #, in the order they first appeared.
            var lines: [String] = []
            for detail in log.details {
                let shape = detail.map { $0.isNumber ? "#" : String($0) }.joined().replacingOccurrences(of: "##", with: "#")
                if !lines.contains(shape) { lines.append(shape) }
            }
            print("REAL_SAMPLE \(number) overlay updates=\(updates.count) progress-first=\(String(format: "%.2f", progress.first ?? -1)) progress-last=\(String(format: "%.2f", progress.last ?? -1)) monotonic=\(zip(progress, progress.dropFirst()).allSatisfy { $0 <= $1 }) cancellable-changes=\(cancellable) lines=\(lines)")
            // What the guest now has: the new package, by its number.
            let now = try await packages()
            let added = now.subtracting(known)
            known = now
            guard let package = added.first ?? installedPackage ?? chosen?.packageName.flatMap({ now.contains($0) ? $0 : nil }), added.count <= 1 else {
                print("REAL_SAMPLE \(number) guest new-packages=\(added.count)")
                print("REAL_SAMPLE \(number) guest active-sessions=\(try await activeSessions(operations))")
                continue
            }
            let files = try await shell(operations, "pm path \(package)").split(whereSeparator: \.isNewline).map { String($0.split(separator: "/").last ?? "") }
            let details = try await shell(operations, "dumpsys package \(package) | grep -m1 -o 'splits=\\[[^]]*\\]'")
            let abi = try await shell(operations, "dumpsys package \(package) | grep -m1 -o 'primaryCpuAbi=[^ ]*'")
            print("REAL_SAMPLE \(number) guest installed-files=\(files.count) config-splits=\(files.filter { $0.hasPrefix("split_config.") }.map { $0.dropFirst("split_config.".count).dropLast(4) }.sorted()) \(abi) \(details.contains("splits=") ? "splits-reported" : "no-splits-line")")
            if let chosen, !chosen.data.isEmpty || chosen.unplacedData > 0 {
                // The session of an app with data names the package the file declares.
                let creates = commands.all.map { $0.replacingOccurrences(of: "\0", with: " ") }
                let bound = creates.filter { $0.contains("--pkg \(package)") }.count
                print("REAL_SAMPLE \(number) session-creates=\(creates.count) bound-to-installed-package=\(bound) plan-package-is-installed-package=\(chosen.packageName == package)")
                let archive = try ZipArchive(source: ZipFileSource(contentsOf: url))
                for (index, file) in chosen.data.enumerated() {
                    guard let place = chosen.guestLocation(of: file), let entry = archive.entry(named: file.entry) else {
                        print("REAL_SAMPLE \(number) data \(index + 1) not-placeable")
                        continue
                    }
                    let guestBytes = try await shell(operations, "stat -c %s '\(place.path)'")
                    let guestHash = try await shell(operations, "sha256sum '\(place.path)' | cut -d' ' -f1")
                    var hasher = SHA256()
                    let reader = try archive.reader(for: entry)
                    while let piece = try reader.read(upTo: 256 * 1024) { hasher.update(data: piece) }
                    let entryHash = hasher.finalize().map { String(format: "%02x", $0) }.joined()
                    print("REAL_SAMPLE \(number) data \(index + 1) entry-bytes=\(entry.size) guest-bytes=\(guestBytes) sha256-entry=\(entryHash.prefix(16)) sha256-guest=\(guestHash.prefix(16)) equal=\(entryHash == guestHash) in-package-folder=\(place.folder.hasSuffix("/Android/obb/\(package)"))")
                }
                let listing = try await shell(operations, "ls -A '/storage/emulated/0/Android/obb/\(package)' 2>/dev/null")
                let others = try await shell(operations, "ls -A /storage/emulated/0/Android/obb 2>/dev/null").split(whereSeparator: \.isNewline).filter { $0 != package && !$0.hasPrefix(".") }
                print("REAL_SAMPLE \(number) obb-folder-entries=\(listing.split(whereSeparator: \.isNewline).count) partial-left=\(listing.contains(".andarium-")) other-obb-folders=\(others.count)")
            }
            print("REAL_SAMPLE \(number) guest active-sessions=\(try await activeSessions(operations))")
        }
    }

    /// The outcome as a word, and as the overlay's text with `<file>` for the name (the model's own strings).
    private static func word(_ outcome: ApkInstallOutcome) -> String {
        switch outcome {
        case .installed: "installed"
        case .installedWithoutData: "installed-without-data"
        case .installedDataNotPlaced: "installed-data-not-placed"
        }
    }

    private static func text(_ outcome: ApkInstallOutcome) -> String {
        switch outcome {
        case .installed:
            "<file> is installed in the virtual machine."
        case .installedWithoutData:
            "<file> is installed in the virtual machine, but not all of its data files were copied. The app may not work without them. Install the file again to copy them."
        case .installedDataNotPlaced:
            "<file> is installed in the virtual machine, but not all of its data files were copied. The app may not work without them."
        }
    }

    /// What a container says about itself, as field names and kinds of value, never the app's own words:
    /// the keys of its manifest or info file, and the forms its data declarations take.
    private static func facts(of archive: ZipArchive, plan: ApkContainerPlan) throws -> String {
        var facts: [String] = []
        let package = plan.packageName
        func redact(_ text: String) -> String { package.map { text.replacingOccurrences(of: $0, with: "<pkg>") } ?? text }
        for name in ["manifest.json", "info.json"] {
            guard let entry = archive.entry(named: name),
                  let object = try? JSONSerialization.jsonObject(with: archive.data(of: entry, limit: 1 << 20)) as? [String: Any]
            else { continue }
            facts.append("\(name)-keys=\(object.keys.sorted())")
            if name == "info.json" {
                let obb = object["obb_files"] as? [String] ?? []
                facts.append("info-pname-is-plan-package=\((object["pname"] as? String) == package)")
                facts.append("info-obb-files=\(obb.count) info-arches=\(object["arches"] ?? "-") info-dpis=\(object["dpis"] ?? "-") info-min-api=\(object["min_api"] ?? "-") info-apkm-version=\(object["apkm_version"] ?? "-")")
                facts.append("info-obb-entries-present=\(obb.filter { archive.entry(named: $0) != nil }.count) info-obb-forms=\(obb.map { $0.contains("/") ? "path" : ($0.hasSuffix(".obb") ? "root-obb" : "other") })")
            } else {
                facts.append("xapk-version=\(object["xapk_version"] ?? "-") split-apks=\((object["split_apks"] as? [Any])?.count ?? -1) split-configs=\((object["split_configs"] as? [Any])?.count ?? -1)")
                for (index, item) in ((object["expansions"] as? [[String: Any]]) ?? []).enumerated() {
                    let file = item["file"] as? String ?? ""
                    let path = item["install_path"] as? String ?? ""
                    let location = item["install_location"] as? String ?? "-"
                    let form: String
                    if path.hasPrefix("Android/obb/") { form = "android-obb" } else if path.hasPrefix("Android/data/") { form = "android-data" } else { form = "other" }
                    let found = archive.entry(named: file)
                    facts.append("expansion\(index + 1)-keys=\(item.keys.sorted()) location=\(location) install-path-form=\(form) file-equals-install-path=\(file == path) path-names-package=\(redact(path).contains("<pkg>")) entry-present=\(found != nil) method=\(found.map { String($0.method) } ?? "-") bytes=\(found?.size ?? 0)")
                }
                facts.append("data-declarations-handled=\(plan.data.count) unplaced=\(plan.unplacedData)")
            }
        }
        return facts.joined(separator: " ")
    }

    // MARK: Helpers

    private static func settledLedger() throws -> (ledger: PartialSweepLedger, directory: URL) {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("ledger-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try PartialSweepLedger.markSettled(in: directory)
        return (PartialSweepLedger(directory: directory), directory)
    }
}

/// Lets an update closure cancel the task that is running the install.
private nonisolated final class TaskBox: @unchecked Sendable {
    private let lock = NSLock()
    private var current: Task<ApkInstallOutcome, Error>?
    var task: Task<ApkInstallOutcome, Error>? {
        get { lock.withLock { current } }
        set { lock.withLock { current = newValue } }
    }
    func cancel() { task?.cancel() }
}

private nonisolated final class Flag: @unchecked Sendable {
    private let lock = NSLock()
    private var raised = false
    func raise() { lock.withLock { raised = true } }
    var isRaised: Bool { lock.withLock { raised } }
}

/// The commands that create install sessions, as the connection carried them.
private nonisolated final class SessionCommands: @unchecked Sendable {
    private let lock = NSLock()
    private var commands: [String] = []
    func add(_ command: String) { lock.withLock { commands.append(command) } }
    func clear() { lock.withLock { commands.removeAll() } }
    var all: [String] { lock.withLock { commands } }
}

/// Reaches a guest's adbd as `LoopbackAdbConnector` does and keeps the commands that create install sessions.
private nonisolated struct RecordingAdbConnector: AdbConnector {
    let base: LoopbackAdbConnector
    let commands: SessionCommands

    func connect() throws -> any AdbTransport { RecordingAdbTransport(base: try base.connect(), commands: commands) }
}

private nonisolated final class RecordingAdbTransport: AdbTransport, @unchecked Sendable {
    private let base: any AdbTransport
    private let commands: SessionCommands

    init(base: any AdbTransport, commands: SessionCommands) {
        self.base = base
        self.commands = commands
    }

    func readExactly(_ count: Int) throws -> Data { try base.readExactly(count) }

    func writeAll(_ data: Data) throws {
        // The service string of an open request, with the header's binary bytes in front of it.
        if data.count < 4096 {
            let text = String(decoding: data, as: UTF8.self)
            if text.contains("install-create") { commands.add(text) }
        }
        try base.writeAll(data)
    }

    func shutdown() { base.shutdown() }
}

/// Reaches a guest's adbd over a loopback TCP port.
nonisolated struct LoopbackAdbConnector: AdbConnector {
    let port: UInt16

    func connect() throws -> any AdbTransport {
        let descriptor = socket(AF_INET, SOCK_STREAM, 0)
        guard descriptor >= 0 else { throw AdbError.io(errno) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard result == 0 else {
            let code = errno
            close(descriptor)
            throw code == ECONNREFUSED ? AdbError.refused : AdbError.io(code)
        }
        return DescriptorTransport(descriptor: descriptor)
    }
}

/// Bytes that do not compress: a fixed pseudo-random sequence.
private func incompressible(_ count: Int) -> Data {
    var state: UInt64 = 0x9E37_79B9_7F4A_7C15
    var bytes = [UInt8](repeating: 0, count: count)
    for index in 0..<count {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        bytes[index] = UInt8(truncatingIfNeeded: state >> 33)
    }
    return Data(bytes)
}
