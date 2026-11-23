// SPDX-License-Identifier: GPL-2.0-or-later

#if FOLLOW_X

import SwiftUI
import UIKit

/// The logo as a template image: the asset of the build's logo catalog
/// (`XLogoLocal.xcassets`, linked to the ignored `Config/Local/x-logo.svg`, or
/// `XLogoGlyph.xcassets`, the glyph the tests draw), which no other build has.
enum FollowXLogo {
    static let assetName = "XLogo"

    /// The logo drawn `height` points tall at `scale`, as a template image: it
    /// takes the colour of the text around it. An inline image in a `Text`
    /// keeps the size it has, so the row draws it at the text's size itself.
    static func inlineImage(height: CGFloat, scale: CGFloat) -> UIImage? {
        guard height > 0, let source = UIImage(named: assetName), source.size.height > 0 else { return nil }
        let size = CGSize(width: height * source.size.width / source.size.height, height: height)
        let format = UIGraphicsImageRendererFormat()
        format.scale = scale
        let drawn = UIGraphicsImageRenderer(size: size, format: format).image { _ in
            source.draw(in: CGRect(origin: .zero, size: size))
        }
        return drawn.withRenderingMode(.alwaysTemplate)
    }
}

/// The name of the service as the row's text spells it for VoiceOver: the
/// logo has no spelling of its own.
enum FollowXWords {
    static let name = "X"

    /// The row's VoiceOver label: the title with the name where the logo is.
    static var accessibilityLabel: String {
        String(localized: "Follow developer on \(name)", table: "FollowX")
    }
}

/// The title: "Follow developer on" and the logo, where each language puts the
/// name. The logo is inline at the size of the text and follows its colour,
/// Dynamic Type and the appearance.
struct FollowXTitle: View {
    @Environment(\.displayScale) private var displayScale
    /// The height of a capital letter of the body text, which the logo takes.
    @ScaledMetric(relativeTo: .body) private var capHeight: CGFloat = 12

    var body: some View {
        if let logo = FollowXLogo.inlineImage(height: capHeight, scale: displayScale) {
            Text("Follow developer on \(Image(uiImage: logo))", tableName: "FollowX")
        } else {
            Text(FollowXWords.accessibilityLabel)
        }
    }
}

/// The row of Info that opens the developer's profile through the system
/// (`openURL`: the X app when it is installed, the browser otherwise). It
/// remembers nothing about a tap.
struct FollowXRow: View {
    let profile: URL

    var body: some View {
        Link(destination: profile) {
            HStack(spacing: 12) {
                IconTile(asset: FollowXLogo.assetName)
                    .accessibilityHidden(true)
                FollowXTitle()
                Spacer()
                Image(systemName: "arrow.up.right")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .accessibilityHidden(true)
            }
        }
        .accessibilityLabel(FollowXWords.accessibilityLabel)
        .accessibilityIdentifier("info-follow-x")
    }
}

extension AppModel {
    /// The address of the developer's profile, when the build names a handle.
    var followXProfile: URL? { followXHandle().flatMap { FollowXProfile.url(handle: $0) } }
}

#endif
