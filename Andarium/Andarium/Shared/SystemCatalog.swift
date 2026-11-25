// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
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
        if let name = object["display_name"] as? String, !name.isEmpty { return name }
        if let android = object["android"] as? [String: Any], let fingerprint = android["fingerprint"] as? String,
           let product = fingerprint.split(separator: "/").dropFirst().first {
            return "\(product) · \(object["release_id"] as? String ?? "")"
        }
        return object["release_id"] as? String
    }

    /// Imports a release folder, or any one image file (`SystemImageImport`):
    /// a full system disk, an EROFS partition image such as a GSI, a sparse
    /// image, or one of those compressed with xz or gzip. A folder without
    /// its own partition layout is read as the image inside it. Runs for
    /// minutes on a multi-gigabyte disk: never call it on the main actor.
    static func importRelease(from source: URL, progress: SystemImageImport.Progress = { _, _ in }) throws -> Entry {
        let manager = FileManager.default
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        var isDirectory: ObjCBool = false
        guard manager.fileExists(atPath: source.path, isDirectory: &isDirectory) else {
            throw ImportError.invalid("The selected item is unavailable. If it is in iCloud, download it in Files first.")
        }
        try manager.createDirectory(at: root, withIntermediateDirectories: true)
        guard isDirectory.boolValue else {
            return try importImage(source, id: uniqueID(for: stem(source)), progress: progress)
        }

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
        func anyImage() -> URL? {
            guard let walker = manager.enumerator(at: source, includingPropertiesForKeys: [.fileSizeKey]) else { return nil }
            var best: (URL, Int)?
            for case let url as URL in walker {
                if walker.level > 3 { walker.skipDescendants(); continue }
                let name = url.lastPathComponent.lowercased()
                guard [".img", ".img.xz", ".img.gz", ".raw", ".xz"].contains(where: name.hasSuffix) else { continue }
                let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
                if size > best?.1 ?? 0 { best = (url, size) }
            }
            return best?.0
        }
        let id = uniqueID(for: source.lastPathComponent)
        guard let disk = find("system.img") ?? anyImage() else { throw ImportError.missing("system image (.img)") }
        guard let layoutFile = find("partition-layout.json"),
              (try? SystemImageImport.kind(of: disk)) == .gptDisk else {
            return try importImage(disk, id: id, progress: progress)
        }
        guard let manifestFile = find(manifest) ?? bundledFolder?.appendingPathComponent(manifest),
              let commandFile = find("kernel-command-line.txt") ?? bundledFolder?.appendingPathComponent(commandLine) else {
            throw ImportError.missing(manifest)
        }
        let target = root.appendingPathComponent(id, isDirectory: true)
        try makeFolders(target)
        do {
            progress(0, "Copying the release")
            try manager.copyItem(at: manifestFile, to: target.appendingPathComponent(manifest))
            try manager.copyItem(at: commandFile, to: target.appendingPathComponent(commandLine))
            try manager.copyItem(at: layoutFile, to: target.appendingPathComponent(layout))
            try manager.copyItem(at: disk, to: target.appendingPathComponent(systemDisk))
            if let problem = check(target) { throw ImportError.invalid(problem) }
        } catch {
            try? manager.removeItem(at: target)
            throw error
        }
        return finish(target, id: id)
    }

    static func importDownload(_ archive: URL, name: String, progress: SystemImageImport.Progress) throws -> Entry {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return try importImage(archive, id: uniqueID(for: name), name: name, deleteArchive: true, progress: progress)
    }

    private static func stem(_ url: URL) -> String {
        var name = url.lastPathComponent
        for suffix in [".zip", ".xz", ".gz", ".img", ".raw"] where name.lowercased().hasSuffix(suffix) {
            name = String(name.dropLast(suffix.count))
        }
        return name
    }

    private static func makeFolders(_ target: URL) throws {
        let manager = FileManager.default
        try manager.createDirectory(at: target.appendingPathComponent("guest"), withIntermediateDirectories: true)
        try manager.createDirectory(at: target.appendingPathComponent("boot"), withIntermediateDirectories: true)
    }

    private static func finish(_ target: URL, id: String) -> Entry {
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        var folder = target
        try? folder.setResourceValues(excluded)
        return Entry(id: id, name: displayName(target) ?? id, folder: target, sizeBytes: size(of: target.appendingPathComponent(systemDisk)))
    }

    /// One image file: unpacked, then read as a whole disk or written into a
    /// copy of the carried disk as a partition.
    private static func importImage(_ source: URL, id: String, name: String? = nil, deleteArchive: Bool = false,
                                    progress: SystemImageImport.Progress) throws -> Entry {
        let manager = FileManager.default
        guard let bundled = bundledFolder else { throw ImportError.invalid("This copy of Andarium carries no system to build on.") }
        let carried = try SystemImageImport.carriedLayout()
        let staging = root.appendingPathComponent(".import-\(UUID().uuidString)", isDirectory: true)
        try manager.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: staging) }
        let target = staging.appendingPathComponent("release", isDirectory: true)

        var current = source
        var kind = try SystemImageImport.kind(of: current)
        let fromZIP = kind == .zip
        if fromZIP {
            current = try SystemImageImport.extractZIP(source, into: staging) { value, what in
                progress(value * 0.3, what)
            }
            kind = try SystemImageImport.kind(of: current)
            if deleteArchive { try manager.removeItem(at: source) }
        }
        let inputName = current.lastPathComponent
        let firstPhase = fromZIP ? 0.3 : 0.0
        let beforeImage = current
        for round in 0 ..< 3 where [.xz, .gzip, .sparse].contains(kind) {
            let next = staging.appendingPathComponent("unpacked-\(round).img")
            try SystemImageImport.unpack(current, kind: kind, into: next) { fraction, what in
                progress(firstPhase + fraction * 0.3, what)
            }
            if current.deletingLastPathComponent() == staging { try? manager.removeItem(at: current) }
            current = next
            kind = try SystemImageImport.kind(of: current)
        }
        let unpacked = current != source
        let partitionImage = kind == .erofs || kind == .ext4
        let prepared = current == beforeImage ? firstPhase : firstPhase + 0.3
        @Sendable func later(_ fraction: Double, _ what: String) {
            progress(prepared + fraction * (1 - prepared), what)
        }

        let imageBytes = size(of: current)
        let partition = SystemImageImport.targetPartition(for: inputName, in: carried.disk)
        let needed: UInt64
        if partitionImage, let old = carried.disk.partitions.first(where: { $0.name == partition }) {
            needed = carried.disk.size - old.size + SystemImageImport.alignUp(imageBytes, carried.disk.alignment)
        } else {
            needed = kind == .gptDisk && !unpacked ? imageBytes : 0
        }
        if let free = try? root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage,
           free > 0, UInt64(free) < needed {
            throw ImportError.invalid("The system disk needs \(ByteCountFormatter.string(fromByteCount: Int64(clamping: needed), countStyle: .file)) of free storage.")
        }
        try makeFolders(target)
        do {
            let disk = target.appendingPathComponent(systemDisk)
            var layoutDisk: SystemImageImport.DiskLayout
            switch kind {
            case .gptDisk:
                later(0, "Copying the system disk")
                if unpacked { try manager.moveItem(at: current, to: disk) } else { try manager.copyItem(at: current, to: disk) }
                let bytes = size(of: disk)
                var found: SystemImageImport.DiskLayout?
                var problem: String?
                for alignment in [carried.disk.alignment, 1 << 20, 4096] {
                    let candidate = try SystemImageImport.readGPT(disk, size: bytes, alignment: alignment)
                    problem = SystemImageImport.hostProblem(candidate)
                    if problem == nil { found = candidate; break }
                }
                guard let found else { throw ImportError.invalid(problem ?? "The disk's partitions cannot be read.") }
                layoutDisk = found
            case .erofs, .ext4:
                layoutDisk = try SystemImageImport.assemble(
                    replacing: partition, with: current, carried: carried.disk,
                    carriedDisk: bundled.appendingPathComponent(systemDisk), into: disk) { fraction, what in later(fraction * 0.6, what) }
                if kind == .ext4 { try SystemImageImport.mountExt4(partition, disk: disk, layout: layoutDisk) }
            case .zip:
                throw ImportError.invalid("The ZIP does not contain a supported system image.")
            case .androidBoot:
                throw ImportError.invalid("This is an Android boot image. Import a full system disk or a system partition image instead.")
            default:
                throw ImportError.invalid("This file is neither an Android system disk nor an ext4 or EROFS partition image.")
            }
            let wholeSHA = try SystemImageImport.hash(disk, layout: &layoutDisk) { fraction, what in
                later((partitionImage ? 0.6 : 0.1) + fraction * (partitionImage ? 0.4 : 0.9), what)
            }
            var document = carried.document
            document["system_disk"] = SystemImageImport.json(layoutDisk, wholeSHA256: wholeSHA)
            try JSONSerialization.data(withJSONObject: document, options: [.sortedKeys, .prettyPrinted])
                .write(to: target.appendingPathComponent(layout), options: .atomic)
            try manager.copyItem(at: bundled.appendingPathComponent(commandLine), to: target.appendingPathComponent(commandLine))
            try writeIdentity(from: bundled.appendingPathComponent(manifest), to: target.appendingPathComponent(manifest),
                              id: id, name: name ?? stem(source))
            if let problem = check(target) { throw ImportError.invalid(problem) }
        } catch {
            try? manager.removeItem(at: target)
            throw error
        }
        let destination = root.appendingPathComponent(id, isDirectory: true)
        try manager.moveItem(at: target, to: destination)
        progress(1, "Done")
        return finish(destination, id: id)
    }

    /// The carried manifest under an identity of the import's own, so it can
    /// never pass for the carried system or for another import.
    private static func writeIdentity(from carried: URL, to target: URL, id: String, name: String) throws {
        guard var object = try JSONSerialization.jsonObject(with: Data(contentsOf: carried)) as? [String: Any],
              var android = object["android"] as? [String: Any] else {
            throw ImportError.invalid("The built-in release metadata cannot be read.")
        }
        object["release_id"] = "custom-\(id)"
        object["display_name"] = name
        object.removeValue(forKey: "acceptance")
        object.removeValue(forKey: "inventory")
        android["fingerprint"] = "andarium/custom/\(id):17/custom/user"
        android["build_number"] = 1
        android["security_patch"] = ""
        android.removeValue(forKey: "build_id")
        android.removeValue(forKey: "abi_list")
        object["android"] = android
        try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]).write(to: target, options: .atomic)
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
/// removing others. Each system is a card; the chosen one is ticked. An import
/// runs off the main actor and shows its progress in place.
struct SystemImageSection: View {
    @State private var entries = SystemCatalog.all()
    @State private var chosen = SystemCatalog.chosenID
    @State private var picking = false
    @State private var importing = false
    @State private var fraction = 0.0
    @State private var phase = ""
    @State private var message: String?
    @State private var removing: SystemCatalog.Entry?
    /// The system this launch is running from.
    private let running = SystemCatalog.chosenID

    var body: some View {
        HomeSection(title: "Android system") {
            VStack(spacing: 10) {
                ForEach(entries) { entry in card(entry) }
                if !importing {
                    Menu("Download an Android skin", systemImage: "arrow.down.circle") {
                        ForEach(MysticSkin.catalog) { skin in
                            Button("\(skin.name) · \(ByteCountFormatter.string(fromByteCount: skin.bytes, countStyle: .file))") {
                                begin { progress in try await SystemCatalog.download(skin, progress: progress) }
                            }
                        }
                    }
                    .buttonStyle(.glass)
                }
                importCard
            }
            .padding(10)
        } footer: {
            SectionNote(chosen == running
                        ? "Import a system disk, an ext4 or EROFS image, or a ROM ZIP containing system.img. Each system keeps its own guest and data."
                        : "Close and reopen Andarium to start the chosen system.")
        }
        .id("systems")
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder, .item]) { result in
            guard case .success(let url) = result else { return }
            begin { progress in try SystemCatalog.importRelease(from: url, progress: progress) }
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

    private func card(_ entry: SystemCatalog.Entry) -> some View {
        let selected = entry.id == chosen
        return HStack(spacing: 12) {
                IconTile(symbol: entry.isBundled ? "shippingbox.fill" : "externaldrive.fill",
                         color: entry.problem != nil ? .gray : (entry.isBundled ? .accentColor : .indigo))
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        Text(entry.name).font(.body.weight(.semibold)).lineLimit(1)
                        if entry.id == running {
                            Text("RUNNING").font(.caption2.weight(.bold)).foregroundStyle(.green)
                        }
                    }
                    Text(entry.problem ?? Formats.size(bytes: entry.sizeBytes))
                        .font(.footnote)
                        .foregroundStyle(entry.problem != nil ? Color.orange : Color.secondary)
                        .lineLimit(2)
                }
                Spacer(minLength: 8)
                if !entry.isBundled {
                    Button(role: .destructive) { removing = entry } label: {
                        Image(systemName: "trash").font(.subheadline)
                    }
                    .buttonStyle(.borderless)
                    .accessibilityLabel("Remove \(entry.name)")
                }
                Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                    .font(.title3)
                    .foregroundStyle(selected ? Color.accentColor : Color(.tertiaryLabel))
                    .accessibilityLabel(selected ? "Chosen" : "Not chosen")
            }
            .padding(12)
            .background(selected ? Color.accentColor.opacity(0.12) : Color(.tertiarySystemFill),
                        in: .rect(cornerRadius: 18, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .strokeBorder(selected ? Color.accentColor.opacity(0.6) : .clear, lineWidth: 1.5)
            }
            .contentShape(.rect(cornerRadius: 18))
            .onTapGesture { if entry.problem == nil, !importing { choose(entry) } }
            .accessibilityAddTraits(.isButton)
            .opacity(entry.problem != nil ? 0.7 : 1)
    }

    @ViewBuilder private var importCard: some View {
        if importing {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(phase.isEmpty ? "Importing…" : phase).font(.subheadline.weight(.semibold))
                    Spacer()
                    Text(fraction, format: .percent.precision(.fractionLength(0)))
                        .font(.subheadline.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
                ProgressView(value: fraction)
                Text("Keep Andarium open until the import finishes.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(14)
            .background(Color(.tertiarySystemFill), in: .rect(cornerRadius: 18, style: .continuous))
        } else {
            Button { picking = true } label: {
                Label("Import a system image", systemImage: "square.and.arrow.down.fill")
                    .font(.body.weight(.semibold))
                    .frame(maxWidth: .infinity)
                    .frame(height: 46)
            }
            .buttonStyle(.glass)
        }
    }

    private func begin(_ operation: @escaping @Sendable (@escaping SystemImageImport.Progress) async throws -> SystemCatalog.Entry) {
        importing = true
        fraction = 0
        phase = "Preparing"
        UIApplication.shared.isIdleTimerDisabled = true
        // The work leaves the main actor: copying and hashing a system disk
        // is gigabytes, and under approachable concurrency a nonisolated
        // async function would otherwise run here and freeze the app.
        let task = Task.detached(priority: .userInitiated) { () -> Result<SystemCatalog.Entry, any Error> in
            let last = LockedTime()
            do {
                let entry = try await operation { value, what in
                    guard last.due() else { return }
                    Task { @MainActor in
                        fraction = min(max(value, 0), 1)
                        phase = what
                    }
                }
                return .success(entry)
            } catch { return .failure(error) }
        }
        Task {
            let outcome = await task.value
            UIApplication.shared.isIdleTimerDisabled = false
            importing = false
            switch outcome {
            case .success(let entry):
                entries = SystemCatalog.all()
                message = "\(entry.name) is ready. Tap it to start it next time."
            case .failure(let error):
                message = "The system could not be imported: \(error.localizedDescription)"
                entries = SystemCatalog.all()
            }
        }
    }

    private func choose(_ entry: SystemCatalog.Entry) {
        guard entry.id != chosen else { return }
        SystemCatalog.chosenID = entry.id
        chosen = entry.id
        if entry.id != running { message = "Close Andarium and open it again to start \(entry.name)." }
    }
}

/// Lets progress through at most ten times a second.
private nonisolated final class LockedTime: @unchecked Sendable {
    private let lock = NSLock()
    private var last = Date.distantPast

    func due() -> Bool {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        guard now.timeIntervalSince(last) >= 0.1 else { return false }
        last = now
        return true
    }
}
