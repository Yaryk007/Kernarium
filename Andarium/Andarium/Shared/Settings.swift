// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// Settings while the guest runs, opened from the side keys. It slides up over
/// the guest, which keeps running behind it: watch the stats, install an app
/// from Files, connect a computer with adb and make a bug report. An
/// operation on the guest dismisses Settings and shows the full-screen
/// progress, and then how it ended.
struct SessionSettingsSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false
    @State private var copied = false
    @State private var confirmingStop = false
    @State private var filesBarInset: CGFloat = 0
    @State private var filesBarHeight: CGFloat = 0
    @State private var detent: PresentationDetent

    init(large: Bool) {
        _detent = State(initialValue: large ? .large : .medium)
    }

    var body: some View {
        let session = model.session
        NavigationStack(path: fileRoute) {
            ScrollViewReader { proxy in
                List {
                    if model.capabilities.statistics { stats(session) }
                    if normalSession, model.capabilities.apkInstall { apps }
                    if normalSession, model.capabilities.guestFiles, model.fileBrowser != nil { files }
                    if normalSession, model.capabilities.adbOverNetwork { developer(session) }
                    if normalSession, model.capabilities.bugReport { diagnostics }
                }
                .task {
                    guard let target = model.sessionPreset.settingsScroll else { return }
                    try? await Task.sleep(for: .milliseconds(500))
                    proxy.scrollTo(target, anchor: .top)
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
        .onDisappear { model.fileBrowser?.sheetClosed() }
        .presentationDetents([.medium, .large], selection: $detent)
        .presentationBackground(Color(.systemGroupedBackground))
        .presentationBackgroundInteraction(.enabled(upThrough: .medium))
        .fileImporter(isPresented: $importing, allowedContentTypes: [InstallRefusal.pickerType]) { result in
            guard case .success(let url) = result else { return }
            // Settings goes; the install, or the refusal of a file that is not
            // one .apk, is on the full-screen overlay.
            model.installPicked(url)
            dismiss()
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
    }

    // MARK: Apps

    private var apps: some View {
        Section("Apps") {
            Button {
                importing = true
            } label: {
                HStack(spacing: 12) {
                    IconTile(symbol: "square.and.arrow.down.fill")
                    Text("Install APK\u{2026}")
                        .foregroundStyle(Color.accentColor)
                    Spacer()
                    Text("From Files")
                        .font(.subheadline)
                        .foregroundStyle(Color(.secondaryLabel))
                }
            }
            .disabled(model.fileBrowser?.busy == true)
        }
    }

    // MARK: Developer

    private func developer(_ session: SessionState) -> some View {
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
                Text("The guest asks to allow the computer the first time it connects. ADB turns off when the guest stops or USB debugging is turned off.")
            } else if !model.adbOverNetworkAvailable {
                Text("Turn on USB debugging in the guest\u{2019}s Developer options to use ADB over network.")
            } else {
                Text("Lets a computer on the same network connect to the guest with adb.")
            }
        }
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

    private func rowLabel(_ title: String, symbol: String) -> some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol)
            Text(title)
                .lineLimit(1)
                .minimumScaleFactor(0.85)
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
        }
    }
}
