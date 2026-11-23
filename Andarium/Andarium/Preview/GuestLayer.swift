// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// The preview has no guest display: its session screens draw mock screens.
/// While a normal boot shows, a fixture frame stands in for what the guest
/// draws under the dimmed boot screen.
struct GuestLayer: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        ZStack {
            // Home states the resolution the guest will get, which is the
            // display's size at the choice. The preview's scene fills its
            // screen: its window has no controls.
            DisplayAreaProbe(cornerEdges: [])
            if case .booting(let kind) = model.phase, kind != .recovery {
                // The image is an overlay so its size never proposes one to the
                // screens above it.
                Color.black
                    .overlay {
                        Image("GuestWallpaper")
                            .resizable()
                            .scaledToFill()
                    }
                    .clipped()
                    .ignoresSafeArea()
                    .accessibilityHidden(true)
            }
        }
    }
}
