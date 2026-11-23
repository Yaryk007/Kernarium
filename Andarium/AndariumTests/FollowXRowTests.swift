// SPDX-License-Identifier: GPL-2.0-or-later

#if FOLLOW_X

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The Follow developer on X row of Info, as the application draws it and
/// VoiceOver reads it, in the build the arrangement makes: a Debug build
/// made with `FOLLOW_X_HANDLE=example_handle` on the xcodebuild line (a run
/// without it does not build these tests), whose logo is the glyph the tests
/// draw. The system's `openURL` is replaced, so nothing opens and no real
/// profile address is in any test. SwiftUI publishes its accessibility
/// elements only while application accessibility is on (the Simulator
/// preference `ApplicationAccessibilityEnabled`, as `Kernarium/scripts/
/// run-ios-simulator-tests.sh` sets it), so a run without it fails with
/// that said.
@MainActor
@Suite(.serialized)
struct FollowXRowTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private static let label = "Follow developer on X"

    /// What the replaced `openURL` was asked to open.
    @MainActor private final class Opener {
        var urls: [URL] = []
    }

    private func model(handle: String? = "example_handle") -> AppModel {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(
                version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1)
        model.followXHandle = { handle }
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

    /// Info on the application's window, with `openURL` replaced by `opener`.
    private func hostInfo(_ model: AppModel, opener: Opener, in lease: HostWindow.Lease) -> UIHostingController<some View> {
        let view = InfoSheet(page: nil)
            .environment(model)
            .environment(\.openURL, OpenURLAction { url in
                opener.urls.append(url)
                return .handled
            })
            .frame(width: 440, height: 1100)
        let controller = UIHostingController(rootView: view)
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    /// Waits for Info to be drawn (its title is there), then reports the row.
    private func row(in root: UIView) async throws -> NSObject? {
        let drawn = await eventually {
            root.setNeedsLayout()
            root.layoutIfNeeded()
            return labels(in: root).contains("Project on GitHub")
        }
        try #require(drawn, "Info was not drawn; is ApplicationAccessibilityEnabled on? \(labels(in: root))")
        return nodes(in: root).first { $0.accessibilityLabel == Self.label }
    }

    // MARK: The row

    @Test func theRowIsALinkThatVoiceOverReadsAsTheTitleWithTheNameWhereTheLogoIs() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostInfo(model(), opener: Opener(), in: lease)
        let row = try #require(try await row(in: controller.view), "\(labels(in: controller.view))")
        #expect(row.accessibilityTraits.contains(.link))
        #expect(row.accessibilityLabel == Self.label)
        #expect(row.accessibilityLabel == FollowXWords.accessibilityLabel)
        // The tile's logo and the arrow are decoration: neither is read on its own.
        let spoken = labels(in: controller.view)
        #expect(!spoken.contains("XLogo") && !spoken.contains("arrow.up.right") && !spoken.contains("Up Right"),
                "\(spoken)")
        #expect(Set(spoken.filter { $0.contains("X") && $0.contains("Follow") }).count == 1, "\(spoken)")
    }

    @Test func aTapOpensTheProfileAddressThroughTheSystemAndNothingElse() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let opener = Opener()
        let controller = hostInfo(model(), opener: opener, in: lease)
        let row = try #require(try await row(in: controller.view))
        // Nothing is kept about a tap.
        let before = UserDefaults.standard.dictionaryRepresentation().keys.sorted()
        let rowsBefore = labels(in: controller.view).filter { $0 == Self.label }.count
        #expect(row.accessibilityActivate())
        #expect(await eventually { !opener.urls.isEmpty })
        #expect(opener.urls.map(\.absoluteString) == ["https://x.com/example_handle"])
        #expect(opener.urls.first == FollowXProfile.handle.flatMap { FollowXProfile.url(handle: $0) })
        #expect(opener.urls.first?.query == nil && opener.urls.first?.fragment == nil)
        #expect(UserDefaults.standard.dictionaryRepresentation().keys.sorted() == before)
        // The row stays where it was: no badge, no changed text, no second row.
        #expect(labels(in: controller.view).filter { $0 == Self.label }.count == rowsBefore)
    }

    @Test func theRowSitsWithTheOtherOutwardLinksAndAfterTheSystemInfo() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostInfo(model(), opener: Opener(), in: lease)
        _ = try await row(in: controller.view)
        let spoken = labels(in: controller.view)
        let github = try #require(spoken.firstIndex(of: "Project on GitHub"), "\(spoken)")
        let follow = try #require(spoken.firstIndex(of: Self.label), "\(spoken)")
        let acknowledgements = try #require(spoken.firstIndex(of: "Acknowledgements"), "\(spoken)")
        #expect(github < follow && follow < acknowledgements, "\(spoken)")
    }

    @Test func withoutAHandleThereIsNoRow() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostInfo(model(handle: nil), opener: Opener(), in: lease)
        #expect(try await row(in: controller.view) == nil)
        #expect(model(handle: nil).followXProfile == nil)
    }

    @Test func aHandleThatIsNoHandleLeavesNoRowAndNoAddress() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        for handle in ["", "has space", "https://x.com/example_handle", String(repeating: "a", count: 16)] {
            let model = model(handle: handle)
            #expect(model.followXProfile == nil, "\(handle)")
        }
        let controller = hostInfo(model(handle: "has space"), opener: Opener(), in: lease)
        #expect(try await row(in: controller.view) == nil)
    }

    // MARK: The build

    @Test func theBuildNamesTheArrangedHandleAndTheModelReadsIt() {
        #expect(Bundle.main.object(forInfoDictionaryKey: FollowXProfile.key) as? String == "example_handle")
        #expect(FollowXProfile.handle == "example_handle")
        let live = AppModel(
            guest: nil, host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1)
        #expect(live.followXProfile?.absoluteString == "https://x.com/example_handle")
    }

    // MARK: The logo

    @Test func theLogoIsATemplateImageDrawnAtTheTextSizeOnItsProportions() throws {
        let source = try #require(UIImage(named: FollowXLogo.assetName), "the build has no logo asset")
        #expect(source.size.width > 0 && source.size.height > 0)
        for height in [12.0, 16.0, 30.0] {
            let image = try #require(FollowXLogo.inlineImage(height: height, scale: 3))
            #expect(image.renderingMode == .alwaysTemplate, "it takes the colour of the text around it")
            #expect(abs(image.size.height - height) < 0.001)
            // The bitmap is whole pixels, so the width is the logo's to within one.
            let ratio = source.size.width / source.size.height
            #expect(abs(image.size.width - height * ratio) <= 1.0 / 3 + 0.001, "proportions are the logo's")
            #expect(image.scale == 3)
        }
        #expect(FollowXLogo.inlineImage(height: 0, scale: 3) == nil)
    }

    @Test(.disabled(if: ProcessInfo.processInfo.isiOSAppOnMac, "an iPad app on a Mac draws text at one size whatever the content size category"))
    func theLogoGrowsWithTheTextSize() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        func titleWidth(_ size: DynamicTypeSize) async throws -> CGFloat {
            let controller = UIHostingController(
                rootView: FollowXTitle().environment(\.dynamicTypeSize, size).fixedSize())
            lease.window.rootViewController = controller
            controller.view.layoutIfNeeded()
            return controller.sizeThatFits(in: CGSize(width: 2000, height: 2000)).width
        }
        let small = try await titleWidth(.xSmall)
        let large = try await titleWidth(.xxxLarge)
        #expect(large > small * 1.5, "\(small) -> \(large)")
    }

    @Test func theTitleReadsWithTheLogoInsteadOfTheWordInEveryLanguage() throws {
        let languages = ["en", "ja", "zh-Hans", "zh-Hant"]
        var seen: Set<String> = []
        for language in languages {
            let path = try #require(Bundle.main.path(forResource: language, ofType: "lproj"), "\(language)")
            let bundle = try #require(Bundle(path: path))
            // English is the key itself: the catalog's source language compiles no table of its own.
            let value = bundle.localizedString(forKey: "Follow developer on %@", value: nil, table: "FollowX")
            #expect(value.contains("%@") && !value.contains("X"), "\(language): \(value)")
            seen.insert(value)
            // The language puts the logo where it puts the name, with no space between it and CJK text.
            if language != "en" {
                #expect(!value.contains(" "), "\(language): \(value)")
            }
        }
        #expect(seen.count == languages.count, "a language shows another's text")
    }
}

#endif
