// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words that Android ran out of graphics memory, over the top of the
/// session. It takes touches only where it is drawn, so the guest keeps the
/// rest of the screen, and it goes when the person dismisses it or after
/// `GraphicsNotice.visibleFor`.
struct GraphicsNoticeBanner: View {
    let dismiss: () -> Void

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.title3)
                .foregroundStyle(Color.orange)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(GraphicsNotice.title)
                    .font(.subheadline.weight(.semibold))
                Text(GraphicsNotice.detail.keepingTogether)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Button(GraphicsNotice.dismissLabel, action: dismiss)
                .buttonStyle(.bordered)
                .controlSize(.small)
                .accessibilityIdentifier("graphics-notice-dismiss")
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 14)
        .frame(maxWidth: 480)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
        .padding(.horizontal, 24)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("graphics-notice")
        // It appears over the guest unasked; VoiceOver says so without
        // moving from where the person is.
        .onAppear { VoiceOver.announce("\(GraphicsNotice.title). \(GraphicsNotice.detail)") }
    }
}

/// The banners over the top of the session when the model has a notice to
/// show: iOS denying Andarium the network, the guest's graphics memory, the
/// lowered frame rate and the camera (paused for other apps sharing the
/// screen, or off when the guest opens it), one below the other in that order,
/// below the statistics capsule when that is up. The stack is one column at the
/// top, each banner as wide as its words, clear of the side keys and the
/// capsule.
///
/// The column is plain while it fits the room it is given, so a tap beside the
/// banners reaches the guest. When the notices are together taller than the
/// room (a short window), it scrolls instead, no wider than the banners and no
/// taller than the room, so every Dismiss can be reached and the side keys stay
/// tappable.
struct SessionNoticesOverlay: View {
    @Environment(AppModel.self) private var model
    @Environment(\.heatQuestionPatience) private var patience
    @Environment(\.alertIsUp) private var alertIsUp

    /// The side padding every banner draws itself with.
    private static let bannerInset: CGFloat = 24

    var body: some View {
        let denial = model.networkDenialShowing
        let opening = model.androidApps.open.pending.flatMap { $0.stage == .opening || $0.stage == .launching ? $0.request : nil }
        let failure = model.androidApps.open.banner
        let showing = denial != nil || model.graphicsNotice.isShowing || model.thermalNotice.isShowing
            || model.cameraNoticeShowing || opening != nil || failure != nil
        ViewThatFits(in: .vertical) {
            column(denial: denial, opening: opening, failure: failure)
            ScrollView(.vertical) {
                // The scroll view is as wide as the glass, and not the
                // padding around it, which would take touches too.
                column(denial: denial, opening: opening, failure: failure)
                    .padding(.horizontal, -Self.bannerInset)
            }
            .scrollBounceBehavior(.basedOnSize)
            .padding(.horizontal, Self.bannerInset)
        }
        .padding(.top, showing ? StatsPlacement.noticeTop(statsShowing: model.session.showStats) : 0)
        .animation(.easeInOut(duration: 0.2), value: denial)
        .animation(.easeInOut(duration: 0.2), value: model.graphicsNotice.isShowing)
        .animation(.easeInOut(duration: 0.2), value: model.thermalNotice.isShowing)
        .animation(.easeInOut(duration: 0.2), value: model.cameraNoticeKind)
        .animation(.easeInOut(duration: 0.2), value: opening)
        .animation(.easeInOut(duration: 0.2), value: failure)
        // The thermal notice's words ask before the frame rate limit goes. The
        // alert is the overlay's, not the notice's, so a notice that changes
        // its layout does not take it down.
        .alert(HeatLimitAlert.title, isPresented: Binding(
            get: { model.askingToTurnOffHeatLimit },
            set: { if !$0 { model.cancelTurnOffHeatLimit() } })
        ) {
            Button(HeatLimitAlert.turnOff, role: .destructive) { model.confirmTurnOffHeatLimit() }
            Button("Cancel", role: .cancel) { model.cancelTurnOffHeatLimit() }
        } message: {
            Text(HeatLimitAlert.message.keepingTogether)
        }
        .task(id: model.askingToTurnOffHeatLimit) { await endQuestionWithoutAlert() }
    }

    /// UIKit can refuse an alert while a sheet is coming or going, and SwiftUI
    /// does not ask again: a question whose alert has not come within
    /// `patience` ends as Cancel does, so the notice goes back to its timer,
    /// the guest has the keys again and the words ask again. The task is the
    /// question's own: it ends with it.
    private func endQuestionWithoutAlert() async {
        guard model.askingToTurnOffHeatLimit else { return }
        do { try await Task.sleep(for: patience) } catch { return }
        if model.askingToTurnOffHeatLimit && !alertIsUp() { model.cancelTurnOffHeatLimit() }
    }

    private func column(denial: NetworkDenial?, opening: AppOpenRequest?, failure: AppOpenBanner?) -> some View {
        VStack(spacing: StatsPlacement.noticeGap) {
            if let opening {
                AppOpenPill(request: opening)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            if let failure {
                AppOpenFailureBanner(
                    banner: failure, retry: { model.retryAndroidAppOpen() },
                    dismiss: { model.dismissAndroidAppBanner() })
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            if let denial {
                NetworkNoticeBanner(
                    denial: denial, openSettings: { model.openSettings() },
                    dismiss: { model.dismissNetworkNotice() }
                )
                .transition(.move(edge: .top).combined(with: .opacity))
            }
            if model.graphicsNotice.isShowing {
                GraphicsNoticeBanner { model.dismissGraphicsNotice() }
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            if model.thermalNotice.isShowing {
                ThermalNoticeBanner(
                    ask: { model.askToTurnOffHeatLimit() }, dismiss: { model.dismissThermalNotice() }
                )
                .transition(.move(edge: .top).combined(with: .opacity))
            }
            if let notice = model.cameraNoticeKind {
                CameraNoticeBanner(notice: notice) { model.dismissCameraNotice() }
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
    }
}

/// The words of the alert that asks before the frame rate limit goes.
enum HeatLimitAlert {
    static let title = String(localized: "Turn off the frame rate limit?")
    static let message = String(
        localized: "This device gets hotter and the battery drains faster. iOS may dim the screen, slow everything down or close Andarium. You can turn it back on in Settings, under Performance.")
    static let turnOff = String(localized: "Turn Off")
}

extension EnvironmentValues {
    /// How long the heat question waits for its alert before it ends.
    @Entry var heatQuestionPatience: Duration = .seconds(4)
    /// Whether UIKit shows an alert now; a hosted test says what it needs.
    @Entry var alertIsUp: @MainActor () -> Bool = { PresentedAlert.isUp }
}
