// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import CryptoKit
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// What Settings' Install APK does with an app built for a much older Android:
/// the package manager's own answer is the signal, the file goes again with the
/// flag only after the person says so, and nothing else can send the flag.
/// The first part runs the real install against a fake adbd; the second plays
/// the model with a stub for the runtime; the third shows the alert.
private let refusalLine =
    "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION: App package must target at least SDK version 24, but found 7]\n"

/// A file whose bytes tell one place from another (a sparse file reads as
/// zeros), of a size that is not a multiple of the install's pieces.
private func patternedApk(bytes: Int = 600_001, name: String = "Gears.apk") throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-\(name)")
    var data = Data(count: bytes)
    for index in 0..<bytes { data[index] = UInt8(truncatingIfNeeded: index &* 31 &+ index >> 8) }
    try data.write(to: url)
    return url
}

/// The SHA-256 of the file's bytes as the fake adbd writes one (hex).
private func fileDigest(_ url: URL) -> String? {
    guard let data = try? Data(contentsOf: url) else { return nil }
    return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

/// An adb identity for the production path; the fake adbd never asks it to sign.
func throwawayIdentity() throws -> AdbKeyStore.Identity {
    var failure: Unmanaged<CFError>?
    let attributes: [CFString: Any] = [
        kSecAttrKeyType: kSecAttrKeyTypeRSA, kSecAttrKeySizeInBits: 2048, kSecAttrIsPermanent: false,
    ]
    guard let key = SecKeyCreateRandomKey(attributes as CFDictionary, &failure) else {
        throw failure?.takeRetainedValue() ?? CocoaError(.coderInvalidValue)
    }
    return AdbKeyStore.Identity(privateKey: key, publicKeyLine: "AAAA")
}

@Suite(.serialized)
struct LegacyApkInstallOperationTests {
    private func install(
        _ url: URL, on adbd: DiscardingAdbd, bypass: Bool = false, hostFree: @escaping @Sendable () -> UInt64 = { .max }
    ) async throws {
        try await AdbOperationRunner.run(identity: nil, clock: GuestRunClock(), connector: adbd) { session in
            try ApkInstallOperation.run(
                session: session, coordinator: FileCoordinatorBox(), url: url, device: "iPad",
                bypassLowTargetSdkBlock: bypass, hostFree: hostFree, update: { _ in })
        }
    }

    private func outcome(_ work: () async throws -> Void) async -> Result<Void, Error> {
        do { return .success(try await work()) } catch { return .failure(error) }
    }

    @Test func theFirstAttemptIsTheOrdinaryInstallAndTheRefusalIsTheSignal() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.installOutputs = [refusalLine]
        let url = try patternedApk()
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await install(url, on: adbd) }
        guard case let .failure(error) = result, let refused = error as? LegacyApkRefused else {
            Issue.record("expected the too-old refusal, got \(result)")
            return
        }
        #expect(refused.target == LegacyTarget(appLevel: 7, requiredLevel: 24))
        // Without the flag: the service is today's, word for word.
        let service = try #require(adbd.servicesOpened.last)
        #expect(service == PackageInstallCommand.service(size: 600_001))
        #expect(!service.contains("bypass"))
        // The words of an unasked caller are the ordinary ones.
        #expect(refused.localizedDescription == PackageInstallMessages.message(forCode: "FAILED_DEPRECATED_SDK_VERSION"))
    }

    @Test func theSameBytesGoAgainWithTheFlagAndNothingElseChanged() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.installOutputs = [refusalLine, "Success\n"]
        let url = try patternedApk()
        defer { try? FileManager.default.removeItem(at: url) }
        _ = await outcome { try await install(url, on: adbd) }
        try await install(url, on: adbd, bypass: true)
        let services = adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") }
        #expect(services == [
            PackageInstallCommand.service(size: 600_001),
            PackageInstallCommand.service(size: 600_001, bypassLowTargetSdkBlock: true),
        ])
        let plain = services[0].split(separator: "\0", omittingEmptySubsequences: false)
        let flagged = services[1].split(separator: "\0", omittingEmptySubsequences: false)
        #expect(flagged.filter { $0 != "--bypass-low-target-sdk-block" } == plain)
        let digests = adbd.installDigests
        #expect(digests.count == 2 && digests[0] == digests[1])
        #expect(digests.last == fileDigest(url))
        #expect(adbd.installBytes == 2 * 600_001)
    }

    @Test func aRefusalWithTheFlagIsAnOrdinaryFailureNotASecondQuestion() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.installOutputs = [refusalLine]
        let url = try patternedApk()
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await install(url, on: adbd, bypass: true) }
        guard case let .failure(error) = result else {
            Issue.record("expected a failure")
            return
        }
        #expect(!(error is LegacyApkRefused))
        #expect((error as? AdbFailure)?.message == PackageInstallMessages.message(forCode: "FAILED_DEPRECATED_SDK_VERSION"))
    }

    @Test func everyOtherRefusalIsTheOrdinaryFailureWhateverTheFlag() async throws {
        let url = try patternedApk()
        defer { try? FileManager.default.removeItem(at: url) }
        for (code, line) in [
            ("FAILED_VERSION_DOWNGRADE", "Failure [INSTALL_FAILED_VERSION_DOWNGRADE: Downgrade detected]\n"),
            ("FAILED_NO_MATCHING_ABIS", "Failure [INSTALL_FAILED_NO_MATCHING_ABIS: Failed to extract native libraries, res=-113]\n"),
            ("FAILED_OLDER_SDK", "Failure [INSTALL_FAILED_OLDER_SDK: Requires newer sdk version #40]\n"),
        ] {
            for bypass in [false, true] {
                let adbd = DiscardingAdbd()
                defer { adbd.shutdown() }
                adbd.installOutputs = [line]
                let result = await outcome { try await install(url, on: adbd, bypass: bypass) }
                guard case let .failure(error) = result else {
                    Issue.record("\(code) installed")
                    continue
                }
                #expect(!(error is LegacyApkRefused), "\(code)")
                #expect((error as? AdbFailure)?.message == PackageInstallMessages.message(forCode: code), "\(code)")
            }
        }
    }

    /// The package manager that gives up at the first write (a streamed install of a large
    /// file) still answers the same way, and the retry sends the whole file.
    @Test func aLargeFileThatThePackageManagerRefusesEarlyStillAsksAndTheRetrySendsItWhole() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.failsAtOnce = true
        adbd.installOutputs = [refusalLine]
        let bytes = 24 << 20
        let url = try patternedApk(bytes: bytes, name: "Large.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        let first = await outcome { try await install(url, on: adbd) }
        guard case let .failure(error) = first, error is LegacyApkRefused else {
            Issue.record("expected the too-old refusal, got \(first)")
            return
        }
        adbd.failsAtOnce = false
        adbd.installOutputs = ["Success\n"]
        try await install(url, on: adbd, bypass: true)
        // The retry sent the whole file, byte for byte; the refused attempt
        // gave up after its first write.
        #expect(adbd.installDigests.count == 2)
        #expect(adbd.installDigests.last == fileDigest(url))
        #expect(adbd.installBytes >= UInt64(bytes))
        #expect(adbd.servicesOpened.last == PackageInstallCommand.service(size: bytes, bypassLowTargetSdkBlock: true))
    }

    /// What the model gets from the production call, `presented` and all: the refusal reaches it
    /// as itself, and with the flag it does not.
    @Test func theRefusalReachesTheModelThroughTheProductionInstallCall() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.installOutputs = [refusalLine, refusalLine]
        let url = try patternedApk()
        defer { try? FileManager.default.removeItem(at: url) }
        let operations = AdbGuestOperations(
            identity: try throwawayIdentity(), clock: GuestRunClock(), device: "iPad", connector: adbd,
            hostFree: { .max }, run: GuestRunMark.random())
        let first = await outcome { try await operations.installApk(at: url, update: { _ in }) }
        guard case let .failure(plain) = first, let refused = plain as? LegacyApkRefused else {
            Issue.record("expected the too-old refusal, got \(first)")
            return
        }
        #expect(refused.target == LegacyTarget(appLevel: 7, requiredLevel: 24))
        let second = await outcome { try await operations.installApk(at: url, bypassLowTargetSdkBlock: true, update: { _ in }) }
        guard case let .failure(flagged) = second else {
            Issue.record("expected a failure with the flag")
            return
        }
        #expect(!(flagged is LegacyApkRefused))
        #expect((flagged as? AdbFailure)?.message == PackageInstallMessages.message(forCode: "FAILED_DEPRECATED_SDK_VERSION"))
        let services = adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") }
        #expect(services == [
            PackageInstallCommand.service(size: 600_001),
            PackageInstallCommand.service(size: 600_001, bypassLowTargetSdkBlock: true),
        ])
    }

    @Test func theRoomChecksRunAgainOnTheRetry() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.installOutputs = [refusalLine, "Success\n"]
        let url = try patternedApk()
        defer { try? FileManager.default.removeItem(at: url) }
        let reads = Counter()
        _ = await outcome { try await install(url, on: adbd, hostFree: { reads.bump(); return .max }) }
        try await install(url, on: adbd, bypass: true, hostFree: { reads.bump(); return .max })
        #expect(reads.value == 2)
        #expect(adbd.shellCommands == ["df -Pk '/data'", "df -Pk '/data'"])
        // The machine has no room by the time of the retry: the ordinary words, and nothing sent.
        adbd.guestFreeKiB = 400 << 10
        let before = adbd.installBytes
        let result = await outcome { try await install(url, on: adbd, bypass: true) }
        guard case let .failure(error) = result else {
            Issue.record("expected a shortage")
            return
        }
        #expect((error as? AdbFailure)?.message.contains("Resize") == true)
        #expect(adbd.installBytes == before)
    }

    @Test func aGuestThatIsGoneOnTheRetryIsTheOrdinaryNotRunningFailure() async throws {
        let url = try patternedApk()
        defer { try? FileManager.default.removeItem(at: url) }
        let gone = GoneConnector()
        let result = await outcome {
            try await AdbOperationRunner.run(
                identity: nil, clock: GuestRunClock(), connector: gone, connectLimit: .seconds(1)
            ) { session in
                try ApkInstallOperation.run(
                    session: session, coordinator: FileCoordinatorBox(), url: url, device: "iPad",
                    bypassLowTargetSdkBlock: true, hostFree: { .max }, update: { _ in })
            }
        }
        guard case let .failure(error) = result else {
            Issue.record("expected a failure")
            return
        }
        let shown = AdbOperationRunner.presented(error, activity: .installApp)
        #expect((shown as? AdbFailure)?.message == AdbFailure(.refused, activity: .installApp).message)
    }
}

private nonisolated final class Counter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0
    func bump() { lock.withLock { count += 1 } }
    var value: Int { lock.withLock { count } }
}

/// A guest that refuses every connection, as one that has been stopped does.
private nonisolated struct GoneConnector: AdbConnector {
    func connect() throws -> any AdbTransport { throw AdbError.refused }
}

// MARK: The model

/// The install as the model plays it with a stub for the runtime: the overlay behind the
/// question, the question's answers, and what each leaves.
@Suite(.serialized)
@MainActor
struct LegacyApkInstallModelTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let target = LegacyTarget(appLevel: 7, requiredLevel: 24)

    private func running() -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.phase = .running(.normal)
        return (model, driver)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    private func apk() throws -> URL { try patternedApk(bytes: 3 << 20, name: "Gears.apk") }

    /// The first attempt is refused for its version; the second does what `second` does.
    private func refusing(
        _ driver: StubDriver, then second: @escaping @Sendable (@Sendable (BusyUpdate) -> Void) async throws -> Void = { _ in }
    ) {
        let target = target
        driver.installAttempt = { bypass, update in
            if !bypass {
                update(BusyUpdate(progress: 0.8, detail: "The virtual machine is installing the app\u{2026}", cancellable: false))
                // Android takes its time over the install before it answers.
                try await Task.sleep(for: .milliseconds(60))
                throw LegacyApkRefused(target: target)
            }
            try await second(update)
        }
    }

    @Test func aTooOldRefusalAsksBehindTheOverlayAndInstallAnywaySendsTheSameFileWithTheFlag() async throws {
        let (model, driver) = running()
        refusing(driver) { update in
            update(BusyUpdate(progress: 0.4))
            update(BusyUpdate(progress: 0.8, detail: "The virtual machine is installing the app\u{2026}", cancellable: false))
        }
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        // The question: the versions, no error code, and the overlay waits behind it, with the
        // Cancel that the alert covers.
        #expect(model.legacyInstallQuestion?.target == target)
        #expect(model.busy?.title == "Installing app" && model.busy?.cancel != nil && model.busy?.result == nil)
        #expect(model.busy?.detail.hasSuffix("Gears.apk \u{00B7} 3 MiB") == true)
        #expect(model.alert == nil && !model.guestHasKeys)
        #expect(driver.installAttempts == [false])
        model.answerLegacyInstall(installAnyway: true)
        #expect(model.legacyInstallQuestion == nil)
        #expect(await eventually { model.busy?.result != nil })
        #expect(driver.installAttempts == [false, true])
        // The same file, the same install: the ordinary success line.
        #expect(Set(driver.installURLs) == [url])
        #expect(model.busy?.title == "App installed")
        #expect(model.busy?.detail.hasSuffix("Gears.apk is installed in the virtual machine.") == true)
        #expect(model.busy?.result?.failed == false && model.alert == nil)
        #expect(model.guestHasKeys)
    }

    @Test func theBarStartsOverAndTheOverlayCancelsAgainForTheSecondAttempt() async throws {
        let (model, driver) = running()
        let gate = AttemptGate()
        refusing(driver) { update in
            update(BusyUpdate(progress: 0.3))
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
        }
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        #expect(model.busy?.progress == 0.8)
        model.answerLegacyInstall(installAnyway: true)
        #expect(await eventually { driver.installAttempts == [false, true] })
        #expect(await eventually { model.busy?.progress == 0.3 })
        #expect(model.busy?.cancel != nil)
        gate.open()
        #expect(await eventually { model.busy?.result != nil })
        // And that Cancel stops the second attempt as it stops any install.
        let (again, driver2) = running()
        refusing(driver2) { _ in while true { try await Task.sleep(for: .milliseconds(5)) } }
        again.installPicked(url)
        #expect(await eventually { again.legacyInstallQuestion != nil })
        again.answerLegacyInstall(installAnyway: true)
        #expect(await eventually { driver2.installAttempts == [false, true] && again.busy?.cancel != nil })
        again.busy?.cancel?.action()
        #expect(await eventually { again.busy == nil })
        #expect(again.alert == nil && again.legacyInstallQuestion == nil)
    }

    @Test func cancelEndsAsACancelledInstallWithNoBannerAndNoSecondAttempt() async throws {
        let (model, driver) = running()
        refusing(driver)
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        model.answerLegacyInstall(installAnyway: false)
        #expect(await eventually { model.busy == nil })
        #expect(model.alert == nil && model.legacyInstallQuestion == nil)
        #expect(driver.installAttempts == [false])
        // Nothing is left to answer twice.
        model.answerLegacyInstall(installAnyway: true)
        try await Task.sleep(for: .milliseconds(50))
        #expect(driver.installAttempts == [false] && model.busy == nil)
    }

    /// An alert that cannot present (another presentation is in its way) must not leave the
    /// person with an overlay they cannot close: the overlay's Cancel ends the install.
    @Test func theOverlaysCancelWhileTheQuestionIsUpEndsTheInstallWithNoBannerAndNoSecondAttempt() async throws {
        let (model, driver) = running()
        refusing(driver)
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        let cancel = try #require(model.busy?.cancel)
        cancel.action()
        #expect(await eventually { model.busy == nil })
        #expect(model.alert == nil && model.legacyInstallQuestion == nil)
        #expect(driver.installAttempts == [false])
        // The answer that never came has nothing left to resume.
        model.answerLegacyInstall(installAnyway: true)
        try await Task.sleep(for: .milliseconds(50))
        #expect(driver.installAttempts == [false] && model.busy == nil && model.alert == nil)
    }

    @Test func aSecondAttemptThatFailsShowsTheOrdinaryWordsAndAsksNothingMore() async throws {
        let (model, driver) = running()
        refusing(driver) { _ in
            throw AdbFailure(message: PackageInstallMessages.message(forCode: "FAILED_NO_MATCHING_ABIS"))
        }
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        model.answerLegacyInstall(installAnyway: true)
        #expect(await eventually { model.busy?.result != nil })
        #expect(model.busy?.title == "App not installed")
        #expect(model.busy?.detail == PackageInstallMessages.message(forCode: "FAILED_NO_MATCHING_ABIS"))
        #expect(model.busy?.result?.failed == true && model.alert == nil && model.legacyInstallQuestion == nil)
        #expect(driver.installAttempts == [false, true])
    }

    @Test func aSecondAttemptRefusedAsTooOldAgainIsAnOrdinaryFailureNotASecondQuestion() async throws {
        let (model, driver) = running()
        let target = target
        driver.installAttempt = { _, _ in throw LegacyApkRefused(target: target) }
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        model.answerLegacyInstall(installAnyway: true)
        #expect(await eventually { model.busy?.result != nil })
        #expect(model.busy?.title == "App not installed")
        #expect(model.busy?.detail == PackageInstallMessages.message(forCode: "FAILED_DEPRECATED_SDK_VERSION"))
        #expect(model.legacyInstallQuestion == nil && driver.installAttempts == [false, true])
    }

    @Test func aDifferentFirstFailureNeverAsks() async throws {
        let (model, driver) = running()
        driver.installAttempt = { _, _ in
            throw AdbFailure(message: PackageInstallMessages.message(forCode: "FAILED_VERSION_DOWNGRADE"))
        }
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        #expect(model.busy?.detail == "A newer version of this app is already installed.")
        #expect(model.legacyInstallQuestion == nil && model.alert == nil && driver.installAttempts == [false])
    }

    @Test func anOrdinaryInstallNeverCarriesTheFlag() async throws {
        let (model, driver) = running()
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        #expect(driver.installAttempts == [false] && model.legacyInstallQuestion == nil)
        #expect(model.busy?.title == "App installed")
    }

    @Test func theGuestEndingWhileTheQuestionIsUpTakesTheQuestionAndTheOverlayAway() async throws {
        let (model, driver) = running()
        refusing(driver)
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.legacyInstallQuestion == nil && model.busy == nil && model.alert == nil)
        try await Task.sleep(for: .milliseconds(100))
        // Nothing was sent again, and nothing is waiting for an answer.
        #expect(driver.installAttempts == [false] && model.busy == nil && model.alert == nil)
        model.answerLegacyInstall(installAnyway: true)
        try await Task.sleep(for: .milliseconds(50))
        #expect(driver.installAttempts == [false])
    }

    /// What the model does with the access a picked file carries: it is taken once, before
    /// anything is read, held while the person decides, and ended once, with the install.
    @Test func thePickedFilesAccessIsHeldAcrossTheQuestionAndEndsOnceWithTheInstall() async throws {
        let (model, driver) = running()
        let events = EventLog()
        model.accessPickedFileStub = { _ in
            events.add("start")
            return { events.add("end") }
        }
        driver.installAttempt = { bypass, _ in
            events.add(bypass ? "second attempt" : "first attempt")
            if !bypass { throw LegacyApkRefused(target: LegacyTarget(appLevel: 7, requiredLevel: 24)) }
        }
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        // The person takes a while: the access is still held.
        try await Task.sleep(for: .milliseconds(100))
        #expect(events.all == ["start", "first attempt"])
        model.answerLegacyInstall(installAnyway: true)
        #expect(await eventually { model.busy?.result != nil })
        #expect(events.all == ["start", "first attempt", "second attempt", "end"])
        #expect(driver.installURLs == [url, url])
    }

    @Test func theAccessEndsOnceWhenTheQuestionIsCancelledAndWhenTheGuestEnds() async throws {
        for ending in ["Cancel", "the guest ends"] {
            let (model, driver) = running()
            refusing(driver)
            let events = EventLog()
            model.accessPickedFileStub = { _ in
                events.add("start")
                return { events.add("end") }
            }
            let url = try apk()
            defer { try? FileManager.default.removeItem(at: url) }
            model.installPicked(url)
            #expect(await eventually { model.legacyInstallQuestion != nil }, "\(ending)")
            #expect(events.all == ["start"], "\(ending)")
            if ending == "Cancel" {
                model.answerLegacyInstall(installAnyway: false)
            } else {
                model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
            }
            #expect(await eventually { model.busy == nil && events.all.count == 2 }, "\(ending)")
            try await Task.sleep(for: .milliseconds(50))
            #expect(events.all == ["start", "end"], "\(ending)")
        }
    }

    @Test func noOtherOperationRaisesTheQuestion() async throws {
        let (model, driver) = running()
        // A bug report that fails, an alert, a refused file: none of them asks.
        driver.bugReport = { _ in throw AdbFailure(message: "no") }
        model.createBugReport()
        #expect(await eventually { model.alert != nil })
        #expect(model.legacyInstallQuestion == nil)
        model.alert = nil
        let notApk = try patternedApk(bytes: 100, name: "photo.zip")
        defer { try? FileManager.default.removeItem(at: notApk) }
        model.installPicked(notApk)
        #expect(model.legacyInstallQuestion == nil && driver.installAttempts.isEmpty)
    }
}

private nonisolated final class EventLog: @unchecked Sendable {
    private let lock = NSLock()
    private var events: [String] = []
    func add(_ event: String) { lock.withLock { events.append(event) } }
    var all: [String] { lock.withLock { events } }
}

private nonisolated final class AttemptGate: @unchecked Sendable {
    private let lock = NSLock()
    private var opened = false
    func open() { lock.withLock { opened = true } }
    var isOpen: Bool { lock.withLock { opened } }
}

// MARK: The alert

/// The question as the person meets it: a system alert over the screens, with the words the
/// catalog has, two buttons that VoiceOver finds by their names, and the answers they give.
@Suite(.serialized)
@MainActor
struct LegacyApkInstallAlertTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    /// A running guest. The person normally reached Settings through the side keys, which ends
    /// their tip, so the tests leave the keys open; `leavingTheTip` keeps them closed, as on a
    /// folding iPhone's cover or standing deck, where the tip can be up.
    private func running(leavingTheTip: Bool = false) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        model.launchApplied = true
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.phase = .running(.normal)
        if !leavingTheTip { model.session.sideKeysOpen = true }
        return (model, driver)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(15)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private func alert(over controller: UIViewController) -> UIAlertController? {
        var top: UIViewController? = controller
        while let next = top?.presentedViewController { top = next }
        return top as? UIAlertController
    }

    /// The view an accessibility user finds by `label`: what VoiceOver reads and activates.
    private func control(labelled label: String, in view: UIView) -> UIView? {
        if view.accessibilityLabel == label, view.isAccessibilityElement || view is UIControl { return view }
        for sub in view.subviews { if let found = control(labelled: label, in: sub) { return found } }
        return nil
    }

    /// What VoiceOver's double tap does to `element`: the element is asked to activate, and a control
    /// that declines it is tapped. Whether it took the activation is the result: an element that is
    /// no control and declines it has not been activated, whatever it did. Before iOS 27 the label is
    /// on the text inside a button, and the views around it up to the alert answer in its place.
    private func activate(_ element: UIView) -> Bool {
        var candidate: UIView? = element
        while let view = candidate {
            if view.accessibilityActivate() { return true }
            if let control = view as? UIControl {
                control.sendActions(for: .touchUpInside)
                return true
            }
            guard #unavailable(iOS 27.0) else { return false }
            candidate = view.superview
        }
        return false
    }

    /// The double tap on the alert's button `title`: the element is asked to activate. Before iOS 27
    /// UIKit's view for an alert's button declines that and does nothing, and VoiceOver then taps the
    /// button where it is; a hosted test has no finger, so the button's action runs, as the tap
    /// makes UIKit run it.
    private func activate(_ element: UIView, titled title: String, on alert: UIAlertController) -> Bool {
        if activate(element) { return true }
        guard #unavailable(iOS 27.0) else { return false }
        guard let action = alert.actions.first(where: { $0.title == title }),
              let handler = action.value(forKey: "handler") as AnyObject? else { return false }
        unsafeBitCast(handler, to: (@convention(block) (UIAlertAction) -> Void).self)(action)
        return true
    }

    /// The views from `element` up to the window, as a failure names them.
    private func chain(_ element: UIView) -> String {
        var parts: [String] = []
        var candidate: UIView? = element
        while let view = candidate {
            parts.append("\(type(of: view))(label \(view.accessibilityLabel ?? "nil"), element \(view.isAccessibilityElement), control \(view is UIControl))")
            candidate = view.superview
        }
        return parts.joined(separator: " < ")
    }

    /// Raises the question for a file Android refuses for its version, with the real screens up.
    private func raise(
        _ model: AppModel, _ driver: StubDriver, over controller: AndariumHostingController<some View>
    ) async throws -> (UIAlertController, URL) {
        let target = LegacyTarget(appLevel: 7, requiredLevel: 24)
        driver.installAttempt = { bypass, _ in if !bypass { throw LegacyApkRefused(target: target) } }
        // No question, no alert.
        try await Task.sleep(for: .milliseconds(300))
        #expect(alert(over: controller) == nil)
        let url = try patternedApk(bytes: 3 << 20)
        model.installPicked(url)
        try #require(await eventually { alert(over: controller) != nil })
        return (try #require(alert(over: controller)), url)
    }

    @Test func theQuestionIsASystemAlertWithCancelAndInstallAnywayAndTheWordsOfTheCatalog() async throws {
        let (model, driver) = running()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        let (shown, url) = try await raise(model, driver, over: controller)
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(shown.preferredStyle == .alert)
        #expect(shown.title == LegacyInstallWords.title)
        #expect(shown.title == "Install this app anyway?")
        let message = try #require(shown.message)
        #expect(message.hasPrefix("This app was built for a much older version of Android. Android blocks apps like this"))
        #expect(message.contains("privacy and security protections"))
        #expect(message.contains("may not work properly"))
        #expect(message.contains("API level 7") && message.contains("API level 24"))
        #expect(!message.contains("INSTALL_") && !message.contains("FAILED") && !message.contains("VM"))
        // Two actions: Cancel, which has the cancel style (the bold default of an alert), and
        // Install anyway, which is a plain action and not a destructive one.
        let styles = Dictionary(uniqueKeysWithValues: shown.actions.map { ($0.title ?? "", $0.style) })
        #expect(styles == ["Cancel": .cancel, "Install anyway": .default])
        #expect(shown.actions.count == 2)
        // Cancel is the default action, the one an alert shows in bold. From iOS 27 UIKit names it the
        // alert's preferred action; before it the cancel style is all that makes it the bold one.
        if #available(iOS 27.0, *) {
            #expect(shown.preferredAction?.title == "Cancel")
        }
        // VoiceOver finds both buttons by name.
        #expect(control(labelled: "Cancel", in: shown.view) != nil)
        #expect(control(labelled: "Install anyway", in: shown.view) != nil)
        // The alert alone has not answered.
        #expect(model.legacyInstallQuestion != nil && driver.installAttempts == [false])
        model.answerLegacyInstall(installAnyway: false)
        try #require(await eventually { alert(over: controller) == nil })
        #expect(model.busy == nil && model.alert == nil)
    }

    @Test(.disabled(if: ProcessInfo.processInfo.isiOSAppOnMac, "an iPad app on a Mac takes no accessibility activation of an alert's buttons"))
    func installAnywayAndCancelEachAnswerFromTheAlertItself() async throws {
        for installAnyway in [false, true] {
            let (model, driver) = running()
            let lease = try await HostWindow.take()
            defer { lease.give() }
            let controller = AndariumHostingController(
                rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
            lease.window.rootViewController = controller
            let (shown, url) = try await raise(model, driver, over: controller)
            defer { try? FileManager.default.removeItem(at: url) }
            let title = installAnyway ? "Install anyway" : "Cancel"
            let button = try #require(control(labelled: title, in: shown.view), "\(title) is not on the alert")
            // What VoiceOver does with a double tap; a control that declines it is tapped.
            try #require(activate(button, titled: title, on: shown), "\(title) takes no activation: \(chain(button))")
            try #require(await eventually { model.legacyInstallQuestion == nil }, "\(title) answered nothing")
            if installAnyway {
                try #require(await eventually { model.busy?.result != nil })
                #expect(driver.installAttempts == [false, true] && model.busy?.title == "App installed")
            } else {
                try #require(await eventually { model.busy == nil })
                #expect(driver.installAttempts == [false] && model.alert == nil)
            }
            try #require(await eventually { alert(over: controller) == nil })
        }
    }

    /// The side keys' tip is a popover over the screens, up whenever the keys are closed. It
    /// is not for the test to close: the question has to come up whatever else is on screen.
    @Test func theQuestionPresentsWhileTheSideKeysTipIsUp() async throws {
        let (model, driver) = running(leavingTheTip: true)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        try #require(await eventually { controller.presentedViewController != nil }, "the tip never came up")
        #expect(alert(over: controller) == nil)
        let target = LegacyTarget(appLevel: 7, requiredLevel: 24)
        driver.installAttempt = { bypass, _ in if !bypass { throw LegacyApkRefused(target: target) } }
        let url = try patternedApk(bytes: 3 << 20)
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        try #require(await eventually { model.legacyInstallQuestion != nil })
        try #require(await eventually { alert(over: controller) != nil }, "the question never presented")
        model.answerLegacyInstall(installAnyway: false)
        try #require(await eventually { alert(over: controller) == nil && model.busy == nil })
    }

    /// An alert of the screens (a failure, a notice) can be up when the answer arrives. The
    /// question presents once it is gone, and the overlay's Cancel serves meanwhile.
    @Test(.disabled(if: ProcessInfo.processInfo.isiOSAppOnMac, "an iPad app on a Mac takes no accessibility activation of an alert's buttons"))
    func theQuestionPresentsOnceAnotherAlertIsGone() async throws {
        let (model, driver) = running()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        let target = LegacyTarget(appLevel: 7, requiredLevel: 24)
        driver.installAttempt = { bypass, _ in
            if !bypass {
                try await Task.sleep(for: .milliseconds(600))
                throw LegacyApkRefused(target: target)
            }
        }
        let url = try patternedApk(bytes: 3 << 20)
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        model.alert = AppAlert(title: "Display rotation unavailable", message: "Rotate the device.")
        try #require(await eventually { alert(over: controller)?.title == "Display rotation unavailable" })
        try #require(await eventually { model.legacyInstallQuestion != nil })
        try await Task.sleep(for: .milliseconds(500))
        #expect(alert(over: controller)?.title == "Display rotation unavailable")
        model.alert = nil
        try #require(await eventually { alert(over: controller)?.title == LegacyInstallWords.title }, "the question never presented")
        let shown = try #require(alert(over: controller))
        // The alert is on screen before its buttons are built.
        try #require(await eventually { control(labelled: "Cancel", in: shown.view) != nil }, "no Cancel on the alert")
        let cancel = try #require(control(labelled: "Cancel", in: shown.view))
        try #require(activate(cancel, titled: "Cancel", on: shown), "Cancel takes no activation: \(chain(cancel))")
        try #require(await eventually { alert(over: controller) == nil && model.busy == nil })
        #expect(driver.installAttempts == [false])
    }

    /// The model can answer for the person: the overlay's Cancel serves while the alert is not up,
    /// and the guest ending takes the question away. The alert goes with the question, whatever it
    /// is doing at that moment, after it was offered again behind another alert too
    /// (`presentUntilShown`). A negative delay answers as the other alert goes.
    @Test(arguments: [-1, 0, 150, 600])
    func theQuestionGoesWithTheModelsAnswerWhateverTheAlertIsDoing(afterMilliseconds: Int) async throws {
        let (model, driver) = running()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        let target = LegacyTarget(appLevel: 7, requiredLevel: 24)
        driver.installAttempt = { bypass, _ in
            if !bypass {
                try await Task.sleep(for: .milliseconds(600))
                throw LegacyApkRefused(target: target)
            }
        }
        let url = try patternedApk(bytes: 3 << 20)
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        model.alert = AppAlert(title: "Display rotation unavailable", message: "Rotate the device.")
        try #require(await eventually { alert(over: controller)?.title == "Display rotation unavailable" })
        try #require(await eventually { model.legacyInstallQuestion != nil })
        try await Task.sleep(for: .milliseconds(500))
        model.alert = nil
        if afterMilliseconds >= 0 {
            try #require(await eventually { alert(over: controller)?.title == LegacyInstallWords.title }, "the question never presented")
            try await Task.sleep(for: .milliseconds(afterMilliseconds))
        }
        model.answerLegacyInstall(installAnyway: false)
        // Whatever the alert had reached, it goes, and stays gone while the loop that offered it ends.
        try #require(await eventually { alert(over: controller) == nil && model.busy == nil }, "the alert stays up after the answer")
        try await Task.sleep(for: .seconds(2))
        #expect(alert(over: controller) == nil, "the alert came back after the answer")
        #expect(driver.installAttempts == [false])
    }

    @Test func theNumbersAreLeftOutWhenTheAnswerNamesNone() {
        let plain = LegacyInstallWords.message(for: LegacyTarget())
        #expect(!plain.contains("API level"))
        #expect(LegacyInstallWords.message(for: LegacyTarget(appLevel: 7)) == plain)
        #expect(LegacyInstallWords.message(for: LegacyTarget(appLevel: 7, requiredLevel: 24)).hasPrefix(plain))
    }

    @Test(arguments: ["ja", "zh-Hans", "zh-Hant"])
    func theWordsAreInEveryLanguageTheApplicationShips(language: String) {
        for key in [
            "Install this app anyway?", "Install anyway", "Cancel",
            "This app was built for a much older version of Android. Android blocks apps like this because they predate its newer privacy and security protections. If you install it anyway, it may not work properly.",
        ] {
            let translated = String(
                localized: LocalizedStringResource(String.LocalizationValue(key), locale: Locale(identifier: language), bundle: .main))
            #expect(!translated.isEmpty && translated != key, "\(language) leaves \(key) in English")
            #expect(!translated.contains("VM"), "\(language): \(translated)")
        }
    }
}
