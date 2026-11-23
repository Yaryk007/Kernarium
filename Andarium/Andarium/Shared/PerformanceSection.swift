// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Settings' Performance section, after Stats: "When hot", whether Android is
/// told how hot the device is so that it limits the frame rate while the
/// device is hot (`AppModel.heatLimitOn`). The row shows the choice and opens a
/// menu of the two; the footnote says what the choice does, and, while the
/// device is held hot, what is happening now. Every row of the menu applies at
/// once and asks nothing: the warning is in the menu and the footnote.
struct PerformanceSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Section {
            Menu {
                choice(limit: true, title: "Limit frame rate", subtitle: "Lets this device cool down")
                choice(limit: false, title: "Full frame rate", subtitle: "Hotter, and the battery drains faster")
            } label: {
                HStack(spacing: 12) {
                    IconTile(symbol: "thermometer.medium", color: model.deviceIsHot ? .orange : .accentColor)
                    Text("When hot")
                        .foregroundStyle(.primary)
                        .layoutPriority(1)
                    Spacer(minLength: 8)
                    value
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .minimumScaleFactor(0.8)
                    Image(systemName: "chevron.up.chevron.down")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.secondary)
                }
                .contentShape(.rect)
            }
            .tint(Color.primary)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text("When hot"))
            .accessibilityValue(value)
            .accessibilityIdentifier("heat-limit-menu")
        } header: {
            Text("Performance")
        } footer: {
            footnote
        }
        .id("performance")
    }

    private var value: Text {
        model.heatLimitOn ? Text("Limit frame rate") : Text("Full frame rate")
    }

    /// A choice of the menu: its name, what it means under it, and a
    /// checkmark on the one in effect.
    private func choice(limit: Bool, title: LocalizedStringKey, subtitle: LocalizedStringKey) -> some View {
        Toggle(isOn: Binding(
            get: { model.heatLimitOn == limit },
            set: { if $0 { model.setHeatLimit(limit) } })
        ) {
            Text(title)
            Text(subtitle)
        }
    }

    /// What the choice does, and the warning while it is off; while the device
    /// is held hot, what is happening to the frame rate now.
    @ViewBuilder
    private var footnote: some View {
        if model.deviceIsHot {
            VStack(alignment: .leading, spacing: 6) {
                Label {
                    if model.heatLimitOn {
                        Text("This device is hot. The frame rate is limited until it cools.")
                    } else {
                        Text("This device is hot. The frame rate is not limited.")
                    }
                } icon: {
                    Image(systemName: "thermometer.high").foregroundStyle(.orange)
                }
                if !model.heatLimitOn { warning }
            }
        } else if model.heatLimitOn {
            Text("While this device is hot, games run at a lower frame rate so it can cool down.")
        } else {
            warning
        }
    }

    private var warning: some View {
        Text("This device gets hotter and the battery drains faster. iOS may dim the screen, slow everything down or close Andarium.")
    }
}
