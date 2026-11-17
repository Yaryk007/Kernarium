// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The top of Home: the Android system the guest runs, its shape as chips
/// (CPU, memory, storage, engine) and which system the next start boots.
struct HomeHero: View {
    @Environment(AppModel.self) private var model
    /// The system this launch boots, read once: the catalog reads files.
    @State private var systemName = SystemCatalog.all().first { $0.id == SystemCatalog.chosenID }?.name ?? "Built-in system"

    var body: some View {
        if let guest = model.guest {
            VStack(alignment: .leading, spacing: 16) {
                HStack(alignment: .center, spacing: 14) {
                    StackMark(lit: true, width: 54)
                        .accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(title)
                            .font(.title3.weight(.bold))
                            .lineLimit(1)
                            .minimumScaleFactor(0.8)
                        Text(subtitle)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                    Spacer(minLength: 0)
                }
                let all = chips(for: guest)
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: 8) { ForEach(all) { $0 } }
                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 8) { ForEach(all.prefix(2)) { $0 } }
                        HStack(spacing: 8) { ForEach(all.dropFirst(2)) { $0 } }
                    }
                }
            }
            .padding(18)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background {
                LinearGradient(
                    colors: [Color.accentColor.opacity(0.28), Color.accentColor.opacity(0.06)],
                    startPoint: .topLeading, endPoint: .bottomTrailing
                )
                .clipShape(.rect(cornerRadius: 30, style: .continuous))
            }
            .glassEffect(.regular, in: .rect(cornerRadius: 30, style: .continuous))
            .accessibilityElement(children: .combine)
            .id("hero")
        }
    }

    private var title: String {
        model.system.version.isEmpty ? "Android" : "Android \(model.system.version)"
    }

    private var subtitle: String {
        "\(systemName) · \(model.system.architecture.isEmpty ? "arm64" : model.system.architecture)"
    }

    private func chips(for guest: Guest) -> [HeroChip] {
        [
            HeroChip(symbol: "cpu", text: Formats.vcpus(guest.cores)),
            HeroChip(symbol: "memorychip", text: Formats.size(mib: guest.memoryMiB)),
            HeroChip(symbol: "internaldrive", text: Formats.size(gib: guest.capacityGiB)),
            engineChip,
        ]
    }

    private var engineChip: HeroChip {
        #if !APP_STORE
        if model.jitAvailable {
            return model.jitOn && model.jitStatus.usable
                ? HeroChip(symbol: "bolt.fill", text: "JIT", highlighted: true)
                : HeroChip(symbol: model.jitOn ? "bolt.badge.clock" : "bolt.slash", text: model.jitOn ? "JIT waiting" : "Gadgets")
        }
        #endif
        return HeroChip(symbol: "square.stack.3d.up", text: "Gadgets")
    }
}

/// One fact of the hero: a glyph and a short value in a capsule.
struct HeroChip: View, Identifiable {
    let symbol: String
    let text: String
    var highlighted = false
    var id: String { symbol + text }

    var body: some View {
        Label(text, systemImage: symbol)
            .font(.footnote.weight(.semibold))
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, 11)
            .padding(.vertical, 7)
            .foregroundStyle(highlighted ? Color.white : Color.primary)
            .background(highlighted ? AnyShapeStyle(Color.accentColor.gradient) : AnyShapeStyle(.thinMaterial), in: .capsule)
    }
}

/// Home's backdrop: the grouped background with a soft wash of the accent
/// at the top, so the glass sections have something to refract.
struct HomeBackdrop: View {
    var body: some View {
        ZStack {
            Color(.systemGroupedBackground)
            RadialGradient(
                colors: [Color.accentColor.opacity(0.22), .clear],
                center: .topLeading, startRadius: 0, endRadius: 520
            )
            RadialGradient(
                colors: [Color.teal.opacity(0.12), .clear],
                center: .topTrailing, startRadius: 0, endRadius: 420
            )
        }
        .ignoresSafeArea()
    }
}
