// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

// The screens other than the session on a display that folds: the decisions
// are `AndariumCore`'s (`FoldHalves`, `FoldSheet`), and each modifier here
// does nothing where the display has no fold band.

extension View {
    /// Keeps the view to the place of `screen` on the inner display while the
    /// device is bent (see `FoldHalves.area(for:)`): the half where the system
    /// puts its own alerts, the leading one upright and the lower one turned,
    /// so a card or a boot step never straddles the fold. Flat, on the cover
    /// and on a display with no hinge there is no padding and nothing is
    /// measured, so it lays out as it always did. It measures the space it is
    /// given and not the window's safe area, so it holds in a sheet and under
    /// a navigation bar. Apply it to a view that fills the space it is shown
    /// in.
    func onCardHalf(_ screen: FoldScreen = .card) -> some View { modifier(OnCardHalf(screen: screen)) }

    /// A sheet's presentation on the turned inner display while the device
    /// stands bent. Upright the system keeps a sheet to the leading half while
    /// the device is bent. Turned and bent, the sheet rises to the fold and no
    /// further: it covers the lower half only and the upper half stays in
    /// sight. Flat, upright or turned, a sheet is the system's, centred as on
    /// an iPad. `reachesBehind` lets touches reach what the sheet leaves
    /// uncovered, as the session's Settings does over the guest. Apply it
    /// outside the sheet's own presentation modifiers. It is handed the model
    /// and does not read the environment: a sheet's presentation preferences
    /// are read before the sheet inherits its presenter's environment, on a
    /// Mac and in a harness that gives the presenter no model. A sheet decides
    /// once, when it is presented, whether it takes the turned presentation,
    /// and keeps that placement until it closes: moving the hinge or turning
    /// the display with it open leaves its content, and what the person has
    /// typed in it, in place.
    func foldSheet(_ model: AppModel, reachesBehind: Bool = false) -> some View {
        modifier(FoldSheetPresentation(model: model, reachesBehind: reachesBehind))
    }
}

private struct OnCardHalf: ViewModifier {
    let screen: FoldScreen
    /// Optional: a card shown without a model, as a harness does, is laid out
    /// as it always was.
    @Environment(AppModel.self) private var model: AppModel?
    /// The space the view is given, in window coordinates, once measured.
    @State private var measured: CGRect?

    func body(content: Content) -> some View {
        let fold = model?.fold
        // Only while the device is bent: flat is the no-hinge path.
        let halves = fold.flatMap(FoldHalves.init)
        let bent = halves != nil
        // The reader takes the space it is given whatever its content does,
        // so the space is measured on it: a view taller than its place
        // overflows, and never grows the space it is measured in (which its
        // own padding would otherwise keep changing). The view fills the space,
        // then keeps to the place of its screen: a card in a stack centres on
        // its place and not on that space. Until the space is measured the
        // window's safe area stands for it. A view taller than its place
        // overflows from the top of a half, so that what a person reads first
        // stays on it; everywhere else (flat, the cover, a display with no
        // hinge) it overflows both ends equally, as it always did.
        GeometryReader { geo in
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding(Self.padding(halves?.insets(for: screen, in: (measured ?? fold?.safeArea) ?? .zero)))
                .frame(width: geo.size.width, height: geo.size.height, alignment: bent ? .topLeading : .center)
        }
        .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { frame in
            if bent, measured != frame { measured = frame }
        }
    }

    private static func padding(_ insets: FoldInsets?) -> EdgeInsets {
        guard let insets else { return EdgeInsets() }
        return EdgeInsets(top: insets.top, leading: insets.leading, bottom: insets.bottom, trailing: insets.trailing)
    }
}

extension View {
    /// While the device is bent, puts the view in a scroll view that scrolls
    /// only when it is taller than the space it is given, which a half can be;
    /// the view keeps its place otherwise. Flat, on the cover and on a display
    /// with no hinge it keeps the view it had.
    func scrollsWhenTall() -> some View { modifier(ScrollsWhenTall()) }
}

private struct ScrollsWhenTall: ViewModifier {
    @Environment(AppModel.self) private var model: AppModel?

    func body(content: Content) -> some View {
        if model?.fold?.pose.spreads == true {
            GeometryReader { geo in
                ScrollView {
                    content.frame(maxWidth: .infinity, minHeight: geo.size.height)
                }
                .scrollBounceBehavior(.basedOnSize)
            }
        } else {
            content
        }
    }
}

private struct FoldSheetPresentation: ViewModifier {
    let model: AppModel
    let reachesBehind: Bool
    /// Decided when the sheet is presented and never again: a branch on the
    /// live reading would move the content to another place in the view tree
    /// when the display turns, and SwiftUI would build it anew.
    @State private var turned: Bool
    /// The detent's height, kept up to date while the display is turned.
    @State private var height: CGFloat

    init(model: AppModel, reachesBehind: Bool) {
        self.model = model
        self.reachesBehind = reachesBehind
        let height = model.fold.flatMap(FoldSheet.detentHeight)
        _turned = State(initialValue: height != nil)
        _height = State(initialValue: height ?? 0)
    }

    func body(content: Content) -> some View {
        if turned {
            content
                .presentationSizing(.page)
                .presentationDetents([.height(height)])
                .presentationBackgroundInteraction(reachesBehind ? .enabled : .disabled)
                .onChange(of: model.fold.flatMap(FoldSheet.detentHeight)) { _, next in
                    if let next, next != height { height = next }
                }
        } else {
            content
        }
    }
}
