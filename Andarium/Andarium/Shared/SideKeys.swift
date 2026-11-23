// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import TipKit

/// What the side keys do. The Simulator preview plays them against mock
/// screens; the device application sends them to Android.
struct SideKeyActions {
    var power: () -> Void
    var powerHold: () -> Void
    var volumeUp: () -> Void
    var volumeDown: () -> Void
    var back: () -> Void
    var home: () -> Void
    var settings: () -> Void
}

/// Where VoiceOver goes as the keys open and close.
enum SideKeyFocus: Hashable {
    case closed, power
}

/// The screen edge the closed side keys are on. The words that tell a person
/// where to touch to come back to VoiceOver name it (`ScreenReaderWords.wayBack`).
enum KeysEdge: Equatable {
    case left, right
}

/// The side keys on the screen edge: closed they are a pair of slim glass
/// keys; tapped they swell in place into Power and a volume rocker, with
/// Settings and a Back and Home pair right beside them as one small group.
/// They move up and down the edge with a drag and across to the other edge;
/// VoiceOver moves them with the closed keys' actions.
///
/// Only the keys take touches, and the tip beside the closed keys on its own
/// card. While they are open, a touch anywhere else reaches the guest as it
/// would with them closed, and the guest's display closes the keys when it
/// receives that touch (`SessionState.closeSideKeys`).
struct SideKeysOverlay: View {
    /// How the keys open and close.
    static let motion = Animation.spring(duration: 0.36, bounce: 0.22)

    let mode: StartMode
    /// Whether the guest's screen is on, for Power's light.
    let lit: Bool
    /// Whether the keys are open. The session owns it, so the guest's
    /// display can close them.
    @Binding var open: Bool
    let actions: SideKeyActions

    @State private var leading: Bool
    @State private var fraction: CGFloat = 0.32
    @State private var dragStart: CGPoint?
    /// Counts key presses for the click felt on each one.
    @State private var keyPresses = 0
    @Namespace private var keysSpace
    private let tip: SideKeysTip
    /// The tip's popover is up: while the closed keys are on screen and the
    /// tip wants to show.
    @State private var tipShown = false
    @Environment(AppModel.self) private var model: AppModel?
    @AccessibilityFocusState private var focus: SideKeyFocus?

    /// `tip` is the tip beside the closed keys. It says where Install APK is
    /// where Settings has the row in this session
    /// (`Capabilities.offersApkInstall`).
    init(
        mode: StartMode, lit: Bool, open: Binding<Bool>, leading: Bool = false, tip: SideKeysTip,
        actions: SideKeyActions
    ) {
        self.mode = mode
        self.lit = lit
        _open = open
        self.actions = actions
        self.tip = tip
        _leading = State(initialValue: leading)
    }

    var body: some View {
        GeometryReader { geo in
            keys(in: geo.size)
        }
        .ignoresSafeArea(.keyboard)
        .interfaceFeedback(.impact(weight: .light), trigger: keyPresses)
        .onAppear { publishEdge() }
        .onChange(of: leading) { publishEdge() }
        .onChange(of: open) { _, isOpen in
            guard isOpen else { return }
            // VoiceOver goes to the first key once the keys have swollen.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                if open { focus = .power }
            }
        }
    }

    @ViewBuilder
    private func keys(in size: CGSize) -> some View {
        let edgeX: CGFloat = leading ? 0 : size.width
        let closedY = min(max(70, size.height * fraction), size.height - 70)
        let openY = min(max(118, size.height * fraction), max(118, size.height - 118))
        let inward: CGFloat = leading ? 1 : -1
        let recoveryMode = mode == .recovery
        // The group's width: Power's column, and in Android the column beside it.
        let groupWidth: CGFloat = recoveryMode ? KeyMetrics.width : KeyMetrics.width * 2 + KeyMetrics.columnGap

        ZStack {
            if open {
                // A soft shade behind the group keeps it legible over any
                // screen. It takes no touch: one beside the group reaches the
                // guest, which closes the keys.
                RadialGradient(
                    colors: [.black.opacity(0.5), .black.opacity(0.16), .black.opacity(0)],
                    center: UnitPoint(x: edgeX / max(size.width, 1), y: openY / max(size.height, 1)),
                    startRadius: 20, endRadius: 300
                )
                .allowsHitTesting(false)
                .transition(.opacity)

                GlassEffectContainer(spacing: 4) {
                    HStack(alignment: .top, spacing: KeyMetrics.columnGap) {
                        if leading {
                            powerColumn
                            if !recoveryMode { guestColumn }
                        } else {
                            if !recoveryMode { guestColumn }
                            powerColumn
                        }
                    }
                }
                .accessibilityAction(.escape) {
                    setOpen(false)
                    focus = .closed
                }
                .position(x: edgeX + inward * (KeyMetrics.edgeGap + groupWidth / 2), y: openY)
            } else {
                ClosedKeys(space: keysSpace, leading: leading)
                    // SwiftUI's own popover, not `popoverTip`: TipKit leaves
                    // its popover on screen when the keys it points at go
                    // away (opening, or the session ending), over the open
                    // keys or Home, where its card takes their touches.
                    // SwiftUI dismisses its popover with the closed keys.
                    .popover(
                        isPresented: Binding(get: { tipShown }, set: { tipShown = $0 }),
                        arrowEdge: leading ? .leading : .trailing
                    ) {
                        SideKeysTipCard(
                            tip: tip, maxHeight: SideKeysTipCard.maxHeight(in: size.height),
                            width: SideKeysTipCard.width(in: size.width))
                    }
                    .task {
                        for await shows in tip.shouldDisplayUpdates { tipShown = shows }
                    }
                    .onDisappear { tipShown = false }
                    .accessibilityFocused($focus, equals: .closed)
                    .accessibilityAction { setOpen(true) }
                    .accessibilityActions { closedKeysActions }
                    .position(x: edgeX + inward * 18, y: closedY)
                    .gesture(dragKeys(size: size))
                    .onTapGesture { setOpen(true) }
            }
        }
    }

    /// What VoiceOver can do with the closed keys besides opening them: move
    /// them as a drag would.
    @ViewBuilder
    private var closedKeysActions: some View {
        Button(leading ? "Move to the right edge" : "Move to the left edge") {
            withAnimation(.snappy) { leading.toggle() }
        }
        if fraction > 0.15 {
            Button("Move up") { withAnimation(.snappy) { fraction = max(0.1, fraction - 0.15) } }
        }
        if fraction < 0.85 {
            Button("Move down") { withAnimation(.snappy) { fraction = min(0.9, fraction + 0.15) } }
        }
    }

    private var powerColumn: some View {
        OpenKeys(
            space: keysSpace, recovery: mode == .recovery,
            lit: lit, focus: $focus,
            power: { pressed(actions.power) }, powerHold: holdPower,
            volumeUp: { pressed(actions.volumeUp) }, volumeDown: { pressed(actions.volumeDown) }
        )
    }

    private var guestColumn: some View {
        GuestKeys(
            settings: openSettings,
            back: { pressed(actions.back) },
            home: { pressed(actions.home) }
        )
        .transition(
            .asymmetric(
                insertion: .scale(scale: 0.6, anchor: leading ? .leading : .trailing)
                    .combined(with: .opacity)
                    .animation(.spring(duration: 0.34, bounce: 0.25).delay(0.04)),
                removal: .opacity.animation(.easeOut(duration: 0.12))
            )
        )
    }

    private func pressed(_ action: () -> Void) {
        keyPresses += 1
        action()
    }

    /// Android's power menu opens over the guest, so the keys step aside.
    private func holdPower() {
        guard mode == .normal else { return }
        actions.powerHold()
        withAnimation(.snappy) { open = false }
    }

    private func openSettings() {
        setOpen(false)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { actions.settings() }
    }

    private func dragKeys(size: CGSize) -> some Gesture {
        DragGesture(minimumDistance: 6, coordinateSpace: .local)
            .onChanged { value in
                if dragStart == nil {
                    dragStart = CGPoint(x: leading ? 0 : size.width, y: size.height * fraction)
                }
                guard let start = dragStart else { return }
                let y = start.y + value.translation.height
                fraction = min(max(0.1, y / size.height), 0.9)
            }
            .onEnded { value in
                // Dragged across the middle: the keys move to the other edge.
                let x = value.location.x
                if leading && x > size.width * 0.6 {
                    withAnimation(.snappy) { leading = false }
                } else if !leading && x < size.width * 0.4 {
                    withAnimation(.snappy) { leading = true }
                }
                dragStart = nil
            }
    }

    /// The session says where the keys are, so the words that send a person back
    /// to them name the edge they are on.
    private func publishEdge() {
        model?.session.keysEdge = leading ? .left : .right
    }

    private func setOpen(_ value: Bool) {
        withAnimation(Self.motion) { open = value }
        if value { tip.invalidate(reason: .actionPerformed) }
    }
}

// MARK: - Closed and open keys

enum KeyMetrics {
    static let width: CGFloat = 52
    static let keyHeight: CGFloat = 62
    static let rowGap: CGFloat = 10
    static let columnGap: CGFloat = 8
    static let edgeGap: CGFloat = 8
    /// A key on the deck, which lies flat under the thumbs: wider than tall.
    static let deckKey = CGSize(width: 66, height: 54)
    /// A key in the cover's rail, which shares the strip with the stats.
    static let railKeyHeight: CGFloat = 56
    static let railGap: CGFloat = 8
}

/// The closed keys: Power and a longer volume rocker, each a plain glass key
/// on the screen edge. They show nothing about Android's state.
struct ClosedKeys: View {
    let space: Namespace.ID
    let leading: Bool

    var body: some View {
        VStack(spacing: 6) {
            key(height: 30)
                .matchedGeometryEffect(id: "power", in: space)
            key(height: 58)
                .matchedGeometryEffect(id: "rocker", in: space)
        }
        .padding(leading ? .leading : .trailing, 3)
        .frame(width: 36, height: 124, alignment: leading ? .leading : .trailing)
        .contentShape(Rectangle())
        .accessibilityElement()
        .accessibilityLabel("Side keys")
        .accessibilityHint("Opens Power, volume, Back, Home and Settings")
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("side-keys")
    }

    private func key(height: CGFloat) -> some View {
        Capsule()
            .fill(.white.opacity(0.9))
            .frame(width: 9, height: height)
            .glassEffect(.regular, in: .capsule)
            .shadow(color: .black.opacity(0.35), radius: 3, x: leading ? 1 : -1)
    }
}

/// The open keys, swollen in place: Power (tap to press, hold for Android's
/// power menu) above the volume rocker.
struct OpenKeys: View {
    let space: Namespace.ID
    let recovery: Bool
    let lit: Bool
    var focus: AccessibilityFocusState<SideKeyFocus?>.Binding
    let power: () -> Void
    let powerHold: () -> Void
    let volumeUp: () -> Void
    let volumeDown: () -> Void

    var body: some View {
        VStack(spacing: KeyMetrics.rowGap) {
            PowerKey(recovery: recovery, lit: lit, power: power, powerHold: powerHold)
                .matchedGeometryEffect(id: "power", in: space)
                .accessibilityFocused(focus, equals: .power)

            KeyPair(
                top: (recovery ? "chevron.up" : "plus", recovery ? String(localized: "Up") : String(localized: "Volume up"), volumeUp),
                bottom: (recovery ? "chevron.down" : "minus", recovery ? String(localized: "Down") : String(localized: "Volume down"), volumeDown)
            )
            .matchedGeometryEffect(id: "rocker", in: space)
        }
        .foregroundStyle(.primary)
    }
}

/// Power: tap to press, hold for Android's power menu. Its light says
/// whether the guest's screen is on.
struct PowerKey: View {
    let recovery: Bool
    let lit: Bool
    var size = CGSize(width: KeyMetrics.width, height: KeyMetrics.keyHeight)
    let power: () -> Void
    let powerHold: () -> Void

    var body: some View {
        ZStack {
            Capsule().fill(.clear)
            VStack(spacing: 3) {
                Image(systemName: "power")
                    .font(.system(size: 20, weight: .semibold))
                Circle()
                    .fill(lit ? Color.accentColor : Color.gray.opacity(0.5))
                    .frame(width: 5, height: 5)
                    .shadow(color: lit ? Color.accentColor : .clear, radius: 3)
            }
        }
        .frame(width: size.width, height: size.height)
        .glassEffect(.regular.interactive(), in: .capsule)
        .contentShape(.capsule)
        .onTapGesture(perform: power)
        .onLongPressGesture(minimumDuration: 0.6, perform: powerHold)
        .accessibilityElement()
        .accessibilityLabel("Power")
        .accessibilityHint(recovery ? "Chooses the highlighted item" : "Hold for the power menu")
        .accessibilityAddTraits(.isButton)
        .accessibilityAction { power() }
        .accessibilityAction(named: "Power menu", powerHold)
        .accessibilityIdentifier("side-keys-power")
    }
}

/// The keys Android has and an iPhone does not, beside Power and volume:
/// Settings across from Power, and Back above Home across from the rocker.
struct GuestKeys: View {
    let settings: () -> Void
    let back: () -> Void
    let home: () -> Void

    var body: some View {
        VStack(spacing: KeyMetrics.rowGap) {
            SettingsKey(action: settings)

            KeyPair(
                top: ("arrowtriangle.backward.fill", String(localized: "Back"), back),
                bottom: ("circle.fill", String(localized: "Home"), home),
                glyphSize: 16
            )
        }
        .foregroundStyle(.primary)
    }
}

/// Settings: Andarium's own, opened from the keys.
struct SettingsKey: View {
    var size = CGSize(width: KeyMetrics.width, height: KeyMetrics.keyHeight)
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: "gearshape")
                .font(.system(size: 20, weight: .semibold))
                .frame(width: size.width, height: size.height)
                .contentShape(.capsule)
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .capsule)
        .accessibilityLabel("Settings")
        .accessibilityHint("Install apps, ADB, stats and options")
    }
}

/// Two keys in one glass capsule, like a volume rocker: stacked on an edge,
/// side by side on the deck.
struct KeyPair: View {
    let top: (symbol: String, label: String, action: () -> Void)
    let bottom: (symbol: String, label: String, action: () -> Void)
    var glyphSize: CGFloat = 19
    var axis: Axis = .vertical
    /// A key's height on an edge.
    var keyHeight = KeyMetrics.keyHeight

    var body: some View {
        Group {
            if axis == .vertical {
                VStack(spacing: 0) {
                    key(top)
                    Capsule()
                        .fill(.primary.opacity(0.18))
                        .frame(width: 22, height: 1.5)
                    key(bottom)
                }
            } else {
                HStack(spacing: 0) {
                    key(top)
                    Capsule()
                        .fill(.primary.opacity(0.18))
                        .frame(width: 1.5, height: 22)
                    key(bottom)
                }
            }
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .capsule)
    }

    private func key(_ key: (symbol: String, label: String, action: () -> Void)) -> some View {
        Button(action: key.action) {
            Image(systemName: key.symbol)
                .font(.system(size: glyphSize, weight: .semibold))
                .frame(
                    width: axis == .vertical ? KeyMetrics.width : KeyMetrics.deckKey.width - 0.75,
                    height: axis == .vertical ? keyHeight - 0.75 : KeyMetrics.deckKey.height)
                .contentShape(Rectangle())
        }
        .accessibilityLabel(key.label)
    }
}

// MARK: - Rail and deck

/// The side keys in the cover's trailing strip, beside the camera's: always
/// open, one column, so every key is one tap and none covers the guest. Power
/// and the volume rocker on top, as on a phone's side; Settings, then Back
/// and Home nearest the thumb.
struct KeyRail: View {
    let mode: StartMode
    let lit: Bool
    let actions: SideKeyActions
    @State private var keyPresses = 0
    @Environment(AppModel.self) private var model: AppModel?

    var body: some View {
        let recovery = mode == .recovery
        let key = CGSize(width: KeyMetrics.width, height: KeyMetrics.railKeyHeight)
        VStack(spacing: KeyMetrics.railGap) {
            PowerKey(recovery: recovery, lit: lit, size: key, power: { pressed(actions.power) }, powerHold: holdPower)
            KeyPair(
                top: (recovery ? "chevron.up" : "plus", recovery ? String(localized: "Up") : String(localized: "Volume up"), { pressed(actions.volumeUp) }),
                bottom: (recovery ? "chevron.down" : "minus", recovery ? String(localized: "Down") : String(localized: "Volume down"), { pressed(actions.volumeDown) }),
                keyHeight: key.height
            )
            if !recovery {
                SettingsKey(size: key, action: actions.settings)
                KeyPair(
                    top: ("arrowtriangle.backward.fill", String(localized: "Back"), { pressed(actions.back) }),
                    bottom: ("circle.fill", String(localized: "Home"), { pressed(actions.home) }),
                    glyphSize: 16, keyHeight: key.height
                )
            }
        }
        .foregroundStyle(.primary)
        .environment(\.colorScheme, .dark)
        .interfaceFeedback(.impact(weight: .light), trigger: keyPresses)
        // The rail is not on an edge: the way back names none.
        .onAppear { model?.session.keysEdge = nil }
    }

    private func pressed(_ action: () -> Void) {
        keyPresses += 1
        action()
    }

    private func holdPower() {
        guard mode == .normal else { return }
        actions.powerHold()
    }
}

/// The side keys on the lower half while the device stands turned, its
/// lower half on the table: one row along the near edge, under the thumbs.
/// Back and Home on the left; Settings in the middle; volume and Power on
/// the right, as a phone's side keys lie when the phone is on its side.
struct KeyDeck: View {
    let mode: StartMode
    let lit: Bool
    let actions: SideKeyActions
    @State private var keyPresses = 0

    var body: some View {
        let recovery = mode == .recovery
        HStack(spacing: 0) {
            if !recovery {
                KeyPair(
                    top: ("arrowtriangle.backward.fill", String(localized: "Back"), { pressed(actions.back) }),
                    bottom: ("circle.fill", String(localized: "Home"), { pressed(actions.home) }),
                    glyphSize: 16, axis: .horizontal
                )
                Spacer(minLength: 16)
                SettingsKey(size: KeyMetrics.deckKey, action: actions.settings)
                Spacer(minLength: 16)
            } else {
                Spacer(minLength: 0)
            }
            KeyPair(
                top: (recovery ? "chevron.up" : "minus", recovery ? String(localized: "Up") : String(localized: "Volume down"),
                      { pressed(recovery ? actions.volumeUp : actions.volumeDown) }),
                bottom: (recovery ? "chevron.down" : "plus", recovery ? String(localized: "Down") : String(localized: "Volume up"),
                         { pressed(recovery ? actions.volumeDown : actions.volumeUp) }),
                axis: .horizontal
            )
            .padding(.trailing, 14)
            PowerKey(
                recovery: recovery, lit: lit, size: KeyMetrics.deckKey,
                power: { pressed(actions.power) }, powerHold: holdPower)
        }
        .foregroundStyle(.primary)
        .environment(\.colorScheme, .dark)
        .interfaceFeedback(.impact(weight: .light), trigger: keyPresses)
    }

    private func pressed(_ action: () -> Void) {
        keyPresses += 1
        action()
    }

    private func holdPower() {
        guard mode == .normal else { return }
        actions.powerHold()
    }
}

// MARK: - Tip

/// Shown once Android has booted, pointing at the side keys. It stops once the
/// keys have been opened or the tip closed, and appears at most three times.
nonisolated struct SideKeysTip: Tip {
    /// The tip's words, which say where Install APK is when Settings has the
    /// row in this session. The tip is the same one either way (its identity
    /// and its count of displays do not change), so a person who closed it is
    /// not shown it again for the added line.
    let words: String

    /// `locale` is the language of the words: the person's own, and a
    /// language's table for a test that reads one.
    init(offersInstall: Bool, locale: Locale = .current) {
        words = Self.words(offersInstall: offersInstall, locale: locale)
    }

    var title: Text { Text("Side keys") }
    var message: Text? { Text(words.keepingTogether) }

    /// The tip's words: the keys, and a second line for Install APK.
    static func words(offersInstall: Bool, locale: Locale = .current) -> String {
        let keys = String(localized: LocalizedStringResource(
            "Tap for Back, Home and Settings. Hold Power for the power menu. Drag to move.", locale: locale))
        guard offersInstall else { return keys }
        return keys + "\n" + String(localized: LocalizedStringResource(
            "To install an app, open Settings here and choose Install APK.", locale: locale))
    }

    var image: Image? { Image(systemName: "hand.tap") }
    var options: [any TipOption] { [MaxDisplayCount(3)] }
}

/// The tip in its popover, drawn as TipKit draws a popover tip: the popover's
/// own background, at most 400 points wide. A touch beside it reaches the
/// guest, as a touch beside TipKit's does. The card takes at most
/// `maxHeight`: what does not fit scrolls inside it, so the card, the
/// icon and the close button stay on the screen on the smallest iPhone and
/// in the half of a folding iPhone.
///
/// The popover is as high as the card measures at its ideal width, so that
/// width has to be the one UIKit gives it: UIKit narrows a popover to the room
/// beside the keys, the text then wraps to more lines than were measured, and
/// the card cuts off the first or the last of them. `width` is the card's
/// ideal width, from `width(in:)`.
struct SideKeysTipCard: View {
    let tip: SideKeysTip
    let maxHeight: CGFloat
    let width: CGFloat

    /// The widest the card is.
    static let widest: CGFloat = 400

    /// The most height the card takes where the keys have `height` points: the
    /// popover keeps its own margins from the screen's edges.
    static func maxHeight(in height: CGFloat) -> CGFloat { max(160, height - 120) }

    /// The width of the card where the keys have `available` points across:
    /// the room beside them less what the popover keeps for its margin, its
    /// arrow and the keys (UIKit gives it 41 points less than the screen is
    /// wide), with some to spare so that UIKit never narrows it.
    static func width(in available: CGFloat) -> CGFloat { min(widest, max(240, available - 60)) }

    var body: some View {
        ScrollView {
            TipView(tip)
                .tipBackground(.clear)
        }
        .scrollBounceBehavior(.basedOnSize)
        .frame(maxHeight: maxHeight)
        .frame(idealWidth: width, maxWidth: Self.widest)
        .fixedSize(horizontal: false, vertical: true)
        .presentationCompactAdaptation(.popover)
        .background(PopoverPassthrough())
    }
}

/// Lets a touch beside the tip's card reach the screen under its popover.
/// UIKit's popover takes the first touch outside it to dismiss itself, so
/// the keys the tip points at never see it; with the screen's view among the
/// popover's pass-through views, the popover stays up and the touch goes
/// where it fell. SwiftUI sets that list itself, and sets it empty again at
/// its next update (`presentationBackgroundInteraction` does not reach a
/// popover), so the list is kept as it should be for as long as the popover
/// shows. While the popover is still appearing its card takes no touch, and a
/// touch on the card reaches the screen too.
private struct PopoverPassthrough: UIViewRepresentable {
    func makeUIView(context: Context) -> PassthroughView { PassthroughView() }
    func updateUIView(_ view: PassthroughView, context: Context) {}

    final class PassthroughView: UIView {
        private var watch: NSKeyValueObservation?

        override func didMoveToWindow() {
            super.didMoveToWindow()
            watch = nil
            attach()
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            if watch == nil { attach() }
        }

        /// The popover is the presentation controller of the nearest view
        /// controller up the responder chain: the card's hosting controller.
        private func attach() {
            guard window != nil else { return }
            var responder: UIResponder? = self
            while let current = responder {
                if let controller = current as? UIViewController,
                   let popover = controller.popoverPresentationController {
                    watch = popover.observe(\.passthroughViews, options: [.initial, .new]) { popover, _ in
                        // UIKit and SwiftUI set the list on the main thread.
                        MainActor.assumeIsolated {
                            guard let screen = popover.presentingViewController.view,
                                  popover.passthroughViews ?? [] != [screen] else { return }
                            popover.passthroughViews = [screen]
                        }
                    }
                    return
                }
                responder = current.next
            }
        }
    }
}
