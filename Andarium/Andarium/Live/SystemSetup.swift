// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Foundation
import SwiftUI
import UIKit

nonisolated enum GuestSystemChoice: String, CaseIterable, Identifiable, Sendable {
    case lineage = "LineageOS 24"
    case xos = "XOS"
    case hyperos = "HyperOS"
    case coloros = "ColorOS"
    case oneUI = "One UI"

    var id: String { rawValue }

    var storageName: String {
        switch self {
        case .lineage: "lineage"
        case .xos: "xos"
        case .hyperos: "hyperos"
        case .coloros: "coloros"
        case .oneUI: "one-ui"
        }
    }
}

nonisolated enum SystemInstallation {
    private static let archiveLimit: UInt64 = 8 * 1024 * 1024 * 1024
    private static let diskLimit: UInt64 = 16 * 1024 * 1024 * 1024
    private static let metadataLimit = 1024 * 1024
    private static let chunkSize = 256 * 1024
    private static let selectionFile = "selected-os"
    static let officialURL = URL(string: "https://github.com/imxieyi/Kernarium/releases/download/andarium-1.1/Andarium-1.1-16.ipa")!
    private static let officialSHA256 = "da277dced66f2208299232172168f71add6de4252a2cbb3f63d450626bf20ac7"

    static var directory: URL {
        URL.libraryDirectory.appendingPathComponent("InstalledAndroid", isDirectory: true)
    }

    static func selection(in folder: URL) -> GuestSystemChoice? {
        guard let data = try? String(contentsOf: folder.appendingPathComponent(selectionFile), encoding: .utf8) else {
            return nil
        }
        return GuestSystemChoice(rawValue: data)
    }

    static var installed: URL? {
        selection(in: directory) == nil ? nil : directory
    }

    enum Problem: LocalizedError {
        case address, download, archive, incompatible, checksum, alreadyInstalled

        var errorDescription: String? {
            switch self {
            case .address: "Enter a direct HTTPS link to an Andarium guest ZIP."
            case .download: "The server did not return a usable download."
            case .archive: "The download must contain one Andarium guest release and its required files."
            case .incompatible: "The image must be an arm64 Android 17 guest for Andarium's 4 KiB kernel."
            case .checksum: "The download or its system disk failed the integrity check."
            case .alreadyInstalled: "An Android system is already installed."
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
        let info = try SystemInfo(manifest: manifest, appVersion: "")
        let manifestObject = try JSONSerialization.jsonObject(with: manifest) as? [String: Any]
        let kernel = manifestObject?["kernel"] as? [String: Any]
        let family = manifestObject?["product_family"] as? String
        let expectedFamily = choice == .lineage ? "lineage-24" : choice.storageName
        guard family == expectedFamily || family?.hasPrefix(expectedFamily + "-") == true,
              info.version == "17", info.abis.contains("arm64-v8a"),
              kernel?["page_size"] as? Int == 4096,
              kernel?["abi_version"] as? Int == 29,
              (kernel?["release"] as? String)?.hasPrefix("6.18.") == true
        else { throw Problem.incompatible }
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

    private static func archiveDigest(_ file: URL) throws -> String {
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
    var didInstall: () -> Void

    @State private var choice = GuestSystemChoice.lineage
    @State private var customAddress = ""
    @State private var job: Task<Void, Never>?
    @State private var installing = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("Operating system", selection: $choice) {
                        ForEach(GuestSystemChoice.allCases) { Text($0.rawValue).tag($0) }
                    }
                    .disabled(job != nil)
                } footer: {
                    if choice == .lineage {
                        Text("Downloads the official LineageOS guest and keeps it on this device. About 1.25 GB to download.")
                    } else {
                        Text("Use a direct link to an Andarium-compatible guest ZIP. Phone firmware and OTA packages cannot boot here.")
                    }
                }
                if choice != .lineage {
                    Section("Guest ZIP URL") {
                        TextField("https://", text: $customAddress)
                            .keyboardType(.URL)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .disabled(job != nil)
                    }
                }
                Section {
                    if let job {
                        ProgressView(installing ? "Importing and verifying…" : "Downloading…")
                        Button("Cancel", role: .cancel) { job.cancel() }
                    } else {
                        Button("Download and install", action: start)
                    }
                    if let error { Text(error).foregroundStyle(.red) }
                }
            }
            .navigationTitle("Set up Andarium")
        }
    }

    private func start() {
        let selected = choice
        error = nil
        job = Task {
            defer {
                job = nil
                installing = false
            }
            let owned = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: owned) }
            do {
                let url = try selected == .lineage ? SystemInstallation.officialURL : SystemInstallation.customURL(customAddress)
                let (file, response) = try await URLSession.shared.download(from: url)
                defer { try? FileManager.default.removeItem(at: file) }
                guard let http = response as? HTTPURLResponse, http.statusCode == 200,
                      let final = http.url, SystemInstallation.isHTTPS(final)
                else { throw SystemInstallation.Problem.download }
                try FileManager.default.moveItem(at: file, to: owned)
                try Task.checkCancellation()
                installing = true
                let worker = Task.detached(priority: .utility) {
                    try SystemInstallation.install(owned, choice: selected, at: SystemInstallation.directory)
                }
                try await withTaskCancellationHandler {
                    try await worker.value
                } onCancel: {
                    worker.cancel()
                }
                didInstall()
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
    @MainActor
    func presentSystemSetup(in window: UIWindow, onInstall: @escaping () -> Void) -> Bool {
        guard SystemInstallation.installed == nil,
              Bundle.main.url(forResource: SystemFolder.name, withExtension: nil) == nil,
              overrides.isEmpty,
              ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] == nil
        else { return false }
        window.rootViewController = UIHostingController(rootView: SystemSetupView(didInstall: onInstall))
        return true
    }
}
