// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
import UIKit
@testable import Andarium

/// The application's hand-over of a start to the runtime, on the real
/// LiveRuntime with the files' preparation held where a test wants it: a start
/// the person stopped leaves its preparation to finish and owns the guest's
/// files until it does, an old boot's console cannot mark the next, the clock
/// the notice counts by stands still while the application is away, and a
/// stop is made off the main actor until the run has ended.
@MainActor @Suite(.serialized)
struct StartHandoverTests {
    private struct Rig {
        let model: AppModel
        let runtime: LiveRuntime
        let directory: URL
    }

    /// An application model on a real runtime over empty storage. A start's
    /// files' preparation is `held`, so it does nothing to the disk.
    private func rig() throws -> Rig {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let release = try SystemFolder.release(in: systemDirectory)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: release.identity, releaseDirectory: systemDirectory)
        let host = HostDevice(
            name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 41_000_000_000)
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1)
        model.launchApplied = true
        let runtime = LiveRuntime(
            model: model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        model.driver = runtime
        model.snapshotDriver = runtime
        return Rig(model: model, runtime: runtime, directory: directory)
    }

    private func until(_ seconds: Double = 8, _ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while !condition() && ContinuousClock.now < deadline { try? await Task.sleep(for: .milliseconds(10)) }
        return condition()
    }

    /// Holds each preparation where it begins, and counts how many are in
    /// hand at once.
    @MainActor private final class Hold {
        private var waits: [CheckedContinuation<Void, Never>] = []
        private(set) var entered = 0
        private(set) var inHand = 0
        private(set) var mostInHand = 0
        /// Whether the preparation that is released ends with a failure.
        var failing = true
        /// Told how many preparations have begun, as each one does.
        var onEnter: ((Int) -> Void)?
        func prepare() async throws {
            entered += 1
            onEnter?(entered)
            inHand += 1
            mostInHand = max(mostInHand, inHand)
            await withCheckedContinuation { waits.append($0) }
            inHand -= 1
            if failing { throw CancellationError() }
        }
        func release() {
            let all = waits
            waits = []
            for wait in all { wait.resume() }
        }
    }

    private func layOut(_ runtime: LiveRuntime) {
        let view = GuestMetalView(frame: .zero, device: nil)
        runtime.viewDidLayout(view, GuestDisplayGeometry(pixelSize: CGSize(width: 100, height: 100), dotsPerInch: 160))
    }

    // MARK: One preparation at a time

    @Test func aStopDuringPreparationKeepsTheNextStartBehindIt() async throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        let hold = Hold()
        hold.failing = false
        rig.runtime.prepareFilesOverride = { _ in try await hold.prepare() }
        defer {
            hold.failing = true
            hold.release()
        }
        rig.model.start(.normal)
        layOut(rig.runtime)
        try #require(await until { hold.entered == 1 })
        rig.model.stopStart()
        #expect(rig.model.phase == .home)
        // A start asked for while the stopped one's preparation still writes the files.
        rig.model.start(.normal)
        layOut(rig.runtime)
        try? await Task.sleep(for: .milliseconds(400))
        #expect(hold.entered == 1 && hold.mostInHand == 1, "a second preparation began beside the first")
        // The first ends, and the second goes on after it: the same files, one at a time.
        hold.release()
        try #require(await until { hold.entered == 2 }, "the queued start never began")
        #expect(hold.mostInHand == 1)
        hold.failing = true
        hold.release()
    }

    @Test func aStartAfterAFailedOneBegins() async throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        let hold = Hold()
        rig.runtime.prepareFilesOverride = { _ in try await hold.prepare() }
        rig.model.start(.normal)
        layOut(rig.runtime)
        try #require(await until { hold.entered == 1 })
        hold.release()
        try #require(await until { rig.model.phase == .failed })
        // The failed start left nothing that owns a launch.
        #expect(!rig.runtime.activity.runtimeActive)
        rig.model.phase = .home
        rig.model.start(.normal)
        layOut(rig.runtime)
        #expect(await until { hold.entered == 2 }, "a start after a failed one never began")
        hold.release()
    }

    @Test func whatChangesTheFilesWaitsForAPreparationThatWasStopped() async throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        let hold = Hold()
        hold.failing = false
        rig.runtime.prepareFilesOverride = { _ in try await hold.prepare() }
        rig.model.start(.normal)
        layOut(rig.runtime)
        try #require(await until { hold.entered == 1 })
        rig.model.stopStart()
        #expect(rig.model.phase == .home)
        let finished = Finished()
        let erase = Task { @MainActor in
            try? await rig.runtime.erase { _ in }
            finished.mark("erase")
        }
        let restore = Task { @MainActor in
            try? await rig.runtime.restoreUpdate()
            finished.mark("restore")
        }
        let create = Task { @MainActor in
            _ = try? await rig.runtime.create(Guest.fresh(on: rig.model.host)) { _ in }
            finished.mark("create")
        }
        try? await Task.sleep(for: .milliseconds(500))
        #expect(finished.names.isEmpty, "\(finished.names) changed the guest's files under a preparation")
        hold.release()
        await erase.value
        await restore.value
        await create.value
        #expect(Set(finished.names) == ["erase", "restore", "create"])
    }

    @MainActor private final class Finished {
        private(set) var names: [String] = []
        func mark(_ name: String) { names.append(name) }
    }

    /// A guest whose update did not finish: the record says so, the snapshot
    /// holds the data the update started from, and `data.img` is what the
    /// update's start wrote since. A Restore puts the snapshot back.
    private func unfinishedUpdate(in directory: URL) throws -> URL {
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let current = try SystemFolder.release(in: systemDirectory).identity
        try #require(current.buildNumber > 1)
        let earlier = ReleaseIdentity(
            releaseID: "earlier-release", buildFingerprint: "earlier", buildNumber: current.buildNumber - 1,
            securityPatch: "2026-01-01", systemDiskSHA256: String(repeating: "a", count: 64),
            dataLayout: current.dataLayout, minimumGiB: current.minimumGiB, defaultGiB: current.defaultGiB,
            maximumGiB: current.maximumGiB)
        let files = InstanceFiles(directory: directory)
        try files.apply(files.decide(for: earlier), release: earlier)
        try Data("before-update".utf8).write(to: files.data)
        try files.apply(files.decide(for: current), release: current)
        try Data("after-update".utf8).write(to: files.data)
        return files.data
    }

    /// The review's sequence on the real storage: a start is stopped while its
    /// preparation runs, Restore waits for it, and a start is asked for while
    /// Restore waits. The start waits for Restore, which puts the data back
    /// before any preparation of the next start touches it.
    @Test func aStartAskedForWhileARestoreWaitsDoesNotOvertakeIt() async throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        let data = try unfinishedUpdate(in: rig.directory)
        let hold = Hold()
        hold.failing = false
        let finished = Finished()
        var dataWhenTheSecondBegan: String?
        var restoredWhenTheSecondBegan = false
        hold.onEnter = { count in
            guard count == 2 else { return }
            restoredWhenTheSecondBegan = finished.names.contains("restore")
            dataWhenTheSecondBegan = (try? String(contentsOf: data, encoding: .utf8))
        }
        rig.runtime.prepareFilesOverride = { _ in try await hold.prepare() }
        defer {
            hold.failing = true
            hold.release()
        }
        rig.model.start(.normal)
        layOut(rig.runtime)
        try #require(await until { hold.entered == 1 })
        rig.model.stopStart()
        let restore = Task { @MainActor in
            try? await rig.runtime.restoreUpdate()
            finished.mark("restore")
        }
        try? await Task.sleep(for: .milliseconds(200))
        #expect(finished.names.isEmpty, "Restore did not wait for the stopped preparation")
        rig.model.start(.normal)
        layOut(rig.runtime)
        try? await Task.sleep(for: .milliseconds(200))
        // The first ends: Restore goes first, then the start's own preparation.
        hold.release()
        await restore.value
        try #require(await until { hold.entered == 2 }, "the start never began after Restore")
        #expect(restoredWhenTheSecondBegan, "the start began its preparation before Restore had ended")
        #expect(dataWhenTheSecondBegan == "before-update", "Restore did not put the data back first")
        hold.failing = true
        hold.release()
    }

    /// The start that waits on a change of the files begins by itself when it
    /// ends, whatever the change was.
    @Test func aStartThatWaitedOnAnEraseBeginsWhenTheEraseEnds() async throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        let hold = Hold()
        hold.failing = false
        rig.runtime.prepareFilesOverride = { _ in try await hold.prepare() }
        defer {
            hold.failing = true
            hold.release()
        }
        rig.model.start(.normal)
        layOut(rig.runtime)
        try #require(await until { hold.entered == 1 })
        rig.model.stopStart()
        let erase = Task { @MainActor in try? await rig.runtime.erase { _ in } }
        try? await Task.sleep(for: .milliseconds(100))
        rig.model.start(.normal)
        layOut(rig.runtime)
        hold.release()
        await erase.value
        #expect(await until { hold.entered == 2 }, "the start never began after the erase")
    }

    // MARK: The console and the restart

    /// The loader delivers the console's text and the restart event on the
    /// main queue in the order the runtime made them: a recovery line the old
    /// boot printed, then the guest's restart. It marks the old boot only.
    @Test func anOldBootsRecoveryLineNeverMarksTheNextBoot() async throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        rig.model.phase = .booting(.normal)
        let sink = rig.runtime.makeConsoleSink()
        let finished = Finished()
        DispatchQueue.main.async { sink.feed(Data("init: starting service 'recovery'\n".utf8)) }
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                rig.runtime.guestRestarted(.began(restart: 1))
                finished.mark("restart")
            }
        }
        try #require(await until { finished.names == ["restart"] })
        // Whatever else is queued behind them has run too.
        try? await Task.sleep(for: .milliseconds(200))
        #expect(!rig.model.bootTracker.recovery && !rig.model.bootTracker.isReady,
            "the old boot's console marked the boot after its restart")
        // The new boot's own lines still count.
        sink.feed(Data("init: starting service 'recovery'\n".utf8))
        #expect(rig.model.bootTracker.recovery && rig.model.bootTracker.isReady)
    }

    /// The sink the runtime keeps for the run's console is told of the
    /// guest's restart, which begins another boot though the console printed
    /// no banner: the second boot's completion is its own.
    @Test func aRestartBeginsTheConsolesNextBootWithoutABanner() async throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        rig.model.phase = .booting(.normal)
        let counted = Counted()
        let sink = ConsoleSink { _, completed, _ in if completed { counted.add() } }
        rig.runtime.console = sink
        sink.feed(Data("init: init first stage started!\napexd: markBootCompleted() received by ApexService\n".utf8))
        #expect(counted.value == 1)
        rig.runtime.guestRestarted(.began(restart: 1))
        sink.feed(Data("init: processing action (sys.boot_completed=1) from (/x:1)\n".utf8))
        #expect(counted.value == 2, "the restart's boot completed without being counted")
    }

    private final class Counted: @unchecked Sendable {
        private let lock = NSLock()
        private var count = 0
        func add() { lock.withLock { count += 1 } }
        var value: Int { lock.withLock { count } }
    }

    // MARK: The first boot

    /// The guest's data disk exists and its record says the first boot did
    /// not complete (an earlier start ended before Android booted): the start
    /// the model begins is long by its nature, as the first start is.
    @Test func aGuestWhoseFirstBootDidNotCompleteStartsWithTheLongerTime() throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let current = try SystemFolder.release(in: systemDirectory).identity
        let files = InstanceFiles(directory: rig.directory)
        try files.apply(files.decide(for: current), release: current)
        try Data("data".utf8).write(to: files.data)
        #expect(rig.runtime.firstBootPending())
        rig.model.start(.normal)
        #expect(rig.model.slowStart.longByNature && rig.model.slowStart.patience == SlowStart.firstStartThreshold)
        rig.model.slowStart.finish()
        rig.model.phase = .home
        // Android booted: the next start is an ordinary one.
        try files.booted(release: current)
        #expect(!rig.runtime.firstBootPending())
        rig.model.start(.normal)
        #expect(!rig.model.slowStart.longByNature && rig.model.slowStart.patience == SlowStart.threshold)
    }

    // MARK: The clock

    /// The clock the notice counts by is the runtime's: it stands still while
    /// the application is away during a start's preparation, and is not the
    /// wall clock.
    @Test func theStartClockStandsStillWhileTheApplicationIsAway() async throws {
        let rig = try rig()
        defer { try? FileManager.default.removeItem(at: rig.directory) }
        let hold = Hold()
        rig.runtime.prepareFilesOverride = { _ in try await hold.prepare() }
        defer { hold.release() }
        rig.model.start(.normal)
        layOut(rig.runtime)
        try #require(await until { hold.entered == 1 })
        let running = rig.model.clock()
        try? await Task.sleep(for: .milliseconds(150))
        #expect(rig.model.clock() > running, "the clock does not run while the application is in front")
        NotificationCenter.default.post(name: UIApplication.didEnterBackgroundNotification, object: nil)
        try? await Task.sleep(for: .milliseconds(100))
        let held = rig.model.clock()
        try? await Task.sleep(for: .milliseconds(400))
        #expect(rig.model.clock() == held, "the clock ran while the application was away")
        NotificationCenter.default.post(name: UIApplication.willEnterForegroundNotification, object: nil)
        try? await Task.sleep(for: .milliseconds(200))
        #expect(rig.model.clock() > held, "the clock stayed held after the application came back")
    }

    // MARK: Stopping the runtime

    private final class Asked: @unchecked Sendable {
        private let lock = NSLock()
        private var count = 0
        private var onMain = false
        private let release = DispatchSemaphore(value: 0)
        /// Blocks the first request until released, as a runtime that waits on a restore does.
        func request(blocking: Bool) {
            lock.withLock {
                count += 1
                if Thread.isMainThread { onMain = true }
            }
            if blocking, lock.withLock({ count }) == 1 { release.wait() }
        }
        func free() { release.signal() }
        var times: Int { lock.withLock { count } }
        var askedOnMain: Bool { lock.withLock { onMain } }
    }

    @Test func aStopIsMadeOffTheMainActorAndAgainUntilTheRunEnds() async throws {
        let effects = LivePauseEffects()
        let stop = RunStop()
        let asked = Asked()
        #expect(stop.enter())
        stop.request()
        let before = ContinuousClock.now
        effects.deliverStop(stop, every: .milliseconds(100), settle: { true }) { asked.request(blocking: true) }
        // The call returns while the runtime's request is still waiting.
        #expect(ContinuousClock.now - before < .milliseconds(500))
        try #require(await until { asked.times == 1 })
        asked.free()
        // Made again: a request from before the runtime's run began is dropped by it.
        try #require(await until { asked.times >= 3 }, "the stop was not made again")
        #expect(!asked.askedOnMain, "the stop was made on the main thread")
        stop.end()
        try? await Task.sleep(for: .milliseconds(300))
        let after = asked.times
        try? await Task.sleep(for: .milliseconds(300))
        #expect(asked.times == after, "a run that ended was asked to stop")
    }

    /// What a stop asked of the runtime, in order, and on which thread.
    private final class Sequence: @unchecked Sendable {
        private let lock = NSLock()
        private var calls: [String] = []
        private var onMain = false
        func add(_ call: String) {
            lock.withLock {
                calls.append(call)
                if Thread.isMainThread { onMain = true }
            }
        }
        var all: [String] { lock.withLock { calls } }
        var askedOnMain: Bool { lock.withLock { onMain } }
    }

    @Test func aStopAsksTheGuestToWriteBackOnceBeforeTheHostStops() async throws {
        let effects = LivePauseEffects()
        let stop = RunStop()
        let sequence = Sequence()
        #expect(stop.enter())
        stop.request()
        effects.deliverStop(
            stop, every: .milliseconds(50), settle: { sequence.add("settle"); return true },
            request: { sequence.add("stop") })
        try #require(await until { sequence.all.count >= 3 }, "the stop was not made again")
        stop.end()
        try? await Task.sleep(for: .milliseconds(200))
        let calls = sequence.all
        #expect(calls.first == "settle", "the host stopped the guest before it was asked to write back")
        #expect(calls.filter { $0 == "settle" }.count == 1, "the guest was asked more than once")
        #expect(calls.dropFirst().allSatisfy { $0 == "stop" })
        #expect(!sequence.askedOnMain, "the write-back was asked on the main thread")
    }

    /// A guest that takes its time delays the host's stop, and does not drop it.
    @Test func theHostStopWaitsForTheWriteBackAndFollowsIt() async throws {
        let effects = LivePauseEffects()
        let stop = RunStop()
        let sequence = Sequence()
        let release = DispatchSemaphore(value: 0)
        #expect(stop.enter())
        stop.request()
        effects.deliverStop(
            stop, every: .milliseconds(50),
            settle: {
                sequence.add("settle began")
                release.wait()
                sequence.add("settle ended")
                return true
            }, request: { sequence.add("stop") })
        try #require(await until { sequence.all == ["settle began"] })
        try? await Task.sleep(for: .milliseconds(300))
        #expect(sequence.all == ["settle began"], "the host stopped the guest while it wrote back")
        release.signal()
        try #require(await until { sequence.all.count >= 3 }, "the host's stop never followed the write-back")
        #expect(Array(sequence.all.prefix(3)) == ["settle began", "settle ended", "stop"])
        stop.end()
    }

    /// The runtime's own write-back, with no guest running: the loader says
    /// there is none to ask, at once, and the stop is not held up. Asking nobody
    /// is not the run's one write-back, so the next delivery asks again.
    @Test func theWriteBackBeforeAStopCostsNothingWithNoGuest() {
        let stop = RunStop()
        let before = ContinuousClock.now
        let reached = LivePauseEffects.settleBeforeStop(for: stop)
        // A guest that never answered would hold it for `StopSettle.bound`.
        #expect(ContinuousClock.now - before < .seconds(3), "a stop with no guest waited for the write-back")
        #expect(!reached, "a write-back that found no guest counted as made")
    }

    /// The write-back of a run that has ended is dropped before it reaches the
    /// runtime, where it would ask whatever runs next.
    @Test func theWriteBackOfARunThatEndedAsksNoRuntime() {
        let stop = RunStop()
        stop.end()
        #expect(!LivePauseEffects.settleBeforeStop(for: stop))
    }

    /// A stop that finds no guest yet is made again: the write-back that
    /// reached one is the run's one, and the host's stop follows every ask.
    @Test func aStopWithNoGuestYetAsksAgainUntilOneIsReached() async throws {
        let effects = LivePauseEffects()
        let stop = RunStop()
        let sequence = Sequence()
        let answers = Sequence()
        #expect(stop.enter())
        stop.request()
        effects.deliverStop(
            stop, every: .milliseconds(50),
            settle: {
                sequence.add("settle")
                answers.add("ask")
                return answers.all.count >= 3
            }, request: { sequence.add("stop") })
        try #require(await until { sequence.all.count >= 8 }, "the stop was not made again")
        stop.end()
        try? await Task.sleep(for: .milliseconds(200))
        let calls = sequence.all
        #expect(calls.filter { $0 == "settle" }.count == 3, "the guest was asked \(calls)")
        #expect(Array(calls.prefix(6)) == ["settle", "stop", "settle", "stop", "settle", "stop"])
        #expect(calls.dropFirst(6).allSatisfy { $0 == "stop" })
    }

    /// A stop before the runtime runs has no guest to ask.
    @Test func aStopBeforeTheRuntimeRunsAsksNoGuest() async throws {
        let effects = LivePauseEffects()
        let stop = RunStop()
        let sequence = Sequence()
        stop.request()
        #expect(!stop.enter())
        effects.deliverStop(
            stop, every: .milliseconds(50), settle: { sequence.add("settle"); return true },
            request: { sequence.add("stop") })
        try? await Task.sleep(for: .milliseconds(300))
        #expect(sequence.all.isEmpty, "a run that never began was asked something")
        stop.end()
    }
}
