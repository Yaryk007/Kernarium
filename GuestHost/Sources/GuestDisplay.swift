// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation
import GuestHostCore
import MetalKit
import SwiftUI
import UIKit

struct GuestDisplayGeometry {
    let pixelSize: CGSize
    let dotsPerInch: UInt32
}

/// What VoiceOver makes of the guest's display, in the application's words.
/// Without `directTouch` the display is one element: VoiceOver's own gestures
/// work over it. With `directTouch` it is a container of one element that
/// covers it, a direct-touch area: its touches reach the guest, so a screen
/// reader in the guest takes them, and VoiceOver says nothing of them. The
/// element is a child of the display, not the display itself, because
/// VoiceOver passes touches on only to a child element. Activating the
/// element, a VoiceOver double-tap, sends the guest no touch.
struct GuestDisplayAccessibility: Equatable {
    var label: String
    /// What the display says of the guest, as its value.
    var value: String?
    var hint: String?
    var directTouch = false
    /// Whether VoiceOver's focus may go to the direct-touch element as it
    /// appears: false while another screen covers the display or an
    /// announcement is still being spoken.
    var takesFocus = false
}

struct GuestDisplayView: UIViewControllerRepresentable {
    /// Kernarium's on-screen text: the guest's view then takes characters
    /// (`TextGuestMetalView`) and this says whether the system keyboard shows.
    /// Without it the view takes no text, so becoming first responder shows
    /// no keyboard and no shortcut bar, whatever has the focus.
    var keyboardVisible: Binding<Bool>?
    /// How many guest pixels the display draws to a point of the view, or the
    /// screen's own pixels (`GuestDisplayScale`). The density the guest is told
    /// follows it, so the guest's interface keeps its size.
    let scale: GuestDisplayScale
    /// What VoiceOver makes of the display; nil leaves it out of VoiceOver,
    /// as while another screen covers it.
    var accessibility: GuestDisplayAccessibility?
    /// Told each time touches begin on the display, after their contacts have
    /// gone to the guest. An application that draws controls over the display
    /// uses it to put them away when a touch lands on the guest instead.
    var touchDidBegin: (@MainActor () -> Void)?
    /// The hardware keyboard and mouse. A display with one takes the keys the
    /// system delivers to it and the system pointer's hover, buttons and
    /// scroll; a display without leaves them to the system, as Kernarium's
    /// does.
    var hardwareInput: GuestHardwareInput?
    /// Whether the display holds the keyboard focus the hardware keys need:
    /// the application says no while a sheet or an alert has the keys.
    var hardwareFocus = false
    /// Told of each contact sent to the guest's touchscreen: its slot, and
    /// whether it is down. A test uses it to see which touches the guest got.
    var contactDidChange: (@MainActor (_ slot: Int, _ pressed: Bool) -> Void)?
    let viewDidLayout: @MainActor (
        GuestMetalView,
        GuestDisplayGeometry
    ) -> Void

    func makeUIViewController(context: Context) -> GuestDisplayViewController {
        let device = MTLCreateSystemDefaultDevice()
        let view: GuestMetalView = keyboardVisible == nil
            ? GuestMetalView(frame: .zero, device: device)
            : TextGuestMetalView(frame: .zero, device: device)
        view.viewDidLayout = viewDidLayout
        view.touchDidBegin = touchDidBegin
        view.contactDidChange = contactDidChange
        view.scale = scale
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.autoResizeDrawable = false
        view.isMultipleTouchEnabled = true
        view.hardwareInput = hardwareInput
        view.applyAccessibility(accessibility)
        return GuestDisplayViewController(metalView: view)
    }

    func updateUIViewController(
        _ controller: GuestDisplayViewController,
        context: Context
    ) {
        let view = controller.metalView
        view.touchDidBegin = touchDidBegin
        view.contactDidChange = contactDidChange
        view.scale = scale
        view.hardwareInput = hardwareInput
        view.applyAccessibility(accessibility)
        if let keyboardVisible {
            view.setKeyboardVisible(keyboardVisible.wrappedValue)
        } else {
            view.setHardwareFocus(hardwareFocus)
        }
        view.reportLayout()
    }

    static func dismantleUIViewController(
        _ controller: GuestDisplayViewController,
        coordinator: ()
    ) {
        controller.metalView.cancelTrackedTouches()
        controller.metalView.finishHostResize()
        controller.metalView.hardwareInput = nil
        controller.metalView.resignFirstResponder()
    }
}



final class GuestDisplayViewController: UIViewController {
    let metalView: GuestMetalView
    private var resizeHold = GuestResizeHold()
    private var sceneGeometryObservation: NSKeyValueObservation?

    init(metalView: GuestMetalView) {
        self.metalView = metalView
        super.init(nibName: nil, bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func loadView() {
        view = metalView
    }

    override func viewWillTransition(
        to size: CGSize,
        with coordinator: UIViewControllerTransitionCoordinator
    ) {
        let (token, action) = resizeHold.transitionWillBegin()
        perform(action)
        super.viewWillTransition(to: size, with: coordinator)
        coordinator.animate(alongsideTransition: nil) { [weak self] _ in
            guard let self else { return }
            self.perform(self.resizeHold.transitionDidComplete(token: token))
        }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        guard sceneGeometryObservation == nil,
              let scene = view.window?.windowScene else { return }
        // An interactive window resize reports no transition of its own to
        // hold across, so the scene's geometry says when the gesture runs.
        sceneGeometryObservation = scene.observe(
            \.effectiveGeometry, options: [.initial, .new]
        ) { [weak self] scene, _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.perform(self.resizeHold.interactiveResizeChanged(
                    scene.effectiveGeometry.isInteractivelyResizing
                ))
            }
        }
    }

    private func perform(_ action: GuestResizeHold.Action) {
        switch action {
        case .none: break
        case .begin: metalView.beginHostResize()
        case .finish: metalView.finishHostResize()
        }
    }
}



/// The display's accessibility element while a screen reader in the guest is
/// on: it covers the display and takes no activation.
final class GuestDisplayElement: UIAccessibilityElement {
    override func accessibilityActivate() -> Bool { true }
}

private extension Notification.Name {
    static let guestDisplayResizeWillBegin = Notification.Name(
        "org.linux-um-darwin.guest-display-resize-will-begin"
    )
    static let guestDisplayResizeDidEnd = Notification.Name(
        "org.linux-um-darwin.guest-display-resize-did-end"
    )
}

class GuestMetalView: MTKView, UIGestureRecognizerDelegate, UIPointerInteractionDelegate {
    var viewDidLayout: (@MainActor (
        GuestMetalView,
        GuestDisplayGeometry
    ) -> Void)?
    var touchDidBegin: (@MainActor () -> Void)?
    var contactDidChange: (@MainActor (_ slot: Int, _ pressed: Bool) -> Void)?
    var scale = GuestDisplayScale.points
    @objc private(set) var umlGuestResizeInProgress = false
    private var touchContacts = GuestTouchContacts<ObjectIdentifier>()
    private var guestContentSize: CGSize?
    private var hardwareFocusWanted = false
    private var hoverRecognizer: UIHoverGestureRecognizer?
    private var scrollRecognizers: [UIPanGestureRecognizer] = []
    private var pointerInteraction: UIPointerInteraction?
    /// The touches of the system pointer's buttons that began as the guest's
    /// mouse: the rest of such a touch is the mouse's whatever the routing
    /// became meanwhile, and a touch that began as a finger stays one.
    private var pointerTouchIDs = Set<ObjectIdentifier>()
    private var windowObserver: NSObjectProtocol?
    /// The direct-touch element while the display describes itself that way,
    /// and whether the focus was last allowed to go to it.
    private var directTouchElement: GuestDisplayElement?
    private var focusWasAllowed = false
    /// What the display last described itself as, for `didMoveToWindow`.
    private var appliedAccessibility: GuestDisplayAccessibility?

    /// The hardware keyboard and mouse this view hands the system's events
    /// to. Without one the view leaves presses, hover and scroll to UIKit, and
    /// every touch is the touchscreen's. Setting another, or none, lifts
    /// whatever the old one holds down.
    var hardwareInput: GuestHardwareInput? {
        didSet {
            guard hardwareInput !== oldValue else { return }
            // A view that has handed the input to another (SwiftUI makes the
            // new view before it lets go of the old) leaves it as it is.
            if let oldValue, oldValue.pointerStyleOwner == nil || oldValue.pointerStyleOwner === self {
                oldValue.releaseAll()
                oldValue.hidesSystemPointerDidChange = nil
                oldValue.pointerStyleOwner = nil
            }
            pointerTouchIDs.removeAll()
            if let hardwareInput {
                hardwareInput.pointerStyleOwner = self
                hardwareInput.hidesSystemPointerDidChange = { [weak self] in self?.pointerStyleDidChange() }
                installPointerRecognizers()
            } else {
                removePointerRecognizers()
            }
        }
    }

    override init(frame frameRect: CGRect, device: MTLDevice?) {
        super.init(frame: frameRect, device: device)
        keepAspectWhileResizing()
    }

    required init(coder: NSCoder) {
        super.init(coder: coder)
        keepAspectWhileResizing()
    }

    /// The layer shows the last presented frame until the guest draws again,
    /// and its default `.resize` stretches that frame to whatever bounds the
    /// layer has meanwhile, which a rotation or a window resize makes another
    /// shape for a moment. Aspect-fit keeps the frame's proportions, and the
    /// bars it leaves beside the frame are black, as the guest's own bars are,
    /// whatever lies behind the view. Where the frame and the view have the
    /// same shape (the drawable is cleared to opaque black) nothing shows of
    /// either, bar the sliver that rounding the drawable down can leave.
    private func keepAspectWhileResizing() {
        layer.contentsGravity = .resizeAspect
        layer.backgroundColor = UIColor.black.cgColor
    }

    override var canBecomeFirstResponder: Bool { true }
    // While the guest has the keyboard focus, three fingers are the guest's
    // touches, not UIKit's undo, redo, copy and paste gestures.
    override var editingInteractionConfiguration: UIEditingInteractionConfiguration { .none }

    override func layoutSubviews() {
        super.layoutSubviews()
        directTouchElement?.accessibilityFrameInContainerSpace = bounds
        reportLayout()
        pointerStyleDidChange()
    }

    func reportLayout() {
        guard let screen = window?.screen,
              // SwiftUI supplies safe-area-excluded point bounds, which the
              // scale turns into guest pixels, fractional native scales
              // included.
              let layout = GuestDisplayLayout(
                  scale: scale, points: bounds.size, nativeScale: Double(screen.nativeScale)
              ) else { return }
        drawableSize = layout.pixelSize
        viewDidLayout?(
            self,
            GuestDisplayGeometry(
                pixelSize: layout.pixelSize,
                dotsPerInch: UInt32(layout.dotsPerInch)
            )
        )
    }

    func beginHostResize() {
        guard !umlGuestResizeInProgress else { return }
        umlGuestResizeInProgress = true
        NotificationCenter.default.post(
            name: .guestDisplayResizeWillBegin,
            object: self
        )
    }

    func finishHostResize() {
        guard umlGuestResizeInProgress else { return }
        reportLayout()
        umlGuestResizeInProgress = false
        NotificationCenter.default.post(
            name: .guestDisplayResizeDidEnd,
            object: self
        )
    }

    @objc(umlSetGuestDisplayWidth:height:)
    func setGuestDisplay(width: UInt32, height: UInt32) {
        cancelTrackedTouches()
        defer { pointerStyleDidChange() }
        guard width > 0, height > 0 else {
            guestContentSize = nil
            return
        }
        guestContentSize = CGSize(
            width: CGFloat(width),
            height: CGFloat(height)
        )
    }

    func applyAccessibility(_ accessibility: GuestDisplayAccessibility?) {
        appliedAccessibility = accessibility
        guard let accessibility, accessibility.directTouch else {
            directTouchElement = nil
            focusWasAllowed = false
            accessibilityElements = nil
            isAccessibilityElement = accessibility != nil
            accessibilityLabel = accessibility?.label
            accessibilityValue = accessibility?.value
            accessibilityHint = accessibility?.hint
            accessibilityTraits = .none
            return
        }
        isAccessibilityElement = false
        accessibilityLabel = nil
        accessibilityValue = nil
        accessibilityHint = nil
        accessibilityTraits = .none
        let appeared = directTouchElement == nil
        let element = directTouchElement ?? GuestDisplayElement(accessibilityContainer: self)
        element.accessibilityLabel = accessibility.label
        element.accessibilityValue = accessibility.value
        element.accessibilityHint = accessibility.hint
        // The trait first: the options apply to a direct-touch element.
        element.accessibilityTraits = .allowsDirectInteraction
        element.accessibilityDirectTouchOptions = .silentOnTouch
        element.accessibilityFrameInContainerSpace = bounds
        directTouchElement = element
        accessibilityElements = [element]
        // VoiceOver's focus is on the element before the first touch, as the
        // element appears or as nothing covers it any more, so that touch is
        // not spent moving the focus there. A post before the view is in a
        // window is lost: it waits for `didMoveToWindow`.
        let mayPost = accessibility.takesFocus && window != nil
        if mayPost, appeared || !focusWasAllowed, UIAccessibility.isVoiceOverRunning {
            UIAccessibility.post(notification: .screenChanged, argument: element)
        }
        focusWasAllowed = mayPost
    }

    /// A VoiceOver activation of the display is no touch: the default would tap
    /// the guest at the element's activation point.
    override func accessibilityActivate() -> Bool {
        isAccessibilityElement ? true : super.accessibilityActivate()
    }

    func setKeyboardVisible(_ visible: Bool) {
        if visible {
            becomeFirstResponder()
        } else {
            resignFirstResponder()
        }
    }

    // MARK: Touches

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesBegan(touches, with: event)
        reclaimHardwareFocus()
        let (pointerTouches, fingers) = split(touches, beginning: true)
        for touch in fingers where touch.type == .indirectPointer {
            hardwareInput?.pointerTouchBeganUnrouted()
        }
        for touch in fingers {
            sendTouchEvents(touchContacts.begin(
                ObjectIdentifier(touch),
                at: touch.location(in: self),
                geometry: touchGeometry()
            ))
        }
        forward(pointerTouches, .began, event)
        touchDidBegin?()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesMoved(touches, with: event)
        let (pointerTouches, fingers) = split(touches, beginning: false)
        for touch in fingers {
            let coalesced = (event?.coalescedTouches(for: touch) ?? []).map {
                $0.location(in: self)
            }
            sendTouchEvents(touchContacts.move(
                ObjectIdentifier(touch),
                through: GuestTouchSamples.ordered(
                    coalesced: coalesced,
                    current: touch.location(in: self)
                ),
                geometry: touchGeometry()
            ))
        }
        forward(pointerTouches, .moved, event)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesEnded(touches, with: event)
        let (pointerTouches, fingers) = split(touches, beginning: false)
        for touch in fingers {
            sendTouchEvents(touchContacts.end(
                ObjectIdentifier(touch),
                at: touch.location(in: self),
                geometry: touchGeometry()
            ))
        }
        forward(pointerTouches, .ended, event)
        for touch in touches { pointerTouchIDs.remove(ObjectIdentifier(touch)) }
    }

    override func touchesCancelled(
        _ touches: Set<UITouch>,
        with event: UIEvent?
    ) {
        super.touchesCancelled(touches, with: event)
        let (pointerTouches, fingers) = split(touches, beginning: false)
        for touch in fingers {
            sendTouchEvents(touchContacts.cancel(ObjectIdentifier(touch)))
        }
        forward(pointerTouches, .cancelled, event)
        for touch in touches { pointerTouchIDs.remove(ObjectIdentifier(touch)) }
    }

    /// The touches of the system pointer's buttons go to the guest's mouse
    /// when the application gives it one; every other touch is a finger of
    /// the touchscreen, as is a pointer's when there is no mouse. A touch is
    /// the mouse's from the moment it begins to its end, whatever the routing
    /// does meanwhile, and a finger's likewise.
    private func split(_ touches: Set<UITouch>, beginning: Bool) -> (pointer: [UITouch], fingers: [UITouch]) {
        guard let hardwareInput else { return ([], Array(touches)) }
        var pointer: [UITouch] = []
        var fingers: [UITouch] = []
        for touch in touches {
            let identity = ObjectIdentifier(touch)
            guard touch.type == .indirectPointer else {
                fingers.append(touch)
                continue
            }
            if beginning {
                if hardwareInput.isActive, hardwareInput.routesPointer {
                    pointerTouchIDs.insert(identity)
                    pointer.append(touch)
                } else {
                    fingers.append(touch)
                }
            } else if pointerTouchIDs.contains(identity) {
                pointer.append(touch)
            } else {
                fingers.append(touch)
            }
        }
        return (pointer, fingers)
    }

    private func forward(_ touches: [UITouch], _ phase: GuestPointerTracker.Phase, _ event: UIEvent?) {
        guard let hardwareInput else { return }
        let mapping = pointerMapping()
        for touch in touches {
            let buttons = HardwareButtons.normalized(uiKitMask: event?.buttonMask.rawValue ?? 0)
            hardwareInput.pointerTouch(
                phase, at: mapping?.position(of: touch.location(in: self)), buttons: buttons)
        }
    }

    private func touchGeometry() -> GuestTouchGeometry? {
        GuestTouchGeometry(
            coordinateSpace: .touchscreen,
            boundsSize: bounds.size,
            drawableSize: drawableSize,
            contentSize: guestContentSize ?? drawableSize
        )
    }

    private func pointerMapping() -> GuestPointerMapping? {
        GuestPointerMapping(
            boundsSize: bounds.size,
            drawableSize: drawableSize,
            contentSize: guestContentSize ?? drawableSize
        )
    }

    private func sendTouchEvents(_ events: [GuestTouchContactEvent]) {
        for contact in events {
            if contact.event.cancelled {
                kernarium_runtime_loader_cancel_touch(contact.slot)
                contactDidChange?(Int(contact.slot), false)
                continue
            }
            kernarium_runtime_loader_send_touch(
                contact.slot,
                contact.event.x,
                contact.event.y,
                contact.event.pressed ? 1 : 0
            )
            contactDidChange?(Int(contact.slot), contact.event.pressed)
        }
    }

    func cancelTrackedTouches() {
        sendTouchEvents(touchContacts.cancelAll())
        pointerTouchIDs.removeAll()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if let windowObserver {
            NotificationCenter.default.removeObserver(windowObserver)
            self.windowObserver = nil
        }
        if let window {
            // Keys held when the window stops being the key one (another
            // window or app took the focus) are not released by UIKit.
            windowObserver = NotificationCenter.default.addObserver(
                forName: UIWindow.didResignKeyNotification, object: window, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.hardwareInput?.releaseKeys() }
            }
        }
        if window == nil {
            cancelTrackedTouches()
            hardwareInput?.releaseAll()
        } else if hardwareFocusWanted {
            claimHardwareFocus()
        }
        if window != nil, appliedAccessibility?.takesFocus == true, !focusWasAllowed {
            applyAccessibility(appliedAccessibility)
        }
    }

    // MARK: Hardware keyboard

    /// Whether the display holds the keyboard focus for the hardware keys.
    /// Becoming first responder of a view that takes no text shows no
    /// keyboard, so the display holds it whenever the guest is on screen and
    /// nothing else (a sheet, an alert) needs the keys.
    func setHardwareFocus(_ wanted: Bool) {
        guard hardwareInput != nil || !wanted else { return }
        hardwareFocusWanted = wanted
        if wanted {
            claimHardwareFocus()
        } else if isFirstResponder {
            resignFirstResponder()
        }
    }

    private func claimHardwareFocus() {
        guard window != nil, !isFirstResponder else { return }
        becomeFirstResponder()
    }

    /// A touch on the display is the person turning to the guest: it takes
    /// the keys back from anything that had them.
    private func reclaimHardwareFocus() {
        if hardwareFocusWanted { claimHardwareFocus() }
    }

    @discardableResult
    override func resignFirstResponder() -> Bool {
        // Keys held when the keys go elsewhere are not released by anyone else.
        hardwareInput?.releaseKeys()
        return super.resignFirstResponder()
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard let hardwareInput else {
            super.pressesBegan(presses, with: event)
            return
        }
        // A press the guest does not take (a remote's button) is the system's.
        var others = Set<UIPress>()
        var keys: [UInt32: Bool] = [:]
        for press in presses {
            guard let key = press.key else {
                others.insert(press)
                continue
            }
            keys[UInt32(truncatingIfNeeded: key.keyCode.rawValue)] = key.modifierFlags.contains(.alphaShift)
        }
        for usage in HardwareKeyboardState.ordered(Array(keys.keys), pressing: true) {
            hardwareInput.keyDown(usage: usage, alphaShift: keys[usage] ?? false)
        }
        if !others.isEmpty { super.pressesBegan(others, with: event) }
    }

    override func pressesChanged(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard hardwareInput != nil else {
            super.pressesChanged(presses, with: event)
            return
        }
        let others = presses.filter { $0.key == nil }
        if !others.isEmpty { super.pressesChanged(others, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard let hardwareInput else {
            super.pressesEnded(presses, with: event)
            return
        }
        let usages = presses.compactMap { $0.key.map { UInt32(truncatingIfNeeded: $0.keyCode.rawValue) } }
        for usage in HardwareKeyboardState.ordered(usages, pressing: false) {
            hardwareInput.keyUp(usage: usage)
        }
        let others = presses.filter { $0.key == nil }
        if !others.isEmpty { super.pressesEnded(others, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard let hardwareInput else {
            super.pressesCancelled(presses, with: event)
            return
        }
        // The system took the keys (Command-H, an alert): none of the ones
        // held will come up on its own.
        hardwareInput.releaseKeys()
        let others = presses.filter { $0.key == nil }
        if !others.isEmpty { super.pressesCancelled(others, with: event) }
    }

    // MARK: System pointer

    private func installPointerRecognizers() {
        guard hoverRecognizer == nil else { return }
        let hover = UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:)))
        hover.delegate = self
        addGestureRecognizer(hover)
        hoverRecognizer = hover
        // A trackpad's two-finger scroll is continuous and a wheel's step is
        // discrete; the two differ in how far a step of the guest's wheel is.
        for mask in [UIScrollTypeMask.continuous, .discrete] {
            let scroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
            scroll.allowedScrollTypesMask = mask
            // Only a scroll event: a finger's drag is a touch of the guest's.
            scroll.maximumNumberOfTouches = 0
            scroll.cancelsTouchesInView = false
            scroll.delaysTouchesBegan = false
            scroll.delaysTouchesEnded = false
            scroll.delegate = self
            addGestureRecognizer(scroll)
            scrollRecognizers.append(scroll)
        }
        let interaction = UIPointerInteraction(delegate: self)
        addInteraction(interaction)
        pointerInteraction = interaction
    }

    private func removePointerRecognizers() {
        if let hoverRecognizer { removeGestureRecognizer(hoverRecognizer) }
        hoverRecognizer = nil
        for recognizer in scrollRecognizers { removeGestureRecognizer(recognizer) }
        scrollRecognizers = []
        if let pointerInteraction { removeInteraction(pointerInteraction) }
        pointerInteraction = nil
    }

    /// Asks UIKit to ask again whether the system pointer is hidden over the
    /// display, after `GuestHardwareInput.routesPointer` changed.
    func pointerStyleDidChange() {
        pointerInteraction?.invalidate()
    }

    @objc private func hovered(_ recognizer: UIHoverGestureRecognizer) {
        guard let hardwareInput else { return }
        switch recognizer.state {
        case .began, .changed:
            // A hovering Apple Pencil is not a mouse.
            guard recognizer.zOffset == 0 else { return }
            let point = recognizer.location(in: self)
            hardwareInput.hover(pointerMapping()?.position(of: point), at: point, beginning: recognizer.state == .began)
        case .ended, .cancelled, .failed:
            hardwareInput.hover(nil, at: nil, beginning: false)
        default:
            break
        }
    }

    @objc private func scrolled(_ recognizer: UIPanGestureRecognizer) {
        guard let hardwareInput else { return }
        switch recognizer.state {
        case .began, .changed:
            let translation = recognizer.translation(in: self)
            recognizer.setTranslation(.zero, in: self)
            hardwareInput.scroll(
                translation: translation, continuous: recognizer.allowedScrollTypesMask == .continuous,
                beginning: recognizer.state == .began)
        default:
            break
        }
    }

    func gestureRecognizer(
        _ gestureRecognizer: UIGestureRecognizer,
        shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer
    ) -> Bool {
        true
    }

    /// The pointer is hidden over the guest's display only, not over the bars
    /// beside it: that is where the system's pointer is still the person's.
    func pointerInteraction(
        _ interaction: UIPointerInteraction, regionFor request: UIPointerRegionRequest,
        defaultRegion: UIPointerRegion
    ) -> UIPointerRegion? {
        hiddenPointerRegion(at: request.location).map { UIPointerRegion(rect: $0) }
    }

    /// The part of the view where the system pointer is hidden, if `location`
    /// is in it: the guest's display while the mouse is the guest's.
    func hiddenPointerRegion(at location: CGPoint) -> CGRect? {
        guard hardwareInput?.hidesSystemPointer == true,
              let frame = pointerMapping()?.displayFrame, frame.contains(location) else { return nil }
        return frame
    }

    func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? {
        hardwareInput?.hidesSystemPointer == true ? .hidden() : nil
    }
}

/// Kernarium's guest view, which also takes the characters of the system
/// keyboard: the on-screen keyboard types into the guest through the Linux
/// keyboard map, by character. Andarium's view takes no text, and its keys
/// are positions (`GuestHardwareInput`).
final class TextGuestMetalView: GuestMetalView, UIKeyInput {
    var hasText: Bool { true }
    var keyboardType: UIKeyboardType = .asciiCapable
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var smartDashesType: UITextSmartDashesType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no

    func insertText(_ text: String) {
        for character in text {
            guard let stroke = LinuxKeyboard.stroke(for: character) else {
                print("Kernarium: unsupported keyboard character \(character)")
                continue
            }
            if stroke.shifted {
                sendKey(code: LinuxKeyboard.leftShift, pressed: true)
            }
            sendKey(code: stroke.code, pressed: true)
            sendKey(code: stroke.code, pressed: false)
            if stroke.shifted {
                sendKey(code: LinuxKeyboard.leftShift, pressed: false)
            }
        }
    }

    func deleteBackward() {
        sendKey(code: LinuxKeyboard.backspace, pressed: true)
        sendKey(code: LinuxKeyboard.backspace, pressed: false)
    }

    private func sendKey(code: UInt16, pressed: Bool) {
        kernarium_runtime_loader_send_key(code, pressed ? 1 : 0)
    }
}
