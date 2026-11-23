// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import UIKit

/// The tool's answers, played: the Simulator preview's, and the probe session's
/// of the UI tests. It answers as
/// `kernarium_apps` is specified to: a digest, a listing, the icons of up to
/// eight apps and the start of an app's launcher activity. The names and the
/// pictures are made up.
nonisolated final class ScriptedAppsTool: AndroidAppsTool, @unchecked Sendable {
    private let lock = NSLock()
    private var current: [AndroidApp]
    private var version = 1
    private var counts = (digests: 0, lists: 0, icons: 0)
    private var listingsNow = 0
    private var listingsMost = 0
    private var asked: [AndroidComponent] = []
    private var answers: [AndroidComponent: AndroidAppOpenAnswer] = [:]
    private var pictures: [String: Data] = [:]
    private var listingOverride: AndroidAppListing?
    private var digestOverride: AndroidAppDigestAnswer?
    private var delayValue = Duration.milliseconds(120)
    private var holdsDigestsValue = false

    init(apps: [AndroidApp] = ScriptedAndroidApps.sampleApps) {
        current = apps
    }

    /// How long each answer takes.
    var delay: Duration {
        get { lock.withLock { delayValue } }
        set { lock.withLock { delayValue = newValue } }
    }

    /// While set, a digest is not answered (the guest does not reply), and no cancellation ends the wait.
    var holdsDigests: Bool {
        get { lock.withLock { holdsDigestsValue } }
        set { lock.withLock { holdsDigestsValue = newValue } }
    }

    var apps: [AndroidApp] { lock.withLock { current } }
    var digestsAsked: Int { lock.withLock { counts.digests } }
    var listingsAsked: Int { lock.withLock { counts.lists } }
    var iconsAsked: Int { lock.withLock { counts.icons } }
    var opened: [AndroidComponent] { lock.withLock { asked } }
    /// The most digest and listing asks that were under way at once: the guest is asked one at a time.
    var mostListingsAtOnce: Int { lock.withLock { listingsMost } }

    /// What a listing answers in place of the apps, and what the digest does.
    func answer(listing: AndroidAppListing?, digest: AndroidAppDigestAnswer? = nil) {
        lock.withLock {
            listingOverride = listing
            digestOverride = digest
        }
    }

    /// What starting an app answers; an app not named here opens.
    func answer(open component: AndroidComponent, _ answer: AndroidAppOpenAnswer) {
        lock.withLock { answers[component] = answer }
    }

    /// An app the person installs: the next digest differs.
    func install(_ app: AndroidApp) {
        lock.withLock {
            current.removeAll { $0.id == app.id }
            current.append(app)
            version += 1
        }
    }

    /// An app the person removes.
    func remove(_ id: String) {
        lock.withLock {
            current.removeAll { $0.id == id }
            version += 1
        }
    }

    func digest(limit: Duration) async -> AndroidAppDigestAnswer {
        let (answer, wait): (AndroidAppDigestAnswer, Duration) = lock.withLock {
            counts.digests += 1
            listingsNow += 1
            listingsMost = max(listingsMost, listingsNow)
            return (digestOverride ?? .digest("scripted-\(version)"), delayValue)
        }
        try? await Task.sleep(for: wait)
        while lock.withLock({ holdsDigestsValue }) {
            await Task { try? await Task.sleep(for: .milliseconds(10)) }.value
        }
        lock.withLock { listingsNow -= 1 }
        return answer
    }

    func list(limit: Duration) async -> AndroidAppListing {
        let (answer, wait): (AndroidAppListing, Duration) = lock.withLock {
            counts.lists += 1
            listingsNow += 1
            listingsMost = max(listingsMost, listingsNow)
            return (listingOverride ?? .apps(AndroidAppList(apps: current, digest: "scripted-\(version)")), delayValue)
        }
        try? await Task.sleep(for: wait)
        lock.withLock { listingsNow -= 1 }
        return answer
    }

    func icons(of apps: [AndroidApp], limit: Duration) async -> [String: AndroidAppIconAnswer] {
        let wait = lock.withLock {
            counts.icons += 1
            return delayValue
        }
        try? await Task.sleep(for: wait)
        var result: [String: AndroidAppIconAnswer] = [:]
        for app in apps {
            if ScriptedAppIcons.hasNoIcon(app) {
                result[app.id] = .noIcon
            } else if let data = ScriptedAppIcons.png(for: app) {
                result[app.id] = .png(data, hasAlpha: false)
            } else {
                result[app.id] = .error
            }
        }
        return result
    }

    func open(_ component: AndroidComponent, limit: Duration) async -> AndroidAppOpenAnswer {
        let (answer, wait): (AndroidAppOpenAnswer, Duration) = lock.withLock {
            asked.append(component)
            let known = current.contains { $0.component == component }
            return (answers[component] ?? (known ? .opened : .notFound), delayValue)
        }
        try? await Task.sleep(for: wait)
        return answer
    }
}

/// A source of Android's apps that plays: the tool above, and Android's
/// readiness for an app, which is ready after a short time unless a test says
/// otherwise.
@MainActor
final class ScriptedAndroidApps: AndroidAppsSource {
    let tool: ScriptedAppsTool
    /// How Android's readiness ends, and how long it takes.
    var readiness = AndroidAppReadiness.Outcome.ready
    var readyDelay = Duration.milliseconds(400)
    private(set) var readinessAsked = 0
    /// Plays the production check's first-HOME gate: while an app is in
    /// front (`homeInFront` false) Android is not ready for an open that
    /// has to see Home resumed, and the wait runs out.
    var requiresHomeResumed = false
    var homeInFront = true
    /// What each wait for readiness was told of the run having seen Home.
    private(set) var homeReachedAsked: [Bool] = []

    init(tool: ScriptedAppsTool = ScriptedAppsTool()) {
        self.tool = tool
    }

    var androidAppsTool: (any AndroidAppsTool)? { tool }

    func awaitAndroidReady(homeReached: Bool) async -> AndroidAppReadiness.Outcome {
        readinessAsked += 1
        homeReachedAsked.append(homeReached)
        try? await Task.sleep(for: readyDelay)
        if requiresHomeResumed, !homeReached, !homeInFront { return .timedOut }
        return readiness
    }

    // MARK: The made-up apps

    private nonisolated static func make(_ package: String, _ label: String, builtIn: Bool = false) -> AndroidApp {
        AndroidApp(
            component: AndroidComponent(packageName: package, activityClass: package + ".MainActivity")!,
            label: label, builtIn: builtIn, iconTag: "0000000000000001")
    }

    /// What the person installed.
    nonisolated static let installedApps = [
        make("com.example.comethop", "Comet Hop"),
        make("com.example.inkleaf", "Inkleaf Notes"),
        make("com.example.moonclock", "Moonclock"),
        make("com.example.sumwise", "Sumwise"),
        make("com.example.wavelet", "Wavelet"),
        make("com.example.trailmark", "Trailmark"),
        make("com.example.pocketgarden", "Pocket Garden"),
        make("com.example.snapframe", "Snapframe"),
    ]

    /// What Android itself carries.
    nonisolated static let builtInApps = [
        make("org.example.browser", "Browser", builtIn: true),
        make("org.example.calculator", "Calculator", builtIn: true),
        make("org.example.camera", "Camera", builtIn: true),
        make("org.example.clock", "Clock", builtIn: true),
        make("org.example.contacts", "Contacts", builtIn: true),
        make("org.example.files", "Files", builtIn: true),
        make("org.example.gallery", "Gallery", builtIn: true),
        make("org.example.settings", "Settings", builtIn: true),
    ]

    nonisolated static let sampleApps = (installedApps + builtInApps).sorted(by: AndroidAppText.inOrder)
}

/// The pictures of the made-up apps: a gradient and a white symbol on an
/// opaque square, as the tool gives an icon.
nonisolated enum ScriptedAppIcons {
    private static let looks: [String: (symbol: String, first: UInt32, second: UInt32)] = [
        "comethop": ("sparkles", 0x7B3DFF, 0xE2449A),
        "inkleaf": ("leaf.fill", 0xFFD84A, 0xF5A312),
        "moonclock": ("moon.stars.fill", 0x34308A, 0x16133F),
        "sumwise": ("plus.slash.minus", 0xFF9A4D, 0xF2581F),
        "wavelet": ("waveform", 0x22C7B5, 0x0B6E7C),
        "trailmark": ("mountain.2.fill", 0x7FD35A, 0x2E8B3E),
        "pocketgarden": ("camera.macro", 0x9ADB7A, 0x4FA35C),
        "snapframe": ("camera.aperture", 0x3E8BFF, 0x1A4FD0),
        "browser": ("globe", 0x3FA2F7, 0x1F6FE5),
        "calculator": ("function", 0x59636E, 0x3B4A5A),
        "camera": ("camera.fill", 0x444A55, 0x2B2F36),
        "clock": ("clock.fill", 0x3F86F0, 0x1F6FE5),
        "contacts": ("person.fill", 0x3CC596, 0x2BA67B),
        "files": ("folder.fill", 0x41A0F5, 0x1E88E5),
        "gallery": ("photo.fill", 0xF29D52, 0xE2574C),
        "settings": ("gearshape.fill", 0x7C8794, 0x5F6B78),
    ]

    private static func key(_ app: AndroidApp) -> String {
        String(app.component.packageName.split(separator: ".").last ?? "")
    }

    /// An app of the sample that declares no icon.
    static func hasNoIcon(_ app: AndroidApp) -> Bool { looks[key(app)] == nil }

    static func png(for app: AndroidApp, pixels: CGFloat = 192) -> Data? {
        guard let look = looks[key(app)] else { return nil }
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        let size = CGSize(width: pixels, height: pixels)
        return UIGraphicsImageRenderer(size: size, format: format).image { context in
            let colors = [color(look.first).cgColor, color(look.second).cgColor] as CFArray
            if let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors, locations: [0, 1]) {
                context.cgContext.drawLinearGradient(
                    gradient, start: .zero, end: CGPoint(x: pixels, y: pixels), options: [])
            }
            let configuration = UIImage.SymbolConfiguration(pointSize: pixels * 0.42, weight: .semibold)
            if let symbol = UIImage(systemName: look.symbol, withConfiguration: configuration)?
                .withTintColor(.white, renderingMode: .alwaysOriginal) {
                let drawn = symbol.size
                symbol.draw(in: CGRect(
                    x: (pixels - drawn.width) / 2, y: (pixels - drawn.height) / 2, width: drawn.width, height: drawn.height))
            }
        }.pngData()
    }

    private static func color(_ rgb: UInt32) -> UIColor {
        UIColor(
            red: CGFloat((rgb >> 16) & 0xFF) / 255, green: CGFloat((rgb >> 8) & 0xFF) / 255,
            blue: CGFloat(rgb & 0xFF) / 255, alpha: 1)
    }
}
