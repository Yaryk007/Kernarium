// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Observation

/// The update notice of a sideloaded application: whether a newer version of
/// it exists, what the card, the detail and Info show of it, and the one read
/// that finds out.
///
/// The read is made at most once in 24 hours, a few seconds after Home shows
/// and only while the application's moment says so (no run active, starting,
/// pending or launching), and never delays or blocks a start: it is a
/// background request whose every outcome is silence. A failed read changes
/// nothing shown. What is shown is always the last answer, checked again
/// against the installed build and this device each time it is read, so a
/// withdrawn or installed version goes away and a build the file lists below
/// the installed one is never offered.
@MainActor
@Observable
public final class UpdateNotice {
    /// The application, as the card and the detail speak of it.
    public struct App: Equatable, Sendable {
        public let name: String
        /// The installed `CFBundleVersion`.
        public let installedBuild: Int
        /// The installed version as Info writes it: "1.0 (15)", with the build kept to the version.
        public let installedLabel: String

        public init(name: String, installedBuild: Int, installedLabel: String) {
            self.name = name
            self.installedBuild = installedBuild
            // A version and its build stay on one line (U+00A0 before the build).
            self.installedLabel = installedLabel.replacingOccurrences(of: " (", with: UpdateVersion.buildSeparator + "(")
        }
    }

    /// How long Home has been showing before a read starts.
    public static let settle: Duration = .seconds(3)

    public let configuration: UpdateConfiguration
    public let app: App

    /// The last answer and what the person put away.
    public private(set) var record: UpdateRecord
    /// The Check for Updates switch.
    public private(set) var checks: Bool
    /// A read is under way.
    public private(set) var reading = false

    @ObservationIgnored private let store: any UpdateStore
    @ObservationIgnored private let fetcher: any UpdateFetching
    @ObservationIgnored private let host: UpdateHost
    @ObservationIgnored private let os: UpdateOSVersion
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored private let wait: (Duration) async throws -> Void
    @ObservationIgnored private let canOpen: (URL) -> Bool
    @ObservationIgnored private let open: (URL) -> Void

    /// `canOpen` answers whether the system has an application for a URL (the
    /// sideloading apps, which the application's `Info.plist` lists), and
    /// `open` opens one: the application's only two uses of the system's
    /// links. `now` and `wait` are the clock; tests supply their own.
    public init(
        configuration: UpdateConfiguration, app: App, store: any UpdateStore, fetcher: any UpdateFetching,
        host: UpdateHost, os: UpdateOSVersion = .current, now: @escaping () -> Date = { Date() },
        wait: @escaping (Duration) async throws -> Void = { try await Task.sleep(for: $0) },
        canOpen: @escaping (URL) -> Bool, open: @escaping (URL) -> Void
    ) {
        self.configuration = configuration
        self.app = app
        self.store = store
        self.fetcher = fetcher
        self.host = host
        self.os = os
        self.now = now
        self.wait = wait
        self.canOpen = canOpen
        self.open = open
        record = store.record
        checks = store.checks
    }

    // MARK: What is shown

    /// The version on offer, or nil: with the switch off, with no answer or an
    /// answer of none, and once the installed build has caught up with it.
    public var offer: UpdateVersion? {
        guard checks, let found = record.found,
            UpdateFile.isOffered(found, installedBuild: app.installedBuild, os: os),
            configuration.links.link(found.appFile.absoluteString) != nil
        else { return nil }
        // A release page the build no longer accepts costs only its row.
        if let notes = found.notes, configuration.links.link(notes.absoluteString) == nil {
            return found.withoutNotes
        }
        return found
    }

    /// Home's card: the offer, until the person puts that build away.
    public var showsCard: Bool {
        guard let offer else { return false }
        return record.hiddenBuild != offer.build
    }

    /// The sideloading apps the detail offers to hand the app file to.
    public var tools: [UpdateTool] { host.tools(canOpen: canOpen) }

    // MARK: What the person does

    /// Closing the card and Skip This Version: this build is not offered on
    /// Home again, and the next one is. Info keeps the update.
    public func hideCard() {
        guard let offer else { return }
        record.hiddenBuild = offer.build
        store.record = record
    }

    public func setChecks(_ on: Bool) {
        checks = on
        store.checks = on
    }

    /// Hands the app file to a sideloading app, which downloads, signs and
    /// installs it. The link is built here from the tool's template.
    public func update(with tool: UpdateTool) {
        guard let offer, let link = tool.installLink(appFile: offer.appFile) else { return }
        open(link)
    }

    /// Opens the app file's address, which the browser downloads.
    public func openAppFile() {
        guard let offer else { return }
        open(offer.appFile)
    }

    /// Opens the release page, when the file names one that passed.
    public func openReleasePage() {
        guard let page = offer?.notes else { return }
        open(page)
    }

    // MARK: The read

    /// Reads the version file when one is due, a few seconds after Home
    /// shows. `moment` says whether the application is still at the point a
    /// read is allowed (Home, with no run active, starting, pending or
    /// launching): it is asked once the wait is over. A task that is cancelled
    /// meanwhile, as Home's is when a start begins, makes no read.
    public func readWhenHomeShows(moment: @MainActor () -> Bool) async {
        guard isDue else { return }
        do {
            try await wait(Self.settle)
        } catch {
            return
        }
        guard isDue, moment() else { return }
        reading = true
        defer { reading = false }
        switch await fetcher.fetch(configuration.address) {
        case .notTried:
            return
        case .failed:
            record.attempted = now()
            store.record = record
        case .data(let data):
            // The day is used up before the body is looked at, so that a body
            // that ends the process cannot make every launch read it again.
            record.attempted = now()
            store.record = record
            let appIdentifier = configuration.appIdentifier
            let installedBuild = app.installedBuild
            let os = self.os
            let links = configuration.links
            // The body is untrusted and up to 1 MiB: it is decoded off the
            // main actor, so that Home never waits for it.
            let reading = await Task.detached(priority: .utility) {
                UpdateFile.read(data, appIdentifier: appIdentifier, installedBuild: installedBuild, os: os, links: links)
            }.value
            if case .read(let version) = reading {
                record.found = version
                store.record = record
            }
        }
    }

    private var isDue: Bool {
        checks && !reading && UpdateSchedule.isDue(attempted: record.attempted, now: now())
    }
}
