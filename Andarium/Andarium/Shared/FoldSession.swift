// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Andarium's own pieces over a session on a folding display: the side keys
/// and the statistics and notices, each where `SessionPlan` puts them. The
/// guest's display is the screen's layer under this (`GuestLayer`, or the
/// preview's mock guest), placed by the same plan.
///
/// A touch beside the pieces reaches the guest as it does on a phone: every
/// frame here is only a place, and only the pieces themselves take touches.
struct FoldSessionOverlay: View {
    @Environment(AppModel.self) private var model
    let plan: SessionPlan
    let mode: StartMode
    /// Whether the guest's screen is on, for Power's light.
    let lit: Bool
    let actions: SideKeyActions
    /// The side keys start on the leading edge (the preview's preset).
    var keysLeading = false

    private var memoryMiB: Int { model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound }

    var body: some View {
        @Bindable var model = model
        GeometryReader { geo in
            let origin = geo.frame(in: .global).origin
            ZStack(alignment: .topLeading) {
                switch plan.keys {
                case .edge:
                    SideKeysOverlay(
                        mode: mode, lit: lit, open: $model.session.sideKeysOpen, leading: keysLeading,
                        tip: SideKeysTip(offersInstall: model.capabilities.offersApkInstall(in: mode)), actions: actions
                    )
                    .frame(width: plan.guest.width, height: plan.guest.height)
                    .offset(x: plan.guest.minX - origin.x, y: plan.guest.minY - origin.y)
                    .transition(.opacity)
                    hud
                        .frame(width: plan.hud.width, height: plan.hud.height, alignment: .top)
                        .offset(x: plan.hud.minX - origin.x, y: plan.hud.minY - origin.y)
                        // VoiceOver reads the statistics and the notices before
                        // the keys, as it does on the phone, though the keys
                        // are declared first (the HUD stays above them).
                        .accessibilitySortPriority(1)
                case .rail(let rail):
                    // The statistics are in the strip; the notices hang from
                    // the guest's top.
                    SessionNoticesOverlay()
                        .frame(width: plan.hud.width, height: plan.hud.height, alignment: .top)
                        .offset(x: plan.hud.minX - origin.x, y: plan.hud.minY - origin.y)
                    VStack(spacing: 0) {
                        if model.session.showStats {
                            RailStats(memoryMiB: memoryMiB, recovery: mode == .recovery)
                                .transition(.opacity)
                        }
                        Spacer(minLength: 8)
                        KeyRail(mode: mode, lit: lit, actions: actions)
                    }
                    .padding(.bottom, 10)
                    .frame(width: rail.width, height: rail.height)
                    .offset(x: rail.minX - origin.x, y: rail.minY - origin.y)
                    .transition(.opacity)
                case .deck(let deck):
                    VStack(spacing: 0) {
                        hud
                        Spacer(minLength: 12)
                        KeyDeck(mode: mode, lit: lit, actions: actions)
                            .padding(.horizontal, 28)
                            .padding(.bottom, 22)
                    }
                    .frame(width: deck.width, height: deck.height)
                    .offset(x: deck.minX - origin.x, y: deck.minY - origin.y)
                    .transition(.opacity)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
        }
        .ignoresSafeArea()
        .animation(.snappy, value: plan)
    }

    /// The statistics capsule and the notices below it, from the top of
    /// `plan.hud`.
    private var hud: some View {
        ZStack(alignment: .top) {
            if model.session.showStats {
                StatsOverlay(memoryMiB: memoryMiB, recovery: mode == .recovery)
                    .transition(.move(edge: .top).combined(with: .opacity))
                    .padding(.top, StatsPlacement.top)
                    .allowsHitTesting(false)
            }
            SessionNoticesOverlay()
        }
    }
}

/// The statistics in the cover's strip: the capsule's figures stacked in a
/// narrow column, under the camera and above the keys.
struct RailStats: View {
    @Environment(AppModel.self) private var model
    let memoryMiB: Int
    var recovery = false

    var body: some View {
        let readout = model.session.readout
        VStack(alignment: .leading, spacing: 7) {
            VStack(alignment: .leading, spacing: 2) {
                metric("CPU", readout.cpuText)
                Sparkline(values: readout.history)
                    .frame(width: 52, height: 12)
            }
            meter("RAM", readout.memoryUsedText, share: readout.memoryFraction(of: memoryMiB))
            meter("VRAM", readout.vramUsedText, share: readout.vramFraction)
            if !recovery {
                metric("FPS", readout.fpsNumber)
            }
        }
        .padding(.vertical, 10)
        .frame(width: 70)
        .glassEffect(.regular, in: .rect(cornerRadius: 18))
        .environment(\.colorScheme, .dark)
        .allowsHitTesting(false)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(readout.spoken(memoryMiB: memoryMiB, recovery: recovery))
        .onAppear { model.statistics(.pill, visible: true) }
        .onDisappear { model.statistics(.pill, visible: false) }
    }

    /// The titles are plain strings: the capsule says CPU, RAM, VRAM and FPS
    /// in every language.
    private func metric(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title)
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
            Text(value)
                .font(.system(size: 13, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .lineLimit(1)
                .minimumScaleFactor(0.8)
        }
        .frame(width: 52, alignment: .leading)
    }

    private func meter(_ title: String, _ value: String, share: Double?) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            metric(title, value)
            ShareBar(share: share)
                .frame(width: 52, height: 3)
        }
    }
}
