// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import UIKit

/// What the resolution row and its sheet say. The written resolution has
/// no-break spaces and is the same in every language; VoiceOver reads "×" as
/// "multiplied by", so its words are "by".
enum ResolutionWords {
    static let title = String(localized: "Resolution")
    static let change = String(localized: "Change")
    static let changeAccessibility = String(localized: "Change resolution")
    static let native = String(localized: "Native")
    static let fastest = String(localized: "Fastest")

    /// "1206 × 2334", never broken across lines.
    static func pixels(_ layout: GuestDisplayLayout) -> String {
        "\(layout.width)\u{00A0}\u{00D7}\u{00A0}\(layout.height)"
    }

    /// "1206 by 2334". The numbers go in as text, so the language's digit
    /// grouping does not turn 1206 into 1,206.
    static func spokenPixels(_ layout: GuestDisplayLayout) -> String {
        String(localized: "\(String(layout.width)) by \(String(layout.height))")
    }

    /// "480 dpi".
    static func dpi(_ layout: GuestDisplayLayout) -> String { dpi(layout.dotsPerInch) }
    static func dpi(_ dotsPerInch: Int) -> String { String(localized: "\(dotsPerInch) dpi") }

    /// The name of an end of the list: "Native" or "Fastest".
    static func name(of end: ResolutionChoices.End) -> String {
        switch end {
        case .native: native
        case .fastest: fastest
        }
    }

    /// Words joined by a no-break space and a middle dot, so a line breaks
    /// before the dot and not after it.
    static func joined(_ parts: [String]) -> String {
        parts.joined(separator: "\u{00A0}\u{00B7} ")
    }

    /// The row's line under its title: "1206 × 2334 · Native", the end's name
    /// only where the choice is an end. Not yet known: a blank that holds the
    /// line's height.
    static func summary(layout: GuestDisplayLayout?, end: ResolutionChoices.End?) -> String {
        guard let layout else { return "\u{00A0}" }
        return joined([pixels(layout)] + [end.map(name(of:))].compactMap { $0 })
    }

    /// What VoiceOver reads for the row.
    static func spokenSummary(layout: GuestDisplayLayout?, end: ResolutionChoices.End?) -> String {
        ([title] + [layout.map(spokenPixels), end.map(name(of:))].compactMap { $0 }).joined(separator: ", ")
    }

    /// A choice's line in the sheet: the density, then the end's name where
    /// it is an end ("480 dpi · Native", "160 dpi · Fastest").
    static func caption(dotsPerInch: Int, end: ResolutionChoices.End?) -> String {
        joined([dpi(dotsPerInch)] + [end.map(name(of:))].compactMap { $0 })
    }

    /// What VoiceOver reads for a choice.
    static func spokenChoice(layout: GuestDisplayLayout?, dotsPerInch: Int, end: ResolutionChoices.End?) -> String {
        ([layout.map(spokenPixels), dpi(dotsPerInch), end.map(name(of:))].compactMap { $0 }).joined(separator: ", ")
    }
}

/// The resolution, as a row of Guest system in the shape of its others: the
/// resolution the guest will get, with Change opening the list of every
/// choice. With one choice there is nothing to change, and the capsule dims.
struct ResolutionRow: View {
    static let symbol = "squareshape.split.3x3"

    @Environment(AppModel.self) private var model

    var body: some View {
        let choices = model.resolutionChoices
        let end = choices.end(at: model.resolutionIndex)
        let layout = model.resolutionLayout
        SystemRow(
            symbol: Self.symbol, title: ResolutionWords.title,
            subtitle: ResolutionWords.summary(layout: layout, end: end),
            spoken: ResolutionWords.spokenSummary(layout: layout, end: end),
            button: SystemRowButton(
                title: ResolutionWords.change, identifier: "home-resolution-change",
                accessibilityName: ResolutionWords.changeAccessibility, isEnabled: choices.hasChoice,
                action: { model.sheet = .resolution }
            )
        )
        .id("resolution")
    }
}

/// Change on the resolution row: every choice, the sharpest first, with its
/// resolution and its density. Save keeps the one ticked; it takes effect
/// when the guest next starts.
struct ResolutionSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var draft: Int?

    var body: some View {
        let choices = model.resolutionChoices
        let current = model.resolutionIndex
        let chosen = draft ?? current
        let order = Array((0..<choices.count).reversed())
        NavigationStack {
            ScrollView {
                VStack(spacing: 0) {
                    ForEach(order, id: \.self) { index in
                        if index != order.first { Divider().padding(.leading, 54) }
                        Button { draft = index } label: { row(index, selected: index == chosen) }
                            .buttonStyle(.plain)
                            .accessibilityLabel(
                                ResolutionWords.spokenChoice(
                                    layout: model.resolutionLayout(at: index),
                                    dotsPerInch: Self.dotsPerInch(choices, index), end: choices.end(at: index)))
                            .accessibilityAddTraits(index == chosen ? .isSelected : [])
                            .accessibilityIdentifier("resolution-choice-\(index)")
                    }
                }
                .glassEffect(.regular, in: .rect(cornerRadius: 26))
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 24)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle(ResolutionWords.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", role: .cancel) { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        if let draft { model.chooseResolution(choices.scale(at: draft)) }
                        dismiss()
                    }
                    .fontWeight(.semibold)
                    .disabled(draft == nil || draft == current)
                    .accessibilityIdentifier("resolution-save")
                }
            }
        }
        .presentationDetents([.height(height(of: choices))])
        .presentationSizing(FixedSheetSizing(width: 540, height: height(of: choices) + 10))
    }

    /// A row is 61 points, and the bar and the margins 72.
    private func height(of choices: ResolutionChoices) -> CGFloat { CGFloat(choices.count) * 61 + 72 }

    private static func dotsPerInch(_ choices: ResolutionChoices, _ index: Int) -> Int {
        let scale = choices.scale(at: index)
        return GuestDisplayLayout.dotsPerInch(factor: scale.resolved(nativeScale: choices.nativeScale) ?? 1)
    }

    private func row(_ index: Int, selected: Bool) -> some View {
        let choices = model.resolutionChoices
        let layout = model.resolutionLayout(at: index)
        return HStack(spacing: 12) {
            Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                .font(.title3)
                .foregroundStyle(selected ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.tertiary))
                .frame(width: 26)
            VStack(alignment: .leading, spacing: 1) {
                Text(layout.map(ResolutionWords.pixels) ?? "\u{2014}")
                    .monospacedDigit()
                Text(ResolutionWords.caption(dotsPerInch: Self.dotsPerInch(choices, index), end: choices.end(at: index)))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .frame(minHeight: 60)
        .contentShape(.rect)
    }
}

/// Reads the display the next start gives the guest, so Home can state its
/// resolution. It is the size the guest's display has in `GuestLayer`, in the
/// same place with the same corner offset for an iPad window's controls
/// (`cornerEdges`, which a scene that covers its screen has none of), and the
/// window's safe area
/// gives the part the status bar takes, which the session hides
/// (`SessionDisplayArea`). The screen's native scale is read from the window's
/// screen. It follows the window's size as it changes. On a display that folds
/// the size is the session plan's guest (`SessionPlan`).
struct DisplayAreaProbe: View {
    @Environment(AppModel.self) private var model
    let cornerEdges: Edge.Set
    @State private var size = CGSize.zero
    @State private var window: WindowReading?

    var body: some View {
        Color.clear
            .containerCornerOffset(cornerEdges, sizeToFit: true)
            .onGeometryChange(for: CGSize.self, of: \.size) {
                size = $0
                publish()
            }
            .background {
                WindowReader {
                    window = $0
                    model.screenDidChange(
                        nativeScale: $0.nativeScale, maximumFramesPerSecond: $0.maximumFramesPerSecond)
                    publish()
                }
            }
            .onChange(of: model.sessionPlan?.guest.size) { publish() }
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }

    private func publish() {
        // Until the window has said what its safe area is, the size is not the
        // session's.
        guard size.width > 0, size.height > 0, let window else { return }
        // A folding display has a layout of its own (`SessionPlan`), which
        // knows the session's safe area whichever screen this is, and where
        // the device stands the upper half.
        if let plan = model.sessionPlan {
            model.displayPointsDidChange(plan.guest.size)
            return
        }
        model.displayPointsDidChange(
            SessionDisplayArea.size(
                home: size, topInset: window.topInset, bottomInset: window.bottomInset, isPhone: window.isPhone))
    }
}

/// What the window says of itself that the display's size depends on.
struct WindowReading: Equatable {
    var nativeScale = 1.0
    /// The most frames a second the window's screen shows.
    var maximumFramesPerSecond = 60
    var topInset: CGFloat = 0
    var bottomInset: CGFloat = 0
    var isPhone = false
}

private struct WindowReader: UIViewRepresentable {
    let report: @MainActor (WindowReading) -> Void

    func makeUIView(context: Context) -> WindowReadingView {
        let view = WindowReadingView()
        view.isUserInteractionEnabled = false
        view.report = report
        return view
    }

    func updateUIView(_ view: WindowReadingView, context: Context) {
        view.report = report
    }
}

private final class WindowReadingView: UIView {
    var report: (@MainActor (WindowReading) -> Void)?
    private var reported: WindowReading?

    override func didMoveToWindow() {
        super.didMoveToWindow()
        update()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        update()
    }

    override func safeAreaInsetsDidChange() {
        super.safeAreaInsetsDidChange()
        update()
    }

    private func update() {
        guard let window, let scene = window.windowScene else { return }
        let reading = WindowReading(
            nativeScale: Double(scene.screen.nativeScale),
            maximumFramesPerSecond: scene.screen.maximumFramesPerSecond,
            topInset: window.safeAreaInsets.top, bottomInset: window.safeAreaInsets.bottom,
            isPhone: UIDevice.current.userInterfaceIdiom == .phone)
        guard reading != reported else { return }
        reported = reading
        // SwiftUI state is not written from inside its own layout pass.
        DispatchQueue.main.async { [weak self] in
            self?.report?(reading)
        }
    }
}
