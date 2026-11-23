// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The file browser's columns on a wide layout: the Modified date is one line
/// in every language, so the rows are as tall as English ones. A date that
/// breaks onto a second line ("2026年11月" over "28日") makes its row taller. SwiftUI
/// publishes its accessibility elements only while application accessibility
/// is on (the Simulator preference `ApplicationAccessibilityEnabled`), so a
/// run without it fails with that said.
@MainActor
@Suite(.serialized)
struct FileBrowserColumnsTests {
    private static let calendar = Calendar(identifier: .gregorian)
    private static let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000)

    /// The widest a Gregorian date is written: two digits of month and of day.
    private static let modified = calendar.date(
        from: DateComponents(timeZone: TimeZone(secondsFromGMT: 0), year: 2026, month: 11, day: 28, hour: 12))!

    private func browsing() async -> (AppModel, FileBrowserModel, String) {
        let host = Self.host
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        driver.storage = true
        model.driver = driver
        model.refreshPlan()
        let download = GuestPath.child("Download", of: GuestPath.root)
        let backend = FileBrowserModelTests.ScriptedBackend()
        backend.folders[download] = [
            GuestFile(path: GuestPath.child("Trip.jpg", of: download), isFolder: false, size: 4_000_000, modified: Self.modified),
            GuestFile(path: GuestPath.child("Notes", of: download), isFolder: true, size: 0, modified: Self.modified),
            GuestFile(path: GuestPath.child("Report.pdf", of: download), isFolder: false, size: 90_000, modified: Self.modified),
        ]
        let browser = FileBrowserModel(backend: backend, center: NotificationCenter())
        model.fileBrowser = browser
        model.start(.normal)
        model.finishBoot(.normal)
        await model.refreshStorage()
        browser.route = [FolderRoute(path: GuestPath.root), FolderRoute(path: download)]
        await browser.load(download)
        return (model, browser, download)
    }

    private func cells(in view: UIView) -> [UICollectionViewCell] {
        (view as? UICollectionViewCell).map { [$0] } ?? view.subviews.flatMap { cells(in: $0) }
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    /// The height of the folder's three rows on a wide page in `language`.
    private func rowHeights(in language: String) async throws -> [CGFloat] {
        let (model, browser, download) = await browsing()
        let locale = Locale(identifier: language)
        let expected = Date.FormatStyle(
            date: .abbreviated, time: .omitted, locale: locale, calendar: Self.calendar,
            timeZone: TimeZone(secondsFromGMT: 0)!
        ).format(Self.modified)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let host = UIHostingController(
            rootView: NavigationStack {
                FolderPage(browser: browser, route: FolderRoute(path: download), close: {})
            }
            .environment(model)
            .environment(\.locale, locale)
            .environment(\.calendar, Self.calendar)
            .environment(\.timeZone, TimeZone(secondsFromGMT: 0)!))
        container.addChild(host)
        host.view.frame = CGRect(x: 0, y: 0, width: 700, height: 900)
        container.view.addSubview(host.view)
        host.didMove(toParent: container)
        defer {
            host.willMove(toParent: nil)
            host.view.removeFromSuperview()
            host.removeFromParent()
        }
        var labels: [String] = []
        let seen = await eventually {
            host.view.layoutIfNeeded()
            labels = FoldSessionHostingTests.allLabels(in: host.view)
            return labels.contains { $0.contains(expected) }
        }
        try #require(
            seen, "\(language): no row carries \(expected); is ApplicationAccessibilityEnabled on? \(labels)")
        // The probe's width reaches the rows one layout after the first.
        try await Task.sleep(for: .milliseconds(400))
        host.view.layoutIfNeeded()
        // The folder's three rows are the tallest cells; the column header is a cell too.
        let rows = Array(cells(in: host.view).map(\.frame.height).sorted(by: >).prefix(3))
        try #require(rows.count == 3, "\(language): the rows are \(rows)")
        return rows
    }

    @Test func theDateIsOneLineInEveryLanguage() async throws {
        let english = try await rowHeights(in: "en")
        let tile = try #require(english.min())
        #expect(english.allSatisfy { abs($0 - tile) < 0.5 }, "en: \(english)")
        for language in ["zh-Hans", "zh-Hant", "ja"] {
            let rows = try await rowHeights(in: language)
            #expect(
                rows.allSatisfy { abs($0 - tile) < 0.5 },
                "\(language): the rows are \(rows) high, one line of date makes them \(tile)")
        }
    }

    /// The probe's dates stand for every month's longest name: twelve
    /// consecutive months, each on its 28th.
    @Test func theMeasuredDatesAreTheTwentyEighthOfTwelveMonths() throws {
        let now = Self.calendar.date(from: DateComponents(year: 2026, month: 3, day: 5, hour: 12))!
        let dates = FileDate.widest(in: Self.calendar, from: now)
        #expect(dates.count == 12)
        #expect(dates.allSatisfy { Self.calendar.component(.day, from: $0) == 28 })
        #expect(Set(dates.map { Self.calendar.component(.month, from: $0) }).count == 12)
        #expect(FileRow.minimumDateWidth == 104)
    }
}
