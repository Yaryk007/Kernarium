// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import UniformTypeIdentifiers

/// The Android systems this copy of Andarium can boot: the one it carries and
/// any release the person imported into `Documents/Systems/<id>/`. A system is
/// a release directory (`guest/system.img`, `guest/partition-layout.json`,
/// `boot/kernel-command-line.txt`, `release-manifest.json`). Each imported
/// system keeps its own guest, so systems never share a data disk. The choice
/// applies at the next launch.
nonisolated enum SystemCatalog {
    static let bundledID = "bundled"
    private static let chosenKey = "andariumChosenSystem"
    static let manifest = "release-manifest.json"
    static let commandLine = "boot/kernel-command-line.txt"
    static let layout = "guest/partition-layout.json"
    static let systemDisk = "guest/system.img"

    struct Entry: Identifiable, Equatable, Sendable {
        var id: String
        var name: String
        var folder: URL
        var sizeBytes: UInt64
        var problem: String?
        var isBundled: Bool { id == SystemCatalog.bundledID }
    }

    enum ImportError: LocalizedError {
        case missing(String)
        case invalid(String)
        var errorDescription: String? {
            switch self {
            case .missing(let what): "The folder has no \(what)."
            case .invalid(let why): why
            }
        }
    }

    static var bundledFolder: URL? { Bundle.main.resourceURL?.appendingPathComponent("andarium-system") }

    static var root: URL {
        let documents = (try? FileManager.default.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true))
            ?? FileManager.default.temporaryDirectory
        return documents.appendingPathComponent("Systems", isDirectory: true)
    }

    static var chosenID: String {
        get { UserDefaults.standard.string(forKey: chosenKey) ?? bundledID }
        set { UserDefaults.standard.set(newValue, forKey: chosenKey) }
    }

    /// The folder the next start boots from: the chosen import while it is
    /// usable, else the carried system.
    static var activeFolder: URL? {
        if chosenID != bundledID, let entry = imported().first(where: { $0.id == chosenID }), entry.problem == nil {
            return entry.folder
        }
        return bundledFolder
    }

    /// Where the chosen system keeps its guest: the carried system's is
    /// `Library/Guest`, each import's `Library/Guests/<id>`.
    static func guestDirectory(base: URL) -> URL {
        guard activeFolder != bundledFolder, chosenID != bundledID else { return base }
        return base.deletingLastPathComponent().appendingPathComponent("Guests", isDirectory: true)
            .appendingPathComponent(chosenID, isDirectory: true)
    }

    static func all() -> [Entry] {
        var list: [Entry] = []
        if let bundled = bundledFolder {
            list.append(Entry(id: bundledID, name: "Built-in system", folder: bundled, sizeBytes: size(of: bundled.appendingPathComponent(systemDisk)),
                              problem: check(bundled)))
        }
        return list + imported()
    }

    static func imported() -> [Entry] {
        let manager = FileManager.default
        guard let names = try? manager.contentsOfDirectory(atPath: root.path) else { return [] }
        return names.sorted().compactMap { name in
            let folder = root.appendingPathComponent(name, isDirectory: true)
            var isDirectory: ObjCBool = false
            guard !name.hasPrefix("."), manager.fileExists(atPath: folder.path, isDirectory: &isDirectory), isDirectory.boolValue else { return nil }
            return Entry(id: name, name: displayName(folder) ?? name, folder: folder,
                         sizeBytes: size(of: folder.appendingPathComponent(systemDisk)), problem: check(folder))
        }
    }

    /// Why a release folder cannot boot, or nil when it can: its four files,
    /// an identity Andarium reads, and a disk of the size its layout declares.
    static func check(_ folder: URL) -> String? {
        let manager = FileManager.default
        for file in [manifest, commandLine, layout, systemDisk] where !manager.fileExists(atPath: folder.appendingPathComponent(file).path) {
            return "\(file) is missing"
        }
        guard DiskPath.isUsable(folder.path) else { return "Its folder name has a space, comma, colon or quotation mark." }
        do {
            let layoutData = try Data(contentsOf: folder.appendingPathComponent(layout))
            _ = try ReleaseIdentity(manifest: Data(contentsOf: folder.appendingPathComponent(manifest)), layout: layoutData)
            guard let object = try JSONSerialization.jsonObject(with: layoutData) as? [String: Any],
                  let system = object["system_disk"] as? [String: Any],
                  let declared = (system["size_bytes"] as? NSNumber)?.uint64Value else {
                return "The partition layout states no system disk size."
            }
            let actual = size(of: folder.appendingPathComponent(systemDisk))
            if actual != declared { return "The system disk is \(actual) bytes, not the \(declared) its layout declares." }
        } catch {
            return error.localizedDescription
        }
        return nil
    }

    private static func size(of url: URL) -> UInt64 {
        UInt64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
    }

    private static func displayName(_ folder: URL) -> String? {
        guard let data = try? Data(contentsOf: folder.appendingPathComponent(manifest)),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        if let android = object["android"] as? [String: Any], let fingerprint = android["fingerprint"] as? String,
           let product = fingerprint.split(separator: "/").dropFirst().first {
            return "\(product) · \(object["release_id"] as? String ?? "")"
        }
        return object["release_id"] as? String
    }

    /// Copies a release out of a folder the person picked. The folder may hold
    /// the release as built (`guest/`, `boot/`) or its files side by side;
    /// `system.img` and `partition-layout.json` are required, and a missing
    /// manifest or kernel command line is taken from the carried system.
    static func importRelease(from source: URL) throws -> Entry {
        let manager = FileManager.default
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }

        func find(_ name: String) -> URL? {
            let direct = [source.appendingPathComponent(name)] + ["guest", "boot", "andarium-system/guest", "andarium-system/boot", "andarium-system"]
                .map { source.appendingPathComponent($0).appendingPathComponent(name) }
            if let hit = direct.first(where: { manager.fileExists(atPath: $0.path) }) { return hit }
            guard let walker = manager.enumerator(at: source, includingPropertiesForKeys: nil) else { return nil }
            for case let url as URL in walker where url.lastPathComponent == name {
                if walker.level > 3 { walker.skipDescendants(); continue }
                return url
            }
            return nil
        }
        guard let disk = find("system.img") else { throw ImportError.missing("system.img") }
        guard let layoutFile = find("partition-layout.json") else { throw ImportError.missing("partition-layout.json") }
        let manifestFile = find(manifest) ?? bundledFolder?.appendingPathComponent(manifest)
        let commandFile = find("kernel-command-line.txt") ?? bundledFolder?.appendingPathComponent(commandLine)
        guard let manifestFile else { throw ImportError.missing(manifest) }
        guard let commandFile else { throw ImportError.missing("kernel-command-line.txt") }

        let id = uniqueID(for: source.lastPathComponent)
        let target = root.appendingPathComponent(id, isDirectory: true)
        try manager.createDirectory(at: target.appendingPathComponent("guest"), withIntermediateDirectories: true)
        try manager.createDirectory(at: target.appendingPathComponent("boot"), withIntermediateDirectories: true)
        do {
            try manager.copyItem(at: manifestFile, to: target.appendingPathComponent(manifest))
            try manager.copyItem(at: commandFile, to: target.appendingPathComponent(commandLine))
            try manager.copyItem(at: layoutFile, to: target.appendingPathComponent(layout))
            try manager.copyItem(at: disk, to: target.appendingPathComponent(systemDisk))
            if let problem = check(target) { throw ImportError.invalid(problem) }
        } catch {
            try? manager.removeItem(at: target)
            throw error
        }
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        var folder = target
        try? folder.setResourceValues(excluded)
        return Entry(id: id, name: displayName(target) ?? id, folder: target, sizeBytes: size(of: target.appendingPathComponent(systemDisk)))
    }

    /// Deletes an imported system and its guest.
    static func remove(_ id: String, guestBase: URL) throws {
        guard id != bundledID else { return }
        let manager = FileManager.default
        try manager.removeItem(at: root.appendingPathComponent(id, isDirectory: true))
        let guest = guestBase.deletingLastPathComponent().appendingPathComponent("Guests", isDirectory: true).appendingPathComponent(id)
        try? manager.removeItem(at: guest)
        if chosenID == id { chosenID = bundledID }
    }

    /// A folder name the kernel's `ubd` argument accepts: lower-case letters,
    /// digits, `-` and `_`.
    private static func uniqueID(for name: String) -> String {
        let allowed = Set("abcdefghijklmnopqrstuvwxyz0123456789-_")
        var base = String(name.lowercased().map { allowed.contains($0) ? $0 : "-" }.prefix(40))
        base = base.trimmingCharacters(in: CharacterSet(charactersIn: "-_"))
        if base.isEmpty || base == bundledID { base = "system" }
        var id = base, n = 2
        while FileManager.default.fileExists(atPath: root.appendingPathComponent(id).path) { id = "\(base)-\(n)"; n += 1 }
        return id
    }
}

/// Home › Android system: which system the next start boots, and importing or
/// removing others.
struct SystemImageSection: View {
    @State private var entries = SystemCatalog.all()
    @State private var chosen = SystemCatalog.chosenID
    @State private var picking = false
    @State private var importing = false
    @State private var message: String?
    @State private var removing: SystemCatalog.Entry?
    /// The system this launch is running from.
    private let running = SystemCatalog.chosenID

    var body: some View {
        HomeSection(title: "Android system") {
            ForEach(Array(entries.enumerated()), id: \.element.id) { index, entry in
                if index > 0 { RowDivider() }
                OptionRow(title: entry.name, symbol: entry.isBundled ? "shippingbox.fill" : "externaldrive.fill",
                          detail: entry.problem ?? Formats.size(bytes: entry.sizeBytes), dimmed: entry.problem != nil) {
                    HStack(spacing: 10) {
                        if entry.id == chosen {
                            Image(systemName: "checkmark").font(.body.weight(.semibold)).foregroundStyle(Color.accentColor)
                                .accessibilityLabel("Chosen")
                        } else {
                            Button("Use") { choose(entry) }.buttonStyle(.bordered).disabled(entry.problem != nil)
                        }
                        if !entry.isBundled {
                            Button(role: .destructive) { removing = entry } label: { Image(systemName: "trash") }
                                .buttonStyle(.borderless).accessibilityLabel("Remove \(entry.name)")
                        }
                    }
                }
            }
            RowDivider()
            OptionRow(title: importing ? "Importing…" : "Import a system", symbol: "square.and.arrow.down.fill",
                      detail: "A folder with system.img and partition-layout.json") {
                if importing { ProgressView() } else {
                    Button("Choose") { picking = true }.buttonStyle(.bordered)
                }
            }
        } footer: {
            SectionNote(chosen == running
                        ? "Each imported system keeps its own guest and data. The system must be an Andarium release: an Android GPT disk built for this kernel."
                        : "Close and reopen Andarium to start the chosen system.")
        }
        .id("systems")
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { result in
            guard case .success(let url) = result else { return }
            importing = true
            Task {
                let outcome = await Self.copy(url)
                importing = false
                switch outcome {
                case .success(let entry): message = "\(entry.name) is ready. Choose Use to start it next time."
                case .failure(let error): message = "The system could not be imported: \(error.localizedDescription)"
                }
                entries = SystemCatalog.all()
            }
        }
        .alert("Android system", isPresented: Binding(get: { message != nil }, set: { if !$0 { message = nil } })) {
            Button("OK", role: .cancel) {}
        } message: { Text(message ?? "") }
        .confirmationDialog("Remove \(removing?.name ?? "")?", isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }),
                            titleVisibility: .visible) {
            Button("Remove system and its guest", role: .destructive) {
                if let removing {
                    do {
                        let base = (try? FileManager.default.url(for: .libraryDirectory, in: .userDomainMask, appropriateFor: nil, create: true))?
                            .appendingPathComponent("Guest") ?? SystemCatalog.root
                        try SystemCatalog.remove(removing.id, guestBase: base)
                    } catch { message = error.localizedDescription }
                }
                removing = nil
                entries = SystemCatalog.all(); chosen = SystemCatalog.chosenID
            }
        } message: { Text("Its Android data is deleted too.") }
    }

    /// The copy runs off the main thread: a system disk is gigabytes.
    private nonisolated static func copy(_ url: URL) async -> Result<SystemCatalog.Entry, any Error> {
        Result { try SystemCatalog.importRelease(from: url) }
    }

    private func choose(_ entry: SystemCatalog.Entry) {
        SystemCatalog.chosenID = entry.id
        chosen = entry.id
        if entry.id != running { message = "Close Andarium and open it again to start \(entry.name)." }
    }
}
