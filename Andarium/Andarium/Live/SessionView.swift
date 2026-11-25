// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import UIKit

/// A run: Android's own display fills the screen, and Andarium keeps only
/// Power, Back and Home on its edge. The host volume keys are forwarded
/// as Android key presses while the guest has focus.
struct SessionView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.scenePhase) private var scenePhase
    let mode: StartMode

    var body: some View {
        @Bindable var model = model
        Color.clear
            .overlay(alignment: .top) {
                if model.session.showStats {
                    StatsOverlay(memoryMiB: model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound, recovery: mode == .recovery)
                        .transition(.move(edge: .top).combined(with: .opacity))
                        .padding(.top, StatsPlacement.top)
                        .allowsHitTesting(false)
                }
            }
            .overlay(alignment: .top) {
                GraphicsNoticeOverlay()
            }
            .overlay {
                SideKeysOverlay(mode: mode, lit: true, open: $model.session.sideKeysOpen, actions: actions)
            }
            .background(alignment: .topLeading) {
                if UIDevice.current.userInterfaceIdiom == .phone, scenePhase == .active,
                   !model.session.settingsOpen, model.spacePause == nil {
                    GuestVolumeKeys { runtime?.press($0) }
                        .frame(width: 1, height: 1)
                        .clipped()
                        .opacity(0.02)
                        .accessibilityHidden(true)
                }
            }
            .statusBarHidden(true)
            // The system honours the bottom edge's deferral only while the
            // home indicator is visible: with it hidden, one swipe goes Home.
            .persistentSystemOverlays(.visible)
            .sessionSettings(isPresented: $model.session.settingsOpen, large: false, model: model)
            // The screen over the session says why the guest is paused, and a
            // sheet would cover it.
            .onChange(of: model.spacePause != nil) { _, paused in
                if paused { model.session.settingsOpen = false }
            }
    }

    private var runtime: LiveRuntime? { model.driver as? LiveRuntime }

    private var actions: SideKeyActions {
        SideKeyActions(
            power: { runtime?.press(.power) },
            powerHold: { runtime?.hold(.power) },
            volumeUp: { runtime?.press(.volumeUp) },
            volumeDown: { runtime?.press(.volumeDown) },
            back: { runtime?.press(.back) },
            home: { runtime?.press(.home) },
            settings: { model.session.openSettings() }
        )
    }
}
