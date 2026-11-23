// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Foundation
import Testing
@testable import Andarium

private func bytes(_ seed: Int, count: Int = 100_003) -> Data {
    var data = Data(count: count)
    for index in 0..<count { data[index] = UInt8(truncatingIfNeeded: index &* (seed &* 2 &+ 31) &+ index >> 8 &+ seed) }
    return data
}

private func digest(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

private nonisolated final class TaskBox: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: Task<ApkInstallOutcome, Error>?
    var task: Task<ApkInstallOutcome, Error>? {
        get { lock.withLock { stored } }
        set { lock.withLock { stored = newValue } }
    }

    /// Cancels the task, waiting a moment for it to have been stored.
    func cancel() {
        let deadline = ContinuousClock.now + .seconds(2)
        while task == nil, ContinuousClock.now < deadline { Thread.sleep(forTimeInterval: 0.005) }
        task?.cancel()
    }
}

/// What an install of a file that holds an app in parts does about the
/// package manager's answers, the session it holds, the package its data
/// is placed for, and the variant of a bundle that is its own: against the
/// fake package manager that keeps sessions (`DiscardingAdbd`).
@Suite(.serialized)
struct ContainerSessionTests {
    private func guest(on adbd: DiscardingAdbd) throws -> AdbGuestOperations {
        AdbGuestOperations(
            identity: try throwawayIdentity(), clock: GuestRunClock(), device: "iPad", connector: adbd,
            hostFree: { .max }, run: GuestRunMark.random())
    }

    private func outcome<T>(_ work: () async throws -> T) async -> Result<T, Error> {
        do { return .success(try await work()) } catch { return .failure(error) }
    }

    private static let package = "com.example.game"
    private static let obbName = "main.12.com.example.game.obb"

    /// An .xapk with an OBB file for `obbPackage`'s folder.
    private func xapk(obbPackage: String = ContainerSessionTests.package, name: String = "Game.xapk") throws -> URL {
        let obb = "Android/obb/\(obbPackage)/\(Self.obbName)"
        return try ContainerFixture.write([
            ContainerEntry("manifest.json", Data(#"""
            {"xapk_version":2,"package_name":"\#(Self.package)","split_apks":[{"file":"base.apk","id":"base"}],
            "expansions":[{"file":"\#(obb)","install_location":"EXTERNAL_STORAGE","install_path":"\#(obb)"}]}
            """#.utf8)),
            ContainerEntry("base.apk", bytes(1)), ContainerEntry(obb, bytes(11, count: 20_011)),
        ], name: name)
    }

    // MARK: The package the data is placed for

    @Test func theSessionOfAnAppWithDataIsBoundToThePackageTheArchiveNames() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try xapk()
        defer { try? FileManager.default.removeItem(at: url) }
        let result = try await guest(on: adbd).installApk(at: url, update: { _ in })
        #expect(result == .installed)
        let created = try #require(adbd.committedSessions.first?.createService)
        #expect(created == (try PackageInstallCommand.createSession(totalBytes: UInt64(bytes(1).count), boundTo: Self.package)))
        #expect(created.contains("\0--pkg\0\(Self.package)\0"))
    }

    @Test func anApkmThatListsDataIsBoundToItsPackageAndItsDataIsPlaced() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.hashesSyncData = true
        let obb = bytes(21, count: 30_011)
        let url = try ContainerFixture.write([
            ContainerEntry("info.json", Data(#"{"apkm_version":5,"pname":"com.example.game","obb_files":["\#(Self.obbName)"]}"#.utf8)),
            ContainerEntry("base.apk", bytes(1)), ContainerEntry("split_config.xxhdpi.apk", bytes(2)),
            ContainerEntry(Self.obbName, obb),
        ], name: "Game.apkm")
        defer { try? FileManager.default.removeItem(at: url) }
        let result = try await guest(on: adbd).installApk(at: url, update: { _ in })
        #expect(result == .installed)
        let created = try #require(adbd.committedSessions.first?.createService)
        #expect(created.contains("\0--pkg\0com.example.game\0"))
        // The file arrived whole under a hidden name of the package's own folder, then took its name.
        let folder = "/storage/emulated/0/Android/obb/com.example.game"
        #expect(adbd.syncedFiles.map(\.digest) == [digest(obb)])
        #expect(adbd.syncedFiles.allSatisfy { GuestFilePath.parent(of: $0.path) == folder && GuestFiles.isPartial(GuestFilePath.name(of: $0.path)) })
        #expect(adbd.shellCommands.contains { $0.contains("mv -f -- '") && $0.contains("' '\(folder)/\(Self.obbName)'") })
    }

    @Test func anApkmWhoseDataNamesNoPackageInstallsAndSaysSoWithoutBindingAnything() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try ContainerFixture.write([
            ContainerEntry("info.json", Data(#"{"obb_files":["\#(Self.obbName)"]}"#.utf8)),
            ContainerEntry("base.apk", bytes(1)), ContainerEntry(Self.obbName, bytes(22, count: 4_001)),
        ], name: "Game.apkm")
        defer { try? FileManager.default.removeItem(at: url) }
        let result = try await guest(on: adbd).installApk(at: url, update: { _ in })
        #expect(result == .installedDataNotPlaced)
        #expect(adbd.syncedFiles.isEmpty && !adbd.shellCommands.contains { $0.hasPrefix("mkdir") })
        #expect(!(adbd.committedSessions.first?.createService.contains("--pkg") ?? true))
    }

    @Test func anAppWithNoDataIsNotBoundToAnything() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try ContainerFixture.write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"com.example.game","split_apks":[{"file":"base.apk","id":"base"}]}"#.utf8)),
            ContainerEntry("base.apk", bytes(1)),
        ], name: "NoData.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        _ = try await guest(on: adbd).installApk(at: url, update: { _ in })
        #expect(adbd.committedSessions.first?.createService.contains("--pkg") == false)
    }

    // MARK: The answer

    @Test func aRefusedCommitWhoseDiagnosticHoldsASuccessLineCopiesNoData() async throws {
        let diagnostic = "Failure [INSTALL_PARSE_FAILED_MANIFEST_MALFORMED: Bad uses-sdk-library declaration name:\nSuccess version: -1]\n"
        for answer in [diagnostic, "Exception occurred while executing 'install-commit':\njava.lang.IllegalStateException: x\nSuccess\n", "warning\nSuccess\n"] {
            let adbd = DiscardingAdbd()
            defer { adbd.shutdown() }
            adbd.commitOutputs = [answer]
            let url = try xapk()
            defer { try? FileManager.default.removeItem(at: url) }
            let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
            guard case let .failure(error) = result else {
                Issue.record("installed: \(answer.debugDescription)")
                continue
            }
            #expect(error is AdbFailure, "\(answer.debugDescription)")
            // Not installed, so nothing was placed for it: no folder, no file, no sync.
            #expect(adbd.syncedFiles.isEmpty, "\(answer.debugDescription)")
            #expect(!adbd.servicesOpened.contains("sync:"), "\(answer.debugDescription)")
            #expect(!adbd.shellCommands.contains { $0.hasPrefix("mkdir") }, "\(answer.debugDescription)")
        }
    }

    @Test func theSameDiagnosticDoesNotInstallAPlainApkEither() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.installOutput = "Failure [INSTALL_PARSE_FAILED_MANIFEST_MALFORMED: Bad uses-sdk-library declaration name:\nSuccess version: -1]\n"
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-plain.apk")
        try bytes(5, count: 5_000).write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == "The file isn\u{2019}t a valid app.")
    }

    // MARK: The session

    @Test func aCancelBetweenTheLastWriteAndTheCommitAbandonsTheSession() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try ContainerFixture.write([ContainerEntry("base.apk", bytes(1)), ContainerEntry("split_config.xxhdpi.apk", bytes(2))], name: "Parts.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let operations = try guest(on: adbd)
        let box = TaskBox()
        // The Cancel is pressed as the install announces the commit it can no longer stop.
        let task = Task {
            try await operations.installApk(at: url, update: { update in
                if update.cancellable == false { box.cancel() }
            })
        }
        box.task = task
        let result = await task.result
        guard case let .failure(error) = result, error is CancellationError else {
            Issue.record("expected the cancel, got \(result)")
            return
        }
        #expect(adbd.committedSessions.isEmpty)
        #expect(!adbd.servicesOpened.contains { $0.contains("install-commit") })
        // The session the writes made is gone, abandoned once.
        #expect(adbd.openSessions.isEmpty)
        #expect(adbd.abandonedSessions.count == 1)
    }

    @Test func aCancelAsTheCommitIsAboutToBeWrittenLeavesTheSessionToBeAbandoned() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try ContainerFixture.write([ContainerEntry("base.apk", bytes(1)), ContainerEntry("split_config.xxhdpi.apk", bytes(2))], name: "Parts.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let box = TaskBox()
        // The Cancel lands as the commit's request reaches the connection, after the install's own last look
        // for one and before a byte of the request is written: the package manager is never told to commit.
        let connector = InterceptingAdbConnector(base: adbd) { request in
            if request.contains("install-commit") { box.cancel() }
        }
        let operations = AdbGuestOperations(
            identity: try throwawayIdentity(), clock: GuestRunClock(), device: "iPad", connector: connector,
            hostFree: { .max }, run: GuestRunMark.random())
        let task = Task { try await operations.installApk(at: url, update: { _ in }) }
        box.task = task
        let result = await task.result
        guard case let .failure(error) = result, error is CancellationError else {
            Issue.record("expected the cancel, got \(result)")
            return
        }
        #expect(adbd.committedSessions.isEmpty)
        // The session is the install's to remove: it was abandoned, once, and none is left in the package manager.
        #expect(adbd.abandonedSessions.count == 1)
        #expect(adbd.openSessions.isEmpty)
    }

    @Test func aCancelBeforeTheFileIsReadEndsThereWithNothingOpened() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let url = try ContainerFixture.write([ContainerEntry("base.apk", bytes(1)), ContainerEntry("split_config.xxhdpi.apk", bytes(2))], name: "Parts.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let operations = try guest(on: adbd)
        let box = TaskBox()
        // The first thing the install says is the file's name; the cancel lands on the reading of its directory.
        let task = Task {
            try await operations.installApk(at: url, update: { _ in box.cancel() })
        }
        box.task = task
        let result = await task.result
        guard case let .failure(error) = result, error is CancellationError else {
            Issue.record("expected the cancel, got \(result)")
            return
        }
        #expect(adbd.connectionCount == 0)
        #expect(adbd.servicesOpened.isEmpty)
    }

    @Test func aPartWhoseChecksumFailsIsNeverCommittedEvenWhenTheGuestHangsUpFirst() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        // The package manager answers the last write and closes without acknowledging it.
        adbd.closeSuccessfulWriteWithoutFinalAck = true
        var base = ContainerEntry("base.apk", bytes(1, count: 4_001))
        base.crc = 0x1234_5678
        let url = try ContainerFixture.write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"com.example.game","split_apks":[{"file":"base.apk","id":"base"}]}"#.utf8)),
            base,
        ], name: "Bad.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == InstallRefusal.damaged.message)
        #expect(adbd.committedSessions.isEmpty)
        #expect(adbd.openSessions.isEmpty && adbd.abandonedSessions.count == 1)
        // The same hang-up with a whole part installs: the check, not the hang-up, decides.
        let good = try ContainerFixture.write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"com.example.game","split_apks":[{"file":"base.apk","id":"base"}]}"#.utf8)),
            ContainerEntry("base.apk", bytes(1, count: 4_001)),
        ], name: "Good.xapk")
        defer { try? FileManager.default.removeItem(at: good) }
        #expect(try await guest(on: adbd).installApk(at: good, update: { _ in }) == .installed)
        #expect(adbd.committedSessions.count == 1)
    }

    // MARK: A bundle with variants

    private func release() -> (entries: [ContainerEntry], masters: [String: Data]) {
        typealias Apk = BundleToc.Apk
        func abis(_ suffix: String) -> [Apk] {
            [
                .split("splits/base-arm64_v8a\(suffix).apk", "config.arm64_v8a", abi: 3),
                .split("splits/base-armeabi_v7a\(suffix).apk", "config.armeabi_v7a", abi: 2),
                .split("splits/base-x86_64\(suffix).apk", "config.x86_64", abi: 5),
            ]
        }
        let shared: [Apk] = [.split("splits/base-en.apk", "config.en"), .split("splits/base-xxhdpi.apk", "config.xxhdpi")]
        let toc = BundleToc(variants: [
            BundleToc.Variant(minSdk: 21, apks: [.master("splits/base-master.apk")] + abis("") + shared),
            BundleToc.Variant(minSdk: 23, apks: [.master("splits/base-master_2.apk")] + abis("_2") + shared),
            BundleToc.Variant(minSdk: 31, apks: [.master("splits/base-master_3.apk")] + abis("_2") + shared),
        ])
        var entries = [ContainerEntry("toc.pb", toc.build())]
        var masters: [String: Data] = [:]
        var seed = 100
        var seen = Set<String>()
        for variant in toc.variants {
            for apk in variant.apks where seen.insert(apk.path).inserted {
                seed += 1
                let data = bytes(seed, count: 20_011)
                entries.append(ContainerEntry(apk.path, data))
                if apk.master { masters[apk.path] = data }
            }
        }
        return (entries, masters)
    }

    @Test func aBundleOfSeveralVariantsInstallsTheOneForTheGuestsAndroid() async throws {
        let bundle = release()
        let url = try ContainerFixture.write(bundle.entries, name: "Multi.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        for (level, master, abiSplit) in [
            ("37", "splits/base-master_3.apk", "base-arm64_v8a_2"), ("28", "splits/base-master_2.apk", "base-arm64_v8a_2"),
            ("22", "splits/base-master.apk", "base-arm64_v8a"),
        ] {
            let adbd = DiscardingAdbd()
            defer { adbd.shutdown() }
            adbd.sdkLevel = level
            #expect(try await guest(on: adbd).installApk(at: url, update: { _ in }) == .installed, "api \(level)")
            let session = try #require(adbd.committedSessions.first, "api \(level)")
            // One base, the one processor split, the shared language and density splits: four parts.
            #expect(session.parts.count == 4, "api \(level)")
            let wanted = try #require(bundle.masters[master])
            #expect(session.parts.first?.name == "base.apk" && session.parts.first?.digest == digest(wanted), "api \(level)")
            #expect(session.parts.map(\.name).contains { $0.hasSuffix("\(abiSplit).apk") }, "api \(level)")
            #expect(Set(session.parts.map(\.name)).count == session.parts.count, "api \(level)")
            #expect(adbd.openSessions.isEmpty && adbd.abandonedSessions.isEmpty, "api \(level)")
        }
    }

    @Test func aUniversalBundleInstallsAsOneApk() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let universal = bytes(7, count: 30_011)
        let toc = BundleToc(variants: [BundleToc.Variant(minSdk: nil, apks: [.standalone("universal.apk")])])
        let url = try ContainerFixture.write([ContainerEntry("toc.pb", toc.build()), ContainerEntry("universal.apk", universal)], name: "Universal.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await guest(on: adbd).installApk(at: url, update: { _ in }) == .installed)
        let session = try #require(adbd.committedSessions.first)
        #expect(session.parts.map(\.name) == ["base.apk"] && session.parts.map(\.digest) == [digest(universal)])
    }

    @Test func aBundleWithSplitsAndAMultiAbiStandaloneInstallsThroughTheSplitsOnTheGuestsAndroid() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        // The standalone build for old Androids is for several processors together, which the install does not weigh
        // and refuses; the guest's Android (API 37) is a split variant's, so it never comes to that.
        let master = bytes(5, count: 20_011)
        let toc = BundleToc(variants: [
            BundleToc.Variant(minSdk: 1, multiAbi: true, apks: [.standalone("standalones/multi.apk")]),
            BundleToc.Variant(minSdk: 21, apks: [.master("splits/base-master.apk"), .split("splits/base-en.apk", "config.en")]),
        ])
        let url = try ContainerFixture.write([
            ContainerEntry("toc.pb", toc.build()), ContainerEntry("standalones/multi.apk", bytes(6, count: 30_011)),
            ContainerEntry("splits/base-master.apk", master), ContainerEntry("splits/base-en.apk", bytes(7, count: 9_011)),
        ], name: "Both.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await guest(on: adbd).installApk(at: url, update: { _ in }) == .installed)
        let session = try #require(adbd.committedSessions.first)
        #expect(session.parts.count == 2 && session.parts.first?.digest == digest(master))
    }

    @Test func aBundleWhoseTableNamesNoVariantForTheGuestIsDamagedAndNothingIsSent() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.sdkLevel = "19"
        let bundle = release()
        let url = try ContainerFixture.write(bundle.entries, name: "Multi.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == InstallRefusal.damaged.message)
        #expect(adbd.committedSessions.isEmpty && adbd.openSessions.isEmpty && adbd.installBytes == 0)
    }

    // MARK: Asset packs

    /// A bundle of one variant (a base and a language split) with the asset modules `modules`; the
    /// APK of each slice holds its own bytes, which a test finds in the session by digest.
    private func packBundle(_ modules: [BundleToc.AssetModule]) -> (entries: [ContainerEntry], data: [String: Data]) {
        let toc = BundleToc(
            variants: [BundleToc.Variant(apks: [.master("splits/base-master.apk"), .split("splits/base-en.apk", "config.en")])],
            assetModules: modules)
        var entries = [ContainerEntry("toc.pb", toc.build())]
        var data: [String: Data] = [:]
        var seed = 300
        let paths = ["splits/base-master.apk", "splits/base-en.apk"] + modules.flatMap { $0.slices.map(\.path) }
        for path in paths where data[path] == nil {
            seed += 1
            data[path] = bytes(seed, count: 12_011)
            entries.append(ContainerEntry(path, data[path]!))
        }
        return (entries, data)
    }

    @Test func theApksOfAnInstallTimePackAreSentInTheSessionOfTheApp() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let bundle = packBundle([
            .installTime("levels", [.slice("asset-slices/levels-master.apk", "levels", master: true), .slice("asset-slices/levels-more.apk", "levels.config.more")]),
        ])
        let url = try ContainerFixture.write(bundle.entries, name: "Packs.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await guest(on: adbd).installApk(at: url, update: { _ in }) == .installed)
        let session = try #require(adbd.committedSessions.first)
        #expect(adbd.committedSessions.count == 1)
        // The base first, its split and the pack's two slices, each under a name of its own.
        #expect(session.parts.count == 4 && session.parts.first?.name == "base.apk")
        #expect(session.parts.first?.digest == digest(try #require(bundle.data["splits/base-master.apk"])))
        for path in ["splits/base-en.apk", "asset-slices/levels-master.apk", "asset-slices/levels-more.apk"] {
            #expect(session.parts.contains { $0.digest == digest(bundle.data[path]!) }, "\(path)")
        }
        #expect(Set(session.parts.map(\.name)).count == 4)
        #expect(adbd.openSessions.isEmpty && adbd.abandonedSessions.isEmpty)
    }

    @Test func aPackThatIsDeliveredLaterIsNotSentAndTheResultIsNotPlainlyInstalled() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let bundle = packBundle([
            .installTime("levels", [.slice("asset-slices/levels-master.apk", "levels", master: true)]),
            .onDemand("extra", [.slice("asset-slices/extra-master.apk", "extra", master: true)]),
            .fastFollow("later", [.slice("asset-slices/later-master.apk", "later", master: true)]),
        ])
        let url = try ContainerFixture.write(bundle.entries, name: "Packs.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await guest(on: adbd).installApk(at: url, update: { _ in }) == .installedDataNotPlaced)
        let session = try #require(adbd.committedSessions.first)
        #expect(session.parts.count == 3)
        #expect(session.parts.contains { $0.digest == digest(bundle.data["asset-slices/levels-master.apk"]!) })
        for path in ["asset-slices/extra-master.apk", "asset-slices/later-master.apk"] {
            #expect(!session.parts.contains { $0.digest == digest(bundle.data[path]!) }, "\(path)")
        }
    }

    @Test func anInstallTimeSliceForAnotherProcessorTheGuestHasIsInstalledWithTheAppAndNotDroppedAfterMatching() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.abiList = "arm64-v8a,armeabi-v7a"
        // The app has an arm64 split; the install-time slice is for armeabi-v7a, which the guest has as well.
        let toc = BundleToc(
            variants: [BundleToc.Variant(apks: [.master("splits/base-master.apk"), .split("splits/base-arm64_v8a.apk", "config.arm64_v8a", abi: 3)])],
            assetModules: [.installTime("req", [.slice("asset-slices/req-master.apk", "req", master: true, abi: 2)])])
        let required = bytes(77, count: 12_011)
        let url = try ContainerFixture.write([
            ContainerEntry("toc.pb", toc.build()), ContainerEntry("splits/base-master.apk", bytes(71, count: 12_011)),
            ContainerEntry("splits/base-arm64_v8a.apk", bytes(72, count: 12_011)), ContainerEntry("asset-slices/req-master.apk", required),
        ], name: "Packs.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await guest(on: adbd).installApk(at: url, update: { _ in }) == .installed)
        let session = try #require(adbd.committedSessions.first)
        #expect(session.parts.count == 3 && session.parts.contains { $0.digest == digest(required) })
    }

    @Test func anInstallTimeSliceForATextureFormatRefusesTheFileBeforeASessionIsMade() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        // Two slices that exclude each other: the install does not know the guest's formats, so it installs neither.
        let bundle = packBundle([
            .installTime("textures", [
                .slice("asset-slices/astc.apk", "textures.astc", texture: (9, 10)), .slice("asset-slices/etc2.apk", "textures.etc2", texture: (10, 9)),
            ]),
        ])
        let url = try ContainerFixture.write(bundle.entries, name: "Packs.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == InstallRefusal.damaged.message)
        #expect(!adbd.servicesOpened.contains { $0.contains("install-create") })
        #expect(adbd.committedSessions.isEmpty && adbd.openSessions.isEmpty && adbd.abandonedSessions.isEmpty)
    }

    // MARK: Cancel and the reading of the file

    @Test func aCancelPendingWhenTheFileIsFirstReadEndsTheInstallAsTheCancelEvenForAFileThatWouldBeRefused() async throws {
        // Read to its end, each of these is refused. The cancel is raised by the update that names the file
        // (the first one only asks for it, and a cancel there ends the coordinated read before any of the
        // install runs), once the file is open and before it is read: what reports it to the reading is the
        // checkpoint, so the install ends as the cancel and not as the refusal.
        let notAnArchive = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-Cut.xapk")
        try bytes(9, count: 6_000).write(to: notAnArchive)
        let noApp = try ContainerFixture.write([ContainerEntry("readme.txt", bytes(3, count: 500))], name: "Empty.apks")
        for url in [notAnArchive, noApp] {
            defer { try? FileManager.default.removeItem(at: url) }
            let adbd = DiscardingAdbd()
            defer { adbd.shutdown() }
            let operations = try guest(on: adbd)
            let box = TaskBox()
            let task = Task {
                try await operations.installApk(at: url, update: { [name = url.lastPathComponent] in
                    if $0.detail?.hasPrefix(name) == true { box.cancel() }
                })
            }
            box.task = task
            let result = await task.result
            guard case let .failure(error) = result else {
                Issue.record("installed: \(url.lastPathComponent)")
                continue
            }
            #expect(error is CancellationError, "\(url.lastPathComponent): \(error)")
            #expect(adbd.connectionCount == 0, "\(url.lastPathComponent)")
        }
    }

    // MARK: Small things

    @Test func anApkIsAnAppByItsExtensionWhateverTheFormOfIt() {
        for name in ["a.apk", "a.apks", "a.xapk", "a.apkm", "A.APKM"] { #expect(GuestFileKind(name: name) == .app, "\(name)") }
        #expect(GuestFileKind(name: "a.zip") == .archive && GuestFileKind(name: "a.aab") == .other)
    }
}

/// Reaches the fake adbd as it does and tells `onRequest` the text of each message the client writes, as it
/// reaches the transport and before any of it is written.
private nonisolated struct InterceptingAdbConnector: AdbConnector {
    let base: DiscardingAdbd
    let onRequest: @Sendable (String) -> Void

    func connect() throws -> any AdbTransport {
        InterceptingAdbTransport(base: try base.connect(), onRequest: onRequest)
    }
}

private nonisolated final class InterceptingAdbTransport: AdbTransport, @unchecked Sendable {
    private let base: any AdbTransport
    private let onRequest: @Sendable (String) -> Void

    init(base: any AdbTransport, onRequest: @escaping @Sendable (String) -> Void) {
        self.base = base
        self.onRequest = onRequest
    }

    func readExactly(_ count: Int) throws -> Data { try base.readExactly(count) }

    func writeAll(_ data: Data) throws {
        // An open request: the header's binary bytes in front of the service string.
        if data.count < 4096 { onRequest(String(decoding: data, as: UTF8.self)) }
        try base.writeAll(data)
    }

    func shutdown() { base.shutdown() }
}
