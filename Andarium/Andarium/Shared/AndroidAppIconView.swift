// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import UIKit

/// The pictures of Android's apps the screens draw, read from the cache and
/// kept until an icon arrives or the list changes. The most kept is a screen
/// or two of rows, so a list of a thousand apps scrolled through does not hold
/// a thousand decoded pictures; the system may drop them sooner under pressure.
@MainActor
enum AndroidAppImages {
    static let mostKept = 120
    private static var revision = -1
    private static let cache: NSCache<NSString, UIImage> = {
        let cache = NSCache<NSString, UIImage>()
        cache.countLimit = mostKept
        return cache
    }()

    /// The app's icon, or nil where the cache has none (the app declares no
    /// icon, or it has not arrived yet).
    static func image(forID id: String, in state: AndroidAppsState) -> UIImage? {
        if revision != state.iconRevision {
            cache.removeAllObjects()
            revision = state.iconRevision
        }
        if let image = cache.object(forKey: id as NSString) { return image }
        guard let data = state.store.icon(forID: id), let image = UIImage(data: data) else { return nil }
        cache.setObject(image, forKey: id as NSString)
        return image
    }
}

/// An Android app's icon under iOS's mask: the picture the tool made (a full
/// square, never masked), or, where there is none, a tile with the app's
/// initial. The picture is decoration: the name beside it is what is read.
struct AndroidAppIconView: View {
    @Environment(AppModel.self) private var model
    let id: String
    let label: String
    var size: CGFloat = 40
    /// Drawn grey: the app is no longer in Android.
    var missing = false

    init(app: AndroidApp, size: CGFloat = 40) {
        id = app.id
        label = app.label
        self.size = size
    }

    init(id: String, label: String, size: CGFloat = 40, missing: Bool = false) {
        self.id = id
        self.label = label
        self.size = size
        self.missing = missing
    }

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: size * 0.2237, style: .continuous)
        Group {
            if let image = AndroidAppImages.image(forID: id, in: model.androidApps) {
                Image(uiImage: image)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFill()
            } else {
                AndroidAppPlaceholder(id: id, label: label)
            }
        }
        .frame(width: size, height: size)
        .clipShape(shape)
        .overlay { shape.strokeBorder(.primary.opacity(0.08), lineWidth: 0.5) }
        .saturation(missing ? 0 : 1)
        .opacity(missing ? 0.5 : 1)
        .accessibilityHidden(true)
    }
}

/// The tile of an app that has no icon: a colour from its identity and its
/// first letter.
struct AndroidAppPlaceholder: View {
    let id: String
    let label: String

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                LinearGradient(colors: [tint(0.62), tint(0.42)], startPoint: .topLeading, endPoint: .bottomTrailing)
                Text(verbatim: initial)
                    .font(.system(size: geometry.size.width * 0.46, weight: .bold, design: .rounded))
                    .foregroundStyle(.white)
            }
        }
        .aspectRatio(1, contentMode: .fit)
    }

    private var initial: String {
        label.first.map { String($0).uppercased() } ?? "?"
    }

    /// A hue from the identifier, the same on every launch.
    private func tint(_ brightness: Double) -> Color {
        var hash: UInt32 = 2_166_136_261
        for byte in id.utf8 { hash = (hash ^ UInt32(byte)) &* 16_777_619 }
        return Color(hue: Double(hash % 360) / 360, saturation: 0.55, brightness: min(brightness + 0.3, 1))
    }
}
