// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Foundation
import SwiftUI
import UIKit
import UniformTypeIdentifiers

nonisolated enum GuestSystemChoice: String, CaseIterable, Identifiable, Sendable {
    case lineage = "LineageOS 24"
    case lineageGApps = "LineageOS 24 with GApps"
    case xos = "XOS"
    case hyperos = "HyperOS"
    case coloros = "ColorOS"
    case oneUI = "One UI"
    case custom = "Custom image"

    var id: String { rawValue }
    var title: String { self == .lineage ? "LineageOS 24 (r28)" : rawValue }
    var skin: MysticSkin? { MysticSkin.catalog.first { $0.name == rawValue } }

    var storageName: String {
        switch self {
        case .lineage: "lineage"
        case .lineageGApps: "lineage-gapps"
        case .xos: "xos"
        case .hyperos: "hyperos"
        case .coloros: "coloros"
        case .oneUI: "one-ui"
        case .custom: "custom"
        }
    }
}

nonisolated enum SystemInstallation {
    private static let archiveLimit: UInt64 = 8 * 1024 * 1024 * 1024
    static let diskLimit: UInt64 = 16 * 1024 * 1024 * 1024
    private static let metadataLimit = 1024 * 1024
    private static let chunkSize = 256 * 1024
    static let selectionFile = "selected-os"
    static let officialURL = URL(string: "https://github.com/imxieyi/Kernarium/releases/download/1.0b15-release/Andarium-1.0-15.ipa")!
    static let officialBytes: Int64 = 1_061_900_449
    private static let officialRelease = "20261115-los24-r28"
    private static let officialSHA256 = "88327002e554190a6e4e084265bc69a1e097d52352824e5808eb28042b341ddc"
    private static let selectedKey = "andariumSelectedInstalledAndroid"
    static let showPickerKey = "andariumChooseAndroidNextLaunch"

    static func directory(for choice: GuestSystemChoice) -> URL {
        URL.libraryDirectory.appendingPathComponent("AndroidSystems", isDirectory: true)
            .appendingPathComponent(choice.storageName, isDirectory: true)
    }

    static func selection(in folder: URL) -> GuestSystemChoice? {
        guard let data = try? String(contentsOf: folder.appendingPathComponent(selectionFile), encoding: .utf8) else {
            return nil
        }
        return GuestSystemChoice(rawValue: data)
    }

    static var installed: URL? {
        if let value = UserDefaults.standard.string(forKey: selectedKey),
           let choice = GuestSystemChoice(rawValue: value) {
            let folder = directory(for: choice)
            if selection(in: folder) == choice { return folder }
        }
        let legacy = URL.libraryDirectory.appendingPathComponent("InstalledAndroid", isDirectory: true)
        return selection(in: legacy) == nil ? nil : legacy
    }

    static var activeChoice: GuestSystemChoice? { installed.flatMap(selection(in:)) }

    static func select(_ choice: GuestSystemChoice) {
        UserDefaults.standard.set(choice.rawValue, forKey: selectedKey)
        UserDefaults.standard.removeObject(forKey: showPickerKey)
    }

    enum Problem: LocalizedError {
        case address, download, archive, incompatible, checksum, alreadyInstalled
        case invalid(String)

        var errorDescription: String? {
            switch self {
            case .address: "Enter a direct HTTPS link to a system image or ROM ZIP."
            case .download: "The server did not return a usable download."
            case .archive: "Choose an Andarium guest ZIP or a ROM archive containing a single system image."
            case .incompatible: "This image does not match Andarium’s arm64 4 KiB guest format."
            case .checksum: "The download or its system disk failed the integrity check."
            case .alreadyInstalled: "This Android system is already installed."
            case .invalid(let message): message
            }
        }
    }

    static func customURL(_ value: String) throws -> URL {
        guard let url = URL(string: value.trimmingCharacters(in: .whitespacesAndNewlines)),
              isHTTPS(url)
        else { throw Problem.address }
        return url
    }

    static func isHTTPS(_ url: URL) -> Bool {
        url.scheme?.lowercased() == "https" && url.host?.isEmpty == false
            && url.user == nil && url.password == nil
    }

    /// Extract only the files the runtime reads. The destination becomes visible
    /// to the app only after the release has passed validation.
    static func install(_ archive: URL, choice: GuestSystemChoice, at destination: URL) throws {
        let manager = FileManager.default
        guard !manager.fileExists(atPath: destination.path) else { throw Problem.alreadyInstalled }
        let source = try ZipFileSource(contentsOf: archive)
        guard source.size <= archiveLimit else { throw Problem.archive }
        if choice == .lineage, try archiveDigest(archive) != officialSHA256 { throw Problem.checksum }

        let release = try ZipArchive(source: source, checkpoint: { try Task.checkCancellation() })
        let manifests = release.entries.filter {
            $0.name == SystemFolder.manifest || $0.name.hasSuffix("/" + SystemFolder.manifest)
        }
        guard manifests.count == 1, let entry = manifests.first else { throw Problem.archive }
        let prefix = String(entry.name.dropLast(SystemFolder.manifest.count))
        let manifest = try release.data(of: entry, limit: metadataLimit)
        let kind: GuestManifest.Kind = switch choice {
        case .lineage: .official(release: officialRelease)
        case .lineageGApps: .gapps(base: officialRelease)
        case .custom: .other(family: nil)
        default: .other(family: choice.storageName)
        }
        guard GuestManifest.fits(manifest, as: kind) else { throw Problem.incompatible }
        // Home reads the same manifest again, so one it cannot read is refused here.
        _ = try SystemInfo(manifest: manifest, appVersion: "")
        let layout = try release.data(of: member(SystemFolder.layout, prefix: prefix, in: release), limit: metadataLimit)
        let identity = try ReleaseIdentity(manifest: manifest, layout: layout)
        let layoutObject = try JSONSerialization.jsonObject(with: layout) as? [String: Any]
        let disk = layoutObject?["system_disk"] as? [String: Any]
        let partitions = disk?["partitions"] as? [[String: Any]] ?? []
        let names = Set(partitions.compactMap { $0["name"] as? String })
        guard layoutObject?["schema_version"] as? Int == 2,
              layoutObject?["sector_size"] as? Int == 512,
              names.isSuperset(of: ["init_boot", "recovery", "system", "vendor"])
        else { throw Problem.incompatible }
        guard identity.minimumGiB <= identity.defaultGiB,
              identity.defaultGiB <= identity.maximumGiB,
              !identity.isUnsignedProof
        else { throw Problem.incompatible }

        var stage = destination.deletingLastPathComponent()
            .appendingPathComponent(".android-install-" + UUID().uuidString, isDirectory: true)
        try manager.createDirectory(at: stage, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: stage) }
        var attributes = URLResourceValues()
        attributes.isExcludedFromBackup = true
        try stage.setResourceValues(attributes)

        let paths: [(String, UInt64)] = [
            (SystemFolder.manifest, UInt64(metadataLimit)), (SystemFolder.layout, UInt64(metadataLimit)),
            (SystemFolder.commandLine, UInt64(metadataLimit)), (SystemFolder.systemDisk, diskLimit),
        ]
        for (path, limit) in paths {
            let digest = try extract(path, prefix: prefix, from: release, into: stage, limit: limit)
            if path == SystemFolder.systemDisk, digest != identity.systemDiskSHA256 {
                throw Problem.checksum
            }
        }

        // Only the pinned upstream archive is trusted to supply a patched ramdisk.
        if choice == .lineage, let root = RootRelease(manifest: manifest) {
            for path in [RootRelease.bootImage, root.apkPath] {
                _ = try extract(path, prefix: prefix, from: release, into: stage, limit: 128 * 1024 * 1024)
            }
        }
        _ = try SystemFolder.release(in: stage)
        let diskFile = try FileHandle(forReadingFrom: stage.appendingPathComponent(SystemFolder.systemDisk))
        defer { try? diskFile.close() }
        try diskFile.seek(toOffset: 512)
        guard try diskFile.read(upToCount: 8) == Data("EFI PART".utf8) else { throw Problem.incompatible }
        try choice.rawValue.write(to: stage.appendingPathComponent(selectionFile), atomically: true, encoding: .utf8)
        try Task.checkCancellation()
        try manager.moveItem(at: stage, to: destination)
    }

    private static func member(_ path: String, prefix: String, in archive: ZipArchive) throws -> ZipEntry {
        guard let entry = archive.entry(named: prefix + path), !entry.isDirectory, entry.size > 0 else {
            throw Problem.archive
        }
        return entry
    }

    private static func extract(
        _ path: String, prefix: String, from archive: ZipArchive, into directory: URL, limit: UInt64
    ) throws -> String {
        let entry = try member(path, prefix: prefix, in: archive)
        guard entry.size <= limit else { throw Problem.archive }
        let file = directory.appendingPathComponent(path)
        let manager = FileManager.default
        try manager.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        guard manager.createFile(atPath: file.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        let output = try FileHandle(forWritingTo: file)
        defer { try? output.close() }
        let reader = try archive.reader(for: entry, capacity: chunkSize)
        var hash = SHA256()
        while let bytes = try reader.read(upTo: chunkSize) {
            try output.write(contentsOf: bytes)
            hash.update(data: bytes)
        }
        return hex(hash.finalize())
    }

    static func archiveDigest(_ file: URL) throws -> String {
        let input = try FileHandle(forReadingFrom: file)
        defer { try? input.close() }
        var digest = SHA256()
        while let bytes = try input.read(upToCount: chunkSize), !bytes.isEmpty {
            try Task.checkCancellation()
            digest.update(data: bytes)
        }
        return hex(digest.finalize())
    }

    private static func hex(_ digest: SHA256.Digest) -> String {
        digest.map { String(format: "%02x", $0) }.joined()
    }
}

struct SystemSetupView: View {
    var canReturn: Bool
    var didInstall: () -> Void

    @State private var choice = SystemInstallation.activeChoice ?? .lineage
    @State private var customAddress = ""
    @State private var picking = false
    @State private var job: Task<Void, Never>?
    @State private var progress = GuestSystemInstallProgress.working("Preparing…")
    @State private var error: String?

    private var available: [GuestSystemChoice] { [.lineage, .oneUI, .hyperos, .coloros, .custom] }
    private var displayedChoice: GuestSystemChoice { choice == .lineageGApps ? .lineage : choice }
    private var alreadyInstalled: Bool {
        SystemInstallation.selection(in: SystemInstallation.directory(for: choice)) == choice
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("Operating system", selection: Binding(get: { displayedChoice }, set: { choice = $0 })) {
                        ForEach(available) { Text($0.title).tag($0) }
                    }
                    .disabled(job != nil)
                } footer: {
                    if displayedChoice == .lineage {
                        Text("Downloads the official LineageOS 24 r28 guest on request. About 1.06 GB.")
                    } else if let skin = choice.skin {
                        Text("Downloads the \(skin.name) image from MysticGSI and uses LineageOS r28 for the guest’s boot and vendor partitions.")
                    } else {
                        Text("Choose a local system image or ROM ZIP, or enter a direct HTTPS download URL.")
                    }
                }
                if displayedChoice == .lineage {
                    Section {
                        Toggle("GApps", isOn: Binding(
                            get: { choice == .lineageGApps },
                            set: { enabled in
                                choice = enabled ? .lineageGApps : .lineage
                                error = nil
                            }
                        ))
                        .disabled(job != nil)
                    } footer: {
                        Text("LineageOS keeps separate Android apps and data with GApps on or off.")
                    }
                }
                if choice == .custom, !alreadyInstalled {
                    Section("Custom image") {
                        Button("Choose from Files", systemImage: "folder") { picking = true }
                            .disabled(job != nil)
                        TextField("https://", text: $customAddress)
                            .keyboardType(.URL)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .disabled(job != nil)
                    }
                }
                Section {
                    if let job {
                        installProgress
                        Button("Cancel", role: .cancel) { job.cancel() }
                    } else if alreadyInstalled {
                        Button("Use installed system") { finish(choice) }
                    } else if choice == .lineageGApps {
                        Button("Import LineageOS with GApps", systemImage: "folder") { picking = true }
                        Text("Choose an Andarium guest ZIP built from LineageOS r28 with GApps included.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    } else {
                        Button("Download and install") { start() }
                            .disabled(choice == .custom && customAddress.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                    if let error { Text(error).foregroundStyle(.red) }
                }
            }
            .navigationTitle("Choose Android")
            .toolbar {
                if canReturn {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Keep current system") {
                            UserDefaults.standard.removeObject(forKey: SystemInstallation.showPickerKey)
                            didInstall()
                        }
                        .disabled(job != nil)
                    }
                }
            }
            .documentCopyPicker(isPresented: $picking, types: [.item]) { result in
                switch result {
                case .success(let copies):
                    guard let source = copies.urls.first else { return }
                    start(local: source, keep: copies.folder)
                case .failure(let failure):
                    error = failure.localizedDescription
                }
            }
        }
    }

    @ViewBuilder private var installProgress: some View {
        switch progress {
        case .working(let label):
            ProgressView(label)
        case .downloading(let label, let received, let total, let speed):
            VStack(alignment: .leading, spacing: 8) {
                if let total {
                    ProgressView(value: Double(min(received, total)), total: Double(total)) { Text(label) }
                } else {
                    ProgressView(label)
                }
                HStack {
                    if let total {
                        Text("\(byteCount(received)) of \(byteCount(total))")
                    } else {
                        Text("\(byteCount(received)) downloaded")
                    }
                    Spacer()
                    if let speed {
                        Text("\(byteCount(speed))/s")
                            .accessibilityLabel("\(byteCount(speed)) per second")
                    }
                }
                .font(.footnote.monospacedDigit())
                .foregroundStyle(.secondary)
            }
        }
    }

    private func byteCount(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    private func finish(_ selected: GuestSystemChoice) {
        SystemInstallation.select(selected)
        didInstall()
    }

    private func start(local: URL? = nil, keep folder: PickedFolder? = nil) {
        let selected = choice
        let address = customAddress
        let destination = SystemInstallation.directory(for: selected)
        error = nil
        progress = .working("Preparing…")
        // A locked phone stops the download.
        UIApplication.shared.isIdleTimerDisabled = true
        job = Task {
            defer {
                UIApplication.shared.isIdleTimerDisabled = false
                withExtendedLifetime(folder) {}
                job = nil
            }
            do {
                try await GuestSystemInstaller.install(selected, address: address, local: local, at: destination) { update in
                    progress = update
                }
                finish(selected)
            } catch {
                if !Task.isCancelled {
                    self.error = error is ZipError ? SystemInstallation.Problem.archive.localizedDescription
                        : error.localizedDescription
                }
            }
        }
    }
}

extension AppEntry {
    /// Interrupted installs hold gigabytes; a harness launch leaves the person's alone.
    func sweepInstallLeftovers() {
        guard overrides.isEmpty, ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] == nil else { return }
        // Listed before an install can begin, so that none of its own is in it.
        let leftovers = SystemImageImport.leftovers()
        Task.detached(priority: .utility) {
            for leftover in leftovers { try? FileManager.default.removeItem(at: leftover) }
        }
    }

    @MainActor
    func presentSystemSetup(in window: UIWindow, onInstall: @escaping () -> Void) -> Bool {
        let installed = SystemInstallation.installed != nil
            || Bundle.main.url(forResource: SystemFolder.name, withExtension: nil) != nil
        guard (!installed || UserDefaults.standard.bool(forKey: SystemInstallation.showPickerKey)),
              overrides.isEmpty, ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] == nil
        else { return false }
        window.rootViewController = UIHostingController(rootView: SystemSetupView(canReturn: installed, didInstall: onInstall))
        return true
    }
}
