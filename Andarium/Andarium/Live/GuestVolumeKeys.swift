// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AVFAudio
import MediaPlayer
import SwiftUI

/// Mirror foreground media-volume changes to Android. MPVolumeView presents
/// the app's volume control to iOS while the guest draws its own volume panel.
struct GuestVolumeKeys: UIViewRepresentable {
    let send: (GuestKey) -> Void

    func makeUIView(context: Context) -> VolumeView {
        let view = VolumeView(frame: .zero)
        view.showsRouteButton = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        return view
    }

    func updateUIView(_ view: VolumeView, context: Context) {
        view.send = send
    }

    static func dismantleUIView(_ view: VolumeView, coordinator: ()) {
        view.send = nil
        view.detach()
    }

    final class VolumeView: MPVolumeView {
        var send: ((GuestKey) -> Void)?
        private var observation: NSKeyValueObservation?
        private var routeIDs: [String] = []
        private var observedSession = 0

        override func didMoveToWindow() {
            super.didMoveToWindow()
            detach()
            guard window != nil else { return }
            let audio = AVAudioSession.sharedInstance()
            routeIDs = audio.currentRoute.outputs.map(\.uid)
            let observedSession = self.observedSession
            observation = audio.observe(\.outputVolume, options: [.old, .new]) { [weak self] _, change in
                guard let old = change.oldValue, let new = change.newValue, old != new else { return }
                Task { @MainActor [weak self] in
                    guard let self, self.observedSession == observedSession,
                          window?.windowScene?.activationState == .foregroundActive else { return }
                    let outputs = AVAudioSession.sharedInstance().currentRoute.outputs.map(\.uid)
                    guard outputs == routeIDs else { routeIDs = outputs; return }
                    send?(new > old ? .volumeUp : .volumeDown)
                }
            }
        }

        func detach() {
            observation?.invalidate()
            observation = nil
            observedSession += 1
        }
    }
}
