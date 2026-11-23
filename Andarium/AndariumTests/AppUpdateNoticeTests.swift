// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// What the update notice's pieces are made of in these tests: a version file
/// that was read a moment ago, so no read is due, on `example.com`, the only
/// address a test uses.
@MainActor
enum AppUpdateFixture {
    static let configuration = UpdateConfiguration(
        address: URL(string: "https://example.com/source.json")!, appIdentifier: "com.example.andarium")
    nonisolated static let appFile = URL(string: "https://example.com/downloads/andarium-1.1.ipa")!
    nonisolated static let page = URL(string: "https://example.com/releases/1.1")!

    nonisolated static func version(build: Int = 16, notes: URL? = URL(string: "https://example.com/releases/1.1")!) -> UpdateVersion {
        UpdateVersion(
            version: "1.\(build - 15)", build: build, released: UpdateDay(year: 2026, month: 11, day: 24),
            sizeBytes: 641_728_512, minimumOS: UpdateOSVersion(major: 26), appFile: appFile, notes: notes)
    }

    @MainActor final class Store: UpdateStore {
        var record: UpdateRecord
        var checks: Bool

        init(found: UpdateVersion? = AppUpdateFixture.version(), hidden: Int? = nil, checks: Bool = true) {
            record = UpdateRecord(attempted: Date(), found: found, hiddenBuild: hidden)
            self.checks = checks
        }
    }

    /// What the notice was asked to open.
    @MainActor final class Opener {
        var urls: [URL] = []
    }

    /// Nothing is read: a read is not due.
    struct NoRead: UpdateFetching {
        func fetch(_ address: URL) async -> UpdateFetch { .notTried }
    }

    static func notice(
        store: Store? = nil, host: UpdateHost = .device, installed: Set<String> = [], opener: Opener? = nil
    ) -> UpdateNotice {
        let store = store ?? Store()
        let opener = opener ?? Opener()
        return UpdateNotice(
            configuration: configuration,
            app: UpdateNotice.App(name: "Andarium", installedBuild: 15, installedLabel: "1.0 (15)"),
            store: store, fetcher: NoRead(), host: host, os: UpdateOSVersion(major: 27),
            canOpen: { url in url.scheme.map(installed.contains) ?? false },
            open: { opener.urls.append($0) })
    }
}

/// The update notice where a person meets it: Home's card first among the
/// notices, the detail the card opens, and Info's Updates section, as the
/// application draws them and VoiceOver reads them. SwiftUI publishes its
/// accessibility elements only while application accessibility is on (the
/// Simulator preference `ApplicationAccessibilityEnabled`, as
/// `Kernarium/scripts/run-ios-simulator-tests.sh` sets it), so a run without it
/// fails with that said. Nothing opens and nothing is read: the system's links
/// are replaced and no read is due.
@MainActor
@Suite(.serialized)
struct AppUpdateNoticeTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000)

    private func model(notice: UpdateNotice?) -> AppModel {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(
                version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages",
                appVersion: "1.0 (15)"),
            capabilities: .device, createRequirementBytes: 1)
        model.launchApplied = true
        model.driver = StubDriver(plan: .normal)
        model.phase = .home
        model.appUpdate = notice
        return model
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private func nodes(in node: NSObject, depth: Int = 0) -> [NSObject] {
        guard depth < 64 else { return [] }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        return [node] + children.flatMap { nodes(in: $0, depth: depth + 1) }
    }

    private func labels(in root: UIView) -> [String] { nodes(in: root).compactMap(\.accessibilityLabel) }

    /// The first element whose label starts with `prefix`; SwiftUI's elements
    /// carry no identifier in the process, so a row is found by its words.
    private func element(_ prefix: String, in root: UIView) -> NSObject? {
        nodes(in: root).first { $0.accessibilityLabel?.hasPrefix(prefix) == true }
    }

    /// Home on the application's window.
    private func hostHome(_ model: AppModel, in lease: HostWindow.Lease) -> UIViewController {
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        return controller
    }

    /// The detail alone, as Info's row pushes it or the card's sheet shows it.
    private func hostDetail(
        _ model: AppModel, in lease: HostWindow.Lease
    ) -> UIHostingController<some View> {
        let view = UpdateDetailSheet(notice: model.appUpdate!) { Color.clear.frame(height: 60) }
            .environment(model)
            .frame(width: 440, height: 1100)
        let controller = UIHostingController(rootView: view)
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    private func hostInfo(_ model: AppModel, in lease: HostWindow.Lease) -> UIHostingController<some View> {
        let view = InfoSheet(page: nil).environment(model).frame(width: 440, height: 1100)
        let controller = UIHostingController(rootView: view)
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    /// Waits until `words` are all on screen under `root`, so a run without
    /// application accessibility says so instead of failing on the first word.
    private func drawn(_ words: [String], in root: UIView) async throws {
        let shown = await eventually {
            root.setNeedsLayout()
            root.layoutIfNeeded()
            let spoken = labels(in: root)
            return words.allSatisfy { word in spoken.contains { $0.contains(word) } }
        }
        try #require(shown, "\(words) are not all drawn; is ApplicationAccessibilityEnabled on? \(labels(in: root))")
    }

    // MARK: Home's card

    @Test func theCardIsFirstAmongTheNoticesAndNamesBothVersions() async throws {
        let model = model(notice: AppUpdateFixture.notice())
        model.notice = .closedInBackground
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostHome(model, in: lease)
        try await drawn(["Update available", "iOS closed Andarium"], in: controller.view)
        let spoken = labels(in: controller.view)
        let card = try #require(spoken.firstIndex { $0.contains("Update available") }, "\(spoken)")
        let other = try #require(spoken.firstIndex { $0.contains("iOS closed Andarium") }, "\(spoken)")
        #expect(card < other, "the update card comes before the other notices: \(spoken)")
        // A version and its build stay on one line: a no-break space joins them.
        #expect(spoken[card].contains("Andarium 1.1\u{A0}(16) is out. You have 1.0\u{A0}(15)."), "\(spoken[card])")
        // The card is one element whose action is Get Update, as every Home card with a button is.
        let element = try #require(nodes(in: controller.view).first { $0.accessibilityLabel == spoken[card] })
        #expect(element.accessibilityCustomActions?.map(\.name) == ["Get Update"])
        #expect(spoken.contains("Later"), "\(spoken)")
    }

    @Test func homeShowsNoCardWithNoNoticeNoAnswerAnOffSwitchOrAPutAwayBuild() async throws {
        let cases: [(String, UpdateNotice?)] = [
            ("no notice", nil),
            ("no answer", AppUpdateFixture.notice(store: .init(found: nil))),
            ("switch off", AppUpdateFixture.notice(store: .init(checks: false))),
            ("put away", AppUpdateFixture.notice(store: .init(hidden: 16))),
            ("installed", AppUpdateFixture.notice(store: .init(found: AppUpdateFixture.version(build: 15)))),
        ]
        for (name, notice) in cases {
            let model = model(notice: notice)
            let lease = try await HostWindow.take()
            defer { lease.give() }
            let controller = hostHome(model, in: lease)
            try await drawn(["Virtual machine"], in: controller.view)
            try await Task.sleep(for: .milliseconds(500))
            #expect(!labels(in: controller.view).contains { $0.contains("Update available") }, "\(name)")
        }
    }

    @Test func closingTheCardPutsAwayThatBuildAndTheRecordKeepsIt() async throws {
        let store = AppUpdateFixture.Store()
        let notice = AppUpdateFixture.notice(store: store)
        let model = model(notice: notice)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostHome(model, in: lease)
        try await drawn(["Update available"], in: controller.view)
        let close = try #require(nodes(in: controller.view).first { $0.accessibilityLabel == "Later" })
        #expect(close.accessibilityActivate())
        #expect(await eventually { !notice.showsCard })
        #expect(store.record.hiddenBuild == 16)
        #expect(await eventually { !labels(in: controller.view).contains { $0.contains("Update available") } })
        // Info keeps the update, and the next build brings the card back.
        #expect(notice.offer?.build == 16)
        store.record.found = AppUpdateFixture.version(build: 17)
        #expect(AppUpdateFixture.notice(store: store).showsCard)
    }

    @Test func theCardsButtonOpensTheDetailInASheet() async throws {
        let model = model(notice: AppUpdateFixture.notice(installed: ["altstore"]))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostHome(model, in: lease)
        try await drawn(["Update available"], in: controller.view)
        let card = try #require(element("Update available", in: controller.view))
        #expect(card.accessibilityActivate())
        #expect(await eventually { model.sheet == .appUpdate })
        #expect(await eventually { controller.presentedViewController?.view.window != nil })
        let sheet = try #require(controller.presentedViewController)
        try await drawn(["Update with AltStore"], in: sheet.view)
        // Nothing of the card's own is behind a second sheet.
        try await drawn(["Skip This Version"], in: sheet.view)
    }

    // MARK: The detail

    @Test func theDetailOffersTheInstalledToolsTheAppFileAndTheReleasePage() async throws {
        let opener = AppUpdateFixture.Opener()
        let model = model(notice: AppUpdateFixture.notice(installed: ["altstore", "sidestore"], opener: opener))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostDetail(model, in: lease)
        try await drawn(["Update with AltStore", "Update with SideStore", "Download App File"], in: controller.view)
        let spoken = labels(in: controller.view)
        for word in ["Open Release Page", "Skip This Version", "Released", "Size", "Requires", "Installed"] {
            #expect(spoken.contains { $0.contains(word) }, "\(word) in \(spoken)")
        }
        #expect(!spoken.contains("How to update"), "the steps stand in for tools only: \(spoken)")
        let altStore = try #require(element("Update with AltStore", in: controller.view))
        #expect(altStore.accessibilityTraits.contains(.link))
        #expect(altStore.accessibilityActivate())
        #expect(await eventually { opener.urls.count == 1 })
        #expect(
            opener.urls.map(\.absoluteString)
                == ["altstore://install?url=https%3A%2F%2Fexample.com%2Fdownloads%2Fandarium-1.1.ipa"])
        let file = try #require(element("Download App File", in: controller.view))
        #expect(file.accessibilityActivate())
        let page = try #require(element("Open Release Page", in: controller.view))
        #expect(page.accessibilityActivate())
        #expect(await eventually { opener.urls.count == 3 })
        #expect(opener.urls.suffix(2) == [AppUpdateFixture.appFile, AppUpdateFixture.page])
    }

    @Test func aToolThatIsNotInstalledHasNoRow() async throws {
        let model = model(notice: AppUpdateFixture.notice(installed: ["sidestore"]))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostDetail(model, in: lease)
        try await drawn(["Update with SideStore", "Download App File"], in: controller.view)
        #expect(!labels(in: controller.view).contains { $0.contains("AltStore") })
        #expect(element("Update with AltStore", in: controller.view) == nil)
    }

    @Test(arguments: [UpdateHost.mac, UpdateHost.vision])
    func onAMacOrVisionProTheDetailHasNoToolRowsAndTheStepsInTheirPlace(host: UpdateHost) async throws {
        let opener = AppUpdateFixture.Opener()
        // The tools are installed, which the notice does not ask about on these hosts.
        let model = model(notice: AppUpdateFixture.notice(host: host, installed: ["altstore", "sidestore"], opener: opener))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostDetail(model, in: lease)
        try await drawn(["How to update", "Download App File", "Open Release Page"], in: controller.view)
        let spoken = labels(in: controller.view)
        #expect(!spoken.contains { $0.contains("AltStore") || $0.contains("SideStore") }, "\(spoken)")
        #expect(spoken.contains { $0.contains("Download the app file.") }, "\(spoken)")
        #expect(
            spoken.contains { $0.contains("Install it with the same app or computer and the same Apple ID as before.") },
            "\(spoken)")
        #expect(!spoken.contains { $0.contains("Sign it with") }, "the steps are two: \(spoken)")
        #expect(opener.urls.isEmpty)
    }

    @Test func aReleasePageTheFileDidNotNameHasNoRow() async throws {
        let store = AppUpdateFixture.Store(found: AppUpdateFixture.version(notes: nil))
        let model = model(notice: AppUpdateFixture.notice(store: store, installed: ["altstore"]))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostDetail(model, in: lease)
        try await drawn(["Download App File"], in: controller.view)
        #expect(element("Open Release Page", in: controller.view) == nil)
    }

    @Test func skipThisVersionPutsTheBuildAwayAndClosesTheSheet() async throws {
        let store = AppUpdateFixture.Store()
        let notice = AppUpdateFixture.notice(store: store)
        let model = model(notice: notice)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostHome(model, in: lease)
        try await drawn(["Update available"], in: controller.view)
        model.sheet = .appUpdate
        #expect(await eventually { controller.presentedViewController?.view.window != nil })
        let sheet = try #require(controller.presentedViewController)
        try await drawn(["Skip This Version"], in: sheet.view)
        let skip = try #require(element("Skip This Version", in: sheet.view))
        #expect(skip.accessibilityActivate())
        #expect(await eventually { store.record.hiddenBuild == 16 })
        #expect(await eventually { controller.presentedViewController == nil })
        #expect(!notice.showsCard)
    }

    @Test func aVersionWithdrawnWhileTheDetailIsOpenClosesIt() async throws {
        let notice = AppUpdateFixture.notice()
        let model = model(notice: notice)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostHome(model, in: lease)
        try await drawn(["Update available"], in: controller.view)
        model.sheet = .appUpdate
        #expect(await eventually { controller.presentedViewController?.view.window != nil })
        let sheet = try #require(controller.presentedViewController)
        try await drawn(["Skip This Version", "Done"], in: sheet.view)
        // What a read that ends now would do to a version the file has dropped.
        notice.setChecks(false)
        #expect(notice.offer == nil)
        #expect(await eventually { controller.presentedViewController == nil }, "the sheet, with its Done, is gone")
        #expect(model.sheet == nil)
    }

    @Test func aBuildAlreadyPutAwayHasNoSkipRow() async throws {
        let model = model(notice: AppUpdateFixture.notice(store: .init(hidden: 16)))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostDetail(model, in: lease)
        try await drawn(["Download App File"], in: controller.view)
        #expect(element("Skip This Version", in: controller.view) == nil)
    }

    // MARK: Info

    @Test func infoHasTheUpdateRowTheSwitchAndWhatTheCheckReads() async throws {
        let model = model(notice: AppUpdateFixture.notice())
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostInfo(model, in: lease)
        try await drawn(["Update Available", "Check for Updates"], in: controller.view)
        let spoken = labels(in: controller.view)
        #expect(
            spoken.contains {
                $0.contains("Checks once a day for a newer version. Sends no account, identifier or usage data.")
            }, "\(spoken)")
        let row = try #require(element("Update Available", in: controller.view))
        #expect(row.accessibilityLabel?.contains("Update Available") == true && row.accessibilityLabel?.contains("1.1\u{A0}(16)") == true)
        // The Updates section follows the app's own header and precedes System info.
        let updates = try #require(spoken.firstIndex(of: "Updates"), "\(spoken)")
        let system = try #require(spoken.firstIndex(of: "System info"), "\(spoken)")
        #expect(updates < system, "\(spoken)")
    }

    @Test func theSwitchTurnsTheCheckOffAndKeepsTheChoice() async throws {
        let store = AppUpdateFixture.Store()
        let notice = AppUpdateFixture.notice(store: store)
        let model = model(notice: notice)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostInfo(model, in: lease)
        try await drawn(["Update Available", "Check for Updates"], in: controller.view)
        let toggle = try #require(element("Check for Updates", in: controller.view))
        #expect(toggle.accessibilityActivate())
        #expect(await eventually { !notice.checks })
        #expect(!store.checks)
        // The row's going (the screen redrawn) is checked in the preview's UI test, where the
        // switch is tapped; a hosting controller outside a running scene redraws on its own time.
        #expect(!AppUpdateFixture.notice(store: store).checks, "a new launch reads the choice")
    }

    @Test func infoWithNothingNewerHasTheSwitchAlone() async throws {
        let model = model(notice: AppUpdateFixture.notice(store: .init(found: nil)))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostInfo(model, in: lease)
        try await drawn(["Check for Updates"], in: controller.view)
        #expect(element("Update Available", in: controller.view) == nil)
    }

    @Test func aBuildWithNoNoticeHasNoUpdatesSectionAndNoneOfItsWords() async throws {
        let model = model(notice: nil)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostInfo(model, in: lease)
        try await drawn(["System info"], in: controller.view)
        let spoken = labels(in: controller.view)
        #expect(!spoken.contains("Updates") && !spoken.contains { $0.contains("Check for Updates") }, "\(spoken)")
    }

    // MARK: The launch

    private func withDefaults<Result>(_ body: (UserDefaults) -> Result) -> Result {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        return body(defaults)
    }

    private static let keys: [String: Any] = [
        "UpdateSourceHost": "example.com", "UpdateSourcePath": "/source.json",
        "UpdateAppIdentifier": "com.example.andarium",
    ]

    private func entry(announces: Bool, build: Int? = 15, defaults: UserDefaults) -> AppEntry {
        var entry = AppEntry(overrides: LaunchOverrides())
        entry.announcesWhatsNew = announces
        entry.build = build
        entry.defaults = defaults
        return entry
    }

    @Test func aLaunchOfAPersonInABuildWithAnAddressHasTheNotice() {
        withDefaults { defaults in
            let model = model(notice: nil)
            let notice = entry(announces: true, defaults: defaults).makeAppUpdate(on: model, info: Self.keys)
            #expect(notice?.configuration.address.absoluteString == "https://example.com/source.json")
            #expect(notice?.configuration.appIdentifier == "com.example.andarium")
            #expect(notice?.app.installedBuild == 15)
            #expect(notice?.app.installedLabel == "1.0\u{A0}(15)")
            #expect(notice?.checks == true && notice?.offer == nil)
        }
    }

    @Test func aTestOrHarnessLaunchABuildWithNoAddressAndAnUnreadableAddressHaveNoNotice() {
        withDefaults { defaults in
            let model = model(notice: nil)
            #expect(entry(announces: false, defaults: defaults).makeAppUpdate(on: model, info: Self.keys) == nil)
            #expect(entry(announces: true, defaults: defaults).makeAppUpdate(on: model, info: [:]) == nil)
            #expect(entry(announces: true, defaults: defaults).makeAppUpdate(on: model, info: nil) == nil)
            #expect(entry(announces: true, build: nil, defaults: defaults).makeAppUpdate(on: model, info: Self.keys) == nil)
            var broken = Self.keys
            broken["UpdateSourceHost"] = "https://example.com"
            #expect(entry(announces: true, defaults: defaults).makeAppUpdate(on: model, info: broken) == nil)
            #expect(defaults.dictionaryRepresentation().keys.allSatisfy { !$0.hasPrefix("UpdateNotice") })
        }
    }

    @Test func theNoticeKeepsItsRecordAndSwitchInTheLaunchsDefaults() {
        withDefaults { defaults in
            let model = model(notice: nil)
            let notice = try? #require(entry(announces: true, defaults: defaults).makeAppUpdate(on: model, info: Self.keys))
            notice?.setChecks(false)
            #expect(defaults.object(forKey: DefaultsUpdateStore.checksKey) as? Bool == false)
            let again = entry(announces: true, defaults: defaults).makeAppUpdate(on: model, info: Self.keys)
            #expect(again?.checks == false)
        }
    }

    @Test func aReadIsAllowedOnlyAtHomeWhileTheApplicationIsActive() {
        let model = model(notice: AppUpdateFixture.notice())
        #expect(model.appUpdateReadAllowed)
        model.applicationActive = false
        #expect(!model.appUpdateReadAllowed)
        model.applicationActive = true
        model.phase = .running(.normal)
        #expect(!model.appUpdateReadAllowed)
        model.phase = .booting(.normal)
        #expect(!model.appUpdateReadAllowed)
    }

    // MARK: The build

    /// The hosted run is made with `UPDATE_SOURCE_HOST=example.com
    /// UPDATE_SOURCE_PATH=/source.json` on the xcodebuild line, as
    /// `FollowXRowTests` is with its handle; a build without them has no keys
    /// and this test has nothing to read.
    @Test(.enabled(if: Bundle.main.object(forInfoDictionaryKey: UpdateConfiguration.hostKey) != nil))
    func theBuildNamesTheArrangedAddressAndTheOriginalIdentifierAndAsksAboutBothTools() throws {
        let info = try #require(Bundle.main.infoDictionary)
        let configuration = try #require(UpdateConfiguration(info: info), "UPDATE_SOURCE_PATH=/source.json is not on the line")
        #expect(configuration.address.absoluteString == "https://example.com/source.json")
        #expect(info[UpdateConfiguration.appIdentifierKey] as? String == info["CFBundleIdentifier"] as? String)
        #expect((info["LSApplicationQueriesSchemes"] as? [String])?.sorted() == ["altstore", "sidestore"])
    }
}
