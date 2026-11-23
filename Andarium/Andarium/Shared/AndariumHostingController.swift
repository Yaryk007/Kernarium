// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Observation
import SwiftUI
import UIKit

@MainActor
protocol SceneGeometryObserver: AnyObject {
    func sceneGeometryDidChange()
}

extension Phase {
    /// The guest's display is under the phase's screens: the boot and the
    /// run. `GuestLayer` draws it, and the scene's root keeps everything
    /// behind it black.
    var showsGuestDisplay: Bool {
        switch self {
        case .booting, .running: true
        case .home, .failed: false
        }
    }
}

/// The scene's root controller owns the supported orientations and, in a
/// run, the screen edges that defer system gestures. A child of SwiftUI's
/// default root cannot reliably supply these scene-wide preferences. It also
/// owns what shows wherever SwiftUI has not drawn: while the guest's display
/// is shown that is black, in the light appearance too. It never asks iPadOS
/// to lock the interface: an iPad's interface turns with the device in a
/// session as it does at Home, and the guest follows it
/// (`SessionOrientationPolicy`). It also reads the folding display a scene may
/// be on (`FoldSceneReader`) for the layout and for the rotation rule: the
/// inner display of a folding iPhone turns like an iPad's, its cover is an
/// iPhone's.
final class AndariumHostingController<Content: View>: UIHostingController<Content>, SceneGeometryObserver {
    private let model: AppModel
    /// The run's deferred edges; nil leaves SwiftUI's preference in charge.
    private var deferredEdges: ScreenEdges?
    private var updateScheduled = false
    /// What the last update found, which a later trigger compares with.
    private var appliedInputs: SceneUpdateInputs?
    /// Where the update reads its inputs when a test supplies them instead of
    /// the scene and the model.
    var inputsSource: (@MainActor () -> SceneUpdateInputs?)?
    private var phaseWatcher: PhaseWatcher?
    private var pointerLockWatcher: PointerLockWatcher?
    let foldReader = FoldSceneReader()
    /// The backing of the root view and of its window as they were before
    /// the guest's display painted them black, for the screens that follow
    /// the system appearance to get back.
    private var viewPaintedForGuest = false
    private var viewBackingBeforeGuest: UIColor?
    private weak var windowPaintedForGuest: UIWindow?
    private var windowBackingBeforeGuest: UIColor?

    init(rootView: Content, model: AppModel) {
        self.model = model
        super.init(rootView: rootView)
        phaseWatcher = PhaseWatcher(model: model) { [weak self] in self?.phaseDidChange() }
        pointerLockWatcher = PointerLockWatcher(model: model, controller: self)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    /// Whether the interface turns with the device: an iPad's does, an
    /// iPhone's is portrait-only, and a folding iPhone's inner display turns
    /// while its cover does not (`FoldPose.interfaceRotates`); a guest that
    /// follows the device's orientation on a device that is no iPhone (a launch
    /// states it) keeps a portrait-only interface as an iPhone does. The one
    /// place that decides it for this scene's display, for the supported
    /// orientations and the guest's policy.
    private var interfaceRotates: Bool {
        let phoneLike = UIDevice.current.userInterfaceIdiom == .phone || model.followsDeviceOrientation
        return model.foldPose.interfaceRotates(idiomIsPhone: phoneLike)
    }

    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        interfaceRotates ? .all : .portrait
    }

    /// An Android application captured the pointer: iOS keeps the system's
    /// pointer where it is and delivers the mouse's raw motion only, which the
    /// guest takes. The system may refuse (a window that is not full screen).
    override var prefersPointerLocked: Bool { model.pointerLockWanted }

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

    override func viewDidLoad() {
        super.viewDidLoad()
        foldReader.hingeDidChange = { [weak self] in self?.updateFold() }
        foldReader.attach(to: view)
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        updateBacking()
    }

    /// Reads the folding display again and hands it to the model, which the
    /// screens lay out from. A display that does not fold never has a reading,
    /// so nothing here changes anything there. A move between the cover and
    /// the inner display changes whether the interface turns.
    private func updateFold() {
        let reading = foldReader.read(view)
        guard reading != model.fold else { return }
        let rotated = interfaceRotates
        model.fold = reading
        if interfaceRotates != rotated { setNeedsUpdateOfSupportedInterfaceOrientations() }
        scheduleUpdate()
    }

    override func viewWillTransition(to size: CGSize, with coordinator: UIViewControllerTransitionCoordinator) {
        super.viewWillTransition(to: size, with: coordinator)
        // A fold or a turn changes the window and its reserved regions
        // together: the layout must have them when the transition completes,
        // which is when the guest takes its new size.
        coordinator.animate(alongsideTransition: { [weak self] _ in self?.updateFold() }) { [weak self] _ in
            self?.updateFold()
        }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        updateBacking()
        scheduleUpdate()
    }

    func sceneGeometryDidChange() {
        scheduleUpdate()
    }

    private func phaseDidChange() {
        updateBacking()
        sceneGeometryDidChange()
    }

    /// While the guest's display is shown, the root view and its window are
    /// black whatever the appearance. SwiftUI's own black layer
    /// (`GuestLayer`) covers only what SwiftUI has laid out, and what shows
    /// beyond it (a rotation, a window resize, the first frame of a phase,
    /// the area outside the safe area before it lays out) is these two, which
    /// follow the system and are white in the light appearance. Home, sheets
    /// and the session's own controls are SwiftUI's and keep following the
    /// system. Every layout pass runs this, so a backing is assigned only when
    /// it is not black already: a layer is not dirtied for nothing.
    private func updateBacking() {
        guard isViewLoaded else { return }
        if model.phase.showsGuestDisplay {
            if !viewPaintedForGuest {
                viewPaintedForGuest = true
                viewBackingBeforeGuest = view.backgroundColor
            }
            if view.backgroundColor != .black { view.backgroundColor = .black }
            if let window = view.window {
                if windowPaintedForGuest !== window {
                    restoreWindowBacking()
                    windowPaintedForGuest = window
                    windowBackingBeforeGuest = window.backgroundColor
                }
                if window.backgroundColor != .black { window.backgroundColor = .black }
            }
        } else {
            if viewPaintedForGuest {
                viewPaintedForGuest = false
                view.backgroundColor = viewBackingBeforeGuest
                viewBackingBeforeGuest = nil
            }
            restoreWindowBacking()
        }
    }

    private func restoreWindowBacking() {
        guard let window = windowPaintedForGuest else { return }
        window.backgroundColor = windowBackingBeforeGuest
        windowPaintedForGuest = nil
        windowBackingBeforeGuest = nil
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        // The window may have come after the phase did.
        updateBacking()
        updateFold()
        scheduleUpdate()
    }

    /// What the update reads, or nil while the root is not in a scene.
    private func currentInputs() -> SceneUpdateInputs? {
        if let inputsSource { return inputsSource() }
        guard let scene = view.window?.windowScene else { return nil }
        let geometry = scene.effectiveGeometry
        return SceneUpdateInputs(
            scene: ObjectIdentifier(scene), driver: model.driver.map { ObjectIdentifier($0) },
            sceneSize: geometry.coordinateSpace.bounds.size, screenSize: scene.screen.bounds.size,
            interfaceOrientation: geometry.interfaceOrientation.rawValue, phase: model.phase,
            interfaceRotates: interfaceRotates)
    }

    /// Every layout of the root, every geometry change and every phase change
    /// asks for the update, and SwiftUI lays the root out after each redraw. An
    /// update that would find every input where the last one did has nothing
    /// to do, so it is not scheduled: what else the guest follows (the
    /// foreground, its boot, the overlays, the location) reaches it by its own
    /// trigger, not through the root.
    private func scheduleUpdate() {
        guard !updateScheduled else { return }
        if let inputs = currentInputs(), inputs == appliedInputs { return }
        updateScheduled = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            updateScheduled = false
            guard let inputs = currentInputs() else { return }
            appliedInputs = inputs
            model.hostDrivesGuestRotation = SessionOrientationPolicy(
                phase: inputs.phase, interfaceRotates: interfaceRotates).hostDrivesGuestRotation
            let deferral = SessionGestureDeferral(
                phase: inputs.phase, sceneSize: inputs.sceneSize, screenSize: inputs.screenSize)
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
/// and reaches the root through a closure, on the main actor only.
@MainActor
private final class PhaseWatcher {
    private let model: AppModel
    private let changed: @MainActor () -> Void

    init(model: AppModel, changed: @escaping @MainActor () -> Void) {
        self.model = model
        self.changed = changed
        observe()
    }

    private func observe() {
        withObservationTracking {
            _ = model.phase
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.observe()
                self?.changed()
            }
        }
    }
}

/// Asks iOS to lock or release the pointer when the guest's wish changes, and
/// tells the driver when iOS has done it. Observation reports a change on
/// whichever thread made it, so the hop to the main actor does not carry the
/// root's generic type, as for the phase.
@MainActor
private final class PointerLockWatcher {
    private let model: AppModel
    private weak var controller: UIViewController?
    /// Read only by `deinit`, which cannot be isolated, after the last use.
    nonisolated(unsafe) private var lockObserver: NSObjectProtocol?

    init(model: AppModel, controller: UIViewController) {
        self.model = model
        self.controller = controller
        observe()
        lockObserver = NotificationCenter.default.addObserver(
            forName: UIPointerLockState.didChangeNotification, object: nil, queue: .main
        ) { [weak self] note in
            let scene = note.userInfo?[UIPointerLockState.sceneUserInfoKey] as? UIScene
            MainActor.assumeIsolated { self?.lockStateChanged(scene) }
        }
    }

    deinit {
        if let lockObserver { NotificationCenter.default.removeObserver(lockObserver) }
    }

    private func observe() {
        withObservationTracking {
            _ = model.pointerLockWanted
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.observe()
                self?.controller?.setNeedsUpdateOfPrefersPointerLocked()
            }
        }
    }

    private func lockStateChanged(_ scene: UIScene?) {
        // Only the scene of this controller's window counts.
        guard let controller, scene == nil || scene === controller.view.window?.windowScene else { return }
        let locked = controller.view.window?.windowScene?.pointerLockState?.isLocked ?? false
        model.driver?.pointerLockDidChange(locked)
    }
}
