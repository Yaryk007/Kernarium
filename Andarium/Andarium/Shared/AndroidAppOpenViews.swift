// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// What a request to open an Android app shows while it waits and when it
/// fails: a card above the boot's steps while Android starts for it, a card at
/// Home above the notice that needs the person's answer, a capsule over the
/// session while the app is being opened, and a banner over Android when it
/// could not be.

/// The boot screen's card while Android starts for a request: what it is for,
/// when the app opens, and the way to not open it (Android keeps starting).
struct AppOpenBootCard: View {
    @Environment(AppModel.self) private var model
    let request: AppOpenRequest
    let dimmed: Bool

    var body: some View {
        HStack(alignment: .top, spacing: 14) {
            AndroidAppIconView(id: request.component.identifier, label: request.label, size: 48)
            VStack(alignment: .leading, spacing: 2) {
                Text(AndroidAppsWords.openingTitle(request.label).keepingTogether)
                    .font(.headline)
                    .lineLimit(2)
                    .accessibilityAddTraits(.isHeader)
                Text(AndroidAppsWords.openingDetail.keepingTogether)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Button(AndroidAppsWords.dontOpen) { model.dontOpenAndroidApp() }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .padding(.top, 8)
                    .accessibilityIdentifier("app-open-dont-open")
            }
            .accessibilityElement(children: .contain)
            Spacer(minLength: 0)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .modifier(BootPanel(dimmed: dimmed, radius: 22))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("app-open-boot-card")
    }
}

/// Home's card above the notice that asks first (an unfinished update, too
/// little free space): which app waits, and why. The notice keeps its own
/// buttons; the close mark here drops the request.
struct AppOpenWaitingCard: View {
    @Environment(AppModel.self) private var model
    let request: AppOpenRequest
    let hold: AppOpenHold

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            AndroidAppIconView(id: request.component.identifier, label: request.label, size: 30)
            VStack(alignment: .leading, spacing: 2) {
                Text(title.keepingTogether).font(.subheadline.weight(.semibold))
                Text(text.keepingTogether)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .accessibilityElement(children: .combine)
            Spacer(minLength: 0)
            Button { model.dontOpenAndroidApp() } label: {
                Image(systemName: "xmark")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                    // A 44-point target, drawn as the 28-point mark.
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(-8)
            .accessibilityLabel(AndroidAppsWords.dontOpen)
            .accessibilityIdentifier("app-open-dont-open")
        }
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("app-open-waiting-card")
    }

    private var title: String {
        hold == .space
            ? AndroidAppsWords.waitingSpaceTitle(request.label) : AndroidAppsWords.waitingUpdateTitle(request.label)
    }

    private var text: String {
        hold == .space
            ? AndroidAppsWords.waitingSpaceText(device: model.host.name) : AndroidAppsWords.waitingUpdateText
    }
}

/// The capsule over the session while Android is ready for the app and it is
/// being opened. It takes no touch.
struct AppOpenPill: View {
    let request: AppOpenRequest

    var body: some View {
        HStack(spacing: 10) {
            AndroidAppIconView(id: request.component.identifier, label: request.label, size: 26)
            Text(AndroidAppsWords.openingPill(request.label))
                .font(.subheadline.weight(.semibold))
                .lineLimit(1)
            ProgressView().controlSize(.small)
        }
        .padding(.vertical, 8)
        .padding(.leading, 8)
        .padding(.trailing, 14)
        .glassEffect(.regular, in: .capsule)
        .allowsHitTesting(false)
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("app-open-pill")
    }
}

/// The banner over Android when an app could not be opened, in the shape of
/// the session's other notices. It goes by itself after a while; Try Again is
/// offered where trying again can help.
struct AppOpenFailureBanner: View {
    let banner: AppOpenBanner
    let retry: () -> Void
    let dismiss: () -> Void

    var body: some View {
        let words = AndroidAppsWords.banner(banner.failure, app: banner.label)
        HStack(alignment: .top, spacing: 12) {
            ZStack(alignment: .bottomTrailing) {
                AndroidAppIconView(
                    id: banner.app.identifier, label: banner.label, size: 40,
                    missing: banner.failure == .notInAndroid)
                Image(systemName: badge)
                    .font(.system(size: 11, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 20, height: 20)
                    .background(banner.failure == .notInAndroid ? Color.gray : Color.orange, in: .circle)
                    .offset(x: 5, y: 5)
            }
            .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 3) {
                Text(words.title.keepingTogether).font(.subheadline.weight(.semibold))
                Text(words.text.keepingTogether)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if banner.failure.canRetry {
                    Button(AndroidAppsWords.tryAgain, action: retry)
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                        .padding(.top, 6)
                        .accessibilityIdentifier("app-open-try-again")
                }
            }
            .accessibilityElement(children: .contain)
            Spacer(minLength: 0)
            Button(action: dismiss) {
                Image(systemName: "xmark")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(-8)
            .accessibilityLabel(AndroidAppsWords.dismiss)
            .accessibilityIdentifier("app-open-banner-dismiss")
        }
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
        .frame(maxWidth: 640)
        .padding(.horizontal, 24)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("app-open-banner")
        // The banner appears over the Android screen without the cursor moving
        // to it: VoiceOver is told what it says, as the graphics notice does.
        .onAppear { VoiceOver.announce(Self.announcement(for: banner)) }
    }

    /// What VoiceOver is told when the banner appears: its title and its text.
    static func announcement(for banner: AppOpenBanner) -> String {
        let words = AndroidAppsWords.banner(banner.failure, app: banner.label)
        return "\(words.title). \(words.text)"
    }

    private var badge: String {
        switch banner.failure {
        case .tookTooLong: "hourglass"
        case .recovery: "wrench.and.screwdriver.fill"
        case .setupUnfinished: "gearshape.fill"
        case .notInAndroid: "questionmark"
        case .toolMissing: "exclamationmark"
        }
    }
}
