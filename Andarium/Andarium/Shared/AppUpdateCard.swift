// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Home's card for a newer version of Andarium, first among its notices: a
/// title, one line, one button that opens the detail, and a close button that
/// puts away this version only (the next one brings the card back). The words
/// are `UpdateWords`, shared with Kernarium; from the version file only the
/// version appears in them.
struct AppUpdateCard: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let notice = model.appUpdate, notice.showsCard, let offer = notice.offer {
            NoticeCard(
                symbol: "arrow.down.app.fill", color: .accentColor,
                title: UpdateWords.cardTitle,
                text: UpdateWords.cardLine(
                    app: notice.app.name, label: offer.label, installed: notice.app.installedLabel),
                action: NoticeAction(
                    label: UpdateWords.cardButton, identifier: "update-notice-action",
                    perform: { model.sheet = .appUpdate }),
                closeLabel: UpdateWords.cardClose,
                onClose: { withAnimation { notice.hideCard() } }
            )
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("home-update-notice")
        }
    }
}
