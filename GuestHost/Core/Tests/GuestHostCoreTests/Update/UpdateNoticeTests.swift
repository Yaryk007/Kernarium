// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// A store in memory.
@MainActor
final class MemoryUpdateStore: UpdateStore {
    /// Every record written, in order.
    private(set) var writes: [UpdateRecord] = []
    var record = UpdateRecord() { didSet { writes.append(record) } }
    var checks = true
}

/// A fetcher that answers as told and counts its reads.
final class FakeUpdateFetcher: UpdateFetching, @unchecked Sendable {
    private let lock = NSLock()
    private var answers: [UpdateFetch]
    private(set) var addresses: [URL] = []
    /// Suspends every read until `release()`.
    private var gate: CheckedContinuation<Void, Never>?
    private var held = false

    init(_ answers: [UpdateFetch] = []) { self.answers = answers }

    var count: Int { lock.withLock { addresses.count } }

    func push(_ answer: UpdateFetch) { lock.withLock { answers.append(answer) } }

    func hold() { lock.withLock { held = true } }

    func release() {
        let waiting = lock.withLock { () -> CheckedContinuation<Void, Never>? in
            held = false
            defer { gate = nil }
            return gate
        }
        waiting?.resume()
    }

    func fetch(_ address: URL) async -> UpdateFetch {
        lock.withLock { addresses.append(address) }
        if lock.withLock({ held }) {
            await withCheckedContinuation { continuation in
                let resume = lock.withLock { () -> Bool in
                    if held { gate = continuation; return false }
                    return true
                }
                if resume { continuation.resume() }
            }
        }
        return lock.withLock { answers.isEmpty ? .failed : answers.removeFirst() }
    }
}

/// Ticks on the main actor and keeps the longest time between two ticks, so a
/// test can tell whether something held the main actor.
@MainActor
private final class Heartbeat {
    private var last = ContinuousClock.now
    private(set) var longest = Duration.zero
    var running = true

    func run() async {
        while running {
            let now = ContinuousClock.now
            longest = max(longest, now - last)
            last = now
            try? await Task.sleep(for: .milliseconds(5))
        }
    }
}

/// The notice as an application drives it, on a clock and a network of the tests' own.
@MainActor
struct UpdateNoticeTests {
    private typealias Fixture = UpdateFixture

    private let day: TimeInterval = 24 * 60 * 60
    private let app = UpdateNotice.App(name: "Andarium", installedBuild: 15, installedLabel: "1.0 (15)")
    private let configuration = UpdateConfiguration(
        address: UpdateFixture.source, appIdentifier: UpdateFixture.appIdentifier)

    private final class Clock: @unchecked Sendable {
        var now = Date(timeIntervalSince1970: 1_800_000_000)
        func advance(_ seconds: TimeInterval) { now.addTimeInterval(seconds) }
    }

    /// What the notice's closures record and read: the waits it served, the
    /// links it opened and the schemes the device answers for.
    private final class State: @unchecked Sendable {
        var waits: [Duration] = []
        var cancelWait = false
        /// A wait that takes a moment, so that two calls overlap in it.
        var slowWait = false
        var opened: [URL] = []
        var installed: Set<String> = []
    }

    private final class Harness {
        let store = MemoryUpdateStore()
        let fetcher: FakeUpdateFetcher
        let clock = Clock()
        let state = State()
        let notice: UpdateNotice

        @MainActor
        init(
            app: UpdateNotice.App, configuration: UpdateConfiguration, answers: [UpdateFetch], host: UpdateHost = .device,
            os: UpdateOSVersion = UpdateFixture.os, store prepared: ((MemoryUpdateStore) -> Void)? = nil
        ) {
            fetcher = FakeUpdateFetcher(answers)
            prepared?(store)
            let state = state
            let clock = clock
            notice = UpdateNotice(
                configuration: configuration, app: app, store: store, fetcher: fetcher, host: host, os: os,
                now: { clock.now },
                wait: { duration in
                    state.waits.append(duration)
                    if state.cancelWait { throw CancellationError() }
                    if state.slowWait { try await Task.sleep(for: .milliseconds(100)) }
                },
                canOpen: { url in state.installed.contains(url.scheme ?? "") },
                open: { url in state.opened.append(url) })
        }
    }

    private func file(_ builds: [(String, String)] = [("1.1", "16")], notes: Bool = false) -> UpdateFetch {
        let versions = builds.map { Fixture.entry(version: $0.0, build: $0.1) }
        let block = notes
            ? "{ \"format\": 1, \"apps\": { \"com.example.andarium\": { \"versions\": { \"16\": { \"notesURL\": \"https://example.com/releases/andarium-1.1\" } } } } }"
            : nil
        return .data(Fixture.file(apps: [Fixture.app(versions: versions)], block: block))
    }

    private func harness(
        _ answers: [UpdateFetch], host: UpdateHost = .device, app: UpdateNotice.App? = nil,
        store: ((MemoryUpdateStore) -> Void)? = nil
    ) -> Harness {
        Harness(app: app ?? self.app, configuration: configuration, answers: answers, host: host, store: store)
    }

    // MARK: Reading

    @Test func aReadOffersTheNewVersionAndKeepsItForTheNextLaunch() async {
        let h = harness([file()])
        #expect(h.notice.offer == nil)
        #expect(!h.notice.showsCard)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer?.build == 16)
        #expect(h.notice.showsCard)
        #expect(h.fetcher.addresses == [Fixture.source])
        // A new launch reads what was kept, with no read.
        let next = harness([]) { $0.record = h.store.record }
        #expect(next.notice.offer?.build == 16)
        #expect(next.notice.showsCard)
        #expect(next.fetcher.count == 0)
    }

    @Test func theReadWaitsAFewSecondsAfterHomeShows() async {
        let h = harness([file()])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.state.waits == [.seconds(3)])
    }

    @Test func atMostOneReadInADay() async {
        let h = harness([file(), file([("1.2", "17")])])
        await h.notice.readWhenHomeShows(moment: { true })
        h.clock.advance(day - 1)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 1)
        #expect(h.notice.offer?.build == 16)
        h.clock.advance(1)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 2)
        #expect(h.notice.offer?.build == 17)
    }

    @Test func aReadThatFailedAlsoWaitsForTheNextDay() async {
        let h = harness([.failed, file()])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer == nil)
        h.clock.advance(3600)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 1, "no retry within the day")
        h.clock.advance(day)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 2)
        #expect(h.notice.offer?.build == 16)
    }

    @Test func aReadThatNeverStartedDoesNotUseUpTheDay() async {
        let h = harness([.notTried, file()])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.store.record.attempted == nil)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 2)
        #expect(h.notice.offer?.build == 16)
    }

    @Test func aFailedReadChangesNothingThatIsShown() async {
        let h = harness([file(), .failed])
        await h.notice.readWhenHomeShows(moment: { true })
        h.clock.advance(day)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer?.build == 16)
        #expect(h.store.record.found?.build == 16)
    }

    @Test func aFileThatIsNoAnswerChangesNothingThatIsShownAndUsesUpTheDay() async {
        let h = harness([file(), .data(Data("<html>".utf8)), .data(Fixture.file(apps: [Fixture.app(versions: [Fixture.entry()])], block: "{ \"format\": 2 }"))])
        await h.notice.readWhenHomeShows(moment: { true })
        h.clock.advance(day)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 2)
        #expect(h.notice.offer?.build == 16)
        h.clock.advance(day)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer?.build == 16)
        #expect(h.store.record.attempted == h.clock.now)
    }

    /// The day is used up before the body is looked at, so a body that ends the
    /// process cannot make every launch read it again.
    @Test func theDayIsUsedUpBeforeTheBodyIsDecoded() async {
        let h = harness([file()])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.store.writes.count == 2)
        #expect(h.store.writes.first?.attempted == h.clock.now)
        #expect(h.store.writes.first?.found == nil, "the stamp is stored before the answer is known")
        #expect(h.store.writes.last?.found?.build == 16)
    }

    @Test func aBodyThatIsNotUTF8IsNoAnswerAndUsesUpTheDay() async {
        let bad = Data("{".utf8) + Data([0xFF]) + Data("\"apps\": []}".utf8)
        let h = harness([.data(bad), file()])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer == nil)
        #expect(h.store.record.attempted == h.clock.now)
        h.clock.advance(3600)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 1, "the day is used up")
    }

    /// A body of the largest size that is slow to decode (each element fails)
    /// must not hold the main actor, which draws Home.
    @Test func aLargeBodyIsDecodedOffTheMainActor() async {
        let body = Data(("{\"apps\": [" + String(repeating: "{},", count: 340_000) + "{}]}").utf8)
        #expect(body.count < UpdateFile.largestBytes)
        let h = harness([.data(body)])
        let beat = Heartbeat()
        let ticking = Task { await beat.run() }
        await h.notice.readWhenHomeShows(moment: { true })
        // The ticker records a long gap when it next runs, so let it.
        try? await Task.sleep(for: .milliseconds(50))
        beat.running = false
        await ticking.value
        #expect(h.notice.offer == nil)
        #expect(h.store.record.attempted != nil)
        #expect(beat.longest < .milliseconds(300), "the main actor was held for \(beat.longest)")
    }

    @Test func aTamperedKeptVersionShowsNothing() throws {
        let kept = UpdateVersion(
            version: "1.1", build: 16, released: UpdateDay(year: 2026, month: 11, day: 20), sizeBytes: 1,
            appFile: URL(string: "https://example.com/a.ipa")!)
        let text = String(
            decoding: try JSONEncoder().encode(UpdateRecord(attempted: nil, found: kept, hiddenBuild: nil)), as: UTF8.self)
        let tampered = text.replacingOccurrences(
            of: "\"version\":\"1.1\"", with: "\"version\":\"Visit evil.test now for the update!\"")
        #expect(tampered != text)
        let record = try JSONDecoder().decode(UpdateRecord.self, from: Data(tampered.utf8))
        let h = harness([]) { $0.record = record }
        #expect(h.notice.offer == nil)
        #expect(!h.notice.showsCard)
    }

    @Test func theInstalledVersionKeepsItsBuildOnTheSameLine() {
        let app = UpdateNotice.App(name: "Andarium", installedBuild: 15, installedLabel: "1.0 (15)")
        #expect(app.installedLabel == "1.0\u{A0}(15)")
    }

    @Test func aWithdrawnVersionGoesAwayAtTheNextAnswer() async {
        let h = harness([file(), .data(Fixture.file(apps: [Fixture.app(versions: [Fixture.entry(version: "1.0", build: "15")])]))])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer?.build == 16)
        h.clock.advance(day)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer == nil)
        #expect(!h.notice.showsCard)
    }

    @Test func theMomentIsAskedOnceTheWaitIsOverAndNoReadHappensIfItHasPassed() async {
        let h = harness([file()])
        var asked = 0
        await h.notice.readWhenHomeShows(moment: { asked += 1; return false })
        #expect(asked == 1)
        #expect(h.fetcher.count == 0)
        #expect(h.store.record.attempted == nil)
        // The wait was served before the moment was asked.
        #expect(h.state.waits == [.seconds(3)])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 1)
    }

    @Test func aTaskCancelledDuringTheWaitMakesNoRead() async {
        let h = harness([file()])
        h.state.cancelWait = true
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 0)
        #expect(h.store.record.attempted == nil)
    }

    @Test func noReadWhileTheSwitchIsOffAndNothingIsShown() async {
        let h = harness([file(), file()])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.showsCard)
        h.notice.setChecks(false)
        #expect(h.store.checks == false)
        #expect(h.notice.offer == nil)
        #expect(!h.notice.showsCard)
        h.clock.advance(5 * day)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 1)
        h.notice.setChecks(true)
        #expect(h.store.checks == true)
        #expect(h.notice.offer?.build == 16, "the last answer returns with the switch")
    }

    @Test func theSwitchIsOnUntilThePersonTurnsItOff() {
        let h = harness([])
        #expect(h.notice.checks)
        let off = harness([]) { $0.checks = false }
        #expect(!off.notice.checks)
    }

    @Test func twoReadsAtOnceMakeOne() async {
        let h = harness([file()])
        h.fetcher.hold()
        let first = Task { await h.notice.readWhenHomeShows(moment: { true }) }
        while h.fetcher.count == 0 { try? await Task.sleep(for: .milliseconds(5)) }
        #expect(h.notice.reading)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 1)
        h.fetcher.release()
        await first.value
        #expect(!h.notice.reading)
        #expect(h.notice.offer?.build == 16)
    }

    @Test func twoCallsThatBothWaitMakeOneRead() async {
        let h = harness([file(), file()])
        h.state.slowWait = true
        let notice = h.notice
        let first = Task { await notice.readWhenHomeShows(moment: { true }) }
        let second = Task { await notice.readWhenHomeShows(moment: { true }) }
        await first.value
        await second.value
        #expect(h.fetcher.count == 1)
        #expect(h.notice.offer?.build == 16)
    }

    @Test func aClockSetBackDoesNotBlockReadsForever() async {
        let h = harness([file(), file()])
        await h.notice.readWhenHomeShows(moment: { true })
        h.clock.advance(-10 * day)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.fetcher.count == 2)
    }

    // MARK: What is offered

    @Test func aBuildAtOrBelowTheInstalledOneIsNeverShownEvenFromTheKeptAnswer() async {
        let stale = UpdateVersion(
            version: "1.0", build: 15, released: UpdateDay(year: 2026, month: 11, day: 15), sizeBytes: 1,
            appFile: URL(string: "https://example.com/a.ipa")!)
        let h = harness([]) { $0.record = UpdateRecord(attempted: Date(), found: stale) }
        #expect(h.notice.offer == nil)
        #expect(!h.notice.showsCard)
    }

    @Test func installingTheUpdateTakesTheCardAway() async {
        let h = harness([file()])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.showsCard)
        let updated = harness([], app: UpdateNotice.App(name: "Andarium", installedBuild: 16, installedLabel: "1.1 (16)")) {
            $0.record = h.store.record
        }
        #expect(updated.notice.offer == nil)
        #expect(!updated.notice.showsCard)
    }

    @Test func aKeptVersionNeedingANewerIOSIsNotShown() async {
        let h = harness([file()])
        await h.notice.readWhenHomeShows(moment: { true })
        let older = Harness(
            app: app, configuration: configuration, answers: [], host: .device, os: UpdateOSVersion(major: 25, minor: 9)
        ) { $0.record = h.store.record }
        #expect(older.notice.offer == nil)
    }

    @Test func aKeptVersionWhoseLinkTheBuildNoLongerAcceptsIsNotShown() async {
        let h = harness([file()])
        await h.notice.readWhenHomeShows(moment: { true })
        let moved = UpdateConfiguration(
            address: URL(string: "https://elsewhere.example.net/source.json")!, appIdentifier: UpdateFixture.appIdentifier)
        let other = Harness(app: app, configuration: moved, answers: []) { $0.record = h.store.record }
        #expect(other.notice.offer == nil)
    }

    @Test func aKeptReleasePageThatIsNoLongerAcceptedCostsOnlyItsRow() async {
        let h = harness([file(notes: true)])
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer?.notes != nil)
        let narrowed = UpdateConfiguration(
            address: UpdateFixture.source, appIdentifier: UpdateFixture.appIdentifier)
        var record = h.store.record
        let found = record.found!
        record.found = UpdateVersion(
            version: found.version, build: found.build, released: found.released, sizeBytes: found.sizeBytes,
            appFile: found.appFile, notes: URL(string: "https://evil.example.net/x")!)
        let other = Harness(app: app, configuration: narrowed, answers: []) { $0.record = record }
        #expect(other.notice.offer?.build == 16)
        #expect(other.notice.offer?.notes == nil)
    }

    @Test func aRecordOfAnotherShapeIsAnEmptyOne() {
        let suite = "UpdateNoticeTests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        defaults.set(Data("not json".utf8), forKey: DefaultsUpdateStore.recordKey)
        let store = DefaultsUpdateStore(defaults: defaults)
        #expect(store.record == UpdateRecord())
        #expect(store.checks == true)
        store.checks = false
        #expect(DefaultsUpdateStore(defaults: defaults).checks == false)
        let version = UpdateVersion(
            version: "1.1", build: 16, released: UpdateDay(year: 2026, month: 11, day: 20), sizeBytes: 5,
            appFile: URL(string: "https://example.com/a.ipa")!)
        store.record = UpdateRecord(attempted: Date(timeIntervalSince1970: 100), found: version, hiddenBuild: 15)
        #expect(DefaultsUpdateStore(defaults: defaults).record.found == version)
        #expect(DefaultsUpdateStore(defaults: defaults).record.hiddenBuild == 15)
    }

    // MARK: Closing and skipping

    @Test func closingTheCardHidesThatBuildOnlyAndInfoKeepsTheUpdate() async {
        let h = harness([file(), file([("1.2", "17")])])
        await h.notice.readWhenHomeShows(moment: { true })
        h.notice.hideCard()
        #expect(!h.notice.showsCard)
        #expect(h.notice.offer?.build == 16, "Info still lists the update")
        #expect(h.store.record.hiddenBuild == 16)
        // The next launch keeps it hidden.
        let next = harness([]) { $0.record = h.store.record }
        #expect(!next.notice.showsCard)
        #expect(next.notice.offer?.build == 16)
        // The next version brings the card back.
        h.clock.advance(day)
        await h.notice.readWhenHomeShows(moment: { true })
        #expect(h.notice.offer?.build == 17)
        #expect(h.notice.showsCard)
    }

    @Test func closingWithNothingOfferedChangesNothing() {
        let h = harness([])
        h.notice.hideCard()
        #expect(h.store.record == UpdateRecord())
    }

    // MARK: The ways to update

    @Test func theToolsAreTheOnesTheDeviceHas() {
        let h = harness([])
        #expect(h.notice.tools == [])
        h.state.installed = ["altstore"]
        #expect(h.notice.tools == [.altStore])
        h.state.installed = ["altstore", "sidestore"]
        #expect(h.notice.tools == [.altStore, .sideStore])
    }

    @Test(arguments: [UpdateHost.mac, UpdateHost.vision])
    func aMacAndAVisionProOfferNoToolEvenWithOneInstalled(host: UpdateHost) {
        let h = harness([], host: host)
        h.state.installed = ["altstore", "sidestore"]
        #expect(h.notice.tools == [])
    }

    @Test func aToolOpensTheLinkMadeFromItsTemplate() async {
        let h = harness([file()])
        await h.notice.readWhenHomeShows(moment: { true })
        h.notice.update(with: .altStore)
        h.notice.update(with: .sideStore)
        #expect(h.state.opened.map(\.absoluteString) == [
            "altstore://install?url=https%3A%2F%2Fexample.com%2Freleases%2Fandarium-1.1-16.ipa",
            "sidestore://install?url=https%3A%2F%2Fexample.com%2Freleases%2Fandarium-1.1-16.ipa",
        ])
    }

    @Test func theAppFileAndTheReleasePageOpenInTheBrowser() async {
        let h = harness([file(notes: true)])
        await h.notice.readWhenHomeShows(moment: { true })
        h.notice.openAppFile()
        h.notice.openReleasePage()
        #expect(h.state.opened.map(\.absoluteString) == [
            "https://example.com/releases/andarium-1.1-16.ipa", "https://example.com/releases/andarium-1.1",
        ])
    }

    @Test func withNoReleasePageNothingOpens() async {
        let h = harness([file()])
        await h.notice.readWhenHomeShows(moment: { true })
        h.notice.openReleasePage()
        #expect(h.state.opened.isEmpty)
    }

    @Test func withNothingOfferedNothingOpens() {
        let h = harness([])
        h.notice.openAppFile()
        h.notice.openReleasePage()
        h.notice.update(with: .altStore)
        #expect(h.state.opened.isEmpty)
    }

    @Test func nothingElseOfTheFileIsEverOpened() async {
        // A file full of other links: only the app file's and the page's are ever opened.
        let data = Fixture.file(
            apps: [Fixture.app(versions: [Fixture.entry(build: "16")],
                extra: "\"iconURL\": \"https://example.com/i.png\", \"screenshotURLs\": [\"https://example.com/s.png\"]")],
            extra: "\"website\": \"https://example.com/\", \"news\": [{ \"url\": \"https://example.com/n\" }]")
        let h = harness([.data(data)])
        await h.notice.readWhenHomeShows(moment: { true })
        h.notice.openAppFile()
        h.notice.openReleasePage()
        h.notice.update(with: .altStore)
        #expect(h.state.opened.count == 2)
        #expect(h.state.opened.allSatisfy { $0.absoluteString.contains("andarium-1.1-16.ipa") })
    }
}
