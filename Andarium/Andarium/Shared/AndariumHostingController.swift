// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Observation
import SwiftUI
import UIKit

@MainActor
protocol SceneGeometryObserver: AnyObject {
    func sceneGeometryDidChange()
}

/// The scene's root controller owns the orientation preference and, in a
/// run, the screen edges that defer system gestures. A child of SwiftUI's
/// default root cannot reliably supply these scene-wide preferences.
final class AndariumHostingController<Content: View>: UIHostingController<Content>, SceneGeometryObserver {
    private let model: AppModel
    private var lockRequested = false
    /// The run's deferred edges; nil leaves SwiftUI's preference in charge.
    private var deferredEdges: ScreenEdges?
    private var updateScheduled = false
    private var phaseWatcher: PhaseWatcher?

    init(rootView: Content, model: AppModel) {
        self.model = model
        super.init(rootView: rootView)
        phaseWatcher = PhaseWatcher(model: model, target: self)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        UIDevice.current.userInterfaceIdiom == .phone ? .portrait : .all
    }

    override var prefersInterfaceOrientationLocked: Bool { lockRequested }

    // The scene policy belongs to this root, including when SwiftUI embeds
    // a UIKit display controller whose default lock preference is false.
    override var childForInterfaceOrientationLock: UIViewController? { nil }

    // SwiftUI defers the left and right edges unless a view says otherwise,
    // and on Home it hands the choice to a navigation child. In a run the
    // root decides alone. A presented sheet supplies its own preference.
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge {
        guard let deferredEdges else { return super.preferredScreenEdgesDeferringSystemGestures }
        return UIRectEdge(rawValue: deferredEdges.rawValue)
    }

    override var childForScreenEdgesDeferringSystemGestures: UIViewController? {
        deferredEdges == nil ? super.childForScreenEdgesDeferringSystemGestures : nil
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        scheduleUpdate()
    }

    func sceneGeometryDidChange() {
        scheduleUpdate()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        scheduleUpdate()
    }

    private func scheduleUpdate() {
        guard !updateScheduled else { return }
        updateScheduled = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            updateScheduled = false
            guard let scene = view.window?.windowScene else { return }
            let size = scene.effectiveGeometry.coordinateSpace.bounds.size
            let policy = SessionOrientationPolicy(
                phase: model.phase, isPhone: UIDevice.current.userInterfaceIdiom == .phone,
                sceneSize: size, screenSize: scene.screen.bounds.size,
                interfaceLocked: scene.effectiveGeometry.isInterfaceOrientationLocked)
            if policy.requestInterfaceLock != lockRequested {
                lockRequested = policy.requestInterfaceLock
                setNeedsUpdateOfPrefersInterfaceOrientationLocked()
            }
            model.hostDrivesGuestRotation = policy.hostDrivesGuestRotation
            let deferral = SessionGestureDeferral(
                phase: model.phase, sceneSize: size, screenSize: scene.screen.bounds.size)
            if deferral.deferredEdges != deferredEdges {
                deferredEdges = deferral.deferredEdges
                setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
            }
            // One call per turn, whatever the number of layouts and geometry
            // updates that scheduled it.
            model.driver?.sceneGeometryDidChange()
        }
    }
}

/// Tells the scene's root when the model's phase changes. Observation
/// reports a change on whichever thread made it, so the hop back to the main
/// actor must not carry the root's generic type: this class is not generic
/// and reaches the root through the protocol, on the main actor only.
@MainActor
private final class PhaseWatcher {
    private let model: AppModel
    private weak var target: (any SceneGeometryObserver)?

    init(model: AppModel, target: any SceneGeometryObserver) {
        self.model = model
        self.target = target
        observe()
    }

    private func observe() {
        withObservationTracking {
            _ = model.phase
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.observe()
                self?.target?.sceneGeometryDidChange()
            }
        }
    }
}
