// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// A titled section of Home: rows in one glass panel under a small heading,
/// and a note beneath.
struct HomeSection<Content: View, Footer: View>: View {
    private let title: Text
    private let content: Content
    private let footer: Footer

    init(title: LocalizedStringKey, @ViewBuilder content: () -> Content, @ViewBuilder footer: () -> Footer) {
        self.title = Text(title)
        self.content = content()
        self.footer = footer()
    }

    /// A title that is not translated: a section only the development builds have.
    init(verbatim title: String, @ViewBuilder content: () -> Content, @ViewBuilder footer: () -> Footer) {
        self.title = Text(verbatim: title)
        self.content = content()
        self.footer = footer()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            title.homeSectionHeading()
            VStack(spacing: 0) { content }
                .glassEffect(.regular, in: .rect(cornerRadius: 26))
            footer
        }
    }
}

extension View {
    /// The small heading above a section's panel.
    func homeSectionHeading() -> some View {
        font(.subheadline.weight(.semibold))
            .foregroundStyle(.secondary)
            .padding(.leading, 18)
            .accessibilityAddTraits(.isHeader)
    }
}

/// A note beneath a group whose switch says it for VoiceOver: drawn, and with
/// VoiceOver running not read a second time as a loose element.
struct SpokenNote: View {
    @Environment(\.accessibilityVoiceOverEnabled) private var voiceOverRunning
    let text: String

    init(_ text: String) { self.text = text }

    var body: some View {
        SectionNote(text).accessibilityHidden(voiceOverRunning)
    }
}

/// One panel of a section that has several, with its note beneath and no
/// heading of its own: the section's heading is above the first group.
struct HomeGroup<Content: View, Footer: View>: View {
    private let content: Content
    private let footer: Footer

    init(@ViewBuilder content: () -> Content, @ViewBuilder footer: () -> Footer) {
        self.content = content()
        self.footer = footer()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            VStack(spacing: 0) { content }
                .glassEffect(.regular, in: .rect(cornerRadius: 26))
            footer
        }
    }
}

extension HomeSection where Footer == EmptyView {
    init(title: LocalizedStringKey, @ViewBuilder content: () -> Content) {
        self.init(title: title, content: content, footer: { EmptyView() })
    }
}

/// A note beneath a section.
struct SectionNote: View {
    let text: String

    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.footnote)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 18)
    }
}

/// One row of a section: the icon tile, the title (and a line under it) and
/// the control on the right. VoiceOver reads the row as its control, which
/// carries the title as its label, followed by the line under it.
struct OptionRow<Control: View>: View {
    let title: String
    let symbol: String
    var detail: String?
    /// What the line under the title does when it is touched; the line then
    /// reads as a button. None leaves it plain text.
    var detailAction: (() -> Void)?
    /// The capability is off and unreachable: the tile and the words dim.
    var dimmed = false
    @ViewBuilder var control: Control

    var body: some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol, color: dimmed ? .gray : .accentColor)
                .opacity(dimmed ? 0.55 : 1)
            VStack(alignment: .leading, spacing: 1) {
                // A long title (a translation) wraps rather than being cut.
                Text(title)
                    .fixedSize(horizontal: false, vertical: true)
                    .opacity(dimmed ? 0.55 : 1)
                    // The control's own label says it.
                    .accessibilityHidden(true)
                if let detail {
                    if let detailAction {
                        Button(action: detailAction) {
                            HStack(spacing: 4) {
                                Text(detail)
                                Image(systemName: "info.circle")
                            }
                            .font(.footnote)
                            .foregroundStyle(Color.accentColor)
                            .contentShape(.rect)
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("option-detail-button")
                    } else {
                        Text(detail)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            Spacer(minLength: 8)
            control
        }
        .padding(.horizontal, 16)
        .padding(.vertical, detail == nil ? 0 : 6)
        .frame(minHeight: 54)
        .accessibilityElement(children: .combine)
    }
}

/// The rule between two rows of a section.
struct RowDivider: View {
    var body: some View { Divider().padding(.leading, 58) }
}

// MARK: System

/// The guest's system in one section, as four rows of one shape: CPU and
/// memory states the vCPUs and RAM, with Edit opening the sheet that changes
/// them; Resolution states the guest's display, with Change opening its
/// choices (`ResolutionRow`); Storage states what the guest holds of its
/// capacity under a bar, with Resize; Snapshots states how many there are and
/// the space they use, with Manage opening them.
struct SystemSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let guest = model.guest {
            HomeSection(title: "Virtual machine") {
                SystemRow(
                    symbol: "cpu", title: String(localized: "CPU and memory"), subtitle: Self.summary(of: guest),
                    button: SystemRowButton(
                        title: SystemRowButton.edit, identifier: "home-system-edit",
                        accessibilityName: String(localized: "Edit system"),
                        action: { model.sheet = .system }
                    )
                )
                RowDivider()
                ResolutionRow()
                RowDivider()
                StorageRow(guest: guest, stops: model.capacityStopsGiB, resize: { model.sheet = .resize })
                RowDivider()
                SnapshotsRow()
            }
        }
    }

    /// "6 vCPUs · 2.5 GiB".
    static func summary(of guest: Guest) -> String {
        "\(CountWords.vcpus(guest.cores)) · \(Formats.size(mib: guest.memoryMiB))"
    }
}

/// The button at the end of a Virtual machine row.
struct SystemRowButton {
    let title: String
    let identifier: String
    /// What VoiceOver says when the title alone is not enough.
    var accessibilityName: String?
    var isEnabled = true
    let action: () -> Void

    static let edit = String(localized: "Edit")
    static let resize = String(localized: "Resize")
    static let manage = SnapshotWords.manage

    /// Every title a button of the section has: each button is as wide as the
    /// widest of them, at any text size, so the buttons of the rows share one
    /// width and one edge in every language.
    static let allTitles = [edit, resize, manage]
}

extension VerticalAlignment {
    private struct HeaderCenterID: AlignmentID {
        static func defaultValue(in context: ViewDimensions) -> CGFloat { context[VerticalAlignment.center] }
    }

    /// The middle of a row's title and the line under it. The icon tile and
    /// the button centre on it, so what hangs below the text (a bar and a
    /// note) never moves them off the words they belong to.
    fileprivate static let headerCenter = VerticalAlignment(HeaderCenterID.self)
}

/// One row of the Virtual machine section: the icon tile, the title with one
/// line under it, anything further under that line in the same column (a bar,
/// a note), and the row's button at the end. The text column reads as one
/// accessibility element; the button is the other.
struct SystemRow<Below: View>: View {
    let symbol: String
    let title: String
    let subtitle: String
    let button: SystemRowButton
    /// What VoiceOver reads for the title and the line under it, where
    /// the written line does not read well.
    var spoken: String?
    @ViewBuilder var below: Below

    var body: some View {
        HStack(alignment: .headerCenter, spacing: 12) {
            tile
            VStack(alignment: .leading, spacing: 8) {
                heading.alignmentGuide(.headerCenter) { $0[VerticalAlignment.center] }
                below
            }
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityElement(children: .combine)
            capsule
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    private var tile: some View {
        IconTile(symbol: symbol).accessibilityHidden(true)
    }

    private var heading: some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title)
            Text(subtitle)
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .modifier(SpokenLine(text: spoken))
    }

    private var capsule: some View {
        Button(action: button.action) {
            ZStack {
                ForEach(SystemRowButton.allTitles, id: \.self) { Text($0).hidden() }
                Text(button.title)
            }
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(button.isEnabled ? Color.accentColor : Color(.tertiaryLabel))
            .padding(.horizontal, 16)
            .frame(minHeight: 34)
            .background(
                button.isEnabled ? Color.accentColor.opacity(0.13) : Color(.quaternarySystemFill),
                in: .capsule
            )
            .contentShape(.capsule)
            .fixedSize()
        }
        .buttonStyle(.plain)
        .disabled(!button.isEnabled)
        .accessibilityIdentifier(button.identifier)
        .accessibilityLabel(button.accessibilityName ?? button.title)
    }
}

extension SystemRow where Below == EmptyView {
    init(symbol: String, title: String, subtitle: String, spoken: String? = nil, button: SystemRowButton) {
        self.init(
            symbol: symbol, title: title, subtitle: subtitle, button: button, spoken: spoken,
            below: { EmptyView() })
    }
}

/// A row's title and line as one element that VoiceOver reads as `text`;
/// without `text` the words are read as they are written.
private struct SpokenLine: ViewModifier {
    let text: String?

    func body(content: Content) -> some View {
        if let text {
            content.accessibilityElement(children: .ignore).accessibilityLabel(text)
        } else {
            content
        }
    }
}

/// The third row of Virtual machine: how many snapshots there are and the space
/// all of them use, with Manage opening the Snapshots sheet. Home reads them
/// when none was read, after an operation and when a run ends, not each time
/// it appears (`SnapshotsModel.reload`).
struct SnapshotsRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let store = model.snapshots
        SystemRow(
            symbol: "clock.arrow.circlepath", title: SnapshotWords.title,
            // Not yet read: the line holds its height and says nothing.
            subtitle: store.listing == nil
                ? "\u{00A0}" : SnapshotWords.homeSubtitle(count: store.count, total: store.total),
            button: SystemRowButton(
                title: SnapshotWords.manage, identifier: "home-snapshots-manage",
                accessibilityName: SnapshotWords.manageAccessibility,
                action: { model.sheet = .snapshots }
            )
        )
        .task { await store.reload() }
    }
}

/// Whether the next start of Android asks for the 120 Hz display
/// (`AppModel.highRefreshOn`, kept for the install, off at first). It is read
/// when Android starts and never during a run, so it is on Home, which shows
/// only while no guest runs; the row is drawn only where the screen can show
/// it (`AppModel.highRefreshOffered`), in Performance (`HomePerformanceSection`).
struct HighRefreshRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        OptionRow(title: String(localized: "120 Hz"), symbol: "speedometer") {
            Toggle("120 Hz", isOn: Binding(
                get: { model.highRefreshOn },
                set: { model.setHighRefresh($0) }
            ))
            .labelsHidden()
            .accessibilityIdentifier("home-high-refresh-switch")
        }
        // VoiceOver says the note, which is beneath the group, and then the
        // name, as it did when the note was in the cell.
        .accessibilityLabel(HighRefreshWords.spoken)
        // Voice Control matches the name alone, as it is on the screen.
        .accessibilityInputLabels([String(localized: "120 Hz")])
        .id("high-refresh")
    }
}

/// The note beneath the 120 Hz group.
enum HighRefreshWords {
    static let note = String(localized: "Smoother motion, more battery use.")
    /// What VoiceOver says for the row: the note and the name.
    static let spoken: String = "\(note), \(String(localized: "120 Hz"))"
}

/// The note beneath the 120 Hz group.
struct HighRefreshNote: View {
    var body: some View { SpokenNote(HighRefreshWords.note) }
}

// MARK: Capabilities

/// What the guest may use of this device: the location, the microphone and the
/// camera, which iOS asks about when they are turned on, and the haptics on a
/// device that has them. The motion sensors are always the guest's where the device
/// has them, so there is no row for them.
struct CapabilitiesSection: View {
    @Environment(AppModel.self) private var model

    /// The rows, top to bottom; VoiceOver reads them in this order.
    enum Row: CaseIterable {
        case location, microphone, camera, haptics
    }

    /// The rows a device shows: Haptics only where there is a Taptic Engine
    /// to switch. An iPad, a Mac and the Simulator have none, and a switch
    /// that could do nothing is not shown.
    static func rows(hapticsAvailable: Bool) -> [Row] {
        Row.allCases.filter { $0 != .haptics || hapticsAvailable }
    }

    var body: some View {
        if model.guest != nil {
            HomeSection(title: "Capabilities") {
                ForEach(Array(Self.rows(hapticsAvailable: model.hapticsAvailable).enumerated()), id: \.element) { index, row in
                    if index > 0 { RowDivider() }
                    switch row {
                    case .location: LocationRow()
                    case .microphone: MicrophoneRow()
                    case .camera: CameraRow()
                    case .haptics: HapticsRow()
                    }
                }
            } footer: {
                ForEach(CameraWords.notes(access: model.cameraAccess), id: \.self) { SectionNote($0) }
            }
            .id("capabilities")
        }
    }
}

/// The words under Microphone while iOS refuses access.
enum CapabilityWords {
    static let microphoneDenied = String(localized: "Not allowed in Settings")
}

struct MicrophoneRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let denied = model.microphoneState == .denied
        OptionRow(
            title: String(localized: "Microphone"), symbol: "mic.fill",
            detail: denied ? CapabilityWords.microphoneDenied : nil, dimmed: denied
        ) {
            Toggle("Microphone", isOn: Binding(
                get: { model.microphoneShownOn },
                set: { model.setMicrophone($0) }
            ))
            .labelsHidden()
        }
        .accessibilityHint(denied ? String(localized: "Andarium is not allowed to use the microphone. Turning this on offers to open Settings.") : "")
    }
}

/// What the guest's camera apps see: the device's cameras, from the next
/// start. The switch is read when the guest starts and never during a run, so
/// it is on Home, which shows only while no guest runs.
struct CameraRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let access = model.cameraAccess
        OptionRow(
            title: CameraWords.title, symbol: "camera.fill",
            detail: CameraWords.detail(for: access), dimmed: access.refuses
        ) {
            Toggle(CameraWords.title, isOn: Binding(
                get: { model.cameraShownOn },
                set: { model.setCamera($0) }
            ))
            .labelsHidden()
            .disabled(!access.offered)
            .accessibilityIdentifier("home-camera-switch")
        }
        .accessibilityHint(CameraWords.hint(for: access))
    }
}

/// What the guest plays on this device's Taptic Engine: Android's vibrator
/// and haptic feedback. A game controller's rumble is the controller's and
/// stays as it is.
struct HapticsRow: View {
    @Environment(AppModel.self) private var model

    /// The vibrating phone, which reads as haptics where "waveform" reads as
    /// sound.
    static let symbol = "iphone.radiowaves.left.and.right"

    var body: some View {
        OptionRow(title: String(localized: "Haptics"), symbol: Self.symbol) {
            Toggle("Haptics", isOn: Binding(
                get: { model.hapticsOn },
                set: { model.setHaptics($0) }
            ))
            .labelsHidden()
            .accessibilityIdentifier("home-haptics-switch")
        }
    }
}

struct LocationRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let access = model.locationAuthorization
        let shownOn = model.locationShownOn
        let approximate = access.isApproximate && shownOn
        OptionRow(
            title: LocationWords.title, symbol: "location.fill",
            detail: LocationWords.detail(for: access, shownOn: shownOn),
            detailAction: approximate ? { model.permissionPrompt = .location } : nil,
            dimmed: access.refuses
        ) {
            Toggle(LocationWords.title, isOn: Binding(
                get: { model.locationShownOn },
                set: { model.setLocation($0) }
            ))
            .labelsHidden()
            .accessibilityIdentifier("home-location-switch")
        }
        .accessibilityHint(LocationWords.hint(for: access, shownOn: shownOn))
    }
}

/// When iOS has refused the microphone there is nothing to ask again: say so,
/// and offer Andarium's page in Settings.
struct MicrophoneAccessAlert: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        @Bindable var model = model
        content.alert(
            "Allow microphone access", isPresented: Binding(
                get: { model.permissionPrompt == .microphone },
                set: { if !$0 { model.permissionPrompt = nil } }
            )
        ) {
            Button("Open Settings") { model.openMicrophoneSettings() }
            Button("Not now", role: .cancel) {}
        } message: {
            Text(String(localized: "iOS is not allowing Andarium to use the microphone. Turn on Microphone for Andarium in Settings, then come back to use it.").keepingTogether)
        }
    }
}

extension View {
    func microphoneAccessAlert() -> some View { modifier(MicrophoneAccessAlert()) }
}

/// When iOS refuses the location, or gives only an approximate one, there is
/// nothing to ask: say why, and offer Andarium's page in Settings where it has
/// the way out.
struct LocationAccessAlert: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        @Bindable var model = model
        let words = LocationWords.alert(for: model.locationAuthorization)
        content.alert(
            words?.title ?? "", isPresented: Binding(
                get: { model.permissionPrompt == .location && words != nil },
                set: { if !$0, model.permissionPrompt == .location { model.permissionPrompt = nil } }
            )
        ) {
            if words?.offersSettings == true {
                Button("Open Settings") { model.openLocationSettings() }
                Button("Not now", role: .cancel) {}
            } else {
                Button("OK", role: .cancel) {}
            }
        } message: {
            Text((words?.message ?? "").keepingTogether)
        }
    }
}

extension View {
    func locationAccessAlert() -> some View { modifier(LocationAccessAlert()) }
}

/// When iOS refuses the camera, or the device's settings restrict it, there is
/// nothing to ask: say why, and offer Andarium's page in Settings where it has
/// the way out.
struct CameraAccessAlert: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        @Bindable var model = model
        let words = CameraWords.alert(for: model.cameraAccess)
        content.alert(
            words?.title ?? "", isPresented: Binding(
                get: { model.permissionPrompt == .camera && words != nil },
                set: { if !$0, model.permissionPrompt == .camera { model.permissionPrompt = nil } }
            )
        ) {
            if words?.offersSettings == true {
                Button("Open Settings") { model.openCameraSettings() }
                Button("Not now", role: .cancel) {}
            } else {
                Button("OK", role: .cancel) {}
            }
        } message: {
            Text((words?.message ?? "").keepingTogether)
        }
    }
}

extension View {
    func cameraAccessAlert() -> some View { modifier(CameraAccessAlert()) }
}

// MARK: Performance

/// Performance: what the guest runs with, in one section under one heading,
/// as groups top to bottom, each a panel of one switch with its note beneath:
/// "120 Hz" where the screen can show it (`AppModel.highRefreshOffered`), then
/// AOT (`TranslatedLibrariesRow`) in a build that carries the translated-code
/// framework (`AppModel.translatedLibrariesAvailable`), then JIT where a build
/// can have it (`AppModel.jitAvailable`). The heading is drawn once, above the
/// first group that is drawn, and a group that is not offered leaves out its
/// note with it; none offered draws nothing. The groups are closer together
/// than two sections are. The note of JIT is its status
/// (`AppModel.jitNote`): the ready sentence is there only while the row is on,
/// so a group with the row off and nothing else to say ends at its panel.
struct HomePerformanceSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if model.guest != nil,
            model.highRefreshOffered || model.translatedLibrariesAvailable || model.jitAvailable
        {
            VStack(alignment: .leading, spacing: 8) {
                Text("Performance").homeSectionHeading()
                VStack(alignment: .leading, spacing: Self.groupSpacing) {
                    if model.highRefreshOffered {
                        HomeGroup { HighRefreshRow() } footer: { HighRefreshNote() }
                    }
                    if model.translatedLibrariesAvailable {
                        HomeGroup { TranslatedLibrariesRow() } footer: { TranslatedLibrariesNote() }
                    }
                    if model.jitAvailable {
                        HomeGroup { JitRow() } footer: {
                            if let note = model.jitNote { SectionNote(note) }
                        }
                    }
                }
            }
            .id("performance")
        }
    }

    /// The space between two groups of the section: half what Home leaves
    /// between two sections, so that the groups read as one section.
    static let groupSpacing: CGFloat = 14
}

/// Whether the guest runs on the JIT engine, which a debugger the person
/// attached (StikDebug) makes possible, and what the next start can do about
/// it. It applies when the guest next starts.
struct JitRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        OptionRow(title: "JIT", symbol: "bolt.fill") {
            Toggle(isOn: Binding(
                get: { model.jitOn },
                set: { model.setJit($0) }
            )) { Text(verbatim: "JIT") }
            .labelsHidden()
        }
        .task { model.refreshJit() }
    }
}

// MARK: Backup

/// Whether iCloud and computer backups of this device take the guest's data.
struct BackupOption: View {
    @Binding var isOn: Bool

    var body: some View {
        OptionRow(title: String(localized: "Include in backups"), symbol: "icloud.fill") {
            Toggle("Include in backups", isOn: $isOn).labelsHidden()
        }
    }
}

/// What the Include in backups row does to iCloud and computer backups.
/// Snapshots are never in a backup, which the note says once there are some.
struct BackupNote: View {
    let host: HostDevice
    var hasSnapshots = false

    var body: some View {
        SectionNote(SnapshotWords.backupNote(device: host.name, hasSnapshots: hasSnapshots))
    }
}

struct BackupSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let guest = model.guest {
            HomeSection(title: "Backup") {
                BackupOption(isOn: Binding(
                    get: { guest.includeInBackups },
                    set: { model.guest?.includeInBackups = $0 }
                ))
            } footer: {
                BackupNote(host: model.host, hasSnapshots: model.snapshots.count > 0)
            }
            .id("backup")
        }
    }
}
