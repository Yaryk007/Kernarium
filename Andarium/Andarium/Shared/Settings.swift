// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// Settings while the guest runs, opened from the side keys. It slides up over
/// the guest, which keeps running behind it: install an app from Files and
/// browse the guest's files (the first sections, where the side keys' tip
/// points), watch the stats, connect a computer with adb, make a bug report
/// and, last, turn Android's screen reader (TalkBack) on and off. An operation on the guest dismisses
/// Settings and shows the full-screen progress, and then how it ended.
struct SessionSettingsSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false
    @State private var copied = false
    @State private var confirmingStop = false
    @State private var filesBarInset: CGFloat = 0
    @State private var filesBarHeight: CGFloat = 0
    @State private var detent: PresentationDetent
    /// The Old app compatibility page is pushed.
    @State private var oldAppsOpen = false
    /// The Add to Home Screen page is pushed.
    @State private var addToHomeOpen = false

    init(large: Bool) {
        _detent = State(initialValue: large ? .large : .medium)
    }

    var body: some View {
        let session = model.session
        NavigationStack(path: fileRoute) {
            ScrollViewReader { proxy in
                List {
                    if offersInstall { apps }
                    if normalSession, model.capabilities.guestFiles, model.fileBrowser != nil { files }
                    if model.capabilities.statistics { stats(session) }
                    // The thermal session runs in a normal run; Recovery has no HAL.
                    if normalSession { PerformanceSection() }
                    if model.host.supportsPointer { pointer }
                    if let probe = model.inputProbe { inputProbe(probe) }
                    if normalSession, model.capabilities.adbOverNetwork || model.capabilities.textMessages {
                        developer(session)
                    }
                    if normalSession, model.capabilities.bugReport { diagnostics }
                    if normalSession, model.capabilities.screenReader { accessibility(session) }
                }
                .task {
                    guard let target = model.sessionPreset.settingsScroll else { return }
                    try? await Task.sleep(for: .milliseconds(500))
                    proxy.scrollTo(target, anchor: .top)
                }
                .task {
                    // A launch argument of the preview opens the page directly.
                    guard model.sessionPreset.oldAppsPage else { return }
                    try? await Task.sleep(for: .milliseconds(400))
                    oldAppsOpen = true
                }
                .task {
                    guard model.sessionPreset.addToHomePage, model.hostOffersAddToHomeScreen else { return }
                    try? await Task.sleep(for: .milliseconds(400))
                    addToHomeOpen = true
                }
            }
            .listSectionSpacing(.compact)
            .endingAboveFilesBar(filesBarHeight)
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done", systemImage: "checkmark", action: close)
                }
            }
            .navigationDestination(for: FolderRoute.self) { route in
                if let browser = model.fileBrowser {
                    FolderPage(browser: browser, route: route, close: close)
                }
            }
            .navigationDestination(isPresented: $oldAppsOpen) { OldAppsPage() }
            .navigationDestination(isPresented: $addToHomeOpen) {
                AddToHomeScreenPage(query: model.sessionPreset.addToHomeQuery ?? "")
            }
        }
        // A picker that failed (Install APK and the file browser's Import are
        // here): the alert is the sheet's own, which keeps the sheet open.
        .background {
            Color.clear
                .accessibilityHidden(true)
                .alert(
                    model.documentPickerProblem?.title ?? "",
                    isPresented: Binding(
                        get: { model.documentPickerProblem != nil },
                        set: { if !$0 { model.documentPickerProblem = nil } }),
                    presenting: model.documentPickerProblem
                ) { _ in
                    Button("OK", role: .cancel) {}
                } message: { problem in
                    Text(problem.message.keepingTogether)
                }
        }
        .environment(\.filesBarHeight, filesBarHeight)
        // The bar is outside the pages, so folders slide under it.
        .safeAreaBar(edge: .bottom) {
            if let browser = model.fileBrowser,
               (model.filesAvailable && browser.browsing) || browser.transfer != nil || browser.conflict != nil {
                FilesBar(browser: browser, usable: model.filesAvailable, bottomInset: filesBarInset)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .readFilesBarHeight(into: $filesBarHeight)
        .onGeometryChange(for: CGFloat.self) { $0.safeAreaInsets.bottom } action: { filesBarInset = $0 }
        .animation(.snappy, value: model.fileBrowser?.browsing)
        .interactiveDismissDisabled(model.fileBrowser?.running == true)
        .confirmationDialog("Stop the transfer?", isPresented: $confirmingStop, titleVisibility: .visible) {
            Button("Stop and Close", role: .destructive) {
                model.fileBrowser?.closeTransfer()
                dismiss()
            }
            Button("Keep Going", role: .cancel) {}
        }
        // A browser wants the room.
        .onChange(of: model.fileBrowser?.browsing) { _, browsing in
            if browsing == true { detent = .large }
        }
        .task { await model.watchSettings() }
        .onDisappear {
            model.fileBrowser?.sheetClosed()
            model.documentPickerProblem = nil
        }
        .presentationDetents([.medium, .large], selection: $detent)
        .presentationBackground(Color(.systemGroupedBackground))
        .presentationBackgroundInteraction(.enabled(upThrough: .medium))
        // The picker takes a copy of the file (`CopyDocumentPicker`).
        .documentCopyPicker(isPresented: $importing, types: InstallRefusal.pickerTypes) { result in
            switch result {
            case .success(let picked):
                guard let url = picked.urls.first else { return }
                model.installPicked(url, staged: picked.folder)
                dismiss()
            case .failure(let error):
                model.documentPickerFailed(error, title: AppModel.installFailure)
            }
        }
    }

    private var fileRoute: Binding<[FolderRoute]> {
        Binding(get: { model.fileBrowser?.route ?? [] }, set: { model.fileBrowser?.route = $0 })
    }

    /// Done, here or on a folder. A running transfer would end with the
    /// sheet, so that is asked first.
    private func close() {
        if model.fileBrowser?.running == true {
            confirmingStop = true
        } else {
            dismiss()
        }
    }

    /// Recovery runs no adb: its operations are for a normal session.
    private var normalSession: Bool {
        if case .running(.normal) = model.phase { return true }
        return false
    }

    /// Whether this session has Install APK: the rule the side keys' tip
    /// reads to point here, one function for both.
    private var offersInstall: Bool {
        guard case .running(let mode) = model.phase else { return false }
        return model.capabilities.offersApkInstall(in: mode)
    }

    // MARK: Accessibility

    /// TalkBack, Android's screen reader, and Android's own Accessibility
    /// page. The switch follows what Android reports, read while Settings is
    /// open (`AppModel.watchSettings`), and the line under it says every state
    /// the switch cannot: not read yet, no answer, no screen reader in the
    /// image, a change under way or failed, a transfer that holds the channel.
    private func accessibility(_ session: SessionState) -> some View {
        let reader = session.reader
        let row = model.screenReaderRow
        let unavailable = row == .unavailable
        return Section {
            Toggle(isOn: Binding(get: { reader.isOn }, set: { model.setScreenReader($0) })) {
                HStack(spacing: 12) {
                    IconTile(symbol: "accessibility", color: unavailable ? .gray : .accentColor)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(ScreenReaderWords.name)
                        if let line = row.line {
                            Text(line)
                                .font(.footnote)
                                .foregroundStyle(row.isFailure ? Color.red : Color.secondary)
                        }
                        if case .failed(_, let reason) = row {
                            Text(reason)
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .disabled(!model.screenReaderCanChange)
            .accessibilityIdentifier("settings-talkback")
            if row.offersTryAgain {
                Button(ScreenReaderWords.tryAgain) {
                    switch row {
                    case .failed(let on, _): model.setScreenReader(on)
                    default: Task { await model.refreshScreenReader() }
                    }
                }
                .accessibilityIdentifier("settings-talkback-try-again")
            }
            Button {
                // Android's page opens on the virtual machine's screen, behind the sheet.
                dismiss()
                model.openAccessibilitySettings()
            } label: {
                HStack(spacing: 12) {
                    IconTile(symbol: "slider.horizontal.3", color: reader.isOn ? .accentColor : .gray)
                    Text(ScreenReaderWords.androidSettings)
                        .foregroundStyle(reader.isOn ? Color.accentColor : Color.secondary)
                    Spacer()
                }
            }
            // VoiceOver can read Android's page only while the screen reader is on.
            .disabled(!reader.isOn)
            .accessibilityIdentifier("settings-accessibility-settings")
        } header: {
            Text(ScreenReaderWords.section)
        } footer: {
            Text(unavailable ? ScreenReaderWords.footerUnavailable : ScreenReaderWords.footer(session.keysEdge))
        }
        .id("accessibility")
    }

    // MARK: File browser

    private var files: some View {
        Section("File browser") {
            NavigationLink(value: FolderRoute(path: GuestPath.root)) {
                HStack(spacing: 12) {
                    IconTile(symbol: "folder.fill")
                    Text("Internal storage")
                }
            }
        }
    }

    // MARK: Diagnostics

    /// Tapping the row starts at once.
    private var diagnostics: some View {
        Section("Diagnostics") {
            Button {
                dismiss()
                model.createBugReport()
            } label: {
                HStack(spacing: 12) {
                    IconTile(symbol: "ladybug.fill")
                    Text("Create bug report\u{2026}")
                        .foregroundStyle(Color.accentColor)
                    Spacer()
                }
            }
            // The guest's one adb channel is busy with the transfer.
            .disabled(model.fileBrowser?.busy == true)
        }
        .id("diagnostics")
    }

    // MARK: Apps

    private var apps: some View {
        Section("Apps") {
            Button {
                importing = true
            } label: {
                HStack(spacing: 12) {
                    IconTile(symbol: "square.and.arrow.down.fill")
                    // The name gets its width first: it wraps only when the
                    // line is too narrow for it alone, and the hint beside
                    // it gives way.
                    Text("Install APK\u{2026}")
                        .foregroundStyle(Color.accentColor)
                        .layoutPriority(1)
                    Spacer(minLength: 0)
                    Text("From Files")
                        .font(.subheadline)
                        .foregroundStyle(Color(.secondaryLabel))
                        .lineLimit(1)
                        .minimumScaleFactor(0.8)
                }
            }
            .disabled(model.fileBrowser?.busy == true)
            // Offered only when the last listing found an app it can apply to.
            if model.session.oldApps.offered { OldAppsRow { oldAppsOpen = true } }
            // Offered while Android runs and its image lists apps.
            if model.offersAddToHomeScreen { AddToHomeScreenRow { addToHomeOpen = true } }
        }
    }

    // MARK: Developer

    /// ADB over network with its footer, and, as a group of its own under that
    /// footer, the entrance to the text message bar, so the footer describes
    /// the toggle alone.
    @ViewBuilder
    private func developer(_ session: SessionState) -> some View {
        if model.capabilities.adbOverNetwork { adbOverNetwork(session) }
        if model.offersTextMessage {
            Section {
                textMessageRow
            } header: {
                // The group stands alone only where there is no ADB section above it.
                if !model.capabilities.adbOverNetwork { Text("Developer") }
            }
            .id(model.capabilities.adbOverNetwork ? "text-message" : "developer")
        }
    }

    private func adbOverNetwork(_ session: SessionState) -> some View {
        Section {
            Toggle(isOn: Binding(get: { session.adb }, set: { model.setAdbOverNetwork($0) })) {
                HStack(spacing: 12) {
                    IconTile(symbol: "terminal.fill", color: session.adb ? .accentColor : .gray)
                    Text("ADB over network")
                }
            }
            .disabled(!session.adb && !model.adbOverNetworkAvailable)
            if session.adb {
                if let address = session.adbAddress {
                    connectCommand("adb connect \(address):\(AdbNetworkBridge.defaultPort)")
                } else {
                    Text(AdbNetworkMessages.noAddress)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .padding(.vertical, 4)
                }
            }
        } header: {
            Text("Developer")
        } footer: {
            if let problem = session.adbProblem {
                Text(problem).foregroundStyle(.red)
            } else if session.adb {
                Text("The virtual machine asks to allow the computer the first time it connects. ADB turns off when the virtual machine stops or USB debugging is turned off.")
            } else if !model.adbOverNetworkAvailable {
                Text("Turn on USB debugging in the virtual machine\u{2019}s Developer options to use ADB over network.")
            } else {
                Text("Lets a computer on the same network connect to the virtual machine with adb.")
            }
        }
        .id("developer")
    }

    /// The entrance to the text message bar: Settings closes, as Install APK
    /// does, and the bar comes up over Android.
    private var textMessageRow: some View {
        Button {
            dismiss()
            model.presentTextMessage()
        } label: {
            HStack(spacing: 12) {
                IconTile(symbol: "message.fill")
                Text(SmsMessages.row)
                    .foregroundStyle(Color.accentColor)
                Spacer(minLength: 0)
            }
        }
        // The guest's one adb channel is busy with the transfer.
        .disabled(model.fileBrowser?.busy == true)
        .accessibilityIdentifier("settings-text-message")
    }

    /// The command to type on the computer, with a button that copies it.
    private func connectCommand(_ command: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("On a computer on the same network, run:")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            HStack(spacing: 10) {
                Text(command)
                    .font(.system(.callout, design: .monospaced).weight(.medium))
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                    .textSelection(.enabled)
                Spacer(minLength: 0)
                Button {
                    UIPasteboard.general.string = command
                    withAnimation(.snappy) { copied = true }
                    Task {
                        try? await Task.sleep(for: .seconds(1.6))
                        withAnimation(.snappy) { copied = false }
                    }
                } label: {
                    Image(systemName: copied ? "checkmark" : "doc.on.doc")
                        .font(.system(size: 15, weight: .semibold))
                        .contentTransition(.symbolEffect(.replace))
                        .frame(width: 36, height: 36)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel(copied ? "Copied" : "Copy command")
            }
            .padding(.leading, 14)
            .padding(.trailing, 4)
            .frame(minHeight: 46)
            .background(Color(.tertiarySystemFill), in: .rect(cornerRadius: 12, style: .continuous))
        }
        .padding(.vertical, 4)
    }

    // MARK: Pointer

    /// Off, Android sees a mouse with its own cursor; on, the pointer's clicks
    /// are touches, as a game that only understands touch needs.
    private var pointer: some View {
        Section {
            Toggle(isOn: Binding(get: { model.pointerAsTouch }, set: { model.setPointerAsTouch($0) })) {
                rowLabel("Use the pointer as touch", symbol: "cursorarrow.click")
            }
            .accessibilityIdentifier("pointerAsTouch")
        }
    }

    /// The text field a UI test types into while Settings is open.
    private func inputProbe(_ probe: InputProbe) -> some View {
        @Bindable var probe = probe
        return Section {
            TextField(text: $probe.fieldText) { Text(verbatim: "Input probe") }
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .accessibilityIdentifier("inputProbeField")
        }
    }

    // MARK: Stats

    private func stats(_ session: SessionState) -> some View {
        @Bindable var session = session
        let memoryMiB = model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound
        return Section("Stats") {
            LabeledContent {
                HStack(spacing: 10) {
                    Sparkline(values: session.readout.history)
                        .frame(width: 44, height: 18)
                    value(session.readout.cpuText)
                        .frame(minWidth: 44, alignment: .trailing)
                }
            } label: {
                rowLabel("CPU", symbol: "cpu")
            }
            // The rows read the statistics while the section shows; the CPU
            // row stands for it, since a modifier on the section would apply
            // to every row.
            .onAppear { model.statistics(.settings, visible: true) }
            .onDisappear { model.statistics(.settings, visible: false) }
            LabeledContent {
                usage(session.readout.memoryText(of: memoryMiB), share: session.readout.memoryFraction(of: memoryMiB))
            } label: {
                rowLabel("Memory", symbol: "memorychip")
            }
            LabeledContent {
                usage(session.readout.vramText, share: session.readout.vramFraction)
            } label: {
                rowLabel("VRAM", symbol: "cube")
            }
            LabeledContent {
                value(session.readout.fpsText)
            } label: {
                rowLabel("Frame rate", symbol: "speedometer")
            }
            Toggle(isOn: $session.showStats.animation(.snappy)) {
                rowLabel("Show on screen", symbol: "rectangle.topthird.inset.filled")
            }
        }
        .id("stats")
    }

    /// A figure of memory in use after a bar of its share, as the CPU
    /// figure follows its sparkline.
    private func usage(_ text: String, share: Double?) -> some View {
        HStack(spacing: 10) {
            ShareBar(share: share)
                .frame(width: 44, height: 6)
            value(text)
        }
    }

    private func rowLabel(_ title: LocalizedStringKey, symbol: String) -> some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol)
            Text(title)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func value(_ text: String) -> some View {
        Text(text)
            .font(.system(.body, design: .rounded).weight(.medium))
            .monospacedDigit()
            .foregroundStyle(.secondary)
            .contentTransition(.numericText())
    }
}

extension View {
    /// Settings over a session, which hides the status bar. The sheet is
    /// handed the model and keeps the status bar hidden too: on a Mac, SwiftUI
    /// reads a sheet's presentation preferences before the sheet inherits the
    /// session's environment, and a full-height sheet whose status bar differs
    /// from the session's flips its visibility on every layout until AppKit
    /// ends the application.
    func sessionSettings(isPresented: Binding<Bool>, large: Bool, model: AppModel) -> some View {
        sheet(isPresented: isPresented) {
            SessionSettingsSheet(large: large)
                .environment(model)
                .statusBarHidden(true)
                // On a turned inner display the sheet rises to the fold and
                // lets touches through to the guest above it.
                .foldSheet(model, reachesBehind: true)
        }
    }
}
