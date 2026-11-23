// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// The application's own touch feedback: the click of a side key and the tick
/// of a ruler. The Haptics switch is the guest's vibrator's and does not
/// govern it. A launch that silences haptics (`HapticsLaunch`) does, because
/// the taps and drags of an automated run reach it and must not make the phone
/// vibrate. Every view plays its feedback through `interfaceFeedback`, which
/// `tests/tooling/andarium/test_interface_feedback.py` holds to.
enum InterfaceFeedback {
    /// What plays now: `feedback`, or nothing while the launch silences it. A
    /// view with no model (a preview) plays as it always did.
    static func playing(_ feedback: SensoryFeedback, in model: AppModel?) -> SensoryFeedback? {
        model?.interfaceFeedbackPlays == false ? nil : feedback
    }
}

private struct InterfaceFeedbackModifier<Trigger: Equatable>: ViewModifier {
    @Environment(AppModel.self) private var model: AppModel?
    let feedback: SensoryFeedback
    let trigger: Trigger

    func body(content: Content) -> some View {
        let playing = InterfaceFeedback.playing(feedback, in: model)
        content.sensoryFeedback(trigger: trigger) { _, _ in playing }
    }
}

extension View {
    /// `sensoryFeedback` for the application's own controls, silenced with the
    /// launch (`InterfaceFeedback`).
    func interfaceFeedback<Trigger: Equatable>(_ feedback: SensoryFeedback, trigger: Trigger) -> some View {
        modifier(InterfaceFeedbackModifier(feedback: feedback, trigger: trigger))
    }
}
