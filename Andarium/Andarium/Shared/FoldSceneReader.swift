// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import UIKit

/// Reads what the system says of the folding display a scene is on, for the
/// layout decisions of `AndariumCore` (`FoldReading`): the hinge, through a
/// `UIHingeInteraction`, and the fold's band and the regions the system keeps
/// for the camera, as the view's reserved regions. Only an iPhone folds, so
/// the interaction is attached on the phone idiom alone; a device that does not
/// fold delivers no hinge, so the reader never produces a reading there and
/// nothing that follows from one ever runs.
///
/// The system gives no notification for a reserved region. The root reads
/// again at every layout and every geometry change, and the hinge's own
/// updates make it read when only the band's state changed: bending a turned
/// display moves nothing else.
@MainActor
final class FoldSceneReader {
    /// The hinge as the interaction last reported it; nil until it reports
    /// one, which a display that does not fold never does.
    private var hinge: FoldReading.Hinge?
    private var interaction: AnyObject?
    /// Called when the hinge reported a status that differs from the last.
    var hingeDidChange: (() -> Void)?
    /// A reading a test supplies in place of the system's, to put the scene
    /// root in a pose the Simulator is not in.
    var stub: (@MainActor () -> FoldReading?)?

    /// The system reports a hinge: the display folds.
    var reportsHinge: Bool { hinge != nil }

    func attach(to view: UIView) {
        guard #available(iOS 27.1, *), interaction == nil, UIDevice.current.userInterfaceIdiom == .phone else {
            return
        }
        let interaction = UIHingeInteraction { [weak self] _, update in
            MainActor.assumeIsolated { self?.hingeReported(update.hinge) }
        }
        view.addInteraction(interaction)
        self.interaction = interaction
    }

    /// Only an update without a hinge (the interaction left the view
    /// hierarchy's display) clears the reading. A status the system adds or
    /// cannot tell (`.unknown`) leaves the last one in place, so that a blip in
    /// the sensor does not swap the session's layout for the phone's and back.
    @available(iOS 27.1, *)
    private func hingeReported(_ reported: UIHinge?) {
        let next: FoldReading.Hinge? = switch reported?.status {
        case nil: nil
        case .closed?: .closed
        case .partiallyOpen?: .partiallyOpen
        case .fullyOpen?: .fullyOpen
        default: hinge
        }
        guard next != hinge else { return }
        hinge = next
        hingeDidChange?()
    }

    /// The scene root's view as the system reports it now; nil on a display
    /// that does not fold.
    func read(_ view: UIView) -> FoldReading? {
        if let stub { return stub() }
        guard #available(iOS 27.1, *), hinge != nil else { return nil }
        let window = view.bounds
        let safe = window.inset(by: view.safeAreaInsets)
        let division = view.reservedRegions(kind: .division, options: .includeInactive).first
            .map { FoldReading.Region(frame: $0.frame, isActive: $0.isActive) }
        let occlusions = view.reservedRegions(kind: .occlusion).map(\.frame)
        return FoldReading(window: window, safeArea: safe, hinge: hinge, division: division, occlusions: occlusions)
    }
}
